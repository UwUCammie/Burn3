#include "takedown/audio/host_audio_sink.hpp"

#include "takedown/audio/audio_queue_upper_bound.hpp"
#include "takedown/audio/time_stretch.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace takedown::audio {

namespace {

// Frames of recent output kept for the emergency time-stretch filler.
constexpr std::size_t kHistoryFrames = 4096U;

[[nodiscard]] SDL_AudioSpec s16_spec(std::uint32_t sample_rate, std::uint8_t channels) noexcept {
    return SDL_AudioSpec{SDL_AUDIO_S16LE, channels, static_cast<int>(sample_rate)};
}

} // namespace

struct HostAudioSink::Impl {
    // Producer-side bound on the device queue; invalidated by every write.
    AudioQueueUpperBound queue_upper_bound;
    OpenParams params{};
    SDL_AudioStream* stream{nullptr};
    SDL_AudioDeviceID device{0};
    bool sdl_audio_initialized{false};
    Status status{Status::Closed};
    float gain{1.0f};
    bool buffering_paused{false};
    bool playback_started{false};
    bool submitted_audio{false};
    std::uint64_t underruns_recovered{0};
    std::uint64_t no_device_frames_discarded{0};
    std::uint64_t overflow_resyncs{0};
    std::uint64_t overflow_frames_dropped{0};
    bool time_stretch_enabled{false};
    // Ring of the most recent interleaved samples handed to the device.
    std::vector<std::int16_t> history{};
    std::size_t history_write{0};
    std::size_t history_count{0};
    std::uint64_t stretch_fills{0};
    std::uint64_t stretched_frames{0};
};

void apply_gain(HostAudioSink::Impl& impl) {
    if (impl.stream != nullptr) {
        SDL_SetAudioStreamGain(impl.stream, impl.gain);
    }
}

HostAudioSink::HostAudioSink() : impl_(std::make_unique<Impl>()) {}

HostAudioSink::~HostAudioSink() {
    close();
}

void HostAudioSink::close() {
    impl_->queue_upper_bound.invalidate();
    if (impl_->stream != nullptr) {
        SDL_UnbindAudioStream(impl_->stream);
        SDL_DestroyAudioStream(impl_->stream);
        impl_->stream = nullptr;
    }
    if (impl_->device != 0) {
        SDL_CloseAudioDevice(impl_->device);
        impl_->device = 0;
    }
    if (impl_->sdl_audio_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        impl_->sdl_audio_initialized = false;
    }
    impl_->status = Status::Closed;
    impl_->buffering_paused = false;
    impl_->playback_started = false;
    impl_->submitted_audio = false;
    impl_->underruns_recovered = 0;
    impl_->no_device_frames_discarded = 0;
    impl_->overflow_resyncs = 0;
    impl_->overflow_frames_dropped = 0;
    impl_->history.clear();
    impl_->history_write = 0;
    impl_->history_count = 0;
    impl_->stretch_fills = 0;
    impl_->stretched_frames = 0;
}

HostAudioSink::HostAudioSink(HostAudioSink&&) noexcept = default;
HostAudioSink& HostAudioSink::operator=(HostAudioSink&&) noexcept = default;

HostAudioSink::Status HostAudioSink::status() const noexcept {
    return impl_->status;
}

std::string_view HostAudioSink::backend_name() const noexcept {
    const char* driver = SDL_GetCurrentAudioDriver();
    return driver != nullptr ? std::string_view{driver} : std::string_view{"unopened"};
}

bool HostAudioSink::open() {
    return open(OpenParams{});
}

bool HostAudioSink::open(const OpenParams& params) {
    close();
    // SDL takes the rate as an int.
    if (params.sample_rate == 0 || params.sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        params.channels == 0) {
        return false;
    }
    if (params.device_required && !params.open_playback_device) {
        return false;
    }

    impl_->params = params;
    impl_->time_stretch_enabled = params.time_stretch_underruns;
    impl_->gain = std::isfinite(params.gain) ? std::clamp(params.gain, 0.0f, 1.0f) : 1.0f;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::cerr << "HostAudioSink: SDL audio init failed: " << SDL_GetError() << '\n';
        return false;
    }
    impl_->sdl_audio_initialized = true;

    const SDL_AudioSpec spec = s16_spec(params.sample_rate, params.channels);
    impl_->stream = SDL_CreateAudioStream(&spec, &spec);
    if (impl_->stream == nullptr) {
        std::cerr << "HostAudioSink: SDL_CreateAudioStream failed: " << SDL_GetError() << '\n';
        close();
        return false;
    }
    apply_gain(*impl_);
    impl_->status = Status::Partial;
    if (!params.open_playback_device) {
        return true;
    }

    impl_->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (impl_->device == 0) {
        if (params.device_required) {
            std::cerr << "HostAudioSink: SDL_OpenAudioDevice failed: " << SDL_GetError() << '\n';
            close();
            return false;
        }
        // No device: stay Partial and discard submissions.
        return true;
    }
    if (!SDL_BindAudioStream(impl_->device, impl_->stream)) {
        std::cerr << "HostAudioSink: SDL_BindAudioStream failed: " << SDL_GetError() << '\n';
        SDL_CloseAudioDevice(impl_->device);
        impl_->device = 0;
        if (params.device_required) {
            close();
            return false;
        }
        return true;
    }
    impl_->status = Status::Playing;

    if (params.startup_buffer_frames != 0) {
        // Hold the device until the startup buffer has been queued.
        if (!SDL_PauseAudioDevice(impl_->device)) {
            std::cerr << "HostAudioSink: initial SDL_PauseAudioDevice failed: " << SDL_GetError() << '\n';
            if (params.device_required) {
                close();
                return false;
            }
        } else {
            impl_->buffering_paused = true;
        }
    }
    return true;
}

