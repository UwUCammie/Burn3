#pragma once

// A segment probed against a car's local collision triangles (the ground
// and walls around it): the nearest triangle it crosses and how far along
// it, in the game's rounding (engine/math/segment_triangle.hpp).
//
// Racing cars only probe ordinary surfaces (below 21) and special surface
// 38; wrecked cars probe every surface but skip triangles that face less
// than 75 degrees from the car's up axis (their normal's dot with it below
// 0.25). Every triangle passed also updates the car's special surface
// marker: surface 38 sets it, any other turns a set marker to its cleared
// value.

#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::engine {

inline constexpr std::uint8_t kSpecialSurface = 38U;

struct CollisionTriangle {
    Vector3 a;
    Vector3 b;
    Vector3 c;
    Vector3 normal;
    std::uint8_t surface{};
};

struct MeshProbeRules {
    bool raced{};                   // a player's or a racer's car
    bool wrecked{};
    Vector3 up;
    float marker_set{};             // the marker values for surface 38 and after it
    float marker_cleared{};
};

// Points the probe passes (for a caller that accounts for work by path).
enum class MeshProbeStep : std::uint8_t {
    Start,
    TriangleCheck,
    TriangleStart,
    AnySurface,
    WreckedAnySurface,
    SpecialSurface,
    LowSurface,
    MiddleSurface,
    HighSurface,
    WreckCheck,
    Slope,
    Steep,
    TestCheck,
    Test,
    Miss,
    Hit,
    Nearer,
    MarkerCheck,
    MarkerSpecial,
    MarkerOther,
    MarkerCleared,
    TriangleDone,
    Finish,
    Count,
};

struct MeshProbe {
    std::int32_t nearest{-1};
    float distance{};            // along the segment, 999 lengths when nothing was crossed
    float marker{};
    std::array<std::uint32_t, static_cast<std::size_t>(MeshProbeStep::Count)> passes{};
    bool tested{};               // at least one triangle was tested
    SegmentTriangleTest last_test;
    std::size_t last_tested{};   // its triangle
    bool solved{};               // a test got past the determinant check
    SegmentTriangleTest last_solved;
    float length_squared{};      // of the segment, as summed for its length
    Vector3 length_terms;        // the squared components
    float length{};

    void pass(const MeshProbeStep step) noexcept { ++passes[static_cast<std::size_t>(step)]; }
};

[[nodiscard]] MeshProbe probe_mesh(std::span<const CollisionTriangle> triangles, Vector3 start, Vector3 end,
                                   const MeshProbeRules& rules, float marker);

} // namespace takedown::engine
