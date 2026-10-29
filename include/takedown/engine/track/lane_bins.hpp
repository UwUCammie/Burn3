#pragma once

// A profile of 256 bins across an axis (a sideways slice of the road around
// a car): per bin the closest distance seen (a byte, 8/255 units), a height
// level (a short, 500/32767 units) and the tag of what set the distance.
// Quads of the track's strip mesh are projected onto the axis and lower the
// bins they cover.

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cstdint>

namespace takedown::engine {

inline constexpr int kLaneBinCount = 256;
inline constexpr float kLaneBinsPerUnit = 5.0F;

struct LaneBinAxis {
    Vector3 origin;
    Vector3 direction;
    float centre{};  // the axis offset of bin 128
};

// The bin of a point (bin 128 at the centre), unclamped.
[[nodiscard]] inline std::int32_t lane_bin(const LaneBinAxis& axis, const Vector3 point) noexcept {
    const float offset = dot(point - axis.origin, axis.direction);
    return truncate_to_int(kLaneBinsPerUnit * (offset - axis.centre)) + 128;
}

struct LaneMark {
    float distance{};      // 0 to 8
    float level{};         // -500 to 500
    std::uint8_t tag{};
    float distance_unit{}; // byte value to distance
    float level_unit{};    // short value to level
};

// Lowers bins `low` to `high` (each clamped into the profile; nothing when
// the span lies outside it) to `mark`. `Bins` provides distance(bin),
// set_distance(bin, byte, tag), level(bin) and set_level(bin, short).
template <class Bins>
void mark_lane_bins(Bins& bins, std::int32_t low, std::int32_t high, const LaneMark& mark) noexcept {
    if (high < 0 || low >= kLaneBinCount) {
        return;
    }
    low = low < 0 ? 0 : low;
    high = high >= kLaneBinCount ? kLaneBinCount - 1 : high;
    const auto distance_byte = static_cast<std::uint8_t>(truncate_to_int(255.0F * mark.distance / 8.0F));
    const float limited_level = minimum(maximum(mark.level, -500.0F), 500.0F);
    const auto level_short = static_cast<std::int16_t>(truncate_to_int(32767.0F * limited_level / 500.0F));
    for (std::int32_t bin = low; bin <= high; ++bin) {
        if (mark.distance < mark.distance_unit * static_cast<float>(bins.distance(bin))) {
            bins.set_distance(bin, distance_byte, mark.tag);
        }
        if (mark.level < mark.level_unit * static_cast<float>(bins.level(bin))) {
            bins.set_level(bin, level_short);
        }
    }
}

// The bin of an axis offset relative to the centre, clamped into the profile.
[[nodiscard]] inline std::int32_t clamped_lane_bin(const float offset) noexcept {
    const std::int32_t bin = truncate_word(chop_mul(kLaneBinsPerUnit, offset)) + 128;
    return bin < 0 ? 0 : (bin >= kLaneBinCount ? kLaneBinCount - 1 : bin);
}

struct LaneGapWeights {
    float bin_width{};       // applied to the summed distance bytes
    float distance_unit{};   // byte value to distance
    float least_score{};     // a gap must score at least this
};

// The most open bin strictly inside `low`..`high`: each bin is scored by
// the distances around it (bins -3, -1, 0, 1 and 3) times two tents that
// favour the middle of the profile and the middle of the span; the last
// best score wins. Bin 128 when none reaches the least score. Arithmetic in
// the game's rounding so ties land as they do in game.
template <class Bins>
[[nodiscard]] std::int32_t best_lane_gap(const Bins& bins, const std::int32_t low, const std::int32_t high,
                                         const LaneGapWeights& weights) noexcept {
    const auto tent = [](const float x) { return game_less(1.0F, x) ? chop_sub(2.0F, x) : x; };
    const float span = chop_from_int(high - low);
    float best = weights.least_score;
    std::int32_t best_bin = 128;
    for (std::int32_t bin = low + 3; bin < high - 3; ++bin) {
        const std::int32_t sum = bins.distance(bin - 3) + bins.distance(bin - 1) + bins.distance(bin) +
                                 bins.distance(bin + 1) + bins.distance(bin + 3);
        const float openness = chop_mul(chop_mul(weights.bin_width, chop_from_int(sum)), weights.distance_unit);
        const float centred = tent(chop_from_int(bin) / 128.0F);
        const float inside = tent(chop_from_int(2 * (bin - low)) / span);
        const float score = chop_mul(openness, chop_mul(centred, inside));
        if (!game_less(score, best)) {
            best = score;
            best_bin = bin;
        }
    }
    return best_bin;
}

// The bin a car covering bins `low`..`high` would move to, to get clear of
// what blocks it, searching up (direction 1) or down (direction -1) from
// its far edge. The search follows the distances outward while they keep
// opening (a bin within `bin_width` of the last opening counts as open) and
// stops at the first bin closer than `nearest` (the car's own nearest
// distance) or, once a car width past the car, at the first closed one. The
// result is the middle of the open run found, kept a bin inside the profile.
// Bins may be read one past either end of the profile, as the game does.
// `steps` counts the bins looked at.
template <class Bins>
[[nodiscard]] std::int32_t lane_escape_bin(const Bins& bins, const std::int32_t low, const std::int32_t high,
                                           const std::int32_t width, const int direction, const float nearest,
                                           const float distance_unit, const float bin_width,
                                           std::uint32_t& steps) noexcept {
    const auto distance = [&](const std::int32_t bin) {
        return chop_mul(distance_unit, chop_from_int(bins.distance(bin)));
    };
    float margin = chop_sub(nearest, bin_width);
    if (game_less(margin, 0.0F)) {
        margin = 0.0F;
    }
    float previous = nearest;
    std::int32_t edge = direction > 0 ? low : high;
    while (direction > 0 ? edge < kLaneBinCount : edge >= 0) {
        ++steps;
        const float here = distance(edge);
        const bool past_car = direction > 0 ? high < edge - width : edge + width < low;
        if (game_less(here, margin)) {
            if (past_car) {
                edge -= direction;
                break;
            }
            if (game_less(here, nearest)) {
                break;
            }
            edge += direction;
            continue;
        }
        if (!game_less(here, previous)) {
            margin = chop_sub(here, bin_width);
        }
        previous = here;
        edge += direction;
    }
    std::int32_t start = edge;
    while (direction > 0 ? low < start : start < high) {
        ++steps;
        if (game_less(distance(start), margin)) {
            start += direction;
            break;
        }
        start -= direction;
    }
    const std::int32_t middle = (edge + start) / 2;
    const std::int32_t least = direction > 0 ? 1 : 0;
    const std::int32_t most = direction > 0 ? kLaneBinCount - 1 : kLaneBinCount - 2;
    return middle < least ? least : (middle > most ? most : middle);
}

} // namespace takedown::engine
