#pragma once

// The AI drivers' per-step speed control, the game's routine at SLUS-21050
// 0x00161b50: from the obstacles and cars ahead it picks the driver's state
// (+70), its target gap (+16) and speed change (+24), then integrates its
// speed (+44). The record's roles are inferred from how the routine uses it.
//
// `Memory` is the adapter described in lane_steering.hpp, extended with
//   vec3(address)        the xyz of the 16-byte aligned vector at address
//   put_u32/put_f32      writes.
// Every call is checked first (the adapter runs it on a log of the writes), so
// a path that needs the original is found before anything changes.
// The routine starts spawning an object (0x001b82f0, when a driver is about to
// hit one it can avoid: out of scope) on one path; `spawns` reports that the
// code reached it and the call must be left to the original.

#include "takedown/engine/ai/lane_steering.hpp"
#include "takedown/engine/math/chopped_vector.hpp"

#include <cstdint>

namespace takedown::engine::ai {

struct DriverSpeedOutcome {
    bool spawns{};
    std::uint32_t instructions{};  // the original's work, roughly
};

namespace driver_speed_detail {

inline constexpr std::uint32_t kCarRecords = kGameBase + 0x175b0U;          // 384-byte cars
inline constexpr std::uint32_t kBodyRecords = kGameBase + 0x34210U;         // 80-byte bodies (position at +48)
inline constexpr std::uint32_t kObstacleTable = kGameBase + 0x122a10U;      // pointers to the first kind of obstacle
inline constexpr std::uint32_t kSecondObstacleTable = kGameBase + 0x12ad28U;
inline constexpr std::uint32_t kSecondObstacleCount = 0x64d37cU;
inline constexpr std::uint32_t kRandomWord = kGameBase + 0x3b128U;
inline constexpr std::uint32_t kRandomCarry = kGameBase + 0x3b12cU;
inline constexpr std::uint32_t kTightAvoidanceFlag = 0x55d7f8U;
inline constexpr std::uint32_t kLaneSteeringOff = 0x55d7f7U;
inline constexpr std::uint32_t kStateJumpTable = 0x4b7110U;

[[nodiscard]] constexpr std::uint32_t car_record(const std::uint32_t index) noexcept {
    return kCarRecords + 384U * index;
}
[[nodiscard]] constexpr std::uint32_t body_record(const std::uint32_t index) noexcept {
    return kBodyRecords + 80U * index;
}

// Driver states (+70).
inline constexpr std::uint32_t kStateFollow = 1U;
inline constexpr std::uint32_t kStateAvoid = 4U;

// Target jump-table entries of the final speed update, by the code they stand for.
inline constexpr std::uint32_t kCaseCruise = 0x1629b0U;
inline constexpr std::uint32_t kCaseHold = 0x162a24U;
inline constexpr std::uint32_t kCaseAvoid = 0x162a48U;
inline constexpr std::uint32_t kCaseOvertake = 0x162a54U;
inline constexpr std::uint32_t kCaseRecover = 0x162a80U;
inline constexpr std::uint32_t kCaseIntegrate = 0x162accU;  // the states that keep their speed change

} // namespace driver_speed_detail

template <class Memory>
[[nodiscard]] DriverSpeedOutcome update_driver_speed(Memory& memory, const std::uint32_t self,
                                                     const std::int64_t obstacle_count,
                                                     const std::uint32_t quiet_a, const std::uint32_t quiet_b,
                                                     const std::uint32_t gp) {
    using namespace driver_speed_detail;
    DriverSpeedOutcome outcome;
    const auto gp_float = [&](const std::uint32_t below) { return memory.f32(gp - below); };
    constexpr float kHalf = 0.5F;
    constexpr float kTwo = 2.0F;
    const auto store_speed_delta = [&](const float value) { memory.put_f32(self + 24U, value); };

    const std::uint32_t state = memory.u8(self + 70U);
    if (state == 0U) {
        outcome.instructions = 30U;
        return outcome;
    }
    outcome.instructions = 100U;

    std::uint32_t car = car_record(memory.u8(self + 74U));
    const auto finish = [&]() {
        // 0x00162acc: integrate the speed.
        const float speed = chop_add(memory.f32(self + 44U), memory.f32(self + 24U));
        memory.put_f32(self + 44U, ee_max(speed, 0.0F));
        outcome.instructions += 12U;
    };

    if ((memory.u8(car + 372U) & 4U) != 0U && static_cast<std::int8_t>(memory.u8(car + 371U)) != 8) {
        const float accel = chop_sub(chop_mul(gp_float(29820U), memory.f32(self)), memory.f32(self + 44U));
        store_speed_delta(accel);
        const float speed = chop_add(memory.f32(self + 44U), accel);
        memory.put_f32(self + 44U, ee_max(speed, 0.0F));
        outcome.instructions += 30U;
        return outcome;
    }

    const std::uint32_t body = body_record(memory.u8(self + 73U));
    if (state != 2U && state != 6U && state != 7U) {
        memory.put_u8(self + 70U, static_cast<std::uint8_t>(kStateFollow));
        memory.put_u32(self + 16U, 0xbf800000U);
    }
    const bool tight = memory.u8(kTightAvoidanceFlag) != 0U;
    const float look_ahead = tight ? 30.0F : 35.0F;
    const float side_limit = tight ? 4.0F : 10.0F;
    const Vector3 body_position = memory.vec3(body + 48U);

    // The driver commits to avoiding an obstacle: a new state and, usually, a
    // fresh random lateral offset (+28) from the game's generator.
    const auto commit_to_avoidance = [&]() {
        memory.put_u8(self + 70U, static_cast<std::uint8_t>(kStateAvoid));
        const float offset = memory.f32(self + 28U);
        if (game_less(offset, gp_float(29740U)) || !game_less_equal(offset, gp_float(30264U))) {
            return;
        }
        std::uint32_t word = memory.u32(kRandomWord);
        std::uint32_t carry = memory.u32(kRandomCarry);
        word = (word << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(word) >> 16);
        memory.put_u32(kRandomWord, word);
        word = word + carry;
        memory.put_u32(kRandomWord, word);
        carry = carry + word;
        memory.put_u32(kRandomCarry, carry);
        const float unit = chop_mul(2.3283064365386963e-10F, chop_from_unsigned(memory.u32(kRandomWord)));
        const float value = chop_sub(chop_mul(unit, 5.0F), kTwo);
        memory.put_f32(self + 28U, ee_min(ee_max(value, gp_float(29736U)), gp_float(31428U)));
    };

    // The last tests of both obstacle searches, after which the original
    // spawns an object: the obstacle lies within the side limit of the
    // driver's line, within 2 of its height, and the driver is faster than 10.
    const auto beside_path = [&](const Vector3 delta) {
        const float across = chop_dot(delta, memory.vec3(body));
        if (game_less_equal(across, -side_limit) || !game_less(across, side_limit)) {
            return false;
        }
        return game_less(ee_abs(delta.y), kTwo) && game_less(10.0F, memory.f32(self + 44U));
    };

    // Whether the driver reacts to an obstacle `distance` away whose top is
    // `obstacle_top` high (it is close enough, in time, at a speed that matters);
    // when it does the driver commits to avoiding it (unless it is held quiet).
    const auto reacts_to = [&](const float distance, const float obstacle_top) {
        const float clearance = ee_max(chop_sub(distance, obstacle_top), 0.0F);
        if (!game_less(chop_mul(kHalf, clearance), memory.f32(self + 44U))) {
            return false;
        }
        if (memory.u8(self + 70U) != kStateFollow && game_less_equal(kHalf, memory.f32(self + 16U))) {
            return false;
        }
        if (quiet_a == 0U && quiet_b == 0U) {
            commit_to_avoidance();
        }
        return true;
    };

    if ((memory.u8(car + 372U) & 6U) == 0U) {
        for (std::int64_t i = 0; i < obstacle_count; ++i) {
            outcome.instructions += 55U;
            const std::uint32_t obstacle = memory.u32(kObstacleTable + 4U * static_cast<std::uint32_t>(i));
            const Vector3 delta = chop_sub(memory.vec3(obstacle + 5008U), body_position);
            const float distance = chop_sqrt(chop_dot(delta, delta));
            const float closing = chop_dot(delta, memory.vec3(body + 32U));
            if (!game_less(distance, look_ahead) || game_less_equal(closing, 0.0F)) {
                continue;
            }
            const Vector3 top = memory.vec3(memory.u32(obstacle + 5024U) + 3712U);
            const float obstacle_top = chop_add(chop_add(top.z, memory.f32(self + 8U)), kTwo);
            outcome.instructions += 70U;
            if (!reacts_to(distance, obstacle_top) || !beside_path(delta)) {
                continue;
            }
            outcome.spawns = true;
            return outcome;
        }
        if ((memory.u8(car + 372U) & 2U) == 0U) {
            const std::int32_t count = static_cast<std::int32_t>(memory.u32(kSecondObstacleCount));
            for (std::int32_t i = 0; i < count; ++i) {
                outcome.instructions += 20U;
                const std::uint32_t obstacle = memory.u32(kSecondObstacleTable + 4U * static_cast<std::uint32_t>(i));
                if (obstacle == 0U || memory.u8(obstacle + 6378U) == 0U) {
                    continue;
                }
                outcome.instructions += 35U;
                const Vector3 delta = chop_sub(memory.vec3(obstacle + 48U), body_position);
                const float distance = chop_sqrt(chop_dot(delta, delta));
                const float closing = chop_dot(delta, memory.vec3(body + 32U));
                if (!game_less(distance, look_ahead) || game_less_equal(closing, 0.0F)) {
                    continue;
                }
                const Vector3 top = memory.vec3(memory.u32(obstacle + 64U) + 3712U);
                const float obstacle_top = chop_add(chop_add(top.z, memory.f32(self + 8U)), kTwo);
                outcome.instructions += 70U;
                if (!reacts_to(distance, obstacle_top) || !beside_path(delta)) {
                    continue;
                }
                outcome.spawns = true;
                return outcome;
            }
        }
    }

    // Brake for the car or obstacle the driver is following (+68 / +69's
    // waypoint) when the speed it can still hold is lower than its own.
    const std::uint32_t followed = memory.u8(self + 68U);
    if (followed != kNoRecord && (memory.u8(car + 372U) & 4U) == 0U) {
        outcome.instructions += 110U;
        const std::uint32_t waypoint = waypoint_record(followed);
        const Vector3 delta = chop_sub(memory.vec3(body_record(followed) + 48U), body_position);
        const float distance = chop_sqrt(chop_dot(delta, delta));
        const float gap = ee_max(chop_sub(chop_add(distance, chop_sub(memory.f32(waypoint + 12U), memory.f32(self + 8U))), 2.5F), 0.0F);
        memory.put_f32(self + 4U, gap);
        const float range = memory.f32(self + 20U);
        if (game_less(gap, range)) {
            const float share = game_div(gap, range);
            const float limit = ee_max(
                ee_max(chop_mul(memory.f32(waypoint + 44U), share), chop_mul(gp_float(31792U), gap)), 0.0F);
            if (!game_less_equal(memory.f32(self + 44U), limit)) {
                const float held = memory.f32(self + 16U);
                if (game_less(held, 0.0F) || game_less(gap, held)) {
                    memory.put_f32(self + 16U, gap);
                    memory.put_u8(self + 70U, 3U);
                    store_speed_delta(chop_mul(gp_float(29724U), chop_sub(limit, memory.f32(self + 44U))));
                }
            }
        }
    }

    // The car it is racing against (+64).
    const std::uint32_t rival_index = memory.u8(self + 64U);
    const auto rival_gap = [&](const std::uint32_t rival_owner) {
        // Distance to the rival, less its size.
        const Vector3 delta = chop_sub(memory.vec3(body_record(memory.u8(rival_owner + 73U)) + 48U),
                                       memory.vec3(body_record(memory.u8(self + 73U)) + 48U));
        const float distance = chop_sqrt(chop_dot(delta, delta));
        float gap = chop_add(distance, chop_sub(memory.f32(rival_owner + 12U), memory.f32(self + 8U)));
        if (game_less(gap, 0.0F)) {
            gap = 0.0F;
        }
        const std::uint32_t kind = memory.u8(rival_owner + 77U);
        if (kind == 3U || kind == 5U) {
            gap = chop_sub(gap, 1.5F);
        } else if (kind == 4U) {
            gap = chop_sub(gap, 3.5F);
        }
        return gap;
    };
    if (rival_index != kNoRecord && (memory.u8(car + 372U) & 4U) == 0U) {
        outcome.instructions += 50U;
        car = car_record(rival_index);
        if (memory.u8(kLaneSteeringOff) != 0U) {
            const std::uint32_t rival_owner = memory.u32(car + 276U);
            if (rival_owner == 0U) {
                memory.put_u8(self + 64U, 255U);
            } else {
                outcome.instructions += 110U;
                if (game_less(15.0F, rival_gap(rival_owner))) {
                    memory.put_u8(self + 64U, 255U);
                }
            }
        } else {
            if (memory.u8(car + 94U) != memory.u8(self + 65U)) {
                outcome.instructions += 50U;
                const Vector3 delta = chop_sub(memory.vec3(car + 48U), body_position);
                const float along = chop_dot(delta, memory.vec3(body));
                if (game_less(along, -3.5F) || !game_less_equal(along, 3.5F)) {
                    memory.put_u8(self + 64U, 255U);
                }
            }
            if ((memory.u8(car + 378U) & 1U) != 0U) {
                memory.put_u8(self + 64U, 0U);
            }
        }
    }

    // Slow down for that car when it is close.
    const std::uint32_t rival_now = memory.u8(self + 64U);
    if (rival_now != kNoRecord && (memory.u8(car + 372U) & 4U) == 0U) {
        outcome.instructions += 90U;
        float gap;
        if (memory.u8(kLaneSteeringOff) != 0U) {
            const std::uint32_t rival_owner = memory.u32(car + 276U);
            if (rival_owner == 0U) {
                memory.unsupported();
                return outcome;
            }
            gap = rival_gap(rival_owner);
        } else {
            const Vector3 delta = chop_sub(memory.vec3(car + 48U), body_position);
            gap = chop_sub(chop_sqrt(chop_dot(delta, delta)), 2.5F);
        }
        gap = ee_max(gap, 0.0F);
        const float range = memory.f32(self + 20U);
        if (game_less(gap, range)) {
            const float share = game_div(gap, range);
            const float limit = ee_max(
                ee_max(chop_mul(memory.f32(car + 196U), share), chop_mul(gp_float(31792U), gap)), 0.0F);
            if (!game_less_equal(memory.f32(self + 44U), limit)) {
                const float held = memory.f32(self + 16U);
                if (game_less(held, 0.0F) || game_less(gap, held)) {
                    memory.put_f32(self + 16U, gap);
                    memory.put_u8(self + 70U, 5U);
                    store_speed_delta(chop_mul(gp_float(29724U), chop_sub(limit, memory.f32(self + 44U))));
                }
            }
        }
    }

    // The speed change for the state the driver ended up in.
    const std::uint32_t final_state = memory.u8(self + 70U);
    std::uint32_t target = kCaseCruise;
    if (final_state < 8U) {
        target = memory.u32(kStateJumpTable + 4U * final_state);
    }
    if (target != kCaseCruise && target != kCaseHold && target != kCaseAvoid && target != kCaseOvertake &&
        target != kCaseRecover && target != kCaseIntegrate) {
        memory.unsupported();  // a jump table this code was not written from
        return outcome;
    }
    outcome.instructions += 30U;
    switch (target) {
    case kCaseCruise: {
        store_speed_delta(chop_sub(chop_mul(gp_float(29820U), memory.f32(self)), memory.f32(self + 44U)));
        if (memory.u8(kLaneSteeringOff) == 0U && memory.u8(self + 71U) != kNoRecord && memory.u32(self + 56U) != 0U) {
            outcome.instructions += 250U;
            const float correction = lane_steering_correction(memory, self, gp);
            store_speed_delta(chop_add(memory.f32(self + 24U), correction));
        }
        store_speed_delta(ee_min(ee_max(memory.f32(self + 24U), gp_float(29740U)), gp_float(29728U)));
        break;
    }
    case kCaseHold:
        store_speed_delta(ee_min(
            chop_sub(chop_mul(gp_float(31792U), memory.f32(self + 16U)), memory.f32(self + 44U)), 0.0F));
        break;
    case kCaseAvoid:
        memory.put_u32(self + 24U, memory.u32(self + 28U));
        break;
    case kCaseOvertake: {
        const float scaled = chop_mul(gp_float(29820U), memory.f32(self));
        const float target_speed = chop_mul(gp_float(31196U), scaled);
        store_speed_delta(chop_mul(gp_float(29724U), chop_sub(target_speed, memory.f32(self + 44U))));
        break;
    }
    case kCaseRecover: {
        const float target_speed = chop_mul(gp_float(30644U), chop_mul(gp_float(29820U), memory.f32(self)));
        const bool braking = (memory.u8(self + 75U) & 4U) != 0U;
        store_speed_delta(chop_mul(gp_float(braking ? 29720U : 29724U), chop_sub(target_speed, memory.f32(self + 44U))));
        break;
    }
    default:
        break;  // kCaseIntegrate: the speed is only integrated
    }
    finish();
    return outcome;
}

} // namespace takedown::engine::ai