bool HostAudioSink::submit(const std::span<const std::int16_t> samples) {
    impl_->queue_upper_bound.invalidate();
    if (impl_->status == Status::Closed || impl_->stream == nullptr || samples.empty() ||
        samples.size() % impl_->params.channels != 0) {
        return false;
    }
    // SDL takes the byte count as an int.
    if (samples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) / sizeof(std::int16_t)) {
        return false;
    }

    if (impl_->status == Status::Partial || impl_->device == 0) {
        // No device: drop the audio rather than let SDL's queue grow forever.
        impl_->no_device_frames_discarded +=
            std::min(std::numeric_limits<std::uint64_t>::max() - impl_->no_device_frames_discarded,
                     samples.size() / impl_->params.channels);
        return true;
    }

    const auto queued_before = queued_frames();

    if (impl_->status == Status::Playing && impl_->params.max_queued_frames != 0 &&
        queued_before > impl_->params.max_queued_frames) {
        // Drop the stale backlog and keep the newest audio.
        SDL_ClearAudioStream(impl_->stream);
        ++impl_->overflow_resyncs;
        impl_->overflow_frames_dropped +=
            std::min(std::numeric_limits<std::uint64_t>::max() - impl_->overflow_frames_dropped,
                     static_cast<std::uint64_t>(queued_before));
    }

    if (impl_->status == Status::Playing && impl_->playback_started && !impl_->buffering_paused &&
        impl_->params.underrun_recovery_frames != 0 && impl_->submitted_audio && queued_before == 0 &&
        !impl_->time_stretch_enabled) {
        // The queue drained: pause and refill before resuming.
        if (SDL_PauseAudioDevice(impl_->device)) {
            impl_->buffering_paused = true;
            ++impl_->underruns_recovered;
        }
    }

    // Keep a ring of recent output for the emergency time-stretch filler.
    if (impl_->time_stretch_enabled) {
        const auto history_capacity = impl_->params.channels * kHistoryFrames;
        if (impl_->history.size() != history_capacity) {
            impl_->history.assign(history_capacity, 0);
        }
        for (std::size_t index = 0; index < samples.size(); ++index) {
            impl_->history[impl_->history_write] = samples[index];
            impl_->history_write = (impl_->history_write + 1) % history_capacity;
        }
        impl_->history_count =
            std::min(impl_->history_count + samples.size() / impl_->params.channels, kHistoryFrames);
    }

    if (!SDL_PutAudioStreamData(impl_->stream, samples.data(),
                                static_cast<int>(samples.size() * sizeof(std::int16_t)))) {
        return false;
    }
    impl_->submitted_audio = true;

    if (impl_->time_stretch_enabled && impl_->status == Status::Playing && impl_->playback_started &&
        !impl_->buffering_paused && impl_->params.underrun_recovery_frames != 0 &&
        impl_->history_count == kHistoryFrames && impl_->params.underrun_recovery_frames >= queued_before) {
        const auto recovery_frames = impl_->params.underrun_recovery_frames;
        const auto queued_now = queued_frames();
        const auto history_capacity = impl_->params.channels * kHistoryFrames;
        if (queued_now < recovery_frames) {
            // Top the stream up from recent history instead of letting it drain.
            const auto filler_capacity =
                impl_->params.channels *
                std::max(recovery_frames - queued_now + kStretchFillSlackFrames, kOlaStretchGrainFrames);
            const auto history_span_start =
                (impl_->history_write + history_capacity - impl_->params.channels * impl_->history_count) %
                history_capacity;
            std::vector<std::int16_t> ordered_history(impl_->params.channels * impl_->history_count);
            for (std::size_t index = 0; index < impl_->history_count * impl_->params.channels; ++index) {
                ordered_history[index] = impl_->history[(index + history_span_start) % history_capacity];
            }
            std::vector<std::int16_t> filler(filler_capacity);
            const auto produced = ola_time_stretch_2x(ordered_history, filler, impl_->params.channels);
            if (produced != 0 &&
                SDL_PutAudioStreamData(impl_->stream, filler.data(),
                                       static_cast<int>(impl_->params.channels * produced * sizeof(std::int16_t)))) {
                ++impl_->stretch_fills;
                impl_->stretched_frames += produced;
            }
        }
    }

    // Resume once the startup (or underrun refill) buffer is queued.
    if (impl_->status == Status::Playing && impl_->buffering_paused) {
        const auto threshold =
            impl_->playback_started ? impl_->params.underrun_recovery_frames : impl_->params.startup_buffer_frames;
        if (threshold == 0 || queued_frames() >= threshold) {
            if (SDL_ResumeAudioDevice(impl_->device)) {
                impl_->buffering_paused = false;
                impl_->playback_started = true;
            }
        }
    } else if (impl_->status == Status::Playing) {
        impl_->playback_started = true;
    }
    return true;
}

