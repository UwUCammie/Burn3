#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::audio {

// Slack frames added on top of the sink's fill target when sizing the
// overlap-add filler buffer.
inline constexpr std::size_t kStretchFillSlackFrames = 256U;

// The stretcher emits whole 2048-frame grains; a filler buffer smaller than
// this produces nothing.
inline constexpr std::size_t kOlaStretchGrainFrames = 2048U;

// Emergency time-stretch filler for host-side audio underruns (transport
// only). Given the most recent interleaved s16 frames handed to the sink,
// produces a pitch-preserving lengthened version via classic overlap-add:
// consecutive analysis grains (Hann-windowed) are written at half the
// analysis rate, so the output lasts twice as long as its input. No
// correlation search (WSOLA) — this is an emergency filler whose job is to
// keep the playback stream non-empty through a production shortfall, not a
// studio effect. Deterministic: identical input yields identical output.
//
// `source` is interleaved s16 (channels-wide frames); `destination` receives
// interleaved s16 frames. Returns the number of interleaved frames written
// (never more than destination_frames). The stretch is fixed at 2:1
// (analysis hop 1024, synthesis hop 512, grain 2048, Hann window): a
// 4096-frame history yields up to 8192 filler frames (~85 ms of source
// becomes ~170 ms of filler at 48 kHz).
[[nodiscard]] std::size_t ola_time_stretch_2x(
    std::span<const std::int16_t> source, std::span<std::int16_t> destination,
    std::uint8_t channels);

namespace detail {

// Hann window over one grain, computed once per call site.
[[nodiscard]] inline double hann(std::size_t n, std::size_t grain) noexcept {
    if (grain <= 1U) return 1.0;
    return 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 *
                                 static_cast<double>(n) /
                                 static_cast<double>(grain)));
}

} // namespace detail

} // namespace takedown::audio
