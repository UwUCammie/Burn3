#include "takedown/engine/camera/chase_camera.hpp"

#include "takedown/engine/math/axis_rotation.hpp"
#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/game_library.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace takedown::engine {
namespace {

// `v` through the rows (x, then y, then z, each a multiply-add).
Vector3 rotated(const RotationRows& r, const Vector3 v) noexcept {
    return chop_add(chop_add(chop_mul(r.rows[0], v.x), chop_mul(r.rows[1], v.y)), chop_mul(r.rows[2], v.z));
}

// The rotation by `degrees` about `axis` (normalised first).
RotationRows rotation_about(const Vector3 axis, const float degrees, const float radians_per_degree) noexcept {
    return axis_angle_rotation_chopped(chop_normalized(axis), chop_mul(radians_per_degree, degrees));
}

// The scalar unit's root of `value` halved, and a half over that root (the
// scale of the quaternion's other lanes).
inline std::pair<float, float> half_root(const float value) noexcept {
    const float root = game_sqrt(value);
    return {chop_mul(root, 0.5F), game_div(0.5F, root)};
}

struct Orientation {
    Vector4 quaternion;
    int which{};  // 3 for a positive trace, else the largest diagonal
    bool second_diagonal_larger{};
};

// The rotation's quaternion, from its trace when that is positive and else
// from its largest diagonal.
Orientation orientation_of(const RotationRows& m) noexcept {
    const Vector3 r0 = m.rows[0];
    const Vector3 r1 = m.rows[1];
    const Vector3 r2 = m.rows[2];
    const float trace = chop_add(chop_add(r0.x, r1.y), r2.z);
    if (!game_less_equal(trace, 0.0F)) {
        const auto [half, scale] = half_root(chop_add(trace, 1.0F));
        const float x = chop_add(chop_sub(r1.z, r2.y), 0.0F);
        const float y = chop_add(chop_sub(r2.x, r0.z), 0.0F);
        const float z = chop_add(chop_sub(r0.y, r1.x), 0.0F);
        return {{chop_mul(x, scale), chop_mul(y, scale), chop_mul(z, scale), chop_sub(half, 0.0F)}, 3, false};
    }
    Orientation result;
    result.second_diagonal_larger = !game_less_equal(r1.y, r0.x);
    int which;
    if (!result.second_diagonal_larger) {
        which = game_less_equal(r2.z, r0.x) ? 0 : 2;
    } else {
        which = game_less_equal(r2.z, r1.y) ? 1 : 2;
    }
    result.which = which;
    if (which == 0) {
        const auto [half, scale] = half_root(chop_add(chop_sub(r0.x, chop_add(r1.y, r2.z)), 1.0F));
        result.quaternion = {chop_add(half, 0.0F), chop_add(chop_mul(chop_add(r0.y, r1.x), scale), 0.0F),
                             chop_add(chop_mul(chop_add(r0.z, r2.x), scale), 0.0F),
                             chop_sub(chop_mul(chop_sub(r1.z, r2.y), scale), 0.0F)};
    } else if (which == 1) {
        const auto [half, scale] = half_root(chop_add(chop_sub(r1.y, chop_add(r2.z, r0.x)), 1.0F));
        result.quaternion = {chop_add(chop_mul(chop_add(r1.x, r0.y), scale), 0.0F), chop_add(half, 0.0F),
                             chop_add(chop_mul(chop_add(r1.z, r2.y), scale), 0.0F),
                             chop_sub(chop_mul(chop_sub(r2.x, r0.z), scale), 0.0F)};
    } else {
        const auto [half, scale] = half_root(chop_add(chop_sub(r2.z, chop_add(r0.x, r1.y)), 1.0F));
        result.quaternion = {chop_add(chop_mul(chop_add(r2.x, r0.z), scale), 0.0F),
                             chop_add(chop_mul(chop_add(r2.y, r1.z), scale), 0.0F), chop_add(half, 0.0F),
                             chop_sub(chop_mul(chop_sub(r0.y, r1.x), scale), 0.0F)};
    }
    return result;
}

// The zoom's share of the field of view's widening: 1 at full zoom, falling
// off as the square of the distance from it.
float zoom_curve(const float zoom) noexcept {
    const float distance = chop_sub(zoom, 1.0F);
    return chop_sub(1.0F, chop_mul(distance, distance));
}

// Each row of `inner` through `outer`.
RotationRows composed(const RotationRows& outer, const RotationRows& inner) noexcept {
    RotationRows result;
    for (int row = 0; row < 3; ++row) {
        result.rows[row] = rotated(outer, inner.rows[row]);
    }
    return result;
}

} // namespace

