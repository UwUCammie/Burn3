#pragma once

// Racing-line lookups and the lateral steering correction of the AI drivers,
// the game's routines at SLUS-21050 0x0015fc70, 0x0015ffa0, 0x001602f0 and
// 0x001605c0. A lane is a record whose first word points at a curve: a count
// (+16) and a table (+4) of 8-byte entries whose first float is the lane's
// offset at that whole position along the road; positions in between are
// interpolated. Waypoint records tie positions to those lanes.
//
// The code reads the game's records through `Memory`, an adapter with
//   u8/u16/u32/f32(address)   reads (the adapter flags an address it cannot
//                             serve and returns zero),
//   put_u8/put_u16(address, value)  writes,
//   unsupported()             a state the replacement leaves to the original.
// All float arithmetic is the game's (engine/math/chopped.hpp).

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace takedown::engine::ai {

inline constexpr std::uint32_t kGameBase = 0x522660U;  // start of the game's record arrays
inline constexpr std::uint32_t kNoRecord = 255U;

// The game's float constants the lane code reads (words at fixed offsets
// from its global pointer).
struct LaneConstants {
    float speed_scale{};     // gp-29820: scales a vehicle's speed
    float lane_gain{};       // gp-29732: lateral correction per unit of offset
    float width_fraction{};  // gp-31428: share of a lane's width that needs no correction; also the bound of the result
};

// Record addressing.
[[nodiscard]] constexpr std::uint32_t lane_record(const std::uint32_t index) noexcept {
    return kGameBase + 556U + 76U * index;
}
[[nodiscard]] constexpr std::uint32_t waypoint_record(const std::uint32_t index) noexcept {
    return kGameBase + 0x2f2b0U + 80U * index;
}

// Float ordering of the game's max.s / min.s (raw sign-magnitude, so a
// negative zero is below a positive one).
[[nodiscard]] inline float ee_max(const float a, const float b) noexcept {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<float>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}
[[nodiscard]] inline float ee_min(const float a, const float b) noexcept {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<float>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}
[[nodiscard]] inline float ee_abs(const float a) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(a) & 0x7fffffffU);
}
// The sign of `value` as +/-1 (what the game builds from its sign bit).
[[nodiscard]] inline float sign_of(const float value) noexcept {
    return std::bit_cast<float>((std::bit_cast<std::uint32_t>(value) & 0x80000000U) | 0x3f800000U);
}
// floorf as the game's library computes it (the whole-number part below a
// float's precision, otherwise the value itself).
[[nodiscard]] inline float game_floor(const float value) noexcept {
    return std::floor(game_operand(value));
}

// The lane's offset at position `x` along it: the table entry before
// trunc(x) plus the fraction of the step to the next entry. Positions past
// the table's end hold its last entry.
template <class Memory>
[[nodiscard]] float lane_offset_at(Memory& memory, const std::uint32_t lane, const float x) {
    const std::uint32_t curve = memory.u32(lane);
    const auto count = static_cast<std::int32_t>(memory.u32(curve + 16U));
    const std::uint32_t table = memory.u32(curve + 4U);
    std::int32_t index = truncate_word(x);
    if (count < index) {
        index = count;
    }
    const std::int32_t previous = index - 1;
    const float base = previous < 0 ? 0.0F : memory.f32(table + (static_cast<std::uint32_t>(previous) << 3U));
    if (index < count) {
        const float next = memory.f32(table + (static_cast<std::uint32_t>(index) << 3U));
        const float fraction = chop_sub(x, chop_from_int(index));
        return chop_madd(chop_add(0.0F, base), fraction, chop_sub(next, base));
    }
    return base;
}

// 0x001602f0: the lane offset at x minus the offset at y.
template <class Memory>
[[nodiscard]] float lane_offset_difference(Memory& memory, const std::uint32_t lane, const float x, const float y) {
    const float at_x = lane_offset_at(memory, lane, x);
    const float at_y = lane_offset_at(memory, lane, y);
    return chop_sub(at_x, at_y);
}

// Of two waypoints, the one nearer position `x` along the lane.
template <class Memory>
[[nodiscard]] std::uint32_t nearer_waypoint(Memory& memory, const std::uint32_t lane, const std::uint32_t first,
                                            const std::uint32_t second, const float x) {
    const float first_offset = lane_offset_at(memory, lane, memory.f32(first + 48U));
    const float here = lane_offset_at(memory, lane, x);
    const float before = chop_sub(first_offset, here);
    const float second_offset = lane_offset_at(memory, lane, memory.f32(second + 48U));
    const float after = chop_sub(here, second_offset);
    return game_less(before, after) ? first : second;
}

