#pragma once

// Rigid-body integration for cars and loose objects. Forces, torques and
// impulses gathered during a frame advance a body's momentum, velocities and
// transform over one time step; the rotation is then re-orthonormalised and
// the derived quantities (world inverse inertia, inverse transform, speed,
// heading) are refreshed.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct RigidBody {
    Vector3 body_inverse_inertia[3];   // rows, in body space
    Vector3 world_inverse_inertia[3];  // rows, derived from the rotation
    Transform* transform{};             // basis rows: right, up, forward
    Transform inverse_transform;        // transposed basis, inverse translation
    Vector3 velocity;
    float speed{};
    Vector3 heading;                    // direction of travel, forward when still
    Vector3 angular_velocity;
    Vector3 angular_momentum;
    Vector3 force;                      // gathered this frame, cleared by a step
    Vector3 torque;                     // gathered this frame, cleared by a step
    Vector3 impulse;                    // linear, cleared by a step
    Vector3 angular_impulse;            // cleared by a step
    Vector3 position_correction;        // moved by after a step, then cleared
    float mass{};
};

// Gravity as the game applies it: only the vertical component scales with
// the body's mass.
[[nodiscard]] Vector3 gravity_force(const RigidBody& body, Vector3 gravity) noexcept;

// Adds gravity at the body's centre.
void add_gravity(RigidBody& body, Vector3 gravity) noexcept;

// Adds gravity at a point `lever` along the body's up axis, which also
// gives it a torque (cars use this while airborne or rolling over).
void add_gravity_at_lever(RigidBody& body, Vector3 gravity, float lever) noexcept;

// Stops the body: momentum, velocities and everything gathered are zeroed.
void stop_motion(RigidBody& body) noexcept;

// How a step rebuilt two basis rows from cross products while
// re-orthonormalising. The pair of rows that stayed most orthogonal (or
// the two left after a degenerate row) keeps its directions.
enum class BasisRebuild : std::uint8_t {
    RightThenForward,  // right = up x forward, forward = right x up
    UpThenRight,       // up = forward x right, right = up x forward
    ForwardThenUp,     // forward = right x up, up = forward x right
    // The same rebuild as ForwardThenUp, reached when no pair of rows was
    // clearly the most orthogonal (the original then carries an unrelated
    // value into the forward row's unused fourth lane).
    ForwardThenUpUnordered,
};

struct RigidBodyStep {
    BasisRebuild rebuild{};
    bool spin_limited{};  // angular velocity exceeded the limit and was cut
    bool still{};         // speed below the threshold; heading is forward
};

// Advances the body by `dt` seconds. `spin_damping` scales the angular
// momentum when the angular velocity is over the limit.
RigidBodyStep integrate(RigidBody& body, float dt, float spin_damping) noexcept;

} // namespace takedown::engine
