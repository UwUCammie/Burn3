#pragma once

// Where a car's wheels touch the ground, found each frame before the
// suspension runs (engine/physics/suspension.hpp), in the game's rounding.
//
// Each wheel probes the car's collision triangles (engine/physics/
// mesh_probe.hpp) along the car's down axis, from its radius above its rest
// position to three quarters of its travel and its radius below (a tilted
// wheel a quarter of its radius less). A wheel that finds a triangle gets
// the contact point and height, the triangle's normal (the car's up axis
// for a steep one) and, for ordinary surfaces and surface 38, the surface;
// one that does not (or is missing) loses contact and takes the point a
// radius below its centre. Then a segment from the car's centre to 30 below
// it finds the ground under the car: its normal, distance and surface.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/math/vector.hpp"
#include "takedown/engine/physics/mesh_probe.hpp"
#include "takedown/engine/physics/suspension.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::engine {

struct WheelContactWheel {
    WheelMount mount{};
    Vector3 model_position;      // the wheel model's position on the car
    float rest_offset{};
    float radius{};
    Vector3 position;            // the wheel's point in the world (from the last suspension step)
    // Set by the search.
    float top_height{};          // the probe's ends on the car
    float bottom_height{};
    Vector3 top;                 // and in the world
    Vector3 bottom;
    MeshProbe probe;             // when the wheel probed
    bool probed{};
    bool grounded{};
    bool steep{};                // the normal is the car's up axis instead of the triangle's
    bool surface_taken{};        // an ordinary surface or surface 38: the wheel keeps its id
    Vector3 contact_point;
    float ground_offset{};
};

struct WheelContactCar {
    const Transform* transform{};
    float front_travel{};
    float rear_travel{};
    std::int32_t wheel_count{};  // at least two
    std::array<WheelContactWheel, kSuspensionWheels> wheels;
    std::span<const CollisionTriangle> mesh;
    std::span<const std::uint16_t> surfaces;  // per triangle: the surface id in the low byte
    MeshProbeRules rules;
    float marker{};
    float least_upward_normal{};  // a triangle normal with less up than this counts as steep
};

// Points the search passes, for a caller that accounts for work by path
// (each wheel's probe reports its own).
enum class WheelContactStep : std::uint8_t {
    Start,
    SaveCheck,
    Save,
    EndsSetUp,
    EndsCheck,
    Ends,
    FrontTravel,
    RearTravel,
    EndsHeights,
    Upright,
    Tilted,
    TopsSetUp,
    TopsCheck,
    Top,
    BottomsSetUp,
    BottomsCheck,
    Bottom,
    Down,
    ContactCheck,
    ContactStart,
    NoContact,
    Probe,
    ProbeResult,
    Contact,
    SteepNormal,
    TriangleNormal,
    SurfaceCheck,
    LowCheck,
    Low,
    NotLow,
    LowBranch,
    MiddleCheck,
    MiddleRangeCheck,
    Middle,
    NotMiddle,
    MiddleBranch,
    SpecialCheck,
    SurfaceTaken,
    ContactPoint,
    WheelDone,
    GroundSetUp,
    GroundCheck,
    GroundTest,
    GroundMiss,
    GroundHit,
    GroundNearer,
    GroundNext,
    Finish,
    Count,
};

struct WheelContactSearch {
    std::array<std::uint32_t, static_cast<std::size_t>(WheelContactStep::Count)> passes{};
    Vector3 down;                // the probe direction and length shared by the wheels
    Vector3 ground_start;
    Vector3 ground_end;
    std::int32_t ground_triangle{-1};
    float ground_distance{};     // 30 times the nearest crossing (9999 for none)
    std::uint32_t ground_tests{};
    std::uint32_t ground_hits{};
    SegmentTriangleTest last_ground_test;
    bool ground_solved{};        // a ground test got past the determinant check
    SegmentTriangleTest last_ground_solved;
    bool last_ground_test_nearer{};
    float marker{};

    void pass(const WheelContactStep step, const std::uint32_t times = 1U) noexcept {
        passes[static_cast<std::size_t>(step)] += times;
    }
};

[[nodiscard]] WheelContactSearch find_wheel_contacts(WheelContactCar& car);

} // namespace takedown::engine
