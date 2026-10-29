#pragma once

// The car's planar tyre model: four sub-steps of a two-axle ("bicycle")
// model in the car's own frame. Each sub-step turns the rear and the steered
// front axle by the heading gained so far, derives each axle's slip, gives
// the rear axle a friction force that saturates at 3 degrees of slip and the
// front axle one that saturates at 8 degrees, spins the driven wheels up or
// down, and integrates sideways speed, forward speed and yaw rate. The
// change over the step is then handed to the rigid body as a force and a
// torque.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cstdint>

namespace takedown::engine {

struct PlanarTyreCar {
    float mass{};
    float yaw_inverse_inertia{};  // the body's inverse inertia about its up axis
    Vector3 velocity;             // world
    float speed{};
    Vector3 angular_velocity;     // world
    const Transform* transform{}; // right, up, forward rows
    const Transform* inverse_transform{};

    // Inputs, in the game's units.
    float steering_lock{};        // degrees
    float steering{};             // -1 to 1
    float throttle{};
    float drive_gain{};
    float drag_coefficient{};
    float drag_area{};
    float drag_rpm_scale{};
    float drive_torque_scale{};
    float drive_torque{};
    float yaw_damping{};
    float yaw_damping_steer_relief{};
    float centre_of_mass_height{};

    // The body's force and torque gathered so far this frame.
    Vector3 force;
    Vector3 torque;

    // Driven wheel state.
    float wheel_spin{};           // rad/s, updated
    float wheel_radius{};
};

struct PlanarTyreStep {
    float front_wheel_angle{};    // degrees
    float wheel_spin{};
    bool skidding{};              // rear slip over half the forward speed
    Vector3 force;                // the body's force with the tyres' added
    Vector3 torque;               // the body's torque with the tyres' added
};

[[nodiscard]] PlanarTyreStep step_planar_tyres(const PlanarTyreCar& car, float dt) noexcept;

} // namespace takedown::engine
