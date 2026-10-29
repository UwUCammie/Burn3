// Adapters for rigid-body physics (cars and loose objects).

#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/physics/car_drive.hpp"
#include "takedown/engine/physics/gearbox.hpp"
#include "takedown/engine/physics/mesh_probe.hpp"
#include "takedown/engine/physics/planar_tyres.hpp"
#include "takedown/engine/physics/rigid_body.hpp"
#include "takedown/engine/physics/suspension.hpp"
#include "takedown/engine/physics/wheel_contact.hpp"
#include "takedown/engine/text/packed_name.hpp"
#include "takedown/runtime/game_car_motion.hpp"
#include "takedown/runtime/game_segment_test.hpp"
#include "takedown/runtime/guest_call.hpp"
#include "takedown/runtime/guest_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace takedown::runtime {
namespace {

using guest::GuestAddress;

// The game's rigid body. Vectors are 16 bytes (x, y, z, then a fourth lane
// the game mostly ignores; the adapter reproduces what the original leaves
// there).
struct BodyLayout {
    static constexpr GuestAddress kBodyInverseInertia = 16U;   // 3 rows
    static constexpr GuestAddress kWorldInverseInertia = 64U;  // 3 rows
    static constexpr GuestAddress kInverseTransform = 112U;    // 3 rows, translation
    static constexpr GuestAddress kVelocity = 176U;            // fourth lane: speed
    static constexpr GuestAddress kHeading = 192U;
    static constexpr GuestAddress kAngularVelocity = 208U;
    static constexpr GuestAddress kAngularMomentum = 224U;
    static constexpr GuestAddress kForce = 240U;
    static constexpr GuestAddress kTorque = 256U;
    static constexpr GuestAddress kImpulse = 272U;
    static constexpr GuestAddress kAngularImpulse = 288U;
    static constexpr GuestAddress kPositionCorrection = 304U;
    static constexpr GuestAddress kContactTimerA = 404U;       // u32, cleared on stop
    static constexpr GuestAddress kContactTimerB = 408U;       // u32, cleared on stop
    static constexpr GuestAddress kMass = 496U;
    static constexpr GuestAddress kGravityLever = 500U;        // along the up axis
    static constexpr GuestAddress kTransform = 516U;           // pointer
    static constexpr GuestAddress kStopRequested = 526U;       // u8
    static constexpr GuestAddress kGravityAtLever = 528U;      // u8
    static constexpr GuestAddress kKeepMoving = 529U;          // u8, cancels a stop
    static constexpr GuestAddress kContactFlagA = 530U;        // u8, cleared on stop
    static constexpr GuestAddress kContactFlagB = 531U;        // u8, cleared on stop
    static constexpr GuestAddress kState = 533U;               // s8
};
// Bodies in this state take gravity at the lever.
constexpr std::int8_t kStateTumbling = 6;

// Game globals: the gravity vector and the angular momentum kept when the
// spin limit cuts in.
constexpr GuestAddress kGravity = 0x004c9cc0U;
constexpr GuestAddress kSpinDamping = 0x004e06a4U;

// Fourth lanes nothing can rely on. The original stores whatever a vector
// register held in those of the force and torque accumulators, and it moves
// the transform rows' lanes between rows by which rows it rebuilds, at times
// filling one from a stale stack word (0x65656565 in captures). Which rows
// are rebuilt depends on near-ties between tiny dot products, so the native
// rounding can pick the other, equally valid rebuild.
constexpr auto lane(const GuestAddress vector) { return static_cast<std::uint16_t>(vector + 12U); }
constexpr std::int16_t kThroughTransform = BodyLayout::kTransform;
constexpr std::array kUnusedWords{
    UnusedWord{-1, lane(BodyLayout::kForce)},
    UnusedWord{-1, lane(BodyLayout::kTorque)},
    UnusedWord{-1, lane(BodyLayout::kInverseTransform)},
    UnusedWord{-1, lane(BodyLayout::kInverseTransform + 16U)},
    UnusedWord{-1, lane(BodyLayout::kInverseTransform + 32U)},
    UnusedWord{-1, lane(BodyLayout::kInverseTransform + 48U)},
    UnusedWord{-1, lane(BodyLayout::kPositionCorrection)},
    UnusedWord{kThroughTransform, lane(0U)},
    UnusedWord{kThroughTransform, lane(16U)},
    UnusedWord{kThroughTransform, lane(32U)},
};
// The original computes the gravity lever arm as (up * lever + position) -
// position, which at track coordinates keeps about four significant digits
// and depends on the rounding mode (the PS2 vector unit rounds toward zero,
// the native code to nearest): the resulting torque differs by up to 2e-4.
constexpr double kCheckTolerance = 1e-3;

engine::Vector3 load3(const guest::EeMemory& memory, const GuestAddress address) {
    return load_vector4(memory, address).xyz();
}

void store3(guest::EeMemory& memory, const GuestAddress address, const engine::Vector3 value) {
    store_float(memory, address, value.x);
    store_float(memory, address + 4U, value.y);
    store_float(memory, address + 8U, value.z);
}

void store3w(guest::EeMemory& memory, const GuestAddress address, const engine::Vector3 value, const float w) {
    store_vector4(memory, address, {value.x, value.y, value.z, w});
}

float lane_w(const guest::EeMemory& memory, const GuestAddress address) {
    return load_float(memory, address + 12U);
}

// IntegrateRigidBody (original 0x001ac470): applies gravity, handles a
// requested stop, then advances the body at a0 by f12 seconds.
NativeFunctionExecution integrate_rigid_body(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress body_address = call.arg(0);
    const float dt = call.float_arg(0);
    const GuestAddress transform_address = memory.read_u32_le(body_address + BodyLayout::kTransform);
    const auto at = [&](const GuestAddress offset) { return body_address + offset; };

    engine::Transform transform;
    float row_w[3];
    for (int row = 0; row < 3; ++row) {
        const auto address = transform_address + static_cast<GuestAddress>(row * 16);
        transform.basis[row] = load3(memory, address);
        row_w[row] = lane_w(memory, address);
    }
    transform.translation = load3(memory, transform_address + 48U);

    engine::RigidBody body;
    body.transform = &transform;
    for (int row = 0; row < 3; ++row) {
        const auto offset = static_cast<GuestAddress>(row * 16);
        body.body_inverse_inertia[row] = load3(memory, at(BodyLayout::kBodyInverseInertia + offset));
        body.world_inverse_inertia[row] = load3(memory, at(BodyLayout::kWorldInverseInertia + offset));
    }
    body.velocity = load3(memory, at(BodyLayout::kVelocity));
    body.angular_momentum = load3(memory, at(BodyLayout::kAngularMomentum));
    body.force = load3(memory, at(BodyLayout::kForce));
    body.torque = load3(memory, at(BodyLayout::kTorque));
    body.impulse = load3(memory, at(BodyLayout::kImpulse));
    body.angular_impulse = load3(memory, at(BodyLayout::kAngularImpulse));
    body.position_correction = load3(memory, at(BodyLayout::kPositionCorrection));
    body.mass = load_float(memory, at(BodyLayout::kMass));

    const engine::Vector4 gravity = load_vector4(memory, kGravity);
    float velocity_w = lane_w(memory, at(BodyLayout::kVelocity));
    float angular_momentum_w = lane_w(memory, at(BodyLayout::kAngularMomentum));
    float impulse_w = lane_w(memory, at(BodyLayout::kImpulse));
    float stopped_impulse_w;
    const float angular_velocity_w = lane_w(memory, at(BodyLayout::kWorldInverseInertia + 32U));
    const float inertia_w = lane_w(memory, at(BodyLayout::kBodyInverseInertia + 32U));

    if (memory.read_u8(at(BodyLayout::kKeepMoving)) != 0U) {
        memory.write_u8(at(BodyLayout::kStopRequested), 0U);
    }
    const bool at_lever = memory.read_u8(at(BodyLayout::kGravityAtLever)) != 0U ||
                          static_cast<std::int8_t>(memory.read_u8(at(BodyLayout::kState))) == kStateTumbling;
    // A stop copies these lanes from whichever vector the original's
    // registers last held.
    float stop_w;
    if (at_lever) {
        stopped_impulse_w = lane_w(memory, at(BodyLayout::kTorque));
        stop_w = lane_w(memory, transform_address + 48U);
        engine::add_gravity_at_lever(body, gravity.xyz(), load_float(memory, at(BodyLayout::kGravityLever)));
    } else {
        stopped_impulse_w = lane_w(memory, at(BodyLayout::kForce));
        stop_w = gravity.w;
        engine::add_gravity(body, gravity.xyz());
    }
    if (memory.read_u8(at(BodyLayout::kStopRequested)) != 0U) {
        engine::stop_motion(body);
        velocity_w = 0.0F;
        angular_momentum_w = stop_w;
        impulse_w = stopped_impulse_w;
        memory.write_u32_le(at(BodyLayout::kContactTimerA), 0U);
        memory.write_u32_le(at(BodyLayout::kContactTimerB), 0U);
        memory.write_u8(at(BodyLayout::kContactFlagA), 0U);
        memory.write_u8(at(BodyLayout::kContactFlagB), 0U);
    }

    const auto step = engine::integrate(body, dt, load_float(memory, kSpinDamping));

    // A rebuilt row takes the fourth lane of the second row of its cross
    // product; the row normalised last also lends it to the cleared
    // position correction.
    float last_w = 0.0F;
    switch (step.rebuild) {
    case engine::BasisRebuild::RightThenForward:
        row_w[0] = row_w[2];
        row_w[2] = row_w[1];
        last_w = row_w[2];
        break;
    case engine::BasisRebuild::UpThenRight:
        row_w[1] = row_w[0];
        row_w[0] = row_w[2];
        last_w = row_w[0];
        break;
    case engine::BasisRebuild::ForwardThenUp:
    case engine::BasisRebuild::ForwardThenUpUnordered:  // the original: a stale stack word
        row_w[2] = row_w[1];
        row_w[1] = row_w[0];
        last_w = row_w[1];
        break;
    }
    for (int row = 0; row < 3; ++row) {
        store3w(memory, transform_address + static_cast<GuestAddress>(row * 16), transform.basis[row], row_w[row]);
    }
    store3(memory, transform_address + 48U, transform.translation);

    for (int row = 0; row < 3; ++row) {
        const auto offset = static_cast<GuestAddress>(row * 16);
        store3w(memory, at(BodyLayout::kWorldInverseInertia + offset), body.world_inverse_inertia[row], inertia_w);
        store3w(memory, at(BodyLayout::kInverseTransform + offset), body.inverse_transform.basis[row], row_w[row]);
    }
    store3w(memory, at(BodyLayout::kInverseTransform + 48U), body.inverse_transform.translation, row_w[1]);
    store3w(memory, at(BodyLayout::kVelocity), body.velocity, body.speed);
    store3w(memory, at(BodyLayout::kHeading), body.heading, step.still ? row_w[2] : velocity_w);
    store3w(memory, at(BodyLayout::kAngularVelocity), body.angular_velocity, angular_velocity_w);
    store3w(memory, at(BodyLayout::kAngularMomentum), body.angular_momentum, angular_momentum_w);
    store3(memory, at(BodyLayout::kForce), body.force);
    store3(memory, at(BodyLayout::kTorque), body.torque);
    store3w(memory, at(BodyLayout::kImpulse), body.impulse, impulse_w);
    store3w(memory, at(BodyLayout::kAngularImpulse), body.angular_impulse, velocity_w);
    store3w(memory, at(BodyLayout::kPositionCorrection), body.position_correction, last_w);
    return call.finish(920U);
}

// The car record around its rigid body (the body is the start of it).
struct CarLayout {
    static constexpr GuestAddress kBodyInverseInertiaUp = 36U;  // the up axis entry of row 1
    static constexpr GuestAddress kProbeExtentNear = 464U;      // airborne probes: z / 2
    static constexpr GuestAddress kProbeExtentFar = 480U;       // and 2 z
    static constexpr GuestAddress kWheelSpinCopy = 2556U;
    static constexpr GuestAddress kWheelRadius = 2736U;
    static constexpr GuestAddress kWheelSpin = 2748U;
    static constexpr GuestAddress kWheelSpinCopies[2] = {2940U, 3132U};  // with five or more wheels
    static constexpr GuestAddress kWheelObjects = 3272U;  // pointers; the wheel's local position at +48
    static constexpr GuestAddress kFrontWheelAngle = 4452U;
    static constexpr GuestAddress kAirborne = 4456U;      // u8
    static constexpr GuestAddress kWheelCount = 4457U;    // s8
    static constexpr GuestAddress kRollingDrag = 4960U;
    static constexpr GuestAddress kAirborneWeight = 4964U;
    static constexpr GuestAddress kDriveHeight = 4968U;
    static constexpr GuestAddress kTractionHeight = 4972U;
    static constexpr GuestAddress kCentreOfMassHeight = 4976U;
    static constexpr GuestAddress kSlideGripHeight = 4980U;
    static constexpr GuestAddress kDriveTorque = 5004U;
    static constexpr GuestAddress kSlideTimerStart = 5008U;
    static constexpr GuestAddress kYawAssistFast = 5016U;
    static constexpr GuestAddress kYawAssistSlow = 5020U;
    static constexpr GuestAddress kSlideSteerGain = 5028U;
    static constexpr GuestAddress kPlanarSpeed = 5036U;
    static constexpr GuestAddress kSteeringLock = 5040U;
    static constexpr GuestAddress kYawDampingSteerRelief = 5044U;
    static constexpr GuestAddress kYawDamping = 5048U;
    static constexpr GuestAddress kSteerDrag = 5056U;
    static constexpr GuestAddress kDragCoefficient = 5068U;  // also the coasting force
    static constexpr GuestAddress kSlideExit = 5072U;
    static constexpr GuestAddress kDragCutSpeed = 5076U;
    static constexpr GuestAddress kRollDamping = 5080U;
    static constexpr GuestAddress kSlideEntry = 5084U;
    static constexpr GuestAddress kDefinition = 5108U;    // pointer
    static constexpr GuestAddress kThrottle = 5120U;
    static constexpr GuestAddress kDriveTorqueScale = 5124U;
    static constexpr GuestAddress kSteering = 5128U;
    static constexpr GuestAddress kSlideYawRate = 5140U;
    static constexpr GuestAddress kSlideProgress = 5164U;
    static constexpr GuestAddress kSlideTimer = 5168U;
    static constexpr GuestAddress kAirtime = 5180U;
    static constexpr GuestAddress kSlideBlend = 5184U;
    static constexpr GuestAddress kScripted = 5188U;      // u8
    static constexpr GuestAddress kWasAirborne = 5189U;   // u8
    static constexpr GuestAddress kTraffic = 5190U;       // u8
    static constexpr GuestAddress kEngineRpm = 5232U;
    static constexpr GuestAddress kDragArea = 5248U;
    static constexpr GuestAddress kDragRpmScale = 5276U;
    static constexpr GuestAddress kSkidding = 5288U;      // u32, also set while airborne
    static constexpr GuestAddress kHeld = 5320U;          // u32: no low-speed settling
    static constexpr GuestAddress kHeldOther = 5324U;     // u32
    static constexpr GuestAddress kSteeredFrame = 5344U;  // 4 rows
    static constexpr GuestAddress kDriveGain = 5408U;
    static constexpr GuestAddress kSlide = 5412U;         // u32 (engine::kSlide*), cleared by the tyre step
    static constexpr GuestAddress kNoSteerDrag = 5437U;   // u8
};
// Per-wheel records, 192 bytes apart.
struct CarWheelLayout {
    static constexpr GuestAddress kStride = 192U;
    static constexpr GuestAddress kRadius = 2160U;
    static constexpr GuestAddress kDriveTorque = 2164U;
    static constexpr GuestAddress kAngularSpeed = 2172U;
    static constexpr GuestAddress kHeight = 2196U;
    static constexpr GuestAddress kContact = 2259U;       // u8
};
// The car definition (CarLayout::kDefinition).
struct CarDefinitionLayout {
    static constexpr GuestAddress kSpinSettleStart = 4928U;
    static constexpr GuestAddress kPackedName = 6496U;    // u64
};
constexpr GuestAddress kSettleSpeed = 0x004e0e00U;    // 0.2 in the ELF
constexpr GuestAddress kSettleDamping = 0x004e0e14U;  // 0.9 in the ELF

engine::Transform load_transform(const guest::EeMemory& memory, const GuestAddress address) {
    engine::Transform transform;
    for (int row = 0; row < 3; ++row) {
        transform.basis[row] = load3(memory, address + static_cast<GuestAddress>(row * 16));
    }
    transform.translation = load3(memory, address + 48U);
    return transform;
}

// The planar tyre model of the car at car_address over dt seconds, then its
// first two wheels' angular speeds from their contact points and, nearly
// stopped, a settling damping.
void run_planar_tyres(guest::EeMemory& memory, const GuestAddress car_address, const float dt) {
    const auto at = [&](const GuestAddress offset) { return car_address + offset; };
    const auto field = [&](const GuestAddress offset) { return load_float(memory, at(offset)); };

    const GuestAddress transform_address = memory.read_u32_le(at(BodyLayout::kTransform));
    const engine::Transform transform = load_transform(memory, transform_address);
    const engine::Transform inverse = load_transform(memory, at(BodyLayout::kInverseTransform));
    const engine::Vector4 velocity = load_vector4(memory, at(BodyLayout::kVelocity));
    const engine::Vector4 angular_velocity = load_vector4(memory, at(BodyLayout::kAngularVelocity));

    engine::PlanarTyreCar car;
    car.mass = field(BodyLayout::kMass);
    car.yaw_inverse_inertia = field(CarLayout::kBodyInverseInertiaUp);
    car.velocity = velocity.xyz();
    car.speed = velocity.w;
    car.angular_velocity = angular_velocity.xyz();
    car.transform = &transform;
    car.inverse_transform = &inverse;
    car.steering_lock = field(CarLayout::kSteeringLock);
    car.steering = field(CarLayout::kSteering);
    car.throttle = field(CarLayout::kThrottle);
    car.drive_gain = field(CarLayout::kDriveGain);
    car.drag_coefficient = field(CarLayout::kDragCoefficient);
    car.drag_area = field(CarLayout::kDragArea);
    car.drag_rpm_scale = field(CarLayout::kDragRpmScale);
    car.drive_torque_scale = field(CarLayout::kDriveTorqueScale);
    car.drive_torque = field(CarLayout::kDriveTorque);
    car.yaw_damping = field(CarLayout::kYawDamping);
    car.yaw_damping_steer_relief = field(CarLayout::kYawDampingSteerRelief);
    car.centre_of_mass_height = field(CarLayout::kCentreOfMassHeight);
    car.force = load3(memory, at(BodyLayout::kForce));
    car.torque = load3(memory, at(BodyLayout::kTorque));
    car.wheel_spin = field(CarLayout::kWheelSpin);
    car.wheel_radius = field(CarLayout::kWheelRadius);

    const bool reversing = engine::dot(transform.basis[2], car.velocity) < 0.0F;
    memory.write_u32_le(at(CarLayout::kSlide), 0U);
    const engine::PlanarTyreStep step = engine::step_planar_tyres(car, dt);
    store_float(memory, at(CarLayout::kFrontWheelAngle), step.front_wheel_angle);
    store_float(memory, at(CarLayout::kWheelSpin), step.wheel_spin);
    store_float(memory, at(CarLayout::kWheelSpinCopy), step.wheel_spin);
    const auto wheel_count = static_cast<std::int8_t>(memory.read_u8(at(CarLayout::kWheelCount)));
    if (wheel_count >= 5) {
        for (const GuestAddress copy : CarLayout::kWheelSpinCopies) {
            store_float(memory, at(copy), step.wheel_spin);
        }
    }
    if (step.skidding) {
        memory.write_u32_le(at(CarLayout::kSkidding), 1U);
    }
    store3(memory, at(BodyLayout::kForce), step.force);
    store3(memory, at(BodyLayout::kTorque), step.torque);

    // The first two wheels turn at the speed of their contact points.
    for (GuestAddress wheel = 0U; wheel < 2U; ++wheel) {
        const GuestAddress record = at(wheel * CarWheelLayout::kStride);
        const GuestAddress object = memory.read_u32_le(at(CarLayout::kWheelObjects + wheel * 4U));
        engine::Vector3 local = load3(memory, object + 48U);
        local.y = load_float(memory, record + CarWheelLayout::kHeight);
        const engine::Vector3 point =
            ((transform.basis[0] * local.x + transform.basis[1] * local.y) + transform.basis[2] * local.z) +
            transform.translation;
        const engine::Vector3 arm = point - transform.translation;
        const engine::Vector3 contact = engine::cross(car.angular_velocity, arm) + car.velocity;
        const float speed = std::sqrt(engine::dot(contact, contact));
        store_float(memory, record + CarWheelLayout::kAngularSpeed,
                    (reversing ? -speed : speed) / load_float(memory, record + CarWheelLayout::kRadius));
    }

    if (velocity.w < load_float(memory, kSettleSpeed) && memory.read_u8(at(BodyLayout::kKeepMoving)) == 0U &&
        memory.read_u32_le(at(CarLayout::kHeld)) == 0U) {
        for (std::int32_t wheel = 0; wheel < wheel_count; ++wheel) {
            store_float(memory,
                        at(static_cast<GuestAddress>(wheel) * CarWheelLayout::kStride + CarWheelLayout::kAngularSpeed),
                        0.0F);
        }
        const float damping = load_float(memory, kSettleDamping);
        const engine::Vector3 slowed = engine::Vector3{} + velocity.xyz() * damping;
        store_vector4(memory, at(BodyLayout::kVelocity), {slowed.x, slowed.y, slowed.z, velocity.w * damping});
        store3(memory, at(BodyLayout::kAngularVelocity), angular_velocity.xyz() * damping);
    }
}

// StepPlanarTyres (original 0x001c38f0): run_planar_tyres for the car at a0
// over f12 seconds.
NativeFunctionExecution step_planar_tyres(NativeFunctionContext& context) {
    GuestCall call{context};
    run_planar_tyres(call.memory(), call.arg(0), call.float_arg(0));
    return call.finish(2260U);
}

// Game globals the drive model reads for traffic.
constexpr GuestAddress kRaceClock = 0x00522680U;            // seconds
constexpr GuestAddress kTrafficModeFlag = 0x01e65499U;      // u8
constexpr GuestAddress kGameModeState = 0x0051ba88U;        // pointer; its handler at +436
constexpr GuestAddress kQuarterTractionModes[2] = {0x0051b714U, 0x0051b788U};
// Heavy vehicles lose their spin about up for this long after it is armed.
constexpr float kHeavySpinSettleTime = std::bit_cast<float>(0x3e4ccccdU);  // 0.2

bool heavy_vehicle(const char* name) {
    return std::strcmp(name, "HEVYCAR9") == 0 || std::strcmp(name, "HEVYCAR36") == 0;
}

// The steered frame's fourth lanes take whatever the original's vector
// registers held (partly stale stack words).
constexpr std::array kDriveUnusedWords{
    UnusedWord{-1, lane(CarLayout::kSteeredFrame)},
    UnusedWord{-1, lane(CarLayout::kSteeredFrame + 16U)},
    UnusedWord{-1, lane(CarLayout::kSteeredFrame + 32U)},
};

// Force and torque sum a dozen tyre forces (up to about 1e5 N while sliding
// fast) that largely cancel, and the torque's lever arms are computed as
// (point + position) - position, which keeps about four significant digits
// at track coordinates. Both depend on the rounding mode: the PS2 rounds
// toward zero, and run that way the native output matches all 56 race
// captures to 3e-4. Rounding to nearest, the worst differences are 0.16 N of
// force and 35 N m of torque (on 1284 N m of yaw torque, for one frame).
constexpr float kForceRoundingBound = 1.0F;
constexpr float kTorqueRoundingBound = 64.0F;
constexpr std::array kDriveLooseWords{
    LooseWord{BodyLayout::kForce, kForceRoundingBound},
    LooseWord{BodyLayout::kForce + 4U, kForceRoundingBound},
    LooseWord{BodyLayout::kForce + 8U, kForceRoundingBound},
    LooseWord{BodyLayout::kTorque, kTorqueRoundingBound},
    LooseWord{BodyLayout::kTorque + 4U, kTorqueRoundingBound},
    LooseWord{BodyLayout::kTorque + 8U, kTorqueRoundingBound},
};

// DriveCar (original 0x001c1b20): the drive model of the car at a0 over f12
// seconds (see engine/physics/car_drive.hpp).
NativeFunctionExecution drive_car(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress car_address = call.arg(0);
    const float dt = call.float_arg(0);
    const auto at = [&](const GuestAddress offset) { return car_address + offset; };
    const auto field = [&](const GuestAddress offset) { return load_float(memory, at(offset)); };
    const auto flag = [&](const GuestAddress offset) { return memory.read_u8(at(offset)); };

    const GuestAddress transform_address = memory.read_u32_le(at(BodyLayout::kTransform));
    const engine::Vector4 velocity = load_vector4(memory, at(BodyLayout::kVelocity));

    engine::CarDrive car;
    car.mass = field(BodyLayout::kMass);
    car.velocity = velocity.xyz();
    car.speed = velocity.w;
    car.heading = load3(memory, at(BodyLayout::kHeading));
    car.angular_velocity = load3(memory, at(BodyLayout::kAngularVelocity));
    car.angular_momentum = load3(memory, at(BodyLayout::kAngularMomentum));
    car.force = load3(memory, at(BodyLayout::kForce));
    car.torque = load3(memory, at(BodyLayout::kTorque));
    car.transform = load_transform(memory, transform_address);
    car.gravity = load3(memory, kGravity);

    car.wheel_count = static_cast<std::int8_t>(flag(CarLayout::kWheelCount));
    for (GuestAddress index = 0U; index < engine::kMaxCarWheels; ++index) {
        const GuestAddress record = at(index * CarWheelLayout::kStride);
        engine::CarDriveWheel& wheel = car.wheels[index];
        wheel.radius = load_float(memory, record + CarWheelLayout::kRadius);
        wheel.drive_torque = load_float(memory, record + CarWheelLayout::kDriveTorque);
        wheel.angular_speed = load_float(memory, record + CarWheelLayout::kAngularSpeed);
        if (index < 4U) {
            const GuestAddress object = memory.read_u32_le(at(CarLayout::kWheelObjects + index * 4U));
            wheel.mount_x = load_float(memory, object + 48U);
            wheel.mount_z = load_float(memory, object + 56U);
            wheel.height = load_float(memory, record + CarWheelLayout::kHeight);
            wheel.contact = memory.read_u8(record + CarWheelLayout::kContact) != 0U;
        }
    }
    car.front_wheel_angle = field(CarLayout::kFrontWheelAngle);
    car.throttle = field(CarLayout::kThrottle);
    car.drive_torque_scale = field(CarLayout::kDriveTorqueScale);
    car.steering = field(CarLayout::kSteering);
    car.drive_gain = field(CarLayout::kDriveGain);
    car.wheel_rpm_source = field(CarLayout::kDragRpmScale);
    car.engine_rpm = field(CarLayout::kEngineRpm);
    car.airborne = flag(CarLayout::kAirborne);
    car.airtime = field(CarLayout::kAirtime);
    car.lost_grip = memory.read_u32_le(at(CarLayout::kSkidding));
    car.slide = memory.read_u32_le(at(CarLayout::kSlide));
    car.slide_progress = field(CarLayout::kSlideProgress);
    car.slide_timer = field(CarLayout::kSlideTimer);
    car.slide_blend = field(CarLayout::kSlideBlend);
    car.scripted = flag(CarLayout::kScripted) != 0U;
    car.traffic = flag(CarLayout::kTraffic) != 0U;
    car.no_steer_drag = flag(CarLayout::kNoSteerDrag) != 0U;
    car.hold = memory.read_u32_le(at(CarLayout::kHeld));
    car.hold_other = memory.read_u32_le(at(CarLayout::kHeldOther));
    if (car.traffic) {
        bool quarter = false;
        if (memory.read_u8(kTrafficModeFlag) != 0U) {
            const GuestAddress handler = memory.read_u32_le(memory.read_u32_le(kGameModeState) + 436U);
            quarter = handler == kQuarterTractionModes[0] || handler == kQuarterTractionModes[1];
        }
        car.traffic_traction = quarter ? 4.0F : 8.0F;
        const GuestAddress definition = memory.read_u32_le(at(CarLayout::kDefinition));
        const auto name = engine::unpack_name(memory.read_u64_le(definition + CarDefinitionLayout::kPackedName));
        car.settle_heavy_spin =
            heavy_vehicle(name.text.data()) &&
            load_float(memory, kRaceClock) - load_float(memory, definition + CarDefinitionLayout::kSpinSettleStart) <
                kHeavySpinSettleTime;
    }

    engine::CarDriveTuning tuning;
    tuning.rolling_drag = field(CarLayout::kRollingDrag);
    tuning.airborne_weight = field(CarLayout::kAirborneWeight);
    tuning.drive_height = field(CarLayout::kDriveHeight);
    tuning.traction_height = field(CarLayout::kTractionHeight);
    tuning.grip_height = field(CarLayout::kCentreOfMassHeight);
    tuning.slide_grip_height = field(CarLayout::kSlideGripHeight);
    tuning.drive_torque = field(CarLayout::kDriveTorque);
    tuning.slide_timer_start = field(CarLayout::kSlideTimerStart);
    tuning.yaw_assist_fast = field(CarLayout::kYawAssistFast);
    tuning.yaw_assist_slow = field(CarLayout::kYawAssistSlow);
    tuning.slide_steer_gain = field(CarLayout::kSlideSteerGain);
    tuning.planar_speed = field(CarLayout::kPlanarSpeed);
    tuning.steer_drag = field(CarLayout::kSteerDrag);
    tuning.coast_drag = field(CarLayout::kDragCoefficient);
    tuning.slide_exit = field(CarLayout::kSlideExit);
    tuning.drag_cut_speed = field(CarLayout::kDragCutSpeed);
    tuning.roll_damping = field(CarLayout::kRollDamping);
    tuning.slide_entry = field(CarLayout::kSlideEntry);
    tuning.slide_yaw_rate = field(CarLayout::kSlideYawRate);
    tuning.probe_near = field(CarLayout::kProbeExtentNear + 8U) / 2.0F;
    tuning.probe_far = 2.0F * field(CarLayout::kProbeExtentFar + 8U);

    const engine::CarDriveResult result = engine::drive_car(car, tuning, dt);

    memory.write_u8(at(CarLayout::kWasAirborne), car.was_airborne);
    memory.write_u8(at(CarLayout::kAirborne), car.airborne);
    store_float(memory, at(CarLayout::kAirtime), car.airtime);
    memory.write_u32_le(at(CarLayout::kSkidding), car.lost_grip);
    memory.write_u32_le(at(CarLayout::kSlide), car.slide);
    store_float(memory, at(CarLayout::kSlideProgress), car.slide_progress);
    store_float(memory, at(CarLayout::kSlideTimer), car.slide_timer);
    store3(memory, at(BodyLayout::kForce), car.force);
    store3(memory, at(BodyLayout::kTorque), car.torque);
    store3(memory, at(BodyLayout::kAngularMomentum), car.angular_momentum);
    for (GuestAddress index = 0U; index < engine::kMaxCarWheels; ++index) {
        const GuestAddress record = at(index * CarWheelLayout::kStride);
        store_float(memory, record + CarWheelLayout::kDriveTorque, car.wheels[index].drive_torque);
        store_float(memory, record + CarWheelLayout::kAngularSpeed, car.wheels[index].angular_speed);
    }

    if (result == engine::CarDriveResult::Grounded) {
        for (int row = 0; row < 3; ++row) {
            const auto offset = static_cast<GuestAddress>(row * 16);
            store3w(memory, at(CarLayout::kSteeredFrame + offset), car.steered.basis[row],
                    lane_w(memory, transform_address + offset));
        }
        store3w(memory, at(CarLayout::kSteeredFrame + 48U), car.steered.translation,
                lane_w(memory, transform_address + 48U));
    } else if (result == engine::CarDriveResult::PlanarTyres) {
        run_planar_tyres(memory, car_address, dt);
        car.force = load3(memory, at(BodyLayout::kForce));
        car.speed = lane_w(memory, at(BodyLayout::kVelocity));
        engine::press_car_down(car);
        store3(memory, at(BodyLayout::kForce), car.force);
    }
    // The original's instructions by path (the planar one includes
    // StepPlanarTyres; traffic unpacks the car's name, about 5000 more).
    switch (result) {
    case engine::CarDriveResult::Grounded:
        return call.finish(car.traffic ? 7100U : 2070U);
    case engine::CarDriveResult::Airborne:
        return call.finish(700U);
    case engine::CarDriveResult::PlanarTyres:
        break;
    }
    return call.finish(2450U);
}

// InitialiseContactRecord (original 0x00388a90): resets the record at a0 the
// collision response code (0x00278020, 0x002789c0) fills in: three words of
// FLT_MIN, four zeros, three of -1, FLT_MIN, then (from +52) 0, 50, 15, -1
// (a word) and three zero bytes. Words 44 and 48 are left as they are.
NativeFunctionExecution initialise_contact_record(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress record = call.arg(0);
    constexpr std::uint32_t kSmallest = 0x00800000U;  // FLT_MIN
    constexpr std::array<std::uint32_t, 11> kFirstWords{kSmallest,  kSmallest,  kSmallest,  0U, 0U, 0U, 0U,
                                                        0xbf800000U, 0xbf800000U, 0xbf800000U, kSmallest};
    for (GuestAddress index = 0U; index < kFirstWords.size(); ++index) {
        memory.write_u32_le(record + index * 4U, kFirstWords[index]);
    }
    memory.write_u32_le(record + 52U, 0U);
    store_float(memory, record + 56U, 50.0F);
    store_float(memory, record + 60U, 15.0F);
    memory.write_u32_le(record + 64U, 0xffffffffU);
    for (GuestAddress byte = 68U; byte < 71U; ++byte) {
        memory.write_u8(record + byte, 0U);
    }
    return call.finish(24U);
}

// The gearbox record in the car (kGearbox).
struct GearboxLayout {
    static constexpr GuestAddress kRatios = 4U;       // floats, gear 0 first
    static constexpr GuestAddress kFinalDrive = 32U;
    static constexpr GuestAddress kRedline = 40U;     // rpm
    static constexpr GuestAddress kEngineRate = 84U;
    static constexpr GuestAddress kGear = 128U;       // u32
    static constexpr GuestAddress kGearCount = 132U;  // s32
};
constexpr GuestAddress kGearbox = 5192U;
constexpr GuestAddress kRpmToRadiansPerSecond = 0x004e0d80U;  // 2 pi / 60 in the ELF
// Words a set speed resets: two to -1.0f, one to 1.0f, one to zero.
constexpr std::array<GuestAddress, 4> kSetSpeedResets{5280U, 5284U, 5328U, 5332U};
constexpr std::array<std::uint32_t, 4> kSetSpeedResetWords{0xbf800000U, 0U, 0xbf800000U, 0x3f800000U};
constexpr GuestAddress kWheelRadiusForGears = 2544U;  // the third wheel's radius

// Reads the gearbox at `gearbox` and picks the gear for `wheel_rate`.
engine::GearPick pick_car_gear(const guest::EeMemory& memory, const GuestAddress gearbox, const float wheel_rate,
                               const float unchanged_rate) {
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(gearbox + GearboxLayout::kGearCount));
    // Gear counts past the seven the record holds would read on into the
    // following fields, as the original does; 64 bounds the read.
    std::array<float, 64> ratios{};
    const auto gears = static_cast<std::size_t>(std::clamp(count, 0, static_cast<std::int32_t>(ratios.size())));
    for (std::size_t gear = 0U; gear < gears; ++gear) {
        ratios[gear] = load_float(memory, gearbox + GearboxLayout::kRatios + static_cast<GuestAddress>(gear * 4U));
    }
    const float limit = engine::chop_mul(load_float(memory, kRpmToRadiansPerSecond),
                                         load_float(memory, gearbox + GearboxLayout::kRedline));
    return engine::pick_gear(std::span<const float>{ratios.data(), gears},
                             load_float(memory, gearbox + GearboxLayout::kFinalDrive), limit, wheel_rate,
                             unchanged_rate);
}

