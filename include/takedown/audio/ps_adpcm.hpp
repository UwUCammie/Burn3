#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::audio {

// Clean-room PlayStation ADPCM decoder, written from the public PS-ADPCM
// format facts (16-byte frames, 28 samples each, 4-bit signed nibbles, a
// 16-entry {first_order, second_order} predictor table in 1/64 fixed point).
//
// The arithmetic reproduces the decode that the FFmpeg adpcm_psx decoder
// produces sample-for-sample on the game's own data (validated against a
// 860,160-sample oracle dump from TRACKS/AS/C1_V1/E_DJRACE.RWS segment 0:
// zero mismatches). The formulation is:
//
//   scale   = sign_extend_4(nibble) << 12
//   sample  = (scale >> shift)
//           + truncate_toward_zero((history1 * c1 + history2 * c2) / 64)
//   output  = clamp16(sample)
//   history2 = history1; history1 = sample          (unclamped, like the oracle)
//
// The 16-byte frame header is {byte0: coefficient-index<<4 | shift,
// byte1: flags}, followed by 14 bytes holding 28 nibbles low-nibble-first.
// Frames whose flag field (masked to three bits like the oracle) is >= 7
// decode as silence (sample 0) per the oracle's behavior; the shipped
// Burnout 3 data contains no such frames.
//
// History is continuous: one decoder instance models one coded channel
// stream, and it must NOT be reset between 16-byte frames or between RWS
// block deliveries. Verify empirically on the real data: 50 of 60 channel
// blocks in the reference segment start with non-zero predictor entries,
// which is only sensible if history survives block boundaries.
class PsAdpcmDecoder {
  public:
    static constexpr std::size_t kFrameBytes = 16;
    static constexpr std::size_t kSamplesPerFrame = 28;

    // Predictor table, scaled by 64. Values are the widely documented
    // PlayStation ADPCM coefficients (0, 60/64, 115/64, -52/64, ...).
    static constexpr std::array<std::array<std::int16_t, 2>, 16> kPredictors{{
        {{0, 0}},      {{60, 0}},      {{115, -52}},  {{98, -55}},
        {{122, -60}},  {{30, 0}},      {{57, -26}},   {{49, -27}},
        {{61, -30}},   {{15, 0}},      {{29, -13}},   {{25, -14}},
        {{31, -15}},   {{32, -60}},    {{15, -60}},   {{7, -60}},
    }};

    // Advisory frame census. The decoder never rejects input (it reproduces
    // the oracle even for unusual headers); the counters let callers flag
    // data that deviates from the game's normal encoding conventions.
    struct Census {
        std::uint64_t frames{0};
        std::uint64_t flagged_frames{0};       // flag >= 7 → silence per oracle
        std::uint64_t high_coefficient_frames{0};  // coefficient index > 4
        std::uint64_t excessive_shift_frames{0};   // shift > 12
        std::int32_t minimum_sample{0};
        std::int32_t maximum_sample{0};
    };

    PsAdpcmDecoder() = default;

    PsAdpcmDecoder(const PsAdpcmDecoder&) = delete;
    PsAdpcmDecoder& operator=(const PsAdpcmDecoder&) = delete;
    // Movable so offline tools can retain independent per-channel decoder
    // histories in standard containers.
    PsAdpcmDecoder(PsAdpcmDecoder&&) noexcept = default;
    PsAdpcmDecoder& operator=(PsAdpcmDecoder&&) noexcept = default;

    void reset() noexcept;

    // Decodes whole 16-byte frames from `frames` into `samples`. The input
    // length must be a multiple of kFrameBytes and `samples` must hold at
    // least frames.count() / kFrameBytes * kSamplesPerFrame samples.
    // Returns the number of samples written.
    std::size_t decode(std::span<const std::byte> frames, std::span<std::int16_t> samples);

    [[nodiscard]] const Census& census() const noexcept { return census_; }

  private:
    std::int32_t history1_{0};
    std::int32_t history2_{0};
    Census census_{};
};

// FNV-1a 64-bit checksum over the raw little-endian sample stream. The
// reference fixtures capture these values; the checksum gives a strong
// sample-level equality signal without committing decoded audio.
[[nodiscard]] std::uint64_t fnv1a64_samples(std::span<const std::int16_t> samples);

// Root-mean-square of the samples about zero.
[[nodiscard]] double rms_of_samples(std::span<const std::int16_t> samples);

} // namespace takedown::audio
