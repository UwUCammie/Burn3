#include "takedown/audio/time_stretch.hpp"

#include <algorithm>

namespace takedown::audio {

namespace {

// Grains are read from the history at this hop and written at twice it, so
// the filler lasts twice as long as the history it was built from.
constexpr std::size_t kAnalysisHopFrames = kOlaStretchGrainFrames / 4U;
constexpr std::size_t kSynthesisHopFrames = kOlaStretchGrainFrames / 2U;

// First history frame of a grain.
[[nodiscard]] constexpr std::size_t grain_source_frame(const std::size_t grain_index) noexcept {
    return grain_index * kAnalysisHopFrames;
}

} // namespace

// RECOVERY: statements, types, constants and control flow match the original
// GIMPLE; only the internal SSA version order of a few temporaries differs (the
// Hann window arithmetic and the per-sample index temporaries), which the
// checker still reports. Variants of the guard checks, sample conversion and
// destination access were tried without reproducing the original numbering.
std::size_t ola_time_stretch_2x(std::span<const std::int16_t> source, std::span<std::int16_t> destination,
                                const std::uint8_t channels) {
    if (channels == 0U || source.size() == 0U || destination.size() == 0U) {
        return 0U;
    }
    const std::size_t source_frames = source.size() / channels;
    const std::size_t destination_capacity = destination.size() / channels;
    if (source_frames < kOlaStretchGrainFrames || destination_capacity < kSynthesisHopFrames) {
        return 0U;
    }

    std::array<double, kOlaStretchGrainFrames> window{};
    for (std::size_t n = 0; n < kOlaStretchGrainFrames; ++n) {
        window[n] = detail::hann(n, kOlaStretchGrainFrames);
    }

    std::size_t produced_frames = 0U;
    std::size_t grain_index = 0U;
    while (grain_source_frame(grain_index) + kOlaStretchGrainFrames <= source_frames) {
        const auto out_base = grain_index * kSynthesisHopFrames;
        if (out_base + kOlaStretchGrainFrames > destination_capacity) {
            break;
        }
        for (std::size_t n = 0; n < kOlaStretchGrainFrames; ++n) {
            const double window_value = window[n];
            for (std::uint8_t channel = 0; channel < channels; ++channel) {
                const double sample = source[(grain_source_frame(grain_index) + n) * channels + channel];
                auto& out = destination[(out_base + n) * channels + channel];
                out = static_cast<std::int16_t>(
                    std::min(std::max(out + window_value * sample, -32768.0), 32767.0));
            }
        }
        produced_frames = out_base + kOlaStretchGrainFrames;
        ++grain_index;
    }
    return produced_frames;
}

} // namespace takedown::audio