// The original PickGear's instruction count for a pick from `count` gears.
std::uint32_t pick_gear_instructions(const guest::EeMemory& memory, const GuestAddress gearbox, const int gear) {
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(gearbox + GearboxLayout::kGearCount));
    if (count <= 1) {
        return 13U;
    }
    const auto picked = static_cast<std::uint32_t>(gear);
    return gear < count ? 13U * picked + 9U : 13U * picked;
}

// PickGear (original 0x001c6f30): the gear for a car moving at f13 on wheels
// of radius f12, from the gearbox at a0; stores the engine rate and returns
// the gear. With under two gears the stored rate is whatever f3 held.
NativeFunctionExecution pick_gear(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress gearbox = call.arg(0);
    const float wheel_rate = engine::game_div(call.float_arg(1), call.float_arg(0));
    const auto pick = pick_car_gear(memory, gearbox, wheel_rate, std::bit_cast<float>(call.cpu().read_fpr_bits(3U)));
    store_float(memory, gearbox + GearboxLayout::kEngineRate, pick.engine_rate);
    call.return_value(static_cast<std::uint32_t>(pick.gear));
    return call.finish(pick_gear_instructions(memory, gearbox, pick.gear));
}

// SetCarTransform (original 0x001ab610): places the car a0 at the four rows
// at a1 (runtime::set_car_transform).
NativeFunctionExecution set_car_transform_call(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    std::array<engine::Vector4, 4> rows;
    for (GuestAddress row = 0U; row < 4U; ++row) {
        rows[row] = load_vector4(memory, call.arg(1) + row * 16U);
    }
    return call.finish(set_car_transform(memory, call.arg(0), rows));
}

