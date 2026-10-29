#include "takedown/audio/ps_adpcm.hpp"

#include <algorithm>
#include <cmath>

namespace takedown::audio {

namespace {

std::int32_t clamp16(std::int32_t value) noexcept {
    return std::clamp(value, -32768, 32767);
}

// The oracle divides the 1/64 fixed-point prediction with C semantics:
// truncate toward zero, not an arithmetic shift.
std::int32_t truncate_toward_zero_div64(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(value / 64);
}

} // namespace

void PsAdpcmDecoder::reset() noexcept {
    history1_ = 0;
    history2_ = 0;
    census_ = Census{};
}

std::size_t PsAdpcmDecoder::decode(const std::span<const std::byte> frames, const std::span<std::int16_t> samples) {
    const auto frame_count = frames.size() / kFrameBytes;
    const auto sample_count = frame_count * kSamplesPerFrame;
    if (frames.size() % kFrameBytes != 0 || samples.size() < sample_count) {
        return 0;
    }

    auto write = samples.begin();
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        const std::byte* frame = frames.data() + frame_index * kFrameBytes;
        const auto header = std::to_integer<unsigned char>(frame[0]);
        const auto flags = std::to_integer<unsigned char>(frame[1]);
        const unsigned char coefficient_index = header >> 4;
        const unsigned char shift = header & 0x0FU;
        // The oracle masks the flag field to three bits.
        const unsigned char flag = flags & 0x07U;

        ++census_.frames;
        if (flag >= 7) {
            ++census_.flagged_frames;
        }
        if (coefficient_index > 4) {
            ++census_.high_coefficient_frames;
        }
        if (shift > 12) {
            ++census_.excessive_shift_frames;
        }

        const std::int32_t c1 = kPredictors[coefficient_index][0];
        const std::int32_t c2 = kPredictors[coefficient_index][1];
        for (std::size_t sample_in_frame = 0; sample_in_frame < kSamplesPerFrame; ++sample_in_frame) {
            std::int32_t sample = 0;
            if (flag < 7) {
                // Nibbles are packed low-nibble-first.
                const auto packed = std::to_integer<unsigned char>(frame[2 + sample_in_frame / 2]);
                int nibble = (sample_in_frame & 1) != 0 ? packed >> 4 : packed & 0x0F;
                if (nibble > 7) {
                    nibble -= 16;
                }
                const int scale = nibble << 12;
                sample = truncate_toward_zero_div64(static_cast<std::int64_t>(history1_) * c1 +
                                                    static_cast<std::int64_t>(history2_) * c2) +
                         (scale >> shift);
            }
            const std::int32_t output = clamp16(sample);
            *write++ = static_cast<std::int16_t>(output);
            census_.minimum_sample = std::min(output, census_.minimum_sample);
            census_.maximum_sample = std::max(output, census_.maximum_sample);
            // History keeps the unclamped value, like the oracle.
            history2_ = history1_;
            history1_ = sample;
        }
    }
    return sample_count;
}

std::uint64_t fnv1a64_samples(const std::span<const std::int16_t> samples) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto sample : samples) {
        const auto bytes = static_cast<std::uint16_t>(sample);
        hash ^= static_cast<std::uint64_t>(bytes & 0xFFU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint64_t>(bytes >> 8);
        hash *= 1099511628211ULL;
    }
    return hash;
}

double rms_of_samples(const std::span<const std::int16_t> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    std::uint64_t sum_of_squares = 0;
    for (const auto sample : samples) {
        sum_of_squares += static_cast<std::uint64_t>(sample) * static_cast<std::uint64_t>(sample);
    }
    return std::sqrt(static_cast<double>(sum_of_squares) / static_cast<double>(samples.size()));
}

} // namespace takedown::audio
