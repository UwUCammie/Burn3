#pragma once

// A car's suspension for one frame, run after the drive model. Per wheel:
//  - on the ground: the spring and damper push along the contact normal at
//    the wheel's point; a wheel driven past a quarter of its travel is held
//    there, and the car is moved out by the excess and stopped from moving
//    further into the ground by an impulse along its up axis; wrecked cars
//    also drag against the motion of the contact point;
//  - in the air: the wheel drops toward its rest position.
// Then the impulses and corrections are shared out between the wheels that
// took them, and each wheel's model turns with its spin (and the front pair
// with the steering), scaled to the wheel's size.
//
// Everything follows the game's own rounding (engine/math/chopped.hpp):
// the spin feeds back every frame.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"
#include "takedown/engine/physics/rigid_body.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::engine {

inline constexpr std::size_t kSuspensionWheels = 6U;
// Only these first wheels take a share of the impulse (later wheels that
// took one still count toward the sharing).
inline constexpr std::size_t kSuspensionSharingWheels = 4U;

struct SuspensionAxle {
    float damping{};
    float stiffness{};
    float travel{};
};

enum class WheelMount : std::uint8_t {
    Standard,
    Tilted,   // the model leans out (not ported: such a car is left to the caller)
    Missing,  // knocked off: the wheel is skipped
};

struct SuspensionWheel {
    WheelMount mount{};
    bool grounded{};
    Vector3 contact_normal;
    Vector3 contact_point;      // where the contact drag acts
    float offset{};             // the wheel's height below its mount, kept
    float ground_offset{};      // where the ground holds it this frame
    float rest_offset{};
    float spin_angle{};         // radians, kept within +-100 turns
    float spin_speed{};
    float width{};              // the model's scale across and around
    float size{};
    Transform model;            // the wheel model's frame, rebuilt here
    // Set by the update.
    Vector3 position;           // the wheel's point in the world
    bool ground_jumped{};       // the ground moved more than a jump's worth
    bool pushed{};              // took a share of the impulse
    bool dragged{};             // came to the contact drag (wrecked, drag on)
    Vector3 push;               // its impulse along up
};

struct SuspensionCar {
    RigidBody* body{};
    SuspensionAxle front;
    SuspensionAxle rear;
    float steering_degrees{};
    std::int32_t wheel_count{};
    bool gravity_at_lever{};    // wrecked: softer landings, contact drag, spin dies away
    bool raced{};               // a player's or a racer's car
    bool player{};
    bool contact_drag_off{};
    bool full_drag_unraced{};   // other cars drag at half strength without it
    bool slowed_time{};         // the damper rate uses 1.2 frame times
    std::array<SuspensionWheel, kSuspensionWheels> wheels;
};

// Points the update passes, for a caller that accounts for work by path
// (the adapter maps them to the original's instruction counts).
enum class SuspensionStep : std::uint8_t {
    Start,
    NoWheels,
    Setup,
    WheelCheck,
    WheelStart,
    WheelDone,
    WheelPresent,
    FrontAxle,
    RearAxle,
    ContactCheck,
    Grounded,
    GroundJump,
    TravelCheck,
    Bottomed,
    BottomedSlow,
    BottomedPush,
    CorrectionCheck,
    Correction,
    NotBottomed,
    LeverClampCheck,
    LeverClamp,
    RateCheck,
    RateSlowed,
    Rate,
    Spring,
    LeverFadeCheck,
    LeverFade,
    SpringApplied,
    Push,
    PushCheck,
    PushLeverCheck,
    PushStateCheck,
    PushAxleCheck,
    PushStrengthCheck,
    DragStart,
    DragFlagCheck,
    DragSpeedCheck,
    Drag,
    DragHalfCheck,
    DragHalf,
    DragApplied,
    Airborne,
    AirborneLow,
    AirborneCheck,
    AirborneHigh,
    AirbornePosition,
    CorrectionCountCheck,
    CorrectionShared,
    PushCountCheck,
    PushProjected,
    PushShareCheck,
    PushShare,
    PushWheelCheck,
    PushWheel,
    PushWheelNext,
    Steering,
    SpinCheck,
    SpinStart,
    SpinMissing,
    SpinLever,
    SpinRacedCheck,
    SpinDamp,
    SpinDampGrounded,
    SpinAdvance,
    SpinWrapHigh,
    SpinWrapLowCheck,
    SpinWrapLow,
    SpinReduce,
    SpinReduceHigh,
    SpinSine,
    SpinCosineCheck,
    SpinCosineFlip,
    SpinAxleCheck,
    SpinFront,
    SpinRear,
    SpinSideCheck,
    SpinLeft,
    SpinRight,
    SpinRows,
    SpinScale,
    Finish,
    Count,
};

struct SuspensionTrace {
    std::array<std::uint16_t, static_cast<std::size_t>(SuspensionStep::Count)> passes{};
    // Front wheels of the player's wrecked car pushed harder than 2000:
    // the original starts a landing effect for each.
    std::uint32_t hard_landings{};

    void pass(const SuspensionStep step) noexcept { ++passes[static_cast<std::size_t>(step)]; }
};

// Runs the suspension over `dt` seconds. Returns false, leaving the car
// unchanged, for what is not ported: a tilted wheel, or a sharing wheel past
// the wheel count that is still marked as pushed from before (the original
// would read a stale impulse).
bool update_suspension(SuspensionCar& car, float dt, SuspensionTrace& trace);

} // namespace takedown::engine