// SetCarSpeed (original 0x001bed00): runtime::set_car_speed of the car a0 to
// f12; the gear pick sees f3 as the caller left it.
NativeFunctionExecution set_car_speed_call(NativeFunctionContext& context) {
    GuestCall call{context};
    return call.finish(set_car_speed(call.memory(), call.arg(0), call.float_arg(0),
                                     std::bit_cast<float>(call.cpu().read_fpr_bits(3U))));
}

// The suspension's view of the car (UpdateSuspension).
struct SuspensionLayout {
    static constexpr GuestAddress kFrontAxle = 3236U;   // damping, stiffness, travel
    static constexpr GuestAddress kRearAxle = 3252U;
    static constexpr GuestAddress kModelInfo = 3268U;   // pointer, SuspensionModelLayout
    static constexpr GuestAddress kContactDragOff = 4459U;  // u8
};
struct SuspensionModelLayout {
    static constexpr GuestAddress kMounts = 1196U;      // s8 per wheel: 2 tilted, 3 missing
    static constexpr GuestAddress kSpinSpeeds = 1256U;  // a copy of each wheel's
    static constexpr GuestAddress kFullDragUnraced = 4123U;  // u8
};
struct SuspensionWheelLayout {
    static constexpr GuestAddress kPosition = 2080U;      // fourth lane: the car transform's
    static constexpr GuestAddress kContactPoint = 2096U;
    static constexpr GuestAddress kContactNormal = 2112U;
    static constexpr GuestAddress kSpinAngle = 2168U;
    static constexpr GuestAddress kOffset = 2176U;
    static constexpr GuestAddress kGroundOffset = 2180U;
    static constexpr GuestAddress kSize = 2188U;
    static constexpr GuestAddress kWidth = 2192U;
    static constexpr GuestAddress kDragged = 2200U;       // u32, set to 5
    static constexpr GuestAddress kGroundJumped = 2258U;  // u8
    static constexpr GuestAddress kPushed = 2260U;        // u8
};
constexpr GuestAddress kSlowedTime = 0x00665eceU;  // u8
constexpr std::uint32_t kDraggedMark = 5U;
// The stack word (below the caller's stack pointer) that the original leaves
// in the first row of each wheel model's fourth lane.
constexpr GuestAddress kWheelModelStaleLaneBelowEntry = 244U;

