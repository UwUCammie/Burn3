#include "takedown/engine/physics/suspension.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <cmath>

namespace takedown::engine {
namespace {

using Step = SuspensionStep;

constexpr float kGroundJump = 0.12F;           // more than this in one frame is a jump
constexpr float kBottomedFraction = 0.25F;     // of the travel: the most a wheel is driven past
constexpr float kDroopFraction = 0.75F;        // of the travel: how far a wheel hangs below rest
constexpr float kSlowTimeScale = 1.2F;         // the damper rate's frame time when time is slowed
constexpr float kSlowSpeed = 1.0F;
constexpr float kMinimumCorrection = 0.001F;
constexpr float kFadeStart = 0.5F;
constexpr float kContactDragSpeed = 0.1F;
constexpr float kDragMassScale = -3.0F;
constexpr float kDragShare = 4.0F;
constexpr float kHardLanding = 2000.0F;
constexpr float kDegreesToRadians = 1.745329238474369049072265625e-2F;
constexpr float kTurnsPerRadian = 1.5915493667125701904296875e-1F;
constexpr float kTwoPi = 6.283185482025146484375F;
constexpr float kPi = 3.1415927410125732421875F;
constexpr float kHalfPi = 1.57079637050628662109375F;
constexpr float kSpinLimit = 628.31854248046875F;  // +-100 turns
constexpr float kSpinDamping = 0.99F;
constexpr float kGroundedSpinDamping = 0.8F;

// How the grounded wheels shared out their work, for the update to finish.
struct WheelForces {
    Vector3 correction_sum;
    int corrections{};
    int pushes{};
};

// A wheel's turn as its sine and cosine.
struct Spin {
    float sine;
    float cosine;
};

// sin(x) by its odd series, every step rounded toward zero.
float sine_series(const float x) noexcept {
    const float squared = chop_mul(x, x);
    float sum = chop_madd(-2.505075968883829773403704166412353515625e-8F, 1.5896910177115586293439264409244060516357421875e-10F, squared);
    sum = chop_madd(2.755731429715524427592754364013671875e-6F, squared, sum);
    sum = chop_madd(-1.98412701138295233249664306640625e-4F, squared, sum);
    sum = chop_madd(8.333333767950534820556640625e-3F, squared, sum);
    sum = chop_madd(-1.6666667163372039794921875e-1F, squared, sum);
    return chop_madd(chop_add(x, 0.0F), chop_mul(x, squared), sum);
}

// |cos| from the sine.
float cosine_size(const float sine) noexcept {
    return game_sqrt(maximum(0.0F, chop_sub(1.0F, chop_mul(sine, sine))));
}

// A point of the car's model on the car (x, then y, then z, then the
// position, each a multiply-add); the wheel's height is `offset`.
Vector3 wheel_point(const Transform& car, const Vector3 model_position, const float offset) noexcept {
    const Vector3 local{model_position.x, chop_add(0.0F, offset), model_position.z};
    Vector3 point = chop_mul(car.basis[0], local.x);
    point = chop_add(point, chop_mul(car.basis[1], local.y));
    point = chop_add(point, chop_mul(car.basis[2], local.z));
    return chop_add(point, car.translation);
}

// A vector through a matrix of rows.
Vector3 through_rows(const Vector3 (&rows)[3], const Vector3 v) noexcept {
    Vector3 r = chop_mul(rows[0], v.x);
    r = chop_add(r, chop_mul(rows[1], v.y));
    return chop_add(r, chop_mul(rows[2], v.z));
}

Vector3 lanes_from_zero(const Vector3 v) noexcept {
    return {chop_add(0.0F, v.x), chop_add(0.0F, v.y), chop_add(0.0F, v.z)};
}

// A wheel in the air: it drops toward its rest position.
void airborne_wheel(const SuspensionCar& car, SuspensionWheel& wheel, const SuspensionAxle& axle, const float dt,
                    SuspensionTrace& trace) {
    trace.pass(Step::Airborne);
    const float low = chop_mul(axle.travel, kDroopFraction);
    const float lowest = chop_sub(wheel.rest_offset, low);
    float fall = game_div(axle.travel, chop_sub(wheel.rest_offset, lowest));
    const float pull =
        chop_mul(chop_mul(chop_mul(axle.stiffness, minimum(maximum(chop_sub(1.0F, fall), 0.0F), 1.0F)), dt), dt);
    const float quarter = chop_mul(axle.travel, kBottomedFraction);
    wheel.offset = chop_sub(chop_add(lowest, 0.0F), chop_mul(pull, dt));
    const float drop = chop_sub(wheel.rest_offset, wheel.offset);
    if (game_less(drop, quarter)) {
        trace.pass(Step::AirborneLow);
        wheel.offset = chop_sub(wheel.rest_offset, quarter);
    } else {
        trace.pass(Step::AirborneCheck);
        if (!game_less_equal(drop, low)) {
            trace.pass(Step::AirborneHigh);
            wheel.offset = lowest;
        }
    }
    trace.pass(Step::AirbornePosition);
    wheel.position = wheel_point(*car.body->transform, wheel.model.translation, wheel.offset);
}

// The wheel's spin for the frame: it dies away on a wrecked car (and a car
// nobody races), advances with the time, stays within +-100 turns, and gives
// the sine and cosine of the angle within half a turn either side.
Spin advance_spin(const SuspensionCar& car, SuspensionWheel& wheel, const float dt, SuspensionTrace& trace) {
    trace.pass(Step::SpinLever);
    bool damp = true;
    if (!car.gravity_at_lever) {
        trace.pass(Step::SpinRacedCheck);
        damp = !car.raced;
    }
    if (damp) {
        trace.pass(Step::SpinDamp);
        wheel.spin_speed = chop_mul(wheel.spin_speed, kSpinDamping);
        if (wheel.grounded) {
            trace.pass(Step::SpinDampGrounded);
            wheel.spin_speed = chop_mul(wheel.spin_speed, kGroundedSpinDamping);
        }
    }
    trace.pass(Step::SpinAdvance);
    const float angle = chop_madd(chop_add(wheel.spin_angle, 0.0F), wheel.spin_speed, dt);
    wheel.spin_angle = angle;
    if (!game_less_equal(angle, kSpinLimit)) {
        trace.pass(Step::SpinWrapHigh);
        wheel.spin_angle = chop_sub(angle, kSpinLimit);
    } else {
        trace.pass(Step::SpinWrapLowCheck);
        if (game_less(angle, -kSpinLimit)) {
            trace.pass(Step::SpinWrapLow);
            wheel.spin_angle = chop_add(angle, kSpinLimit);
        }
    }

    trace.pass(Step::SpinReduce);
    const float turns = chop_from_int(truncate_word(chop_mul(wheel.spin_angle, kTurnsPerRadian)));
    float x = chop_sub(chop_add(wheel.spin_angle, 0.0F), chop_mul(turns, kTwoPi));
    if (!game_less_equal(x, kPi)) {
        trace.pass(Step::SpinReduceHigh);
        x = chop_sub(x, kTwoPi);
    }
    trace.pass(Step::SpinSine);
    Spin spin;
    spin.sine = sine_series(x);
    spin.cosine = cosine_size(spin.sine);
    trace.pass(Step::SpinCosineCheck);
    if (!game_less_equal(std::fabs(x), kHalfPi)) {
        trace.pass(Step::SpinCosineFlip);
        spin.cosine = chop_mul(spin.cosine, -1.0F);
    }
    return spin;
}

// The wheel model's frame: the spin (and, on the front pair, the steering)
// as rows, scaled to the wheel's size; the left and right wheels mirror.
void orient_wheel(SuspensionWheel& wheel, const std::size_t index, const Spin spin, const Spin steer,
                  SuspensionTrace& trace) {
    trace.pass(Step::SpinAxleCheck);
    Spin turn{0.0F, 1.0F};
    if (index <= 1U) {
        trace.pass(Step::SpinFront);
        turn = steer;
    } else {
        trace.pass(Step::SpinRear);
    }
    trace.pass(Step::SpinSideCheck);
    Vector3 scale{wheel.width, wheel.size, wheel.size};
    if ((index & 1U) != 0U) {
        trace.pass(Step::SpinRight);
        scale.z = -wheel.size;
    } else {
        trace.pass(Step::SpinLeft);
    }
    scale = lanes_from_zero(scale);

    trace.pass(Step::SpinRows);
    const float away = -turn.sine;
    Vector3 rows[3] = {
        lanes_from_zero({turn.cosine, 0.0F, turn.sine}),
        lanes_from_zero({chop_mul(away, spin.sine), spin.cosine, chop_mul(spin.sine, turn.cosine)}),
        lanes_from_zero({chop_mul(away, spin.cosine), -spin.sine, chop_mul(turn.cosine, spin.cosine)}),
    };
    const Vector3 position{wheel.model.translation.x, chop_add(0.0F, wheel.offset), wheel.model.translation.z};

    trace.pass(Step::SpinScale);
    const Vector3 axes[3] = {{scale.x, 0.0F, 0.0F}, {0.0F, scale.y, 0.0F}, {0.0F, 0.0F, scale.z}};
    for (int row = 0; row < 3; ++row) {
        wheel.model.basis[row] = through_rows(rows, axes[row]);
    }
    wheel.model.translation = chop_add(through_rows(rows, Vector3{}), position);
}

// The spring and damper of a wheel on the ground, the impulse it takes, and
// the contact drag of a wrecked car.
void grounded_wheel(SuspensionCar& car, SuspensionWheel& wheel, const SuspensionAxle& axle, const std::size_t index,
                    const float dt, WheelForces& forces, SuspensionTrace& trace) {
    RigidBody& body = *car.body;
    const Transform& transform = *body.transform;
    trace.pass(Step::Grounded);
    if (!game_less_equal(chop_sub(wheel.ground_offset, wheel.offset), kGroundJump)) {
        trace.pass(Step::GroundJump);
        wheel.ground_jumped = true;
    }

    trace.pass(Step::TravelCheck);
    const float quarter = chop_mul(axle.travel, kBottomedFraction);
    const float depth = chop_sub(wheel.rest_offset, wheel.ground_offset);
    float extension = depth;
    bool push = false;
    if (game_less(depth, quarter)) {
        // Driven past a quarter of the travel: held there, the car moved out
        // by the excess.
        trace.pass(Step::Bottomed);
        wheel.ground_offset = chop_sub(wheel.rest_offset, quarter);
        const float excess = chop_sub(quarter, depth);
        push = true;
        if (game_less_equal(body.speed, kSlowSpeed)) {
            trace.pass(Step::BottomedSlow);
            push = !car.gravity_at_lever;
        }
        if (push) {
            trace.pass(Step::BottomedPush);
        }
        trace.pass(Step::CorrectionCheck);
        if (!game_less_equal(excess, kMinimumCorrection)) {
            trace.pass(Step::Correction);
            forces.correction_sum = chop_add(forces.correction_sum, chop_mul(wheel.contact_normal, excess));
            ++forces.corrections;
        }
        extension = quarter;
    } else {
        trace.pass(Step::NotBottomed);
        if (car.gravity_at_lever) {
            trace.pass(Step::LeverClampCheck);
            const float limit = chop_mul(axle.travel, kDroopFraction);
            if (!game_less_equal(depth, limit)) {
                trace.pass(Step::LeverClamp);
                extension = limit;
            }
        }
    }

    trace.pass(Step::RateCheck);
    const float compression = chop_sub(extension, axle.travel);
    const float movement = chop_sub(wheel.ground_offset, wheel.offset);
    float rate;
    if (car.slowed_time) {
        trace.pass(Step::RateSlowed);
        rate = game_div(movement, chop_mul(dt, kSlowTimeScale));
    } else {
        trace.pass(Step::Rate);
        rate = game_div(movement, dt);
    }
    wheel.offset = wheel.ground_offset;

    trace.pass(Step::Spring);
    const float strength = chop_madd(chop_mul(-axle.stiffness, compression), axle.damping, rate);
    wheel.position = wheel_point(transform, wheel.model.translation, wheel.ground_offset);
    Vector3 force = chop_mul(wheel.contact_normal, strength);
    if (car.gravity_at_lever) {
        trace.pass(Step::LeverFadeCheck);
        const float reach = game_div(extension, chop_mul(axle.travel, kDroopFraction));
        if (!game_less_equal(reach, kFadeStart)) {
            trace.pass(Step::LeverFade);
            force = chop_mul(force, chop_mul(chop_sub(1.0F, reach), 2.0F));
        }
    }
    trace.pass(Step::SpringApplied);
    body.force = chop_add(body.force, force);
    body.torque = chop_add(body.torque, chop_cross(chop_sub(wheel.position, transform.translation), force));

    wheel.pushed = false;
    if (push) {
        // The impulse that stops the wheel's point moving into the ground,
        // along the car's up axis.
        trace.pass(Step::Push);
        const Vector3 up = transform.basis[1];
        const Vector3 lever = chop_sub(wheel.position, transform.translation);
        const Vector3 point_velocity = chop_add(chop_cross(body.angular_velocity, lever), body.velocity);
        const float approach = chop_mul(chop_dot(up, point_velocity), -1.0F);
        const Vector3 turn = through_rows(body.world_inverse_inertia, chop_cross(lever, up));
        const float impulse = game_div(approach, chop_add(chop_dot(chop_cross(turn, lever), up), game_div(1.0F, body.mass)));
        wheel.push = chop_mul(up, std::fabs(impulse));
        trace.pass(Step::PushCheck);
        if (!game_less_equal(impulse, 0.0F)) {
            wheel.pushed = true;
            ++forces.pushes;
            trace.pass(Step::PushLeverCheck);
            if (car.gravity_at_lever) {
                trace.pass(Step::PushStateCheck);
                if (car.player) {
                    trace.pass(Step::PushAxleCheck);
                    if (index <= 1U) {
                        trace.pass(Step::PushStrengthCheck);
                        if (!game_less_equal(impulse, kHardLanding)) {
                            ++trace.hard_landings;
                        }
                    }
                }
            }
        }
    }

    // A wrecked car also drags against the motion of the contact point.
    trace.pass(Step::DragStart);
    if (!car.gravity_at_lever) {
        trace.pass(Step::WheelDone);
        return;
    }
    trace.pass(Step::DragFlagCheck);
    if (car.contact_drag_off) {
        trace.pass(Step::WheelDone);
        return;
    }
    trace.pass(Step::DragSpeedCheck);
    wheel.dragged = true;
    if (game_less_equal(body.speed, kContactDragSpeed)) {
        trace.pass(Step::WheelDone);
        return;
    }
    trace.pass(Step::Drag);
    const Vector3 lever = chop_sub(wheel.contact_point, transform.translation);
    const Vector3 point_velocity = chop_add(chop_cross(body.angular_velocity, lever), body.velocity);
    const Vector3 direction = chop_normalized(point_velocity);
    const float slip = std::fabs(chop_dot(direction, transform.basis[0]));
    const float load =
        chop_mul(chop_from_int(car.wheel_count), chop_mul(chop_add(slip, 1.0F), chop_mul(body.mass, kDragMassScale)));
    Vector3 drag = chop_mul(direction, game_div(load, kDragShare));
    if (!car.raced) {
        trace.pass(Step::DragHalfCheck);
        if (!car.full_drag_unraced) {
            trace.pass(Step::DragHalf);
            drag = chop_mul(drag, 0.5F);
        }
    }
    trace.pass(Step::DragApplied);
    body.force = chop_add(body.force, drag);
    body.torque = chop_add(body.torque, chop_cross(lever, drag));
    trace.pass(Step::WheelDone);
}

} // namespace

bool update_suspension(SuspensionCar& car, const float dt, SuspensionTrace& trace) {
    const auto count = static_cast<std::size_t>(car.wheel_count > 0 ? car.wheel_count : 0);
    for (std::size_t index = 0U; index < kSuspensionWheels; ++index) {
        if (index < count && car.wheels[index].mount == WheelMount::Tilted) {
            return false;
        }
        if (index >= count && index < kSuspensionSharingWheels && car.wheels[index].pushed) {
            return false;
        }
    }
    trace.pass(Step::Start);
    if (count == 0U) {
        trace.pass(Step::NoWheels);
        return true;
    }
    trace.pass(Step::Setup);
    RigidBody& body = *car.body;
    const Transform& transform = *body.transform;
    WheelForces forces;

    for (std::size_t index = 0U; index < count; ++index) {
        trace.pass(Step::WheelCheck);
        trace.pass(Step::WheelStart);
        SuspensionWheel& wheel = car.wheels[index];
        wheel.ground_jumped = false;
        wheel.pushed = false;
        wheel.dragged = false;
        if (wheel.mount == WheelMount::Missing) {
            trace.pass(Step::WheelDone);
            continue;
        }
        trace.pass(Step::WheelPresent);
        trace.pass(index <= 1U ? Step::FrontAxle : Step::RearAxle);
        const SuspensionAxle& axle = index <= 1U ? car.front : car.rear;
        trace.pass(Step::ContactCheck);
        if (wheel.grounded) {
            grounded_wheel(car, wheel, axle, index, dt, forces, trace);
        } else {
            airborne_wheel(car, wheel, axle, dt, trace);
            trace.pass(Step::WheelDone);
        }
    }
    trace.pass(Step::WheelCheck);

    // The corrections are shared out between the wheels that took them.
    trace.pass(Step::CorrectionCountCheck);
    if (forces.corrections > 0) {
        trace.pass(Step::CorrectionShared);
        body.position_correction = chop_add(
            body.position_correction,
            chop_mul(forces.correction_sum, game_div(1.0F, chop_from_int(forces.corrections))));
    }

    // The impulses too, after the part along up is taken out of the body's.
    trace.pass(Step::PushCountCheck);
    if (forces.pushes > 0) {
        trace.pass(Step::PushProjected);
        body.impulse = chop_sub(body.impulse, chop_mul(transform.basis[1], chop_dot(body.impulse, transform.basis[1])));
    }
    trace.pass(Step::PushShareCheck);
    if (forces.pushes > 0) {
        trace.pass(Step::PushShare);
        const float share = game_div(1.0F, chop_from_int(forces.pushes));
        for (std::size_t index = 0U; index < kSuspensionSharingWheels; ++index) {
            trace.pass(Step::PushWheelCheck);
            SuspensionWheel& wheel = car.wheels[index];
            if (wheel.pushed) {
                trace.pass(Step::PushWheel);
                wheel.push = chop_mul(wheel.push, share);
                body.impulse = chop_add(body.impulse, wheel.push);
                body.angular_impulse =
                    chop_add(body.angular_impulse, chop_cross(chop_sub(wheel.position, transform.translation), wheel.push));
            }
            trace.pass(Step::PushWheelNext);
        }
    }

    // Each wheel's model turns with its spin, the front pair with the steering.
    trace.pass(Step::Steering);
    const float steering = chop_mul(-car.steering_degrees, kDegreesToRadians);
    Spin steer{};
    steer.sine = sine_series(steering);
    steer.cosine = cosine_size(steer.sine);
    for (std::size_t index = 0U; index < count; ++index) {
        trace.pass(Step::SpinCheck);
        trace.pass(Step::SpinStart);
        SuspensionWheel& wheel = car.wheels[index];
        if (wheel.mount == WheelMount::Missing) {
            trace.pass(Step::SpinMissing);
            continue;
        }
        const Spin spin = advance_spin(car, wheel, dt, trace);

        orient_wheel(wheel, index, spin, steer, trace);
    }
    trace.pass(Step::SpinCheck);
    trace.pass(Step::Finish);
    return true;
}

} // namespace takedown::engine
