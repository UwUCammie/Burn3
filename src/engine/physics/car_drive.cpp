#include "takedown/engine/physics/car_drive.hpp"

#include "takedown/engine/math/trigonometry.hpp"

#include <cmath>

namespace takedown::engine {
namespace {

constexpr float kMilesPerHourPerMetrePerSecond = 2.2374146F;
constexpr float kMetresPerSecondPerMilePerHour = 0.44704F;
constexpr float kRpmToRadiansPerSecond = 9.54929637908935546875F;
constexpr float kDegreesToRadians = 1.745329238474369049072265625e-2F;
constexpr float kLandingDelay = 0.1F;        // seconds airborne before a landing can start a slide
constexpr float kWheelInertia = 25.0F;
constexpr float kHalfDriveShare = 0.5F;      // each rear wheel takes half the drive
constexpr float kAirborneWeightScale = 10.0F;
constexpr float kAirborneBaseWeight = 10.0F;
constexpr float kPressDown = -20.0F;         // extra weight factor of a car moving forward

// One airborne probe: a direction on the car, how hard it pushes against the
// motion along it, and which end of the car (near or far) it sits at.
struct Probe {
    Vector3 direction;
    float stiffness;
    bool far;
};

// RECOVERY: the compiler folded the original's table into the code, so these
// values are reconstructed from the generated instructions (vertical and
// sideways probes at each end of the car, stiffest vertically). The
// generated code matches the original apart from register choices.
constexpr Probe kProbes[4] = {
    {{0.0F, 1.0F, 0.0F}, 1000.0F, false},
    {{1.0F, 0.0F, 0.0F}, 10.0F, false},
    {{0.0F, 1.0F, 0.0F}, 1000.0F, true},
    {{1.0F, 0.0F, 0.0F}, 10.0F, true},
};

// `v` through three rows: v.x * row 0 + v.y * row 1 + v.z * row 2.
Vector3 times_rows(const Vector3 (&rows)[3], const Vector3 v) noexcept {
    return rows[0] * v.x + rows[1] * v.y + rows[2] * v.z;
}

// In the air the wheels only turn by whatever drive torque they were given.
void spin_free_wheels(std::array<CarDriveWheel, kMaxCarWheels>& wheels, const std::int32_t count,
                      const float dt) noexcept {
    for (std::int32_t index = 0; count > index && index != static_cast<std::int32_t>(kMaxCarWheels); ++index) {
        CarDriveWheel& wheel = wheels[static_cast<std::size_t>(index)];
        wheel.angular_speed = wheel.angular_speed + 0.0F + dt * (wheel.drive_torque / kWheelInertia);
        wheel.drive_torque = 0.0F;
    }
}

// Turns a wheel by the torque on it over the frame, and uses the stored drive torque up.
void spin_wheel(CarDriveWheel& wheel, const float torque, const float dt) noexcept {
    wheel.angular_speed = wheel.angular_speed + dt * (torque / kWheelInertia);
    wheel.drive_torque = 0.0F;
}

// The yaw torque of a slide: it holds the slide toward the side it started
// on, and recovers it once the car points back along its heading.
float slide_yaw(const CarDrive& car, const CarDriveTuning& tuning, const float heading_dot) noexcept {
    const float steering = car.steering;
    // The yaw a steered slide asks for.
    const auto steered = [&](const float scale) {
        return (1.0F - car.slide_blend * 0.75F) *
               (tuning.slide_steer_gain * (-steering * std::fabs(steering) * scale));
    };
    if (car.slide == kSlideLeft) {
        if (!(steering <= 0.05F)) {
            return steered(10000.0F);
        }
        if (heading_dot <= -tuning.slide_exit) {
            return -1000.0F;
        }
        if (!(steering < -0.05F)) {
            return tuning.slide_yaw_rate * -1000.0F;
        }
        return steered(5000.0F);
    }
    if (steering < -0.05F) {
        return steered(10000.0F);
    }
    if (!(heading_dot < tuning.slide_exit)) {
        return 1000.0F;
    }
    if (steering <= 0.05F) {
        return tuning.slide_yaw_rate * 1000.0F;
    }
    return steered(5000.0F);
}

// Airborne: four probes along the car damp its vertical and sideways motion,
// roll is damped, and a speed-dependent extra weight pulls it down.
void drive_airborne(CarDrive& car, const CarDriveTuning& tuning, const float dt,
                    const bool reversing, const float mph) noexcept {
    const Transform& transform = car.transform;
    const Vector3 position = transform.translation;
    for (const Probe& probe : kProbes) {
        float along = probe.far ? tuning.probe_far : tuning.probe_near;
        if (reversing) {
            along = 0.0F - along;
        }
        const Vector3 point = position + times_rows(transform.basis, Vector3{0.0F, 0.0F, along});
        const Vector3 direction = transform.basis[0] * probe.direction.x +
                                  transform.basis[1] * probe.direction.y +
                                  transform.basis[2] * probe.direction.z;
        const Vector3 arm = point - position;
        const Vector3 point_velocity = cross(car.angular_velocity, arm) + car.velocity;
        const Vector3 push = direction * (-dot(direction, point_velocity) * probe.stiffness);
        car.force = car.force + push;
        car.torque = car.torque + cross(arm, push);
    }

    // Roll is damped, and a speed-dependent extra weight pulls the car down.
    const Vector3 forward = transform.basis[2];
    car.angular_momentum = car.angular_momentum - forward * dot(forward, car.angular_momentum) * tuning.roll_damping;
    const float weight = mph * (tuning.airborne_weight * kAirborneWeightScale) / 100.0F + kAirborneBaseWeight;
    car.force.y = car.force.y + car.mass * -weight;
    car.wheels[2].drive_torque = car.wheels[2].drive_torque + car.drive_gain * kHalfDriveShare;
    car.wheels[3].drive_torque = car.drive_gain * kHalfDriveShare + car.wheels[3].drive_torque;
    spin_free_wheels(car.wheels, car.wheel_count, dt);
}

// The ground model: the engine or coasting force, the steered frame of the
// front wheels, per wheel the tyre forces (and a slide model while drifting), the
// end of a slide and the limit on the yaw the tyres add.
void drive_on_ground(CarDrive& car, const CarDriveTuning& tuning, const float dt, const float mph) noexcept {
    const Transform& transform = car.transform;
    const Vector3 right = transform.basis[0];
    const Vector3 up = transform.basis[1];
    const Vector3 forward = transform.basis[2];
    const Vector3 position = transform.translation;
    const bool left_down = car.wheels[0].contact || car.wheels[2].contact;
    const bool right_down = car.wheels[1].contact || car.wheels[3].contact;

    car.wheels[2].drive_torque = car.wheels[2].drive_torque + car.drive_gain * kHalfDriveShare;
    car.wheels[3].drive_torque = car.drive_gain * kHalfDriveShare + car.wheels[3].drive_torque;

    // The engine or coasting force, along the heading.
    Vector3 drive_point = position + up * tuning.drive_height;
    const bool sliding = car.slide == kSlideLeft || car.slide == kSlideRight;
    const float coast = (!sliding && car.throttle <= 0.1F) ? 1.0F : 0.0F;
    const float drive = tuning.coast_drag * coast + tuning.drive_torque * car.drive_torque_scale * -20000.0F;
    float push = drive * (car.speed + 1.0F) / 70.0F;
    if (sliding) {
        drive_point = position;
        if (car.slide_progress < 0.3F) {
            push = 0.0F;
        }
    }
    car.force = car.force + car.heading * push;
    car.torque = car.torque + cross(drive_point - position, car.heading * push);

    // The front wheels' frame, turned by the steering.
    const float angle = car.front_wheel_angle * kDegreesToRadians;
    const float sin_angle = wave_cosine(angle - kHalfPi);
    const float cos_angle = 1.0F - (1.0F - wave_cosine(angle));
    car.steered.basis[0] = right * cos_angle + up * 0.0F + forward * (0.0F - sin_angle);
    car.steered.basis[1] = right * 0.0F + up * 1.0F + forward * 0.0F;
    car.steered.basis[2] = right * sin_angle + up * 0.0F + forward * cos_angle;
    car.steered.translation = position + (right * 0.0F + up * 0.0F + forward * 0.0F);

    const Vector3 torque_before = car.torque;
    const float grip_mass = car.mass * 20.0F * 0.25F;
    Vector3 tyre_total{};
    for (std::size_t index = 0U; index != 4U; ++index) {
        CarDriveWheel& wheel = car.wheels[index];
        const Vector3 offset = times_rows(transform.basis, Vector3{wheel.mount_x, wheel.height, wheel.mount_z});
        const Vector3 point = position + offset;
        const Vector3 arm = point - position;
        const Vector3 point_velocity = cross(car.angular_velocity, arm) + car.velocity;
        const Vector3 side = index <= 1U ? car.steered.basis[0] : right;
        const Vector3 ahead = index <= 1U ? car.steered.basis[2] : forward;
        const float sideways_speed = dot(point_velocity, side);
        const float ahead_speed = dot(point_velocity, ahead);
        float rolling = wheel.angular_speed * wheel.radius;
        if (index <= 1U) {
            wheel.angular_speed = ahead_speed / wheel.radius;
        }
        const float slip = ahead_speed - rolling;
        float slip_speed = std::sqrt(sideways_speed * sideways_speed + slip * slip);
        if (rolling == 0.0F) {
            rolling = 1.0e-6F;
        }
        if (slip_speed == 0.0F) {
            slip_speed = 1.0e-7F;
        }

        float traction_grip;
        float cornering_grip;
        float grip_scale;
        if (index <= 1U) {
            cornering_grip = (1.0F - car.throttle) * 2.5F + 2.3F;
            traction_grip = 150000.0F;
            grip_scale = 60.0F;
        } else {
            float speed_grip = car.speed * 0.025F;
            speed_grip = speed_grip < 0.5F ? 0.5F : (speed_grip <= 1.0F ? speed_grip : 1.0F);
            traction_grip = speed_grip * 500000.0F;
            cornering_grip = speed_grip * ((1.0F - car.throttle) * 7.0F + 6.0F);
            grip_scale = 12.0F;
        }

        // Combined slip: the curve and its tangent below the saturation point.
        const float saturation = std::fabs(slip_speed * traction_grip / (grip_mass * grip_scale * rolling));
        const float curve = 0.4F - (saturation <= 1.0F ? saturation : 1.0F) * 0.8F;
        const float below_tangent = (0.3894183337688446F - static_cast<float>(std::sin(static_cast<double>(curve)))) +
                                    (curve - 0.4F) * 0.92106098F;
        const float load = traction_grip * (slip_speed * ((curve + 0.4F) / rolling));
        const float force = below_tangent * (grip_mass * 23.816387F * cornering_grip) + load * 0.5F;
        float traction = slip * -force / slip_speed;
        const float cornering = sideways_speed * -force / slip_speed;
        if (car.traffic) {
            if (traction <= 0.0F) {
                // Only traction that pushes the car along is scaled.
            } else {
                traction = traction * car.traffic_traction;
                if (car.settle_heavy_spin) {
                    car.angular_momentum = car.angular_momentum - up * dot(up, car.angular_momentum);
                }
            }
        }
        spin_wheel(wheel, wheel.drive_torque + wheel.radius * -traction, dt);

        const bool raised = index <= 1U ? (car.wheels[0].contact || car.wheels[1].contact) : wheel.contact;
        Vector3 grip_force;
        Vector3 lever{};
        if (car.slide == kSlideLeft || car.slide == kSlideRight) {
            const Vector3 side_force = side * cornering;
            float slide_force = (dot(car.heading, forward) * 6400.0F + 4200.0F) * (1.0F - car.slide_blend / 1.5F);
            if (cornering <= 0.0F) {
                slide_force = -slide_force;
            }
            if (car.slide_progress < 1.0F) {
                slide_force = car.slide_progress * slide_force;
            }
            grip_force = side * (car.slide_blend * slide_force) + side_force * (1.0F - car.slide_blend);
            if (raised && left_down && right_down) {
                lever = up * tuning.slide_grip_height;
            }
        } else {
            grip_force = side * cornering;
            if (raised) {
                lever = up * tuning.grip_height;
            }
        }
        car.force = car.force + grip_force;
        const Vector3 grip_arm = point + lever - position;
        car.torque = car.torque + cross(grip_arm, grip_force);
        tyre_total = grip_force + tyre_total;

        const Vector3 traction_point = position + up * tuning.traction_height;
        const Vector3 traction_force = ahead * traction;
        car.force = car.force + traction_force;
        if (!car.traffic) {
            car.torque = car.torque + cross(traction_point - position, traction_force);
        }
    }
    if (car.wheel_count > 4) {
        car.wheels[4].angular_speed = car.wheels[2].angular_speed;
        car.wheels[5].angular_speed = car.wheels[3].angular_speed;
    }

    // A slide ends once the car points back along its heading.
    const float heading_across = dot(car.heading, right);
    if (car.slide == kSlideLeft || car.slide == kSlideRight) {
        if (car.slide_progress > 0.5F) {
            if (car.slide == kSlideLeft) {
                if (!(heading_across < -0.01F) && !(car.steering < -0.005F)) {
                    car.slide = kSlideNone;
                }
            } else if (heading_across <= 0.01F && car.steering <= 0.005F) {
                car.slide = kSlideNone;
            }
        }
    }
    if ((car.slide == kSlideLeft || car.slide == kSlideRight) && car.throttle < 0.5F &&
        car.drive_torque_scale < 0.5F && car.steering == 0.0F && mph < 40.0F) {
        car.slide = kSlideNone;
    }

    // Yaw assist: the yaw the tyres added is limited, the rest is kept.
    const Vector3 added = car.torque - torque_before;
    float yaw = dot(up, added);
    const Vector3 flat = added - up * yaw;
    const float spin = dot(car.angular_momentum, up);
    if (car.slide == kSlideLeft || car.slide == kSlideRight) {
        yaw = slide_yaw(car, tuning, heading_across);
    }
    float limit = 800.0F;
    if (!(mph < 60.0F)) {
        if (!(mph < 110.0F)) {
            limit = tuning.yaw_assist_slow;
        } else {
            const float blend = (mph - 60.0F) / 50.0F;
            limit = tuning.yaw_assist_slow * (1.0F - blend) + blend * tuning.yaw_assist_fast;
        }
    }
    if (car.slide_progress < 0.25F) {
        limit = (car.slide_progress + 0.75F) * limit;
    }
    if (!(yaw <= 0.0F)) {
        if (!(spin <= limit)) {
            yaw = yaw * 0.1F;
        }
    }
    if (yaw < 0.0F) {
        if (spin < -limit) {
            yaw = yaw * 0.1F;
        }
    }
    car.torque = torque_before + (flat + up * yaw);

    // Tyres pushing against the heading while sliding: pull back along the heading plus a sideways kick.
    const float along = dot(tyre_total, car.heading);
    if (along < 0.0F && (car.slide == kSlideLeft || car.slide == kSlideRight) && !(car.throttle <= 0.1F)) {
        Vector3 sideways = cross(up, car.heading) * along;
        const Vector3 braking = car.force + car.heading * -along;
        if (heading_across < 0.0F) {
            sideways = -sideways;
        }
        car.force = braking + sideways;
    }
}

} // namespace

CarDriveResult drive_car(CarDrive& car, const CarDriveTuning& tuning, const float dt) noexcept {
    const Transform& transform = car.transform;
    const bool reversing = dot(transform.basis[2], car.velocity) < 0.0F;
    const float mph = car.speed * kMilesPerHourPerMetrePerSecond;
    const float heading_dot = dot(car.heading, transform.basis[0]);

    // Take-off and landing.
    car.was_airborne = car.airborne;
    const auto any_wheel_down = [&car] {
        return car.wheels[0].contact || car.wheels[1].contact || car.wheels[2].contact || car.wheels[3].contact;
    };
    if (!any_wheel_down()) {
        if (car.airborne == 0) {
            car.airtime = 0.0F;
        }
        car.airborne = 1;
        car.airtime = car.airtime + dt;
        car.lost_grip = 1U;
    } else {
        if (car.airborne == 1) {
            car.lost_grip = 0U;
            if (!(car.airtime <= kLandingDelay)) {
                // A landing at an angle starts a slide.
                if (!(tuning.slide_entry >= heading_dot)) {
                    if (car.slide == kSlideNone) {
                        car.slide_progress = 0.0F;
                        car.slide_timer = tuning.slide_timer_start;
                    }
                    car.slide = kSlideRight;
                } else if (-tuning.slide_entry > heading_dot) {
                    if (car.slide == kSlideNone) {
                        car.slide_progress = 0.0F;
                        car.slide_timer = tuning.slide_timer_start;
                    }
                    car.slide = kSlideLeft;
                }
            }
        }
        car.airborne = 0;
    }

    // Rolling and steering drag.
    float steer_drag = 0.0F;
    if (car.slide == kSlideNone && !car.no_steer_drag) {
        steer_drag = car.speed * (car.steering * car.steering) * tuning.steer_drag;
    }
    Vector3 drag{};
    const bool held_still = (car.scripted || car.traffic) &&
                            car.speed < tuning.drag_cut_speed * kMetresPerSecondPerMilePerHour &&
                            (car.hold == car.hold_other || car.wheel_rpm_source * kRpmToRadiansPerSecond < car.engine_rpm);
    if (!held_still) {
        drag = car.velocity * (-car.speed * tuning.rolling_drag - steer_drag);
    }
    car.force = car.force + drag;

    if (car.airborne != 0) {
        drive_airborne(car, tuning, dt, reversing, mph);
        return CarDriveResult::Airborne;
    }

    if (!car.traffic) {
        if (tuning.planar_speed > mph) {
            return CarDriveResult::PlanarTyres;
        }
        if (car.hold == 0xFFFFFFFFU) {
            return CarDriveResult::PlanarTyres;
        }
    }

    drive_on_ground(car, tuning, dt, mph);
    press_car_down(car);
    return CarDriveResult::Grounded;
}

void press_car_down(CarDrive& car) noexcept {
    if (car.speed <= 1.0F || car.transform.basis[2].y <= 0.0F) {
        return;
    }
    const Vector3 lightened = car.force - car.gravity * car.mass;
    car.force = lightened + car.transform.basis[1] * (car.mass * kPressDown);
}

} // namespace takedown::engine