// The original's instruction count for each point the update passes.
constexpr std::array<std::uint8_t, static_cast<std::size_t>(engine::SuspensionStep::Count)> kSuspensionInstructions{
    44, 24, 32, 4, 12, 2, 4, 4, 3, 3,           // Start .. ContactCheck
    8, 3, 6, 10, 3, 1, 3, 9, 3, 4,              // Grounded .. LeverClampCheck
    1, 4, 13, 8, 39, 8, 9, 22, 81, 9,           // LeverClamp .. PushCheck (Push with its fabsf)
    5, 3, 3, 4, 6, 3, 10, 70, 4, 5,             // PushLeverCheck .. DragHalf (Drag with its fabsf)
    16, 25, 3, 3, 1, 26, 2, 11, 2, 17,          // DragApplied .. PushProjected
    2, 7, 7, 21, 4, 47, 4, 6, 1, 6,             // PushShareCheck .. SpinLever
    2, 12, 5, 17, 3, 4, 3, 13, 2, 39,           // SpinRacedCheck .. SpinSine (with its fabsf)
    4, 2, 3, 3, 2, 8, 13, 16, 51, 72,           // SpinCosineCheck .. SpinScale
    25};                                        // Finish
static_assert(kSuspensionInstructions.back() != 0U, "one count per engine::SuspensionStep");

