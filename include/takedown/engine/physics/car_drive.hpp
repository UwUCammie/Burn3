#pragma once

// The car's drive model for one frame, run before the rigid body is
// integrated. It tracks take-off and landing (a landing at an angle starts a
// slide), adds rolling and steering drag, and then, by state:
//  - airborne: four probes along the car damp its vertical and sideways
//    motion, roll is damped and a speed-dependent extra weight pulls it down;
//  - slow (or held): the planar tyre model takes over (see planar_tyres.hpp);
//  - on the ground: the engine or coasting force, then per wheel a combined
//    slip tyre model (the front pair turned by the steering) giving traction
//    and cornering forces, a slide model while drifting, a yaw assist that
//    limits the yaw the tyres add, and finally extra weight while moving.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstdint>

namespace takedown::engine {

inline constexpr std::size_t kMaxCarWheels = 6U;

// Slide (drift) directions.
inline constexpr std::uint32_t kSlideNone = 0U;
inline constexpr std::uint32_t kSlideLeft = 1U;
inline constexpr std::uint32_t kSlideRight = 2U;

struct CarDriveWheel {
    float mount_x{};        // the wheel's local position across the car
    float mount_z{};        // and along it
    float height{};         // the suspension's local height
    float radius{};
    float drive_torque{};   // gathered this frame, consumed here
    float angular_speed{};
    bool contact{};
};

// Per-car handling values.
struct CarDriveTuning {
    float rolling_drag{};
    float airborne_weight{};     // extra weight per mph while airborne
    float drive_height{};        // where the engine force acts, along up
    float traction_height{};
    float grip_height{};         // where cornering forces act
    float slide_grip_height{};   // the same while sliding
    float drive_torque{};
    float slide_timer_start{};
    float yaw_assist_fast{};     // the yaw limit at 110 mph and above
    float yaw_assist_slow{};     // at 60 mph
    float slide_steer_gain{};
    float planar_speed{};        // mph below which the planar model runs
    float steer_drag{};
    float coast_drag{};
    float slide_exit{};          // heading against right that keeps a slide
    float drag_cut_speed{};      // mph
    float roll_damping{};        // airborne
    float slide_entry{};         // heading against right that starts one
    float slide_yaw_rate{};
    float probe_near{};          // airborne probe positions along the car
    float probe_far{};
};

struct CarDrive {
    // The rigid body.
    float mass{};
    Vector3 velocity;
    float speed{};
    Vector3 heading;             // the direction of travel
    Vector3 angular_velocity;
    Vector3 angular_momentum;    // updated
    Vector3 force;               // updated
    Vector3 torque;              // updated
    Transform transform;         // right, up, forward rows
    Vector3 gravity;

    std::array<CarDriveWheel, kMaxCarWheels> wheels{};
    std::int32_t wheel_count{};
    float front_wheel_angle{};   // degrees
    float throttle{};
    float drive_torque_scale{};
    float steering{};            // -1 to 1
    float drive_gain{};          // each rear wheel takes half
    float wheel_rpm_source{};    // times 9.55 against engine_rpm
    float engine_rpm{};

    // Updated state.
    std::uint8_t airborne{};
    std::uint8_t was_airborne{};
    float airtime{};
    std::uint32_t lost_grip{};
    std::uint32_t slide{};
    float slide_progress{};      // 0 to 1
    float slide_timer{};
    float slide_blend{};         // slide model against tyre model

    bool scripted{};
    bool traffic{};              // traffic: traction gives no torque
    bool no_steer_drag{};
    std::uint32_t hold{};        // raw words; all ones holds the car
    std::uint32_t hold_other{};
    float traffic_traction{};    // the traction multiplier for traffic
    bool settle_heavy_spin{};    // a heavy vehicle loses its spin about up

    // The front wheels' frame turned by the steering (written on the ground).
    Transform steered;
};

enum class CarDriveResult : std::uint8_t {
    Grounded,
    Airborne,
    PlanarTyres,  // run the planar tyre model, then press_car_down
};

[[nodiscard]] CarDriveResult drive_car(CarDrive& car, const CarDriveTuning& tuning, float dt) noexcept;

// The extra weight of a car moving forward on the ground.
void press_car_down(CarDrive& car) noexcept;

} // namespace takedown::engine
