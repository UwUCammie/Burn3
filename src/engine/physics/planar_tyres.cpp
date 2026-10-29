#include "takedown/engine/physics/planar_tyres.hpp"

#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/trigonometry.hpp"

#include <cmath>

namespace takedown::engine {
namespace {

constexpr int kSubSteps = 4;
constexpr float kMassCap = 2000.0F;              // the tyres' grip stops growing with mass here
constexpr float kDegreesToRadians = 1.745329238474369049072265625e-2F;
constexpr float kRadiansToDegrees = 5.7295780181884765625e+1F;
constexpr float kRpmToRadiansPerSecond = 9.54929637908935546875F;
constexpr float kRearSaturationAngle = 3.0F;     // degrees of slip
constexpr float kFrontSaturationAngle = 8.0F;
constexpr float kRearGrip = 16.0F;
constexpr float kFrontGrip = 12.0F;
constexpr float kForwardDriveTorque = 6000.0F;
constexpr float kReverseDriveTorque = 5000.0F;
constexpr float kWheelInertia = 25.0F;
constexpr float kNoSlip = 1.0e-12F;              // stands in for a zero slip speed
constexpr float kMinimumSkidSpeed = 3.0517578125e-5F;  // 2^-15
constexpr float kSkidRatio = 0.5F;

// `v` times a 3x3 matrix given as rows.
Vector3 times_rows(const Vector3 v, const Vector3 (&rows)[3]) noexcept {
    return rows[0] * v.x + rows[1] * v.y + rows[2] * v.z;
}

} // namespace

PlanarTyreStep step_planar_tyres(const PlanarTyreCar& car, const float dt) noexcept {
    const Transform& transform = *car.transform;
    const Vector3 right = transform.basis[0];
    const Vector3 up = transform.basis[1];
    const Vector3 forward = transform.basis[2];
    PlanarTyreStep result;

    const float sub_step = dt * 0.25F;
    const float inverse_mass = 1.0F / car.mass;
    result.front_wheel_angle = -car.steering_lock * car.steering;
    const float grip_mass = minimum(car.mass, kMassCap);

    // The car's velocity in its own frame, and its yaw rate.
    const Vector3 local = times_rows(car.velocity, car.inverse_transform->basis);
    float sideways = local.x;
    float forwards = local.z;
    const float yaw_start = dot(car.angular_velocity, up);
    float yaw_rate = yaw_start;

    const float drive = car.throttle * car.drive_gain;
    float drag = 0.0F;
    if (car.throttle < 0.1F && !(car.speed <= 1.0F)) {
        drag = (car.drag_coefficient * 0.5F) * (car.drag_rpm_scale * kRpmToRadiansPerSecond) / car.drag_area;
    }

    float heading = 0.0F;
    float wheel_spin = car.wheel_spin;
    float rear_slip = 0.0F;
    for (int step = 0; step != kSubSteps; ++step) {
        const float front_angle = result.front_wheel_angle * kDegreesToRadians + heading;
        const float rear_sin = wave_cosine(heading - kHalfPi);
        const float rear_cos = wave_cosine(heading);
        const float front_sin = wave_cosine(front_angle - kHalfPi);
        const float front_cos = wave_cosine(front_angle);

        // Each axle's speed: the yaw adds to the front's and takes from the rear's.
        const float yaw_cos = yaw_rate * rear_cos;
        const float yaw_sin = -rear_sin * yaw_rate;
        const float front_sideways = sideways + yaw_cos;
        const float rear_sideways = sideways - yaw_cos;
        const float front_forwards = forwards + yaw_sin;
        const float rear_forwards = forwards - yaw_sin;

        // The driven wheels spin up or down.
        const float engine = wheel_spin < 0.0F
                                 ? car.drive_torque_scale * kForwardDriveTorque * car.drive_torque + drive - drag
                                 : drive - car.drive_torque_scale * kReverseDriveTorque * car.drive_torque + drag;
        wheel_spin = engine / kWheelInertia * sub_step + wheel_spin;

        // Rear axle.
        const float rear_slip_x = -rear_sin * rear_forwards + rear_cos * rear_sideways;
        const float rolling = car.wheel_radius * wheel_spin;
        const float rear_slip_z = (rear_cos * rear_forwards + rear_sin * rear_sideways) - rolling;
        rear_slip = std::sqrt(rear_slip_z * rear_slip_z + rear_slip_x * rear_slip_x);
        if (rear_slip == 0.0F) {
            rear_slip = kNoSlip;
        }
        const float rear_saturated = minimum(
            std::fabs(static_cast<float>(std::atan2(static_cast<double>(rear_slip), static_cast<double>(rolling))) *
                      kRadiansToDegrees),
            kRearSaturationAngle);
        const float rear_grip = grip_mass * kRearGrip * (rear_saturated / kRearSaturationAngle);
        const float rear_force_x = -rear_slip_x * rear_grip / rear_slip;
        const float rear_force_z = -rear_slip_z * rear_grip / rear_slip;
        float force_x = 0.0F + rear_cos * rear_force_x + rear_sin * rear_force_z;
        float force_z = 0.0F + -rear_sin * rear_force_x + rear_cos * rear_force_z;
        wheel_spin = wheel_spin - car.wheel_radius * rear_force_z / kWheelInertia * sub_step;
        float torque = 0.0F - rear_force_x;

        // Front axle, turned by the steering.
        const float front_slip_x = -front_sin * front_forwards + front_cos * front_sideways;
        const float front_rolling = front_cos * front_forwards + front_sin * front_sideways;
        const float front_saturated = minimum(
            std::fabs(static_cast<float>(std::atan2(static_cast<double>(front_slip_x),
                                                    static_cast<double>(front_rolling))) *
                      kRadiansToDegrees),
            kFrontSaturationAngle);
        float front_grip = grip_mass * kFrontGrip * (front_saturated / kFrontSaturationAngle);
        if (!(front_slip_x <= 0.0F)) {
            front_grip = -front_grip;
        }
        force_x = front_grip * front_cos + force_x;
        force_z = -front_sin * front_grip + force_z;
        torque = static_cast<float>(std::cos(static_cast<double>(result.front_wheel_angle * kDegreesToRadians))) *
                     front_grip +
                 torque;

        // Integrate.
        sideways = inverse_mass * force_x * sub_step + sideways;
        forwards = inverse_mass * force_z * sub_step + forwards;
        yaw_rate = car.yaw_inverse_inertia * torque * sub_step + yaw_rate;
        heading = sub_step * yaw_rate + heading;
    }

    result.wheel_spin = wheel_spin;
    if (std::fabs(forwards) >= kMinimumSkidSpeed && !(std::fabs(rear_slip / forwards) <= kSkidRatio)) {
        result.skidding = true;
    }

    // The change over the step as a force and a torque on the body.
    const float impulse_scale = car.mass / dt;
    const Vector3 change =
        right * ((sideways - local.x) * impulse_scale) + forward * ((forwards - local.z) * impulse_scale);
    result.force = car.force + change;

    // Acting half the centre of mass height below the body's origin.
    const Vector3 offset = up * (-car.centre_of_mass_height * 0.5F);
    const Vector3 point = transform.translation - offset;
    const Vector3 lever = point - transform.translation;
    const float yaw_scale = car.yaw_inverse_inertia * dt;
    result.torque = car.torque + cross(lever, change);
    result.torque = up * ((yaw_rate - yaw_start) / yaw_scale) + result.torque;
    const float damping = car.yaw_damping * (-yaw_rate * (car.yaw_damping_steer_relief - std::fabs(car.steering))) /
                          yaw_scale;
    result.torque = up * damping + result.torque;
    return result;
}

} // namespace takedown::engine