struct SuspensionCall {
    engine::RigidBody body;
    engine::Transform transform;
    engine::SuspensionCar car;
    engine::SuspensionTrace trace;
    bool ported{};
    std::uint32_t instructions{};
};

// The suspension of the car at `car_address` over `dt` seconds, read from
// guest memory without writing it.
void evaluate_suspension(const guest::EeMemory& memory, const GuestAddress car_address, const float dt,
                         SuspensionCall& call) {
    const auto at = [&](const GuestAddress offset) { return car_address + offset; };
    const auto field = [&](const GuestAddress offset) { return load_float(memory, at(offset)); };
    call.transform = load_transform(memory, memory.read_u32_le(at(BodyLayout::kTransform)));
    engine::RigidBody& body = call.body;
    body.transform = &call.transform;
    for (int row = 0; row < 3; ++row) {
        body.world_inverse_inertia[row] = load3(memory, at(BodyLayout::kWorldInverseInertia + row * 16U));
    }
    body.velocity = load3(memory, at(BodyLayout::kVelocity));
    body.speed = lane_w(memory, at(BodyLayout::kVelocity));
    body.angular_velocity = load3(memory, at(BodyLayout::kAngularVelocity));
    body.force = load3(memory, at(BodyLayout::kForce));
    body.torque = load3(memory, at(BodyLayout::kTorque));
    body.impulse = load3(memory, at(BodyLayout::kImpulse));
    body.angular_impulse = load3(memory, at(BodyLayout::kAngularImpulse));
    body.position_correction = load3(memory, at(BodyLayout::kPositionCorrection));
    body.mass = field(BodyLayout::kMass);

    engine::SuspensionCar& car = call.car;
    car.body = &body;
    const auto axle = [&](const GuestAddress offset) {
        return engine::SuspensionAxle{field(offset), field(offset + 4U), field(offset + 8U)};
    };
    car.front = axle(SuspensionLayout::kFrontAxle);
    car.rear = axle(SuspensionLayout::kRearAxle);
    car.steering_degrees = field(CarLayout::kFrontWheelAngle);
    car.wheel_count = static_cast<std::int8_t>(memory.read_u8(at(CarLayout::kWheelCount)));
    car.gravity_at_lever = memory.read_u8(at(BodyLayout::kGravityAtLever)) != 0U;
    const auto state = static_cast<std::int8_t>(memory.read_u8(at(BodyLayout::kState)));
    car.raced = state >= 1 && state <= 3;
    car.player = state == 1;
    car.contact_drag_off = memory.read_u8(at(SuspensionLayout::kContactDragOff)) != 0U;
    const GuestAddress model_info = memory.read_u32_le(at(SuspensionLayout::kModelInfo));
    car.full_drag_unraced = memory.read_u8(model_info + SuspensionModelLayout::kFullDragUnraced) != 0U;
    car.slowed_time = memory.read_u8(kSlowedTime) != 0U;
    const auto count = static_cast<std::size_t>(std::clamp<std::int32_t>(car.wheel_count, 0, 6));
    for (std::size_t index = 0U; index < engine::kSuspensionWheels; ++index) {
        const GuestAddress record = at(static_cast<GuestAddress>(index) * CarWheelLayout::kStride);
        engine::SuspensionWheel& wheel = car.wheels[index];
        wheel.pushed = memory.read_u8(record + SuspensionWheelLayout::kPushed) != 0U;
        if (index >= count) {
            continue;
        }
        const auto mount = static_cast<std::int8_t>(
            memory.read_u8(model_info + SuspensionModelLayout::kMounts + static_cast<GuestAddress>(index)));
        wheel.mount = mount == 3 ? engine::WheelMount::Missing
                                 : mount == 2 ? engine::WheelMount::Tilted : engine::WheelMount::Standard;
        wheel.grounded = memory.read_u8(record + CarWheelLayout::kContact) != 0U;
        wheel.contact_normal = load3(memory, record + SuspensionWheelLayout::kContactNormal);
        wheel.contact_point = load3(memory, record + SuspensionWheelLayout::kContactPoint);
        wheel.offset = load_float(memory, record + SuspensionWheelLayout::kOffset);
        wheel.ground_offset = load_float(memory, record + SuspensionWheelLayout::kGroundOffset);
        wheel.rest_offset = load_float(memory, record + CarWheelLayout::kHeight);
        wheel.spin_angle = load_float(memory, record + SuspensionWheelLayout::kSpinAngle);
        wheel.spin_speed = load_float(memory, record + CarWheelLayout::kAngularSpeed);
        wheel.width = load_float(memory, record + SuspensionWheelLayout::kWidth);
        wheel.size = load_float(memory, record + SuspensionWheelLayout::kSize);
        wheel.position = load3(memory, record + SuspensionWheelLayout::kPosition);
        wheel.model = load_transform(
            memory, memory.read_u32_le(at(CarLayout::kWheelObjects + static_cast<GuestAddress>(index) * 4U)));
    }
    call.ported = engine::update_suspension(car, dt, call.trace);
    call.instructions = 0U;
    for (std::size_t step = 0U; step < kSuspensionInstructions.size(); ++step) {
        call.instructions += call.trace.passes[step] * static_cast<std::uint32_t>(kSuspensionInstructions[step]);
    }
}

// The last evaluation of the cost hook. The dispatcher runs the hook on the
// live state right before the implementation, which takes the evaluation
// from here instead of repeating it (it holds pointers into itself, so it
// stays in place).
struct PreparedSuspension {
    GuestAddress car{};
    std::uint32_t dt_bits{};
    bool valid{};
    SuspensionCall call;
};
thread_local PreparedSuspension prepared_suspension;

constexpr unsigned kSuspensionCar = 4U;
constexpr unsigned kSuspensionFrameSeconds = 12U;

// The call's cost, or 0 for what the native code leaves to the original:
// a tilted wheel, a stale push (engine::update_suspension), or a hard
// landing, which starts an effect through code that is not native yet.
std::uint32_t suspension_cost(const NativeFunctionPrepareContext& context) {
    PreparedSuspension& prepared = prepared_suspension;
    prepared.car = context.cpu.read_gpr_u32(kSuspensionCar);
    prepared.dt_bits = context.cpu.read_fpr_bits(kSuspensionFrameSeconds);
    prepared.call = SuspensionCall{};
    evaluate_suspension(context.memory, prepared.car, std::bit_cast<float>(prepared.dt_bits), prepared.call);
    const SuspensionCall& call = prepared.call;
    prepared.valid = call.ported && call.trace.hard_landings == 0U;
    return prepared.valid ? call.instructions : 0U;
}

// UpdateSuspension (original 0x001cc900): the suspension of the car a0 over
// f12 seconds (engine::update_suspension).
NativeFunctionExecution update_suspension(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress car_address = call.arg(0);
    const auto at = [&](const GuestAddress offset) { return car_address + offset; };
    PreparedSuspension& prepared = prepared_suspension;
    if (!prepared.valid || prepared.car != car_address ||
        prepared.dt_bits != call.cpu().read_fpr_bits(kSuspensionFrameSeconds)) {
        prepared.call = SuspensionCall{};
        evaluate_suspension(memory, car_address, call.float_arg(0), prepared.call);
    }
    prepared.valid = false;
    const SuspensionCall& result = prepared.call;
    const engine::RigidBody& body = result.body;
    store3(memory, at(BodyLayout::kForce), body.force);
    store3(memory, at(BodyLayout::kTorque), body.torque);
    store3(memory, at(BodyLayout::kImpulse), body.impulse);
    store3(memory, at(BodyLayout::kAngularImpulse), body.angular_impulse);
    store3(memory, at(BodyLayout::kPositionCorrection), body.position_correction);

    // The rows the original rebuilds keep stale fourth lanes: the first a
    // stack word, the next two the high word of a3 as the caller left it.
    const float first_row_lane = load_float(memory, call.stack_pointer() - kWheelModelStaleLaneBelowEntry);
    const float row_lane = std::bit_cast<float>(call.arg128(3).words[3]);
    const float position_lane = lane_w(memory, memory.read_u32_le(at(BodyLayout::kTransform)) + 48U);
    const GuestAddress model_info = memory.read_u32_le(at(SuspensionLayout::kModelInfo));
    const engine::SuspensionCar& car = result.car;
    const auto count = static_cast<std::size_t>(std::clamp<std::int32_t>(car.wheel_count, 0, 6));
    for (std::size_t index = 0U; index < count; ++index) {
        const GuestAddress record = at(static_cast<GuestAddress>(index) * CarWheelLayout::kStride);
        const engine::SuspensionWheel& wheel = car.wheels[index];
        memory.write_u8(record + SuspensionWheelLayout::kGroundJumped, wheel.ground_jumped ? 1U : 0U);
        memory.write_u8(record + SuspensionWheelLayout::kPushed, wheel.pushed ? 1U : 0U);
        if (wheel.mount == engine::WheelMount::Missing) {
            continue;
        }
        if (wheel.dragged) {
            memory.write_u32_le(record + SuspensionWheelLayout::kDragged, kDraggedMark);
        }
        store_float(memory, record + SuspensionWheelLayout::kOffset, wheel.offset);
        store_float(memory, record + SuspensionWheelLayout::kGroundOffset, wheel.ground_offset);
        store3w(memory, record + SuspensionWheelLayout::kPosition, wheel.position, position_lane);
        store_float(memory, record + SuspensionWheelLayout::kSpinAngle, wheel.spin_angle);
        store_float(memory, record + CarWheelLayout::kAngularSpeed, wheel.spin_speed);
        store_float(memory, model_info + SuspensionModelLayout::kSpinSpeeds + static_cast<GuestAddress>(index) * 4U,
                    wheel.spin_speed);
        const GuestAddress model =
            memory.read_u32_le(at(CarLayout::kWheelObjects + static_cast<GuestAddress>(index) * 4U));
        store3w(memory, model, wheel.model.basis[0], first_row_lane);
        store3w(memory, model + 16U, wheel.model.basis[1], row_lane);
        store3w(memory, model + 32U, wheel.model.basis[2], row_lane);
        store3(memory, model + 48U, wheel.model.translation);
    }
    return call.finish(result.instructions);
}

