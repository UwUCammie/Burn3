#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace takedown::audio {

// SDL3-backed output device for PCM rendered by the modelled SPU2.
//
// This class is deliberately only a transport boundary: it accepts the
// already-mixed stereo samples produced by Spu2Renderer and queues them for
// the host device. It does not decode game assets, synthesize voices, choose
// sounds, or infer guest timing. submit() never blocks on the device.
//
// The sink deliberately knows nothing about PS2 guest memory: callers pass
// only the modelled SPU2's final interleaved PCM bus. Construction is cheap
// and never fails; open() degrades to
// a no-op stream when no audio device is available so a headless or
// audio-less environment still runs (unless the caller marks the device
// required).
class HostAudioSink {
  public:
    struct OpenParams {
        std::uint32_t sample_rate{48000};
        std::uint8_t channels{2};
        float gain{1.0f};
        // When true, open() reports failure if no playback device opens.
        bool device_required{false};
        // Emergency time-stretch: when the queued stream drops to the
        // recovery line while playing, top it up from a history of recent
        // frames through a 2:1 overlap-add stretch instead of letting the
        // device reach the pause-and-refill underrun path. Transport only.
        bool time_stretch_underruns{false};
        // Optional producer-side buffering. When nonzero, playback starts
        // only after this many frames have been queued. This is useful for an
        // emulated producer whose wall-clock delivery cadence can be much
        // slower or burstier than the device clock.
        std::size_t startup_buffer_frames{0};
        // After playback has started, a completely drained stream is paused
        // at the next submission and refilled to this many frames before it
        // resumes. Zero disables automatic underrun recovery.
        std::size_t underrun_recovery_frames{0};
        // When false, initialize an intentionally unbound sink. This is the
        // same bounded discard mode used when the host has no playback
        // device, and is useful for deterministic headless coverage.
        // `device_required` and a false value here are mutually exclusive.
        bool open_playback_device{true};
        // Hard latency bound in queued frames; zero means unbounded (the
        // SDL default). The guest producer is guest-time paced and can
        // outrun the device whenever emulation runs faster than real time;
        // SDL never blocks on Put, so without a bound that backlog is heard
        // as a permanently growing delay. When a submission would exceed
        // the bound, the stale backlog is dropped (the newest audio is
        // kept) and the skip is counted by overflow_resyncs() and
        // overflow_frames_dropped(). This is device-transport behavior only:
        // the modelled SPU2 and the WAV dump are unaffected.
        std::size_t max_queued_frames{0};
    };

    enum class Status {
        Closed,  // open() not called or close() called
        Partial, // SDL audio initialized, stream created, no device
        Playing, // stream created and bound to a playback device
    };

    HostAudioSink();
    ~HostAudioSink();

    HostAudioSink(const HostAudioSink&) = delete;
    HostAudioSink& operator=(const HostAudioSink&) = delete;
    HostAudioSink(HostAudioSink&&) noexcept;
    HostAudioSink& operator=(HostAudioSink&&) noexcept;

    // Initializes SDL audio, creates the conversion stream, and (when
    // possible) opens and binds the default playback device.
    [[nodiscard]] bool open();
    [[nodiscard]] bool open(const OpenParams& params);
    void close();

    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool is_open() const noexcept { return status() != Status::Closed; }

    // Consumes interleaved little-endian s16 samples. A Playing sink queues
    // them for playback. A Partial sink has no bound device and discards them
    // immediately instead of allowing SDL's unconsumed queue to grow without
    // bound; that discard is still a successful consumption and is counted
    // by no_device_frames_discarded(). The sample count must be nonzero and a
    // multiple of the channel count. Returns false when closed or misaligned.
    [[nodiscard]] bool submit(std::span<const std::int16_t> samples);

    // Number of queued sample frames still waiting to be consumed.
    [[nodiscard]] std::size_t queued_frames() const noexcept;
    // Single-producer threshold query; only consumption occurs between writes.
    [[nodiscard]] bool queued_frames_exceed(std::size_t limit) const noexcept;
    [[nodiscard]] std::size_t queued_bytes() const noexcept;

    // Device-level pause controls. A Partial (device-less) sink accepts these
    // as no-ops and reports false from is_paused().
    [[nodiscard]] bool pause() noexcept;
    [[nodiscard]] bool resume() noexcept;
    [[nodiscard]] bool is_paused() const noexcept;

    // Number of confirmed queue drains recovered by the optional buffering
    // policy. Detection happens on the first submission after a drain; SDL
    // does not expose a device callback through this queue-only seam.
    [[nodiscard]] std::uint64_t underruns_recovered() const noexcept;

    // Number of frames deliberately discarded because the sink was open but
    // had no playback device. Saturates rather than wrapping on very long
    // headless runs and resets when the sink is closed or reopened.
    [[nodiscard]] std::uint64_t no_device_frames_discarded() const noexcept;

    // Latency-bound accounting (OpenParams::max_queued_frames): how often a
    // stale backlog was dropped to keep the newest audio, and how many
    // queued frames those drops discarded in total.
    [[nodiscard]] std::uint64_t overflow_resyncs() const noexcept;
    [[nodiscard]] std::uint64_t overflow_frames_dropped() const noexcept;

    // Time-stretch telemetry (time_stretch_underruns enabled): filler
    // events and the interleaved s16 frames the overlap-add filler added.
    [[nodiscard]] std::uint64_t stretch_fills() const noexcept;
    [[nodiscard]] std::uint64_t stretched_frames() const noexcept;

    void set_gain(float gain);
    [[nodiscard]] float gain() const noexcept;

    // Marks the end of the submitted stream (SDL_FlushAudioStream) or drops
    // everything queued so far (SDL_ClearAudioStream).
    void flush();
    void clear();

    [[nodiscard]] std::string_view backend_name() const noexcept;

    // Opaque state handle. The declaration is public so the implementation
    // file's internal helpers can name the type at
    // namespace scope; only the definition stays hidden.
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::audio