// 0x0015fc70: the waypoint of a driver's two (+68, +69) that is nearer to
// where it is (+48); 0 when it has none.
template <class Memory>
[[nodiscard]] std::uint32_t driver_waypoint(Memory& memory, const std::uint32_t lane, const std::uint32_t driver) {
    const std::uint32_t first = memory.u8(driver + 68U);
    const std::uint32_t second = memory.u8(driver + 69U);
    if (first == kNoRecord) {
        return second == kNoRecord ? 0U : waypoint_record(second);
    }
    if (second == kNoRecord) {
        return waypoint_record(first);
    }
    return nearer_waypoint(memory, lane, waypoint_record(first), waypoint_record(second),
                           memory.f32(driver + 48U));
}

// 0x0015ffa0: the waypoint covering position `x` along a lane (looked up in
// the lane's byte map, then the nearer of it and its successor); 0 when the
// map has none.
template <class Memory>
[[nodiscard]] std::uint32_t lane_waypoint(Memory& memory, const std::uint32_t lane, const float x,
                                          const std::uint32_t waypoint_map) {
    constexpr float kTwoPow31 = 2147483648.0F;
    std::uint32_t slot;
    if (game_less_equal(kTwoPow31, x)) {
        slot = static_cast<std::uint32_t>(truncate_word(chop_sub(x, kTwoPow31))) | 0x80000000U;
    } else {
        slot = static_cast<std::uint32_t>(truncate_word(x));
    }
    slot &= 0xffffU;
    const std::uint32_t first = memory.u8(waypoint_map + memory.u16(lane + 4U) + slot);
    if (first == kNoRecord) {
        const std::uint32_t fallback = memory.u8(lane + 70U);
        return fallback == kNoRecord ? 0U : waypoint_record(fallback);
    }
    const std::uint32_t waypoint = waypoint_record(first);
    const std::uint32_t second = memory.u8(waypoint + 69U);
    if (second == kNoRecord) {
        return waypoint;
    }
    return nearer_waypoint(memory, lane, waypoint, waypoint_record(second), x);
}

// The globals the lane code reads: the base of the lanes' byte maps (the word at
// 0x0055d7dc) and the constants at fixed offsets from the global pointer `gp`.
template <class Memory>
[[nodiscard]] LaneConstants load_lane_constants(Memory& memory, const std::uint32_t gp) {
    return {.speed_scale = memory.f32(gp - 29820U),
            .lane_gain = memory.f32(gp - 29732U),
            .width_fraction = memory.f32(gp - 31428U)};
}
inline constexpr std::uint32_t kWaypointMapPointer = 0x55d7dcU;