std::uint32_t segment_test_cost(const NativeFunctionPrepareContext& context) {
    if (!segment_test_program_loaded(context.memory)) {
        return 0U;
    }
    constexpr unsigned kSegment = 4U;
    const GuestAddress segment = context.cpu.read_gpr_u32(kSegment);
    const auto& memory = context.memory;
    const auto test = engine::test_segment_against_triangle(
        load_quad_operand(memory, segment), load_quad_operand(memory, segment + 16U),
        load_quad_operand(memory, context.cpu.read_gpr_u32(5U)), load_quad_operand(memory, context.cpu.read_gpr_u32(6U)),
        load_quad_operand(memory, context.cpu.read_gpr_u32(7U)));
    return test.hit ? kSegmentTestHitInstructions : kSegmentTestInstructions;
}

// SegmentHitsTriangle (original 0x00274c90): whether the segment a0 (start,
// then end, 16 bytes apart) crosses the triangle a1, a2, a3; on a hit stores
// t, u and v at t0, t1 and t2 and returns 1.
NativeFunctionExecution segment_hits_triangle(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress segment = call.arg(0);
    const engine::Vector3 start = load_quad_operand(memory, segment);
    const auto test = engine::test_segment_against_triangle(start, load_quad_operand(memory, segment + 16U),
                                                            load_quad_operand(memory, call.arg(1)),
                                                            load_quad_operand(memory, call.arg(2)),
                                                            load_quad_operand(memory, call.arg(3)));
    leave_segment_test(call.cpu(), memory, load_quad(memory, segment), load_quad(memory, segment + 16U), call.arg(1),
                       call.arg(2), call.arg(3), start, test);
    if (test.hit) {
        store_float(memory, call.arg(4), test.t);
        store_float(memory, call.arg(5), engine::chop_add(0.0F, test.u));
        store_float(memory, call.arg(6), engine::chop_add(0.0F, test.v));
    }
    call.return_value(test.hit ? 1U : 0U);
    return call.finish(test.hit ? kSegmentTestHitInstructions : kSegmentTestInstructions);
}

// A car's local collision triangles (car + kCollisionMesh).
struct CollisionMeshLayout {
    static constexpr GuestAddress kCount = 0U;      // s32
    static constexpr GuestAddress kTriangles = 4U;  // pointer: three corners then the normal, 64 bytes each
    static constexpr GuestAddress kSurfaces = 8U;   // pointer: two bytes each, the surface first
};
constexpr GuestAddress kCollisionMesh = 512U;
constexpr GuestAddress kTriangleBytes = 64U;
constexpr GuestAddress kSpecialSurfaceMarker = 804U;  // float in the model info
constexpr GuestAddress kMarkerSet = 0x004e0698U;
constexpr GuestAddress kMarkerCleared = 0x004e06ccU;

constexpr std::array<std::uint8_t, static_cast<std::size_t>(engine::MeshProbeStep::Count)> kMeshProbeInstructions{
    28, 5, 5, 2, 5, 8, 17, 26, 26, 3,   // Start .. WreckCheck
    20, 1, 2, 12, 44, 54, 3, 3, 4, 6,   // Slope .. MarkerOther (Miss and Hit with SegmentHitsTriangle)
    2, 1, 36};                          // MarkerCleared, TriangleDone, Finish
static_assert(kMeshProbeInstructions.back() != 0U, "one count per engine::MeshProbeStep");

struct MeshProbeCall {
    GuestAddress car{};
    GuestAddress start_address{};
    GuestAddress end_address{};
    bool valid{};
    engine::Vector3 start;
    engine::MeshProbe probe;
    std::uint32_t instructions{};
};
thread_local MeshProbeCall prepared_mesh_probe;
thread_local std::vector<engine::CollisionTriangle> mesh_probe_triangles;

GuestAddress collision_triangle(const guest::EeMemory& memory, const GuestAddress car, const std::size_t index) {
    return memory.read_u32_le(memory.read_u32_le(car + kCollisionMesh) + CollisionMeshLayout::kTriangles) +
           static_cast<GuestAddress>(index) * kTriangleBytes;
}

// The probe of car a0 from a1 to a2, read from guest memory without writing
// it. False when it would test a triangle with the vector unit's program not
// loaded (the original then uploads it).
bool evaluate_mesh_probe(const guest::EeMemory& memory, const GuestAddress car, const GuestAddress start_address,
                         const GuestAddress end_address, MeshProbeCall& call) {
    const GuestAddress mesh = memory.read_u32_le(car + kCollisionMesh);
    const auto count = static_cast<std::int32_t>(memory.read_u32_le(mesh + CollisionMeshLayout::kCount));
    const GuestAddress surfaces = memory.read_u32_le(mesh + CollisionMeshLayout::kSurfaces);
    auto& triangles = mesh_probe_triangles;
    triangles.resize(static_cast<std::size_t>(std::max(count, 0)));
    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        const GuestAddress triangle = collision_triangle(memory, car, index);
        triangles[index] = {load_quad_operand(memory, triangle), load_quad_operand(memory, triangle + 16U),
                            load_quad_operand(memory, triangle + 32U), load_quad_operand(memory, triangle + 48U),
                            memory.read_u8(surfaces + static_cast<GuestAddress>(index) * 2U)};
    }
    const auto state = static_cast<std::int8_t>(memory.read_u8(car + BodyLayout::kState));
    engine::MeshProbeRules rules;
    rules.raced = state >= 1 && state <= 3;
    rules.wrecked = memory.read_u8(car + BodyLayout::kGravityAtLever) != 0U;
    rules.up = load_quad_operand(memory, memory.read_u32_le(car + BodyLayout::kTransform) + 16U);
    rules.marker_set = load_float(memory, kMarkerSet);
    rules.marker_cleared = load_float(memory, kMarkerCleared);
    const GuestAddress model_info = memory.read_u32_le(car + SuspensionLayout::kModelInfo);
    call.car = car;
    call.start_address = start_address;
    call.end_address = end_address;
    call.start = load_quad_operand(memory, start_address);
    call.probe = engine::probe_mesh(triangles, call.start, load_quad_operand(memory, end_address), rules,
                                    load_float(memory, model_info + kSpecialSurfaceMarker));
    call.instructions = 0U;
    for (std::size_t step = 0U; step < kMeshProbeInstructions.size(); ++step) {
        call.instructions += call.probe.passes[step] * static_cast<std::uint32_t>(kMeshProbeInstructions[step]);
    }
    return !call.probe.tested || memory.read_u32_le(kLoadedVu0Program) == kSegmentTestProgram;
}

std::uint32_t mesh_probe_cost(const NativeFunctionPrepareContext& context) {
    MeshProbeCall& call = prepared_mesh_probe;
    call.valid = evaluate_mesh_probe(context.memory, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                     context.cpu.read_gpr_u32(6U), call);
    return call.valid ? call.instructions : 0U;
}

// ProbeCollisionMesh (original 0x001ce1a0): the segment from a1 to a2
// against car a0's collision triangles (engine::probe_mesh). Stores the
// nearest triangle's index at a3 (-1 for none) and the distance to it at t0;
// returns whether there was one.
NativeFunctionExecution probe_collision_mesh(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress car = call.arg(0);
    MeshProbeCall& result = prepared_mesh_probe;
    if (!result.valid || result.car != car || result.start_address != call.arg(1) ||
        result.end_address != call.arg(2)) {
        evaluate_mesh_probe(memory, car, call.arg(1), call.arg(2), result);
    }
    result.valid = false;
    const engine::MeshProbe& probe = result.probe;
    memory.write_u32_le(call.arg(3), static_cast<std::uint32_t>(probe.nearest));
    store_float(memory, call.arg(4), probe.distance);
    const GuestAddress model_info = memory.read_u32_le(car + SuspensionLayout::kModelInfo);
    store_float(memory, model_info + kSpecialSurfaceMarker, probe.marker);

    // The vector unit: the last test's registers (the test reads the
    // segment from the probe's own copy, start then end), then the
    // segment's length.
    guest::R5900State& cpu = call.cpu();
    if (probe.solved) {
        leave_solved_test_registers(cpu, probe.last_solved);
    }
    if (probe.tested) {
        const GuestAddress triangle = collision_triangle(memory, car, probe.last_tested);
        leave_segment_test(cpu, memory, load_quad(memory, call.arg(1)), load_quad(memory, call.arg(2)), triangle,
                           triangle + 16U, triangle + 32U, result.start, probe.last_test);
    }
    set_vu0_x(cpu, kVfMargins, probe.length);
    guest::Gpr128 squares = load_quad(memory, call.arg(2));
    squares.words[0] = std::bit_cast<std::uint32_t>(probe.length_terms.x);
    squares.words[1] = std::bit_cast<std::uint32_t>(probe.length_terms.y);
    squares.words[2] = std::bit_cast<std::uint32_t>(probe.length_terms.z);
    cpu.write_vu0_vector(kVfStart, squares);
    cpu.write_vu0_vector(kVfDirectionQ, load_quad(memory, call.arg(1)));
    guest::Gpr128 acc = cpu.vu0_accumulator();
    acc.words[0] = std::bit_cast<std::uint32_t>(engine::chop_add(probe.length_terms.x, probe.length_terms.y));
    cpu.set_vu0_accumulator(acc);
    cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(probe.length));

    call.return_value(probe.nearest >= 0 ? 1U : 0U);
    return call.finish(result.instructions);
}

// What the contact search keeps per wheel, besides the suspension's fields.
struct WheelContactLayout {
    static constexpr GuestAddress kPreviousPosition = 2128U;      // the positions before this frame
    static constexpr GuestAddress kPreviousContactPoint = 2144U;
    static constexpr GuestAddress kRadius = 2160U;
    static constexpr GuestAddress kSurface = 2256U;               // u16: surface id, then its flags
};
struct GroundBelowLayout {
    static constexpr GuestAddress kGround = 1168U;  // in the model info: the normal, then the distance
};
constexpr GuestAddress kCarSurface = 4448U;          // u16
constexpr GuestAddress kContactsFound = 4946U;       // u8
constexpr GuestAddress kLeastUpwardNormal = 0x004e0698U;
constexpr std::uint32_t kNoGroundDistanceBits = 0x461c3c00U;  // 9999.0

