#include "takedown/engine/physics/rigid_body.hpp"

#include <cmath>

namespace takedown::engine {
namespace {

constexpr float kMaxUpwardSpeed = 120.0F;
constexpr float kMaxSpin = 100.0F;
constexpr float kMaxSpinSquared = kMaxSpin * kMaxSpin;
constexpr float kStillSpeedSquared = 2.3283064365386962890625e-10F;  // 2^-32

// `v` times a 3x3 matrix given as rows: v.x * row 0 + v.y * row 1 + v.z * row 2.
Vector3 times_rows(const Vector3 v, const Vector3 (&rows)[3]) noexcept {
    return rows[0] * v.x + rows[1] * v.y + rows[2] * v.z;
}

// Scales `v` to unit length in place and returns its original length.
float normalize(Vector3& v) noexcept {
    const float length_squared = dot(v, v);
    v = v * reciprocal_sqrt(length_squared);
    return std::sqrt(length_squared);
}

// The unit-length cross product of two rows.
Vector3 rebuilt(const Vector3 a, const Vector3 b) noexcept {
    Vector3 result = cross(a, b);
    normalize(result);
    return result;
}

// Normalises the rows and rebuilds two of them from the pair that stayed most
// orthogonal (a zero-length row is always rebuilt).
BasisRebuild orthonormalize(Vector3 (&basis)[3]) noexcept {
    Vector3& right = basis[0];
    Vector3& up = basis[1];
    Vector3& forward = basis[2];
    const float right_length = normalize(right);
    const float up_length = normalize(up);
    const float forward_length = normalize(forward);
    BasisRebuild rebuild;
    if (right_length <= 0.0F) {
        rebuild = BasisRebuild::RightThenForward;
    } else if (up_length <= 0.0F) {
        rebuild = BasisRebuild::UpThenRight;
    } else if (forward_length <= 0.0F) {
        rebuild = BasisRebuild::ForwardThenUp;
    } else {
        const float up_forward = std::fabs(dot(up, forward));
        const float forward_right = std::fabs(dot(forward, right));
        const float right_up = std::fabs(dot(up, right));
        if (up_forward < forward_right) {
            rebuild = up_forward < right_up ? BasisRebuild::RightThenForward : BasisRebuild::ForwardThenUp;
        } else {
            rebuild = forward_right < right_up ? BasisRebuild::UpThenRight : BasisRebuild::ForwardThenUpUnordered;
        }
    }
    switch (rebuild) {
    case BasisRebuild::RightThenForward:
        right = rebuilt(up, forward);
        forward = rebuilt(right, up);
        break;
    case BasisRebuild::UpThenRight:
        up = rebuilt(forward, right);
        right = rebuilt(up, forward);
        break;
    case BasisRebuild::ForwardThenUp:
    case BasisRebuild::ForwardThenUpUnordered:
        forward = rebuilt(right, up);
        up = rebuilt(forward, right);
        break;
    }
    return rebuild;
}

} // namespace

Vector3 gravity_force(const RigidBody& body, const Vector3 gravity) noexcept {
    return {gravity.x, gravity.y * body.mass, gravity.z};
}

void add_gravity(RigidBody& body, const Vector3 gravity) noexcept {
    body.force = body.force + gravity_force(body, gravity);
}

void add_gravity_at_lever(RigidBody& body, const Vector3 gravity, const float lever) noexcept {
    const Vector3 weight = gravity_force(body, gravity);
    body.force = body.force + weight;
    const Vector3& origin = body.transform->translation;
    const Vector3 point = origin + body.transform->basis[1] * lever;
    body.torque = cross(point - origin, weight) + body.torque;
}

void stop_motion(RigidBody& body) noexcept {
    body.angular_velocity = {};
    body.angular_momentum = {};
    body.velocity = {};
    body.speed = 0.0F;
    body.heading = body.transform->basis[2];
    body.force = {};
    body.torque = {};
    body.impulse = {};
    body.angular_impulse = {};
    body.position_correction = {};
}

RigidBodyStep integrate(RigidBody& body, const float dt, const float spin_damping) noexcept {
    RigidBodyStep step;
    Transform& transform = *body.transform;

    // Forces and torques become impulses over the step.
    const Vector3 linear_impulse = body.impulse + body.force * dt;
    body.force = {};
    const Vector3 turning_impulse = body.angular_impulse + body.torque * dt;
    body.torque = {};

    // Linear: the impulse over the mass changes the velocity, which is capped upward.
    body.velocity = body.velocity + linear_impulse * (1.0F / body.mass);
    body.impulse = {};
    if (!(body.velocity.y <= kMaxUpwardSpeed)) {
        body.velocity.y = kMaxUpwardSpeed;
    }

    // Angular: momentum through the world inverse inertia, with the spin capped.
    body.angular_momentum = body.angular_momentum + turning_impulse;
    body.angular_impulse = {};
    body.angular_velocity = times_rows(body.angular_momentum, body.world_inverse_inertia);
    const float spin_squared = dot(body.angular_velocity, body.angular_velocity);
    if (!(spin_squared <= kMaxSpinSquared)) {
        const Vector3 direction = body.angular_velocity * reciprocal_sqrt(spin_squared);
        body.angular_velocity = direction * ((1.0F / std::sqrt(spin_squared)) * kMaxSpin);
        body.angular_momentum = body.angular_momentum * spin_damping;
        step.spin_limited = true;
    }

    // Move, and turn each basis row by the angular velocity.
    const Vector3 position = transform.translation + body.velocity * dt;
    const Vector3 turn = body.angular_velocity * dt;
    for (int row = 2; row >= 0; --row) {
        transform.basis[row] = transform.basis[row] - cross(transform.basis[row], turn);
    }

    step.rebuild = orthonormalize(transform.basis);

    transform.translation = position + body.position_correction;
    body.position_correction = {};

    // The inverse transform and the world inverse inertia (R * I^-1 * R^T).
    for (int row = 0; row < 3; ++row) {
        const Vector3 (&rows)[3] = transform.basis;
        const Vector3 column = row == 0   ? Vector3{rows[0].x, rows[1].x, rows[2].x}
                               : row == 1 ? Vector3{rows[0].y, rows[1].y, rows[2].y}
                                          : Vector3{rows[0].z, rows[1].z, rows[2].z};
        body.inverse_transform.basis[row] = column;
        body.world_inverse_inertia[row] =
            times_rows(times_rows(column, body.body_inverse_inertia), transform.basis);
    }
    const Vector3 (&inverse_rows)[3] = body.inverse_transform.basis;
    body.inverse_transform.translation = Vector3{} - (inverse_rows[0] * transform.translation.x +
                                                      inverse_rows[1] * transform.translation.y +
                                                      inverse_rows[2] * transform.translation.z);

    // Speed and heading.
    const float speed_squared = dot(body.velocity, body.velocity);
    if (speed_squared < kStillSpeedSquared) {
        body.speed = 0.0F;
        body.heading = transform.basis[2];
        step.still = true;
    } else {
        body.heading = body.velocity * reciprocal_sqrt(speed_squared);
        body.speed = std::sqrt(speed_squared);
    }
    return step;
}

} // namespace takedown::engine
