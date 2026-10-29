#pragma once

// A level eased toward a target picked by how much of a span of units lies
// above a threshold: the units come in blocks of 60, each with a threshold.
// Up to a quarter covered, the scale is 1 and the target the range's top;
// from a quarter to all covered, the scale falls to 0.25 and the target
// moves to the range's bottom.

#include <cstdint>

namespace takedown::engine {

inline constexpr std::uint32_t kCoverageBlockUnits = 60U;

struct BlockCoverage {
    std::uint32_t covered_blocks{};  // bit n: block n's threshold is above the input
    float scale{};
    float level{};                   // updated
};

struct CoverageRange {
    float bottom{};
    float top{};
    float quarter_to_full{};         // 4/3: maps a quarter to all covered onto 0 to 1
};

// `threshold(n)` returns block n's threshold; the input is compared with
// (input - level). The level moves at half the range per second, dt seconds.
template <class Threshold>
[[nodiscard]] BlockCoverage update_block_coverage(const std::uint32_t units, Threshold&& threshold,
                                                  const float input, const float level, const CoverageRange& range,
                                                  const float dt) {
    BlockCoverage result;
    result.level = level;
    const float compared = input - level;
    std::int32_t covered_units = 0;
    std::uint32_t bit = 1U;
    std::uint32_t block = 0U;
    for (std::uint32_t start = 0U; start < units; start += kCoverageBlockUnits, ++block, bit += bit) {
        if (!(threshold(block) <= compared)) {
            result.covered_blocks |= bit;
            const std::uint32_t left = units - start;
            covered_units += static_cast<std::int32_t>(left < kCoverageBlockUnits + 1U ? left : kCoverageBlockUnits);
        }
    }
    const float covered = static_cast<float>(covered_units) / static_cast<float>(static_cast<std::int32_t>(units));
    float target;
    if (covered < 0.25F) {
        result.scale = 1.0F;
        target = range.top;
    } else {
        const float blend = range.quarter_to_full * (covered - 0.25F);
        result.scale = (0.0F + 1.0F) - 0.75F * blend;
        target = (0.0F + range.bottom) + blend * (range.top - range.bottom);
    }
    const float step = 0.5F * dt;
    if (result.level <= target) {
        result.level = (0.0F + result.level) + step * (range.top - range.bottom);
        if (!(result.level <= target)) {
            result.level = target;
        }
    } else {
        result.level = (0.0F + result.level) - step * (range.top - range.bottom);
        if (result.level < target) {
            result.level = target;
        }
    }
    return result;
}

} // namespace takedown::engine