constexpr std::array<std::uint8_t, static_cast<std::size_t>(engine::WheelContactStep::Count)> kWheelContactInstructions{
    11, 4, 9, 7, 4, 11, 2, 1, 32, 2,     // Start .. Upright
    13, 3, 3, 18, 4, 3, 18, 24, 4, 6,    // Tilted .. ContactStart
    19, 8, 2, 18, 4, 1, 10, 3, 1, 2,     // NoContact .. NotLow (Probe without the probe's own)
    2, 3, 3, 1, 2, 2, 3, 2, 36, 1,       // LowBranch .. WheelDone
    33, 7, 12, 44, 53, 28, 3, 9};        // GroundSetUp .. Finish (the ground tests with SegmentHitsTriangle)
static_assert(kWheelContactInstructions.back() != 0U, "one count per engine::WheelContactStep");

struct WheelContactCall {
    GuestAddress car{};
    bool valid{};
    engine::Transform transform;
    engine::WheelContactCar contact;
    engine::WheelContactSearch search;
    std::uint32_t instructions{};
};
thread_local WheelContactCall prepared_wheel_contacts;
thread_local std::vector<std::uint16_t> wheel_contact_surfaces;

// The search for car a0, read from guest memory without writing it. False
// for what the native code leaves to the original: fewer than two wheels
// (the original then reads probe heights it never set), or the vector
// unit's triangle test program not loaded.
bool evaluate_wheel_contacts(const guest::EeMemory& memory, const GuestAddress car, WheelContactCall& call) {
    const auto at = [&](const GuestAddress offset) { return car + offset; };
    call.car = car;
    const auto count = static_cast<std::int8_t>(memory.read_u8(at(CarLayout::kWheelCount)));
    if (count < 2 || count > static_cast<std::int8_t>(engine::kSuspensionWheels) ||
        memory.read_u32_le(kLoadedVu0Program) != kSegmentTestProgram) {
        return false;
    }
    call.transform = load_transform(memory, memory.read_u32_le(at(BodyLayout::kTransform)));
    for (int row = 0; row < 3; ++row) {
        call.transform.basis[row] = {engine::game_operand(call.transform.basis[row].x),
                                     engine::game_operand(call.transform.basis[row].y),
                                     engine::game_operand(call.transform.basis[row].z)};
    }
    engine::WheelContactCar& contact = call.contact;
    contact.transform = &call.transform;
    contact.front_travel = load_float(memory, at(SuspensionLayout::kFrontAxle + 8U));
    contact.rear_travel = load_float(memory, at(SuspensionLayout::kRearAxle + 8U));
    contact.wheel_count = count;
    const GuestAddress model_info = memory.read_u32_le(at(SuspensionLayout::kModelInfo));
    for (std::size_t index = 0U; index < static_cast<std::size_t>(count); ++index) {
        const GuestAddress record = at(static_cast<GuestAddress>(index) * CarWheelLayout::kStride);
        engine::WheelContactWheel& wheel = contact.wheels[index];
        wheel = {};
        const auto mount = static_cast<std::int8_t>(
            memory.read_u8(model_info + SuspensionModelLayout::kMounts + static_cast<GuestAddress>(index)));
        wheel.mount = mount == 3 ? engine::WheelMount::Missing
                                 : mount == 2 ? engine::WheelMount::Tilted : engine::WheelMount::Standard;
        const GuestAddress model =
            memory.read_u32_le(at(CarLayout::kWheelObjects + static_cast<GuestAddress>(index) * 4U));
        wheel.model_position = load3(memory, model + 48U);
        wheel.rest_offset = load_float(memory, record + CarWheelLayout::kHeight);
        wheel.radius = load_float(memory, record + WheelContactLayout::kRadius);
        wheel.position = load3(memory, record + SuspensionWheelLayout::kPosition);
    }

    // The collision triangles, as the probe reads them.
    const GuestAddress mesh = memory.read_u32_le(at(kCollisionMesh));
    const auto triangles = static_cast<std::int32_t>(memory.read_u32_le(mesh + CollisionMeshLayout::kCount));
    const GuestAddress surfaces = memory.read_u32_le(mesh + CollisionMeshLayout::kSurfaces);
    auto& mesh_triangles = mesh_probe_triangles;
    mesh_triangles.resize(static_cast<std::size_t>(std::max(triangles, 0)));
    wheel_contact_surfaces.resize(mesh_triangles.size());
    for (std::size_t index = 0U; index < mesh_triangles.size(); ++index) {
        const GuestAddress triangle = collision_triangle(memory, car, index);
        const auto surface = memory.read_u16_le(surfaces + static_cast<GuestAddress>(index) * 2U);
        mesh_triangles[index] = {load_quad_operand(memory, triangle), load_quad_operand(memory, triangle + 16U),
                                 load_quad_operand(memory, triangle + 32U), load_quad_operand(memory, triangle + 48U),
                                 static_cast<std::uint8_t>(surface & 0xffU)};
        wheel_contact_surfaces[index] = surface;
    }
    contact.mesh = mesh_triangles;
    contact.surfaces = wheel_contact_surfaces;
    const auto state = static_cast<std::int8_t>(memory.read_u8(at(BodyLayout::kState)));
    contact.rules.raced = state >= 1 && state <= 3;
    contact.rules.wrecked = memory.read_u8(at(BodyLayout::kGravityAtLever)) != 0U;
    contact.rules.up = call.transform.basis[1];
    contact.rules.marker_set = load_float(memory, kMarkerSet);
    contact.rules.marker_cleared = load_float(memory, kMarkerCleared);
    contact.marker = load_float(memory, model_info + kSpecialSurfaceMarker);
    contact.least_upward_normal = load_float(memory, kLeastUpwardNormal);

    call.search = engine::find_wheel_contacts(contact);
    call.instructions = 0U;
    for (std::size_t step = 0U; step < kWheelContactInstructions.size(); ++step) {
        call.instructions += call.search.passes[step] * static_cast<std::uint32_t>(kWheelContactInstructions[step]);
    }
    for (std::size_t index = 0U; index < static_cast<std::size_t>(count); ++index) {
        const engine::WheelContactWheel& wheel = contact.wheels[index];
        if (!wheel.probed) {
            continue;
        }
        for (std::size_t step = 0U; step < kMeshProbeInstructions.size(); ++step) {
            call.instructions += wheel.probe.passes[step] * static_cast<std::uint32_t>(kMeshProbeInstructions[step]);
        }
    }
    return true;
}

std::uint32_t wheel_contacts_cost(const NativeFunctionPrepareContext& context) {
    WheelContactCall& call = prepared_wheel_contacts;
    call.valid = evaluate_wheel_contacts(context.memory, context.cpu.read_gpr_u32(4U), call);
    return call.valid ? call.instructions : 0U;
}

