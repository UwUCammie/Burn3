#pragma once

// Driving a traffic car along its lane, in the game's rounding
// (engine/math/chopped.hpp).
//
// A traffic car rides the track strips on two axles (front and rear strip
// riders, engine/track/surface_follow.hpp). Each frame it steers its front
// axle toward a point ahead of its lane's target, moves it by a step that
// closes the distance to the target at its speed, and drags the rear axle
// behind it. After both axles are kept on the road, the car's frame is
// rebuilt from them (forward from rear to front, up from the two axles' up
// directions, side across both), its wheels turn by the distance covered,
// and its lean eases toward the turn (roll) and the slope it is on (pitch).

#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

// A traffic lane at the car's place: its side and ahead directions and the
// point the car heads for.
struct TrafficLane {
    Vector3 side;
    Vector3 ahead;
    Vector3 point;
};

// The game's tuning for traffic (game data).
struct TrafficTuning {
    float approach_rate{};       // speed change per unit the target is ahead (beyond one); also
                                 // scales the slope the pitch follows
    // Steering moves toward the turn by at most one step a frame: the small
    // step further out, the large one back toward straight. The back limits
    // (negative) are the changes beyond which it steps back.
    float steer_small_step{};
    float steer_large_step{};
    float steer_small_back{};
    float steer_large_back{};
    float wheel_turn_wrap{};     // wheel angle range, either side of zero
    float roll_keep{};           // roll kept per frame
    float roll_rate{};           // roll per unit of the route's turn
    float pitch_keep{};          // pitch kept per frame
};

struct TrafficSteerInput {
    Vector3 front;
    Vector3 up;
    Vector3 forward;
    Vector3 rear_anchor;
    TrafficLane lane;
    float route_side{};          // which side of the lane the route keeps to
    float wheel_radius{};
    float speed{};
    float step_time{};
    float wrong_side_reach{};    // look-ahead scale when on the wrong side
    float steer{};               // sine of the steering angle
    float rear_reach{};          // rear axle ahead of the rear anchor
};

struct TrafficSteer {
    Vector3 target;              // the lane point raised by the wheel radius
    Vector3 to_target;
    float distance{};
    float side_offset{};         // across the lane, by the route's side
    float ahead_offset{};        // along the lane
    bool behind{};               // the target is behind
    Vector3 approach;            // to the target, or straight up the lane when behind
    float speed_change{};
    float step{};                // distance moved, at most to the target
    bool step_capped{};          // the step would have passed the target
    bool step_negative{};        // it would have gone backward: zero
    float reach_scale{};         // 2, or the wrong side's scale
    float reach{};               // look-ahead along the lane
    Vector3 look_ahead;
    Vector3 aim;
    float aim_length_squared{};
    bool aim_tiny{};
    float aim_inverse_length{};
    bool aim_degenerate{};
    Vector3 aim_direction;       // unit (or its z set to 1 when too short)
    Vector3 turn;                // aim_direction x forward
    float turn_length{};         // signed by the turn's y
    float steer{};
    bool steered_out{};          // the change was past the step outward
    bool steered_back{};         // or past the back limit
    float steer_cosine{};
    Vector3 heading;             // forward turned by the steering (y from the aim)
    Vector3 front;
    Vector3 rear;
};

[[nodiscard]] TrafficSteer steer_traffic(const TrafficSteerInput& input, const TrafficTuning& tuning) noexcept;

// Forward from the rear axle to the front one, and the rear anchor behind the
// front axle (`axle_offset` along forward).
struct TrafficHeading {
    Vector3 along;               // front - rear
    float length_squared{};
    bool tiny{};                 // too short: z set to 1, not normalised
    float inverse_length{};
    bool degenerate{};           // no length: z set to 1 after normalising
    Vector3 forward;
    Vector3 rear_anchor;
};

[[nodiscard]] TrafficHeading traffic_heading(Vector3 front, Vector3 rear, float axle_offset) noexcept;

// A unit vector with one lane forced to 1 when it has no length.
struct TrafficUnit {
    Vector3 raw;
    float length_squared{};
    float inverse_length{};
    bool degenerate{};
    Vector3 unit;
};

struct TrafficFrame {
    float forward_along_side{};  // new forward along the old side direction
    TrafficUnit axle_up;         // the two axles' up directions summed (y forced)
    TrafficUnit side;            // axle_up x forward (x forced)
    TrafficUnit up;              // forward x side (y forced)
};

[[nodiscard]] TrafficFrame traffic_frame(Vector3 forward, Vector3 old_side, Vector3 front_up, Vector3 rear_up) noexcept;

struct TrafficLeanInput {
    float wheel_angle{};
    float wheel_radius{};
    bool wheels_turn{};
    bool lean{};
    float roll{};
    float roll_gain{};
    float route_turn{};
    float pitch{};
    float pitch_gain{};
    float lean_limit{};
    float speed{};
    float step{};
    float forward_along_side{};
};

struct TrafficLean {
    float wheel_angle{};
    bool wrapped_down{};         // over the range: one range taken off
    bool wrapped_up{};
    float roll{};
    float roll_cosine{};
    float pitch{};
    float pitch_cosine{};
};

[[nodiscard]] TrafficLean lean_traffic(const TrafficLeanInput& input, const TrafficTuning& tuning) noexcept;

} // namespace takedown::engine
