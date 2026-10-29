	#include "takedown/engine/track/traffic_drive.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

namespace takedown::engine {
namespace {

constexpr float kTinyLengthSquared = 2.32830644e-10F;  // 2^-32
constexpr float kLookAheadFloor = 225.0F;              // speed the look-ahead assumes at least
constexpr float kLookAheadTime = 10.0F;

float chop_neg_or_keep(const float value, const bool negate) noexcept {
    return negate ? chop_sub(0.0F, value) : value;
}

TrafficUnit unit_forcing(const Vector3 raw, float Vector3::*forced) noexcept {
    TrafficUnit unit;
    unit.raw = raw;
    unit.length_squared = chop_dot(raw, raw);
    unit.inverse_length = chop_reciprocal_sqrt(unit.length_squared);
    unit.unit = chop_mul(raw, unit.inverse_length);
    unit.degenerate = game_equal(0.0F, game_sqrt(unit.length_squared));
    if (unit.degenerate) {
        unit.unit.*forced = 1.0F;
    }
    return unit;
}

// The sine's cosine, as the game takes it: sqrt(1 - s * s).
float cosine_of(const float sine) noexcept {
    return game_sqrt(chop_sub(chop_add(0.0F, 1.0F), chop_mul(sine, sine)));
}

float limit_lean(const float value, const float limit) noexcept {
    return minimum(maximum(value, -limit), limit);
}

} // namespace

TrafficSteer steer_traffic(const TrafficSteerInput& input, const TrafficTuning& tuning) noexcept {
    TrafficSteer s;
    const TrafficLane& lane = input.lane;
    s.target = chop_add(lane.point, chop_mul(input.up, input.wheel_radius));
    s.to_target = chop_sub(s.target, input.front);
    s.distance = chop_sqrt(chop_dot(s.to_target, s.to_target));
    s.side_offset = chop_mul(chop_dot(s.to_target, lane.side), input.route_side);
    s.ahead_offset = chop_dot(s.to_target, lane.ahead);

    // Close the distance to the target along the lane: faster when it is
    // ahead (and much faster when it has fallen behind).
    s.behind = game_less(s.ahead_offset, 0.0F);
    const float closing = chop_mul(tuning.approach_rate, chop_sub(s.ahead_offset, 1.0F));
    if (s.behind) {
        s.approach = chop_mul(lane.ahead, -s.ahead_offset);
        s.speed_change = chop_add(chop_add(0.0F, closing), chop_mul(2.0F, s.ahead_offset));
    } else {
        s.approach = s.to_target;
        s.speed_change = closing;
    }
    s.step = chop_mul(chop_add(input.speed, s.speed_change), input.step_time);
    s.step_capped = !game_less_equal(s.step, s.distance);
    if (s.step_capped) {
        s.step = s.distance;
    }
    s.step_negative = game_less(s.step, 0.0F);
    if (s.step_negative) {
        s.step = 0.0F;
    }

    // Aim at a point ahead of the target, further at speed.
    s.reach_scale = game_less_equal(s.side_offset, 0.0F)
                        ? game_div(1.0F, chop_mul(2.0F, input.wrong_side_reach))
                        : 2.0F;
    const float floor = chop_mul(kLookAheadFloor, input.step_time);
    const float travel = chop_mul(s.step, input.speed);
    s.reach = chop_mul(s.reach_scale, chop_mul(kLookAheadTime, game_less(travel, floor) ? floor : travel));
    s.look_ahead = chop_mul(lane.ahead, s.reach);
    s.aim = chop_add(s.approach, s.look_ahead);
    s.aim_length_squared = chop_dot(s.aim, s.aim);
    s.aim_tiny = game_less(s.aim_length_squared, kTinyLengthSquared);
    if (s.aim_tiny) {
        s.aim_direction = {s.aim.x, s.aim.y, 1.0F};
    } else {
        s.aim_inverse_length = chop_reciprocal_sqrt(s.aim_length_squared);
        s.aim_direction = chop_mul(s.aim, s.aim_inverse_length);
        s.aim_degenerate = game_equal(0.0F, game_sqrt(s.aim_length_squared));
        if (s.aim_degenerate) {
            s.aim_direction.z = 1.0F;
        }
    }

    // The turn toward the aim (its length signed by the side), approached
    // a step at a time.
    s.turn = chop_cross(s.aim_direction, input.forward);
    s.turn_length = chop_neg_or_keep(chop_sqrt(chop_dot(s.turn, s.turn)), game_less_equal(s.turn.y, 0.0F));
    const float change = chop_sub(s.turn_length, input.steer);
    s.steer = s.turn_length;
    const bool right = !game_less_equal(input.steer, 0.0F);
    const float out_step = right ? tuning.steer_small_step : tuning.steer_large_step;
    const float back_limit = right ? tuning.steer_large_back : tuning.steer_small_back;
    const float back_step = right ? tuning.steer_large_step : tuning.steer_small_step;
    s.steered_out = !game_less_equal(change, out_step);
    if (s.steered_out) {
        s.steer = chop_add(out_step, input.steer);
    }
    s.steered_back = game_less(change, back_limit);
    if (s.steered_back) {
        s.steer = chop_sub(input.steer, back_step);
    }
    s.steer_cosine = cosine_of(s.steer);

    // Turn the forward direction about the vertical by the steering; the
    // climb comes from the aim.
    const Vector3 f = input.forward;
    s.heading = {chop_add(0.0F, chop_sub(chop_mul(f.x, s.steer_cosine), chop_mul(f.z, s.steer))), s.aim_direction.y,
                 chop_add(0.0F, chop_add(chop_mul(f.x, s.steer), chop_mul(f.z, s.steer_cosine)))};
    s.front = chop_add(Vector3{}, chop_add(input.front, chop_mul(s.heading, s.step)));
    s.rear = chop_add(Vector3{}, chop_add(input.rear_anchor, chop_mul(input.forward, input.rear_reach)));
    return s;
}

TrafficHeading traffic_heading(const Vector3 front, const Vector3 rear, const float axle_offset) noexcept {
    TrafficHeading h;
    h.along = chop_sub(front, rear);
    h.length_squared = chop_dot(h.along, h.along);
    h.tiny = game_less(h.length_squared, kTinyLengthSquared);
    if (h.tiny) {
        h.forward = {h.along.x, h.along.y, 1.0F};
    } else {
        h.inverse_length = chop_reciprocal_sqrt(h.length_squared);
        h.forward = chop_mul(h.along, h.inverse_length);
        h.degenerate = game_equal(0.0F, game_sqrt(h.length_squared));
        if (h.degenerate) {
            h.forward.z = 1.0F;
        }
    }
    h.rear_anchor = chop_sub(front, chop_mul(h.forward, axle_offset));
    return h;
}

TrafficFrame traffic_frame(const Vector3 forward, const Vector3 old_side, const Vector3 front_up,
                           const Vector3 rear_up) noexcept {
    TrafficFrame frame;
    frame.forward_along_side = chop_dot(forward, old_side);
    frame.axle_up = unit_forcing(chop_add(front_up, rear_up), &Vector3::y);
    frame.side = unit_forcing(chop_cross(frame.axle_up.unit, forward), &Vector3::x);
    frame.up = unit_forcing(chop_cross(forward, frame.side.unit), &Vector3::y);
    return frame;
}

TrafficLean lean_traffic(const TrafficLeanInput& input, const TrafficTuning& tuning) noexcept {
    TrafficLean lean;
    lean.wheel_angle = input.wheel_angle;
    if (input.wheels_turn) {
        lean.wheel_angle = chop_add(input.wheel_angle, game_div(input.step, input.wheel_radius));
        if (!game_less_equal(lean.wheel_angle, tuning.wheel_turn_wrap)) {
            lean.wheel_angle = chop_sub(lean.wheel_angle, tuning.wheel_turn_wrap);
            lean.wrapped_down = true;
        } else if (game_less(lean.wheel_angle, -tuning.wheel_turn_wrap)) {
            lean.wheel_angle = chop_add(lean.wheel_angle, tuning.wheel_turn_wrap);
            lean.wrapped_up = true;
        }
    }
    lean.roll = input.roll;
    lean.pitch = input.pitch;
    if (!input.lean) {
        return lean;
    }
    // Roll into the route's turn, pitch with the slope (how far forward
    // swung across the old side) at speed; both ease and stay in limits.
    const float roll = chop_add(chop_add(0.0F, chop_mul(input.roll, tuning.roll_keep)),
                                chop_mul(input.roll_gain, chop_mul(tuning.roll_rate, input.route_turn)));
    lean.roll = limit_lean(roll, input.lean_limit);
    lean.roll_cosine = cosine_of(lean.roll);
    const float slope = chop_mul(tuning.approach_rate, input.forward_along_side);
    const float push = chop_mul(input.speed, chop_mul(input.pitch_gain, slope));
    const float pitch =
        chop_add(chop_add(0.0F, chop_mul(input.pitch, tuning.pitch_keep)), chop_mul(input.speed, push));
    lean.pitch = limit_lean(pitch, input.lean_limit);
    lean.pitch_cosine = cosine_of(lean.pitch);
    return lean;
}

} // namespace takedown::engine