// FindWheelContacts (original 0x001cdb70): the wheels' contacts and the
// ground under car a0 (engine::find_wheel_contacts).
NativeFunctionExecution find_wheel_contacts(NativeFunctionContext& context) {
    GuestCall call{context};
    auto& memory = call.memory();
    const GuestAddress car = call.arg(0);
    const auto at = [&](const GuestAddress offset) { return car + offset; };
    WheelContactCall& result = prepared_wheel_contacts;
    if (!result.valid || result.car != car) {
        evaluate_wheel_contacts(memory, car, result);
    }
    result.valid = false;
    const engine::WheelContactCar& contact = result.contact;
    const engine::WheelContactSearch& search = result.search;
    const auto count = static_cast<std::size_t>(contact.wheel_count);
    const GuestAddress transform = memory.read_u32_le(at(BodyLayout::kTransform));
    const guest::Gpr128 up_quad = load_quad(memory, transform + 16U);
    const guest::Gpr128 position_quad = load_quad(memory, transform + 48U);
    const auto quad_with = [](guest::Gpr128 quad, const engine::Vector3 xyz) {
        quad.words[0] = std::bit_cast<std::uint32_t>(xyz.x);
        quad.words[1] = std::bit_cast<std::uint32_t>(xyz.y);
        quad.words[2] = std::bit_cast<std::uint32_t>(xyz.z);
        return quad;
    };
    const auto store_quad = [&](const GuestAddress address, const guest::Gpr128& quad) {
        for (GuestAddress word = 0U; word < 4U; ++word) {
            memory.write_u32_le(address + word * 4U, quad.words[word]);
        }
    };
    const GuestAddress mesh = memory.read_u32_le(at(kCollisionMesh));
    const GuestAddress surfaces = memory.read_u32_le(mesh + CollisionMeshLayout::kSurfaces);

    for (std::size_t index = 0U; index < count; ++index) {
        const GuestAddress record = at(static_cast<GuestAddress>(index) * CarWheelLayout::kStride);
        store_quad(record + WheelContactLayout::kPreviousPosition,
                   load_quad(memory, record + SuspensionWheelLayout::kPosition));
        store_quad(record + WheelContactLayout::kPreviousContactPoint,
                   load_quad(memory, record + SuspensionWheelLayout::kContactPoint));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const GuestAddress record = at(static_cast<GuestAddress>(index) * CarWheelLayout::kStride);
        const engine::WheelContactWheel& wheel = contact.wheels[index];
        if (!wheel.grounded) {
            memory.write_u8(record + CarWheelLayout::kContact, 0U);
            memory.write_u8(record + SuspensionWheelLayout::kPushed, 0U);
            store_quad(record + SuspensionWheelLayout::kContactPoint, quad_with(up_quad, wheel.contact_point));
            store_quad(record + SuspensionWheelLayout::kContactNormal, up_quad);
            continue;
        }
        memory.write_u8(record + CarWheelLayout::kContact, 1U);
        const auto nearest = static_cast<GuestAddress>(wheel.probe.nearest);
        store_quad(record + SuspensionWheelLayout::kContactNormal,
                   wheel.steep ? up_quad : load_quad(memory, collision_triangle(memory, car, nearest) + 48U));
        if (wheel.surface_taken) {
            memory.write_u16_le(record + WheelContactLayout::kSurface, contact.surfaces[nearest]);
        }
        store_quad(record + SuspensionWheelLayout::kContactPoint, quad_with(position_quad, wheel.contact_point));
        store_float(memory, record + SuspensionWheelLayout::kGroundOffset, wheel.ground_offset);
    }
    const GuestAddress model_info = memory.read_u32_le(at(SuspensionLayout::kModelInfo));
    store_float(memory, model_info + kSpecialSurfaceMarker, search.marker);
    memory.write_u16_le(at(kCarSurface), memory.read_u16_le(at(WheelContactLayout::kSurface)));

    // The ground under the car.
    const GuestAddress ground = model_info + GroundBelowLayout::kGround;
    const guest::Gpr128 ground_before = [&] {
        guest::Gpr128 quad = load_quad(memory, ground);
        quad.words[3] = kNoGroundDistanceBits;
        return quad;
    }();
    guest::Gpr128 ground_quad = ground_before;
    if (search.ground_triangle >= 0) {
        const engine::Vector3 normal =
            load3(memory, collision_triangle(memory, car, static_cast<std::size_t>(search.ground_triangle)) + 48U);
        ground_quad = quad_with(ground_quad, engine::chop_add(engine::Vector3{}, normal));
        ground_quad.words[3] = std::bit_cast<std::uint32_t>(search.ground_distance);
        memory.write_u16_le(at(kCarSurface), memory.read_u16_le(surfaces + static_cast<GuestAddress>(
                                                                                search.ground_triangle) * 2U));
    }
    store_quad(ground, ground_quad);
    memory.write_u8(at(kContactsFound), 1U);

    // The vector unit as the last ground test leaves it (and, after a
    // nearer hit there, the ground record it wrote). VF3's fourth lane is
    // the top word of a GPR loaded with a wheel probe's start (the car
    // position's fourth lane) when a probe tested a triangle; otherwise (no
    // probe tested, or a wrecked car's slope checks came after) it is
    // whatever the register held before and is not reproduced.
    guest::R5900State& cpu = call.cpu();
    const bool probe_tested = std::any_of(contact.wheels.begin(), contact.wheels.begin() + count,
                                          [](const engine::WheelContactWheel& wheel) { return wheel.probe.tested; });
    if (probe_tested && !contact.rules.wrecked) {
        guest::Gpr128 lanes = cpu.read_vu0_vector(kVfDirectionQ);
        lanes.words[3] = position_quad.words[3];
        cpu.write_vu0_vector(kVfDirectionQ, lanes);
    }
    guest::Gpr128 end_quad = position_quad;
    end_quad.words[1] = std::bit_cast<std::uint32_t>(search.ground_end.y);
    // VF1 holds the ground record, or the last nearer hit's normal, under
    // the tests' margins.
    cpu.write_vu0_vector(kVfMargins, search.ground_triangle >= 0
                                         ? load_quad(memory, collision_triangle(memory, car, static_cast<std::size_t>(
                                                                                                 search.ground_triangle)) +
                                                                 48U)
                                         : ground_before);
    if (search.ground_solved) {
        leave_solved_test_registers(cpu, search.last_ground_solved);
    } else {
        for (std::size_t index = count; index-- > 0U;) {
            if (contact.wheels[index].probe.solved) {
                leave_solved_test_registers(cpu, contact.wheels[index].probe.last_solved);
                break;
            }
        }
    }
    if (search.ground_tests != 0U) {
        const GuestAddress last = collision_triangle(memory, car, contact.mesh.size() - 1U);
        leave_segment_test(cpu, memory, position_quad, end_quad, last, last + 16U, last + 32U,
                           search.ground_start, search.last_ground_test);
        if (search.last_ground_test_nearer) {
            cpu.write_vu0_vector(kVfMargins, load_quad(memory, last + 48U));
            cpu.write_vu0_vector(kVfStart, ground_quad);
        }
    } else {
        // No triangles: the last wheel probe's length is still in ACC and Q.
        cpu.write_vu0_vector(kVfDirection, end_quad);
        set_vu0_lanes(cpu, kVfStart, {30.0F, 0.0F, std::nullopt, std::nullopt});
        set_vu0_lanes(cpu, kVfDirectionQ, {9999.0F, 0.0F, std::nullopt, std::nullopt});
        for (std::size_t index = count; index-- > 0U;) {
            const engine::WheelContactWheel& wheel = contact.wheels[index];
            if (wheel.probed) {
                guest::Gpr128 acc = cpu.vu0_accumulator();
                acc.words[0] = std::bit_cast<std::uint32_t>(
                    engine::chop_add(wheel.probe.length_terms.x, wheel.probe.length_terms.y));
                cpu.set_vu0_accumulator(acc);
                cpu.write_vu0_control(kVu0ControlQ, std::bit_cast<std::uint32_t>(wheel.probe.length));
                break;
            }
        }
    }
    return call.finish(result.instructions);
}

constexpr std::array kFunctions{
    EngineFunction{0x001cdb70U, 1580U,0xf0f30d292ad87007ULL, "FindWheelContacts", 1000000U, find_wheel_contacts,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, wheel_contacts_cost},
    EngineFunction{0x00274c90U, 300U, 0x09205d08c72f7452ULL, "SegmentHitsTriangle", 49U, segment_hits_triangle,
                   EngineReturn::Integer, {}, 1e-4, 1e-6, {}, segment_test_cost},
    EngineFunction{0x001ce1a0U, 676U, 0xd8284dbda9f0a70aULL, "ProbeCollisionMesh", 60000U, probe_collision_mesh,
                   EngineReturn::Integer, {}, 1e-4, 1e-6, {}, mesh_probe_cost},
    EngineFunction{0x001cc900U, 4720U, 0xcdbbfaae298af06cULL, "UpdateSuspension", 6000U, update_suspension,
                   EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, suspension_cost},
    EngineFunction{0x001c6f30U, 92U, 0x8c29677e9519a462ULL, "PickGear", 80U, pick_gear, EngineReturn::Integer},
    EngineFunction{0x001ab610U, 156U, 0xb696b3d1223b6512ULL, "SetCarTransform", 39U, set_car_transform_call,
                   EngineReturn::Nothing},
    EngineFunction{0x001bed00U, 472U, 0x60f5bd096e8b9384ULL, "SetCarSpeed", 250U, set_car_speed_call,
                   EngineReturn::Nothing},
    EngineFunction{0x00388a90U, 96U, 0xe4230eebd68873f9ULL, "InitialiseContactRecord", 30U, initialise_contact_record,
                   EngineReturn::Nothing},
    EngineFunction{0x001c38f0U, 3056U, 0x586aaf582aaf807dULL, "StepPlanarTyres", 2500U, step_planar_tyres,
                   EngineReturn::Nothing},
    EngineFunction{0x001c1b20U, 7632U, 0xbcdeb01f60b1bffaULL, "DriveCar", 7200U, drive_car, EngineReturn::Nothing,
                   kDriveUnusedWords, kCheckTolerance, 1e-6, kDriveLooseWords},
    EngineFunction{0x001ac470U, 3856U, 0xe5b16f86fbe3d4d3ULL, "IntegrateRigidBody", 960U, integrate_rigid_body,
                   EngineReturn::Nothing, kUnusedWords, kCheckTolerance},
};

} // namespace

std::uint32_t set_car_transform(guest::EeMemory& memory, const GuestAddress car,
                                const std::array<engine::Vector4, 4>& rows) {
    const GuestAddress transform = memory.read_u32_le(car + BodyLayout::kTransform);
    for (GuestAddress row = 0U; row < 4U; ++row) {
        store_vector4(memory, transform + row * 16U, rows[row]);
    }
    // The inverse: the transposed rotation (each row keeping its own fourth
    // lane; the moved entries pass through an add of zero, so a negative zero
    // turns positive) and minus the position through it (with the up row's
    // fourth lane).
    const auto moved = [](const float value) { return engine::chop_add(0.0F, value); };
    const engine::Vector3 columns[3] = {{rows[0].x, moved(rows[1].x), moved(rows[2].x)},
                                        {moved(rows[0].y), rows[1].y, moved(rows[2].y)},
                                        {moved(rows[0].z), moved(rows[1].z), rows[2].z}};
    const engine::Vector3 position = rows[3].xyz();
    const engine::Vector3 inverse = engine::chop_sub(
        engine::Vector3{},
        engine::chop_add(engine::chop_add(engine::chop_mul(columns[0], position.x),
                                          engine::chop_mul(columns[1], position.y)),
                         engine::chop_mul(columns[2], position.z)));
    const GuestAddress out = car + BodyLayout::kInverseTransform;
    for (GuestAddress row = 0U; row < 3U; ++row) {
        store3w(memory, out + row * 16U, columns[row], rows[row].w);
    }
    store3w(memory, out + 48U, inverse, rows[1].w);
    return 39U;
}

std::uint32_t set_car_speed(guest::EeMemory& memory, const GuestAddress car, const float speed,
                            const float unchanged_engine_rate) {
    const GuestAddress transform = memory.read_u32_le(car + BodyLayout::kTransform);
    const engine::Vector4 forward = load_vector4(memory, transform + 32U);
    const auto motion = engine::motion_along(forward.xyz(), speed);
    store3w(memory, car + BodyLayout::kVelocity, motion.velocity, motion.speed);
    store3w(memory, car + BodyLayout::kHeading, motion.heading, forward.w);
    const auto wheels = static_cast<std::int8_t>(memory.read_u8(car + CarLayout::kWheelCount));
    for (std::int32_t wheel = 0; wheel < wheels; ++wheel) {
        const GuestAddress record = car + static_cast<GuestAddress>(wheel) * CarWheelLayout::kStride;
        store_float(memory, record + CarWheelLayout::kAngularSpeed,
                    engine::game_div(motion.speed, load_float(memory, record + CarWheelLayout::kRadius)));
    }
    const float wheel_rate = engine::game_div(motion.speed, load_float(memory, car + kWheelRadiusForGears));
    const auto pick = pick_car_gear(memory, car + kGearbox, wheel_rate, unchanged_engine_rate);
    store_float(memory, car + kGearbox + GearboxLayout::kEngineRate, pick.engine_rate);
    memory.write_u32_le(car + kGearbox + GearboxLayout::kGear, static_cast<std::uint32_t>(pick.gear));
    for (std::size_t word = 0U; word < kSetSpeedResets.size(); ++word) {
        memory.write_u32_le(car + kSetSpeedResets[word], kSetSpeedResetWords[word]);
    }
    // The original's instruction count, path by path.
    const std::uint32_t wheel_count = wheels > 0 ? static_cast<std::uint32_t>(wheels) : 0U;
    const bool flushed = (std::bit_cast<std::uint32_t>(speed) & 0x7f800000U) < 0x37800001U;
    return (motion.moving ? 98U : 81U) + (flushed ? 1U : 0U) + 12U * wheel_count +
           pick_gear_instructions(memory, car + kGearbox, pick.gear);
}

std::span<const EngineFunction> physics_engine_functions() { return kFunctions; }

} // namespace takedown::runtime
