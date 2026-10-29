#pragma once

// A car's gearbox: which gear its engine runs in at a given wheel speed, and
// the motion a car is given when the game sets its speed outright (AI racers
// following the road, cars placed at a start line). In the game's rounding
// (engine/math/chopped.hpp), since the results feed the next frame's
// driving.

#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <bit>
#include <cstdint>
#include <span>

namespace takedown::engine {

struct GearPick {
    int gear{};
    float engine_rate{};  // wheel rate through that gear and the final drive
};

// The lowest gear from second up (`ratios[1]` on) whose engine rate (wheel
// rate times gear ratio times final drive, in radians per second) stays at or
// under `rate_limit` (the redline), or one past the top gear when none does.
// With fewer than two gears the pick is gear 1 at `unchanged_rate` (what the
// engine was doing).
[[nodiscard]] inline GearPick pick_gear(const std::span<const float> ratios, const float final_drive,
                                        const float rate_limit, const float wheel_rate,
                                        const float unchanged_rate) noexcept {
    GearPick pick{1, unchanged_rate};
    for (; pick.gear < static_cast<int>(ratios.size()); ++pick.gear) {
        pick.engine_rate = chop_mul(final_drive, chop_mul(wheel_rate, ratios[static_cast<std::size_t>(pick.gear)]));
        if (game_less_equal(pick.engine_rate, rate_limit)) {
            break;
        }
    }
    return pick;
}

struct SetMotion {
    Vector3 velocity;
    float speed{};
    bool moving{};      // otherwise the heading is the car's forward axis
    Vector3 heading;    // unit velocity while moving
};

// A car moving at `speed` along its `forward` axis. Speeds under 2^-15 are
// zero, and a velocity whose squared length is under 2^-32 counts as still.
[[nodiscard]] inline SetMotion motion_along(const Vector3 forward, float speed) noexcept {
    if ((std::bit_cast<std::uint32_t>(speed) & 0x7f800000U) < 0x37800001U) {
        speed = 0.0F;
    }
    SetMotion motion;
    const Vector3 velocity = chop_mul(forward, speed);
    motion.velocity = chop_add(Vector3{}, velocity);
    const float length2 = chop_dot(velocity, velocity);
    motion.moving = !game_less(length2, 0x1p-32F);
    if (motion.moving) {
        motion.heading = chop_mul(velocity, chop_reciprocal_sqrt(length2));
        motion.speed = game_sqrt(length2);
    } else {
        motion.heading = forward;
    }
    return motion;
}

} // namespace takedown::engine
