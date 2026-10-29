#pragma once

// The race chase camera: each frame it follows a point on the car, eases its
// pitch toward the car's and swings its yaw after the car's heading (both at
// rates that tighten with speed and are applied once per 60th of a second of
// frame time), rolls by a set angle, widens the field of view as the car
// zooms and pulls back to keep the framing. In the game's rounding
// (engine/math/chopped.hpp), since the eased angles feed the next frame.

#include "takedown/engine/math/vector.hpp"

#include <array>

namespace takedown::engine {

// A camera mode's settings record.
struct ChaseCameraSettings {
    Vector3 offset;          // camera position from the followed point, before the rotations
    float offset_lane{};     // the offset's fourth lane
    Vector3 follow_point;    // on the car, in the car's frame
    float pitch_response{};  // 0 to 1: how much of the pitch gap closes per step at full speed factor
    float yaw_response{};    // the same for the yaw swing (speed independent)
    float roll_degrees{};
};

// The camera's eased angles and switches.
struct ChaseCameraState {
    float yaw_degrees{};
    float pitch_degrees{};
    bool tracks_yaw{};  // keeps the yaw swing in yaw_degrees
    bool reversed{};    // looking back: the car's forward and right axes turned round
};

struct ChaseCameraTuning {
    float speed_scale{};          // car speed to the speed factor's units
    float speed_factor_scale{};   // speed factor per unit (capped at 1)
    float steps_per_second{};
    float degrees_per_radian{};
    float radians_per_degree{};
    Vector3 pitch_axis;
    Vector3 yaw_axis;
    float narrow_fov_degrees{};
    float wide_fov_degrees{};
    float pull_back{};
    bool response_power{};        // raise both responses' retained fractions to this power first
    float response_exponent{};
};

struct ChaseCameraInput {
    std::array<Vector4, 4> car_rows;  // right, up, forward, position
    float car_speed{};
    float frame_seconds{};
    float zoom{};                     // 1 is full zoom (widest view), 0 or 2 none
    // Raise the retained fractions to the real number of 60ths that passed
    // instead of that number rounded to whole steps (the original's rule,
    // made for 30 and 60 FPS only).
    bool real_time_easing{};
    // The fourth lane the original leaves in the camera position: its
    // four-way sine and cosine fold a stack word it never set, and the
    // position keeps that lane squared.
    float position_lane{};
};

struct ChaseCameraView {
    float fov_degrees{};
    Vector4 orientation;  // quaternion x, y, z, w
    Vector4 position;
    // How it got there, for callers that account for the work.
    int response_steps{};
    int orientation_case{};  // 3 for a positive trace, else the largest diagonal (0 to 2)
    bool second_diagonal_tested{};  // the second diagonal was larger than the first
    bool speed_factor_capped{};
    bool yaw_swing_flipped{};
};

[[nodiscard]] ChaseCameraView update_chase_camera(ChaseCameraState& state, const ChaseCameraSettings& settings,
                                                  const ChaseCameraTuning& tuning, const ChaseCameraInput& input);

// Whether the car is below `slow_speed` (in speed-scaled units), where the
// camera gets a further low speed adjustment after this update.
[[nodiscard]] bool chase_camera_is_slow(float car_speed, const ChaseCameraTuning& tuning, float slow_speed) noexcept;

} // namespace takedown::engine