// 0x001605c0: the lateral correction for a driver (record `self`) from how far
// it is off the lane (and, when the lane forks, off the lanes it can fork
// into), at most +/- width_fraction times its speed. May update the driver's
// next lanes (+60, +62, +66, +67) first.
template <class Memory>
[[nodiscard]] float lane_steering_correction(Memory& memory, const std::uint32_t self, const std::uint32_t gp) {
    const LaneConstants k = load_lane_constants(memory, gp);
    const std::uint32_t waypoint_map = memory.u32(kWaypointMapPointer);
    const std::uint32_t own_lane_index = memory.u8(self + 65U);
    const std::uint32_t lane = own_lane_index == kNoRecord ? 0U : lane_record(own_lane_index);
    const std::uint32_t widths = memory.u32(self + 56U);
    const std::uint32_t slot = static_cast<std::uint32_t>(memory.u8(self + 71U)) << 2U;
    if (game_less(memory.f32(memory.u32(widths) + slot), 0.0F)) {
        return 0.0F;
    }
    if (lane == 0U) {
        memory.unsupported();  // the original reads from address 0 here
        return 0.0F;
    }
    float correction = 0.0F;
    std::uint32_t lanes_used = 0U;
    std::uint32_t own_lane_used = 0U;
    const std::uint32_t waypoint = driver_waypoint(memory, lane, self);
    if (waypoint != 0U) {
        const float offset = lane_offset_difference(memory, lane, memory.f32(waypoint + 48U), memory.f32(self + 48U));
        float width = memory.f32(memory.u32(memory.u32(self + 56U)) + slot);
        if (game_less(ee_abs(offset), width)) {
            const float own = chop_sub(chop_add(0.0F, memory.f32(self + 44U)), chop_mul(k.speed_scale, memory.f32(self)));
            const float other = chop_sub(chop_add(0.0F, memory.f32(waypoint + 44U)),
                                         chop_mul(k.speed_scale, memory.f32(waypoint)));
            if (!game_less_equal(ee_abs(own), ee_abs(other))) {
                ++lanes_used;
                const std::uint32_t other_widths = memory.u32(waypoint + 56U);
                const std::uint32_t other_slot = memory.u8(waypoint + 71U);
                if (other_widths != 0U && other_slot != kNoRecord) {
                    const float other_width = memory.f32(memory.u32(other_widths) + (other_slot << 2U));
                    width = game_div(chop_add(width, other_width), 2.0F);
                }
                const float excess = chop_sub(ee_abs(offset), width);
                if (!game_less_equal(ee_abs(excess), chop_mul(k.width_fraction, width))) {
                    own_lane_used = 1U;
                    correction = chop_mul(chop_mul(k.lane_gain, excess), sign_of(offset));
                } else {
                    correction = 0.0F;
                }
            }
        }
    }

    if ((memory.u8(self + 75U) & 1U) != 0U) {
        // Take the next lanes from the lane's table of 18-byte entries.
        const std::uint32_t entries = memory.u32(memory.u32(lane) + 8U);
        const std::uint32_t entry = entries + static_cast<std::uint32_t>(truncate_word(memory.f32(self + 48U))) * 18U;
        memory.put_u8(self + 66U, memory.u8(entry + 14U));
        memory.put_u8(self + 67U, memory.u8(entry + 15U));
        memory.put_u16(self + 60U, memory.u16(entry + 4U));
        memory.put_u16(self + 62U, memory.u16(entry + 6U));
    }

    if (own_lane_used == 0U) {
        for (std::uint32_t side = 0U; side < 2U; ++side) {
            const std::uint32_t current_widths = memory.u32(self + 56U);
            const std::uint32_t side_slot = static_cast<std::uint32_t>(memory.u8(self + 71U)) << 2U;
            const std::uint32_t arrays = memory.u32(current_widths + (side == 0U ? 4U : 8U));
            float side_width = memory.f32(arrays + side_slot);
            const std::uint32_t next_index = memory.u8(self + (side == 0U ? 66U : 67U));
            const std::uint32_t position = memory.u16(self + (side == 0U ? 60U : 62U));
            const std::uint32_t next_lane = next_index == kNoRecord ? 0U : lane_record(next_index);
            if (game_less(side_width, 0.0F) || next_lane == 0U || position == 0xffffU) {
                continue;
            }
            const float here = chop_add(chop_from_int(static_cast<std::int32_t>(position)),
                                        chop_sub(memory.f32(self + 48U), game_floor(memory.f32(self + 48U))));
            const std::uint32_t next_waypoint = lane_waypoint(memory, next_lane, here, waypoint_map);
            if (next_waypoint == 0U) {
                continue;
            }
            const float offset = lane_offset_difference(memory, lane, memory.f32(next_waypoint + 48U), here);
            if (!game_less(ee_abs(offset), memory.f32(memory.u32(self + 56U) + 12U))) {
                continue;
            }
            ++lanes_used;
            const std::uint32_t other_widths = memory.u32(next_waypoint + 56U);
            const std::uint32_t other_slot = memory.u8(next_waypoint + 71U);
            if (other_widths != 0U && other_slot != kNoRecord) {
                const std::uint32_t other_array = memory.u32(other_widths + (side == 0U ? 8U : 4U));
                side_width = game_div(chop_add(side_width, memory.f32(other_array + (other_slot << 2U))), 2.0F);
            }
            correction = chop_mul(chop_mul(k.lane_gain, chop_sub(ee_abs(offset), side_width)), sign_of(offset));
        }
    }

    if (lanes_used == 0U) {
        return 0.0F;
    }
    const float mean = game_div(chop_add(0.0F, chop_add(correction, 0.0F)),
                                chop_from_int(static_cast<std::int32_t>(lanes_used)));
    const float bound = chop_mul(k.width_fraction, memory.f32(self));
    return ee_min(ee_max(mean, -bound), bound);
}

} // namespace takedown::engine::ai
