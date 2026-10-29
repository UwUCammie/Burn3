#pragma once

// AI racers driven along the road strip (placed and given a speed each
// frame rather than simulated): each frame such a car turns its
// heading toward the route's at a limited rate, is placed along it on the
// road surface, kept inside the strip's sides and given its speed. These are
// the steps' geometry, in the game's rounding (engine/math/chopped.hpp),
// since a car's state feeds the next frame.

#include "takedown/engine/math/axis_rotation.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/orthonormal.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <cmath>

namespace takedown::engine {

struct HeadingTurnTuning {
    float turn_limit_degrees{};   // per frame
    float degrees_per_radian{};
    float radians_per_degree{};
};

struct HeadingTurn {
    Vector3 heading;
    bool limited{};        // turned by the limit about `up`, not straight to the route
    bool positive_turn{};  // when limited: by +limit (the route is not to the cross side)
};

// The heading after turning from `previous` toward `route`: the route's when
// the angle between them is within the limit, else `previous` rotated about
// `up` by the limit, toward the route's side.
[[nodiscard]] inline HeadingTurn turn_heading_toward(const Vector3 previous, const Vector3 route, const Vector3 up,
                                                     const HeadingTurnTuning& tuning) noexcept {
    const float cosine =
        minimum(maximum(chop_dot(chop_normalized(route), chop_normalized(previous)), -1.0F), 1.0F);
    const auto angle = static_cast<float>(std::acos(static_cast<double>(cosine)));
    if (game_less_equal(chop_mul(tuning.degrees_per_radian, angle), tuning.turn_limit_degrees)) {
        return {route, false, false};
    }
    const float side = chop_dot(chop_cross(previous, up), route);
    const bool positive = game_less_equal(side, 0.0F);
    const float limit = positive ? tuning.turn_limit_degrees : -tuning.turn_limit_degrees;
    const RotationRows rotation = axis_angle_rotation_chopped(chop_normalized(up), chop_mul(tuning.radians_per_degree, limit));
    Vector3 turned = chop_mul(rotation.rows[0], previous.x);
    turned = chop_add(turned, chop_mul(rotation.rows[1], previous.y));
    turned = chop_add(turned, chop_mul(rotation.rows[2], previous.z));
    return {chop_add(turned, Vector3{}), true, positive};
}

// The rotation of a car heading along `heading` on a road whose surface
// rises `rise` (the vertical part of the unit direction along the strip) per
// unit forward: forward is the heading tilted by the rise, right is across
// it from `up`, and up is rebuilt square to both.
[[nodiscard]] inline RotationBasis road_frame(const Vector3 heading, const float rise, const Vector3 up) noexcept {
    RotationBasis frame;
    frame.at = {heading.x, chop_add(heading.y, rise), heading.z};
    frame.right = chop_normalized(chop_cross(up, frame.at));
    frame.up = chop_normalized(chop_cross(frame.at, frame.right));
    return frame;
}

// `position` moved back inside a strip side so the car's half width clears
// it: `normal` points out of the side (any length), `on_side` is a point on
// it.
[[nodiscard]] inline Vector3 kept_inside_side(const Vector3 position, const Vector3 normal, const Vector3 on_side,
                                              const float width) noexcept {
    const Vector3 unit = chop_normalized(normal);
    const float beyond = chop_dot(unit, chop_sub(position, on_side));
    const float back = -chop_add(chop_add(0.0F, beyond), chop_mul(0.5F, width));
    return chop_add(position, chop_mul(unit, back));
}

// The speed such a car is set to: its target, but changing by at most
// `slow_step` down and `speed_up_step` up from its current speed per frame.
[[nodiscard]] inline float follow_speed(const float speed, const float target, const float slow_step,
                                         const float speed_up_step) noexcept {
    const float over = chop_sub(speed, target);
    if (!game_less_equal(over, slow_step)) {
        return chop_sub(speed, slow_step);
    }
    if (game_less(over, -speed_up_step)) {
        return chop_add(speed_up_step, speed);
    }
    return target;
}

} // namespace takedown::engine