// RECOVERY: the maths matches the original statement for statement, but its
// GIMPLE differs in three places that could not be reproduced: the original
// keeps a dead copy of `steps`, and routes the yaw's `offset = ...` and the
// roll's `position = ...` assignments through a temporary (as if those
// targets were still address-taken when chop_add was inlined).
ChaseCameraView update_chase_camera(ChaseCameraState& state, const ChaseCameraSettings& settings,
                                    const ChaseCameraTuning& tuning, const ChaseCameraInput& input) {
    ChaseCameraView view{};
    Vector3 position;

    float speed_factor = chop_mul(tuning.speed_factor_scale, chop_mul(tuning.speed_scale, input.car_speed));
    if (!game_less_equal(speed_factor, 1.0F)) {
        view.speed_factor_capped = true;
        speed_factor = 1.0F;
    }

    // The frame time in 60ths, rounded half away from zero to whole steps.
    const float sixtieths = chop_mul(tuning.steps_per_second, input.frame_seconds);
    const float rounding = std::bit_cast<float>((std::bit_cast<std::uint32_t>(sixtieths) & 0x80000000U) | 0x3f000000U);
    const std::int32_t steps = truncate_word(chop_from_int(truncate_word(chop_add(sixtieths, rounding))));

    // The fractions of the gaps kept per step.
    float pitch_retained = chop_mul(chop_sub(1.0F, settings.pitch_response), speed_factor);
    float yaw_retained = chop_sub(1.0F, settings.yaw_response);
    if (tuning.response_power) {
        pitch_retained = game_pow(pitch_retained, tuning.response_exponent);
        yaw_retained = game_pow(yaw_retained, tuning.response_exponent);
    }
    float pitch_kept = 1.0F;
    float yaw_kept = 1.0F;
    for (std::int32_t step = 0; step < steps; ++step) {
        pitch_kept = chop_mul(pitch_kept, pitch_retained);
        yaw_kept = chop_mul(yaw_kept, yaw_retained);
    }
    if (input.real_time_easing && sixtieths >= 0.0F && pitch_retained >= 0.0F && yaw_retained >= 0.0F) {
        pitch_kept = static_cast<float>(std::pow(static_cast<double>(pitch_retained), static_cast<double>(sixtieths)));
        yaw_kept = static_cast<float>(std::pow(static_cast<double>(yaw_retained), static_cast<double>(sixtieths)));
    }
    view.response_steps = std::max(steps, 0);
    const float pitch_closed = chop_sub(1.0F, pitch_kept);
    const float yaw_closed = chop_sub(1.0F, yaw_kept);

    // The car's frame, its forward and right axes turned round when looking back.
    RotationRows car{{input.car_rows[0].xyz(), input.car_rows[1].xyz(), input.car_rows[2].xyz()}};
    if (state.reversed) {
        car.rows[2] = chop_mul(car.rows[2], -1.0F);
        car.rows[0] = chop_mul(car.rows[0], -1.0F);
    }
    const Vector3 forward = car.rows[2];
    const Vector3 followed = chop_add(rotated(car, settings.follow_point), input.car_rows[3].xyz());

    // Ease the pitch toward the car's.
    const float car_pitch = chop_mul(tuning.degrees_per_radian, game_asin(forward.y));
    state.pitch_degrees =
        chop_add(chop_add(state.pitch_degrees, 0.0F), chop_mul(pitch_closed, chop_sub(car_pitch, state.pitch_degrees)));
    const RotationRows pitch = rotation_about(tuning.pitch_axis, -state.pitch_degrees, tuning.radians_per_degree);
    const RotationRows identity{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    RotationRows frame;
    for (int row = 0; row < 3; ++row) {
        frame.rows[row] = rotated(pitch, identity.rows[row]);
    }
    Vector3 offset = chop_add(rotated(pitch, settings.offset), Vector3{});

    // Then the yaw so far.
    const RotationRows yaw = rotation_about(tuning.yaw_axis, state.yaw_degrees, tuning.radians_per_degree);
    for (int row = 0; row < 3; ++row) {
        frame.rows[row] = rotated(yaw, frame.rows[row]);
    }
    offset = chop_add(rotated(yaw, offset), Vector3{});

    // Swing the yaw after the car's heading: the flat angle between the
    // camera's forward and the car's, toward the car's side.
    const Vector3 flat_car{chop_add(forward.x, 0.0F), 0.0F, chop_add(forward.z, 0.0F)};
    const Vector3 flat_camera{chop_add(frame.rows[2].x, 0.0F), 0.0F, chop_add(frame.rows[2].z, 0.0F)};
    const float cosine = chop_dot(chop_normalized(flat_car), chop_normalized(flat_camera));
    float swing = chop_mul(tuning.degrees_per_radian, game_acos(std::min(std::max(cosine, -1.0F), 1.0F)));
    if (game_less(chop_dot(forward, frame.rows[0]), 0.0F)) {
        swing = chop_mul(swing, -1.0F);
        view.yaw_swing_flipped = true;
    }
    const float swing_step = chop_mul(swing, yaw_closed);
    if (state.tracks_yaw) {
        state.yaw_degrees = chop_add(state.yaw_degrees, swing_step);
    }
    const RotationRows swing_turn = rotation_about(tuning.yaw_axis, swing_step, tuning.radians_per_degree);
    for (int row = 0; row < 3; ++row) {
        frame.rows[row] = rotated(swing_turn, frame.rows[row]);
    }
    position = chop_add(chop_add(rotated(swing_turn, offset), Vector3{}), followed);

    // Roll last.
    const RotationRows roll = rotation_about(tuning.pitch_axis, settings.roll_degrees, tuning.radians_per_degree);
    position = chop_add(rotated(frame, Vector3{}), position);
    frame = composed(frame, roll);

    // Widen the view with the zoom and pull back to keep the framing.
    view.fov_degrees = chop_add(chop_add(tuning.narrow_fov_degrees, 0.0F),
                                chop_mul(zoom_curve(input.zoom), chop_sub(tuning.wide_fov_degrees, tuning.narrow_fov_degrees)));
    const float half_fov_tangent = game_tan(chop_mul(tuning.radians_per_degree, game_div(view.fov_degrees, 2.0F)));
    const float pull = chop_sub(tuning.pull_back, chop_mul(tuning.pull_back, game_div(1.0F, half_fov_tangent)));
    position = chop_add(position, chop_mul(frame.rows[2], pull));

    const Orientation orientation = orientation_of(frame);
    view.orientation = orientation.quaternion;
    view.orientation_case = orientation.which;
    view.second_diagonal_tested = orientation.second_diagonal_larger;
    view.position = {position.x, position.y, position.z, input.position_lane};
    return view;
}

bool chase_camera_is_slow(const float car_speed, const ChaseCameraTuning& tuning, const float slow_speed) noexcept {
    return game_less(chop_mul(tuning.speed_scale, car_speed), slow_speed);
}

} // namespace takedown::engine