std::size_t HostAudioSink::queued_frames() const noexcept {
    if (impl_->status != Status::Playing || impl_->stream == nullptr || impl_->device == 0) {
        return 0;
    }
    const int queued = SDL_GetAudioStreamQueued(impl_->stream);
    if (queued < 0) {
        return 0;
    }
    const auto frame_bytes = impl_->params.channels * sizeof(std::int16_t);
    return static_cast<std::size_t>(queued) / frame_bytes;
}

std::size_t HostAudioSink::queued_bytes() const noexcept {
    if (impl_->status != Status::Playing || impl_->stream == nullptr || impl_->device == 0) {
        return 0;
    }
    return static_cast<std::size_t>(std::max(SDL_GetAudioStreamQueued(impl_->stream), 0));
}

bool HostAudioSink::queued_frames_exceed(const std::size_t limit) const noexcept {
    if (impl_->status != Status::Playing || impl_->stream == nullptr || impl_->device == 0) {
        impl_->queue_upper_bound.invalidate();
        return false;
    }
    return impl_->queue_upper_bound.exceeds(limit, [this]() -> std::optional<std::size_t> {
        const int queued = SDL_GetAudioStreamQueued(impl_->stream);
        if (queued < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(queued) / (impl_->params.channels * sizeof(std::int16_t));
    });
}

bool HostAudioSink::pause() noexcept {
    if (impl_->status != Status::Playing || impl_->device == 0) {
        return false;
    }
    if (!SDL_PauseAudioDevice(impl_->device)) {
        return false;
    }
    impl_->buffering_paused = true;
    return true;
}

bool HostAudioSink::resume() noexcept {
    if (impl_->status != Status::Playing || impl_->device == 0) {
        return false;
    }
    if (!SDL_ResumeAudioDevice(impl_->device)) {
        return false;
    }
    impl_->buffering_paused = false;
    impl_->playback_started = true;
    return true;
}

bool HostAudioSink::is_paused() const noexcept {
    return impl_->status == Status::Playing && impl_->device != 0 && SDL_AudioDevicePaused(impl_->device);
}

std::uint64_t HostAudioSink::underruns_recovered() const noexcept {
    return impl_->underruns_recovered;
}

std::uint64_t HostAudioSink::no_device_frames_discarded() const noexcept {
    return impl_->no_device_frames_discarded;
}

std::uint64_t HostAudioSink::overflow_resyncs() const noexcept {
    return impl_->overflow_resyncs;
}

std::uint64_t HostAudioSink::overflow_frames_dropped() const noexcept {
    return impl_->overflow_frames_dropped;
}

std::uint64_t HostAudioSink::stretch_fills() const noexcept {
    return impl_->stretch_fills;
}

std::uint64_t HostAudioSink::stretched_frames() const noexcept {
    return impl_->stretched_frames;
}

void HostAudioSink::set_gain(const float gain) {
    if (!std::isfinite(gain)) {
        return;
    }
    impl_->gain = std::clamp(gain, 0.0f, 1.0f);
    apply_gain(*impl_);
}

float HostAudioSink::gain() const noexcept {
    return impl_->gain;
}

void HostAudioSink::flush() {
    impl_->queue_upper_bound.invalidate();
    if (impl_->stream != nullptr) {
        SDL_FlushAudioStream(impl_->stream);
    }
}

void HostAudioSink::clear() {
    impl_->queue_upper_bound.invalidate();
    if (impl_->stream != nullptr) {
        SDL_ClearAudioStream(impl_->stream);
    }
}

} // namespace takedown::audio
