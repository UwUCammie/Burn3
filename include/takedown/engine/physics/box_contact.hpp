#pragma once

// Contact between two boxes, and between a mesh's triangles and a box, in the
// game's rounding (engine/math/chopped.hpp).
//
// A box is stored as two corners, the larger one first (+0) and the smaller
// one 16 bytes on. A pair of boxes is tested by their 15 separating axes (the
// first box's three axes, the second box's three, and the nine pairs of
// them): the first box is described in its own frame and a matrix places it
// in the second box's frame, whose axes are the world axes. The axis of least
// overlap gives the contact normal, depth and a contact point.
//
// Several of the original's results are lanes it leaves holding whatever its
// registers last held (the w of a cross product, of a sum). They are not
// data, but the original writes them to the caller's memory, so the callers
// pass the values that reach those lanes.

#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace takedown::engine {

struct BoxContactConstants {
    float axis_epsilon{};       // an edge pair this parallel gives no contact
    float face_low{};           // a box axis this far below the direction counts as -1
    float face_high{};          // ... above it as +1
    float depth_start{};        // the depth a face or edge must beat
    std::array<Vector4, 3> world_axes{};
    std::array<std::int8_t, 3> edge_order{};  // which axis each of a box's three edge groups uses
};

struct BoxPair {
    Vector4 first_upper;
    Vector4 first_lower;
    Vector4 second_upper;
    Vector4 second_lower;
    std::array<Vector4, 4> frame;  // three axes and the origin of the first box in the second box's frame
};

struct BoxContact {
    bool hit{};
    bool has_contact{};
    float depth{};
    Vector4 point;
    Vector4 normal;
    std::uint32_t axes_tested{};
    std::int32_t axis{-1};  // the axis the contact is along (0-14)
};

// Lanes the original leaves to chance (see above): the w of the normal for
// each of the nine edge pair axes, and the w of the contact point for an edge
// pair.
struct BoxContactStaleLanes {
    std::array<float, 9> edge_normal_w{};
    float edge_point_w{};
};

BoxContact test_box_pair(const BoxPair& pair, bool want_contact, const BoxContactConstants& constants,
                         const BoxContactStaleLanes& stale) noexcept;

// A mesh polygon: three vertices and the polygon's plane (xyz = normal).
struct MeshPolygon {
    std::array<Vector4, 3> vertices;
    Vector4 plane;
    std::uint16_t id{};
};

struct MeshBoxConstants {
    float distance_floor{};          // smallest distance reported
    Vector4 minimum_start;           // starting corners of the clipped vertices' bounds
    Vector4 maximum_start;
    float normal_epsilon{};          // a summed normal this short is no contact
};

struct MeshBoxResult {
    bool hit{};
    bool has_bounds{};               // the bounds were written (some polygon touched the box)
    std::uint16_t last_id{};
    Vector4 bounds_minimum;
    Vector4 bounds_maximum;
    bool has_contact{};
    Vector4 point;                   // mean of the touching polygons' centres
    Vector4 normal;                  // their summed normals, unit length
    Vector4 push;                    // the normal times the distance to the box's face
};

// Clips every polygon (placed by `to_box`, the world to box matrix) to the
// box; `polygon(index)` returns polygon `index`. `to_world` places a clipped
// polygon's centre back; `leftover_w` is the w lane the sums start with.
MeshBoxResult test_mesh_against_box(std::size_t polygon_count, const std::function<MeshPolygon(std::size_t)>& polygon,
                                    const std::array<Vector4, 4>& to_box, const std::array<Vector4, 4>& to_world,
                                    const Vector4& box_upper, const Vector4& box_lower, float leftover_w,
                                    const MeshBoxConstants& constants) noexcept;

} // namespace takedown::engine
