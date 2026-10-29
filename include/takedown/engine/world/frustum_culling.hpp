#pragma once

// Box-versus-frustum culling for world objects.

#include <xmmintrin.h>

#include <cstdint>

namespace takedown::engine {

// Four float lanes, x y z w.
using Vec4 = __m128;

// How much of a bounding box lies inside a view frustum.
enum class Visibility : std::uint8_t {
    Outside = 0,      // every corner is outside one plane: skip the object
    Intersecting = 1, // partly visible: draw with clipping
    Inside = 2,       // fully visible: draw without clipping
};

// An oriented bounding box: one corner and three edge vectors (xyz lanes).
struct OrientedBox {
    Vec4 origin;
    Vec4 edge_a;
    Vec4 edge_b;
    Vec4 edge_c;
};

// A view frustum as the game stores it (96 bytes per view). A point p is
// outside a plane when distance - normal . p is negative. The four side planes
// are stored transposed, one plane per lane; the two depth planes are stored
// one per row (xyz = normal, w = distance).
struct Frustum {
    Vec4 near_plane;
    Vec4 far_plane;
    Vec4 side_normal_x;
    Vec4 side_normal_y;
    Vec4 side_normal_z;
    Vec4 side_distance;
};

// Outside when all eight corners are outside one plane, Inside when no corner
// is outside any plane, Intersecting otherwise. A distance of exactly -0 counts
// as outside, like the original's sign test.
[[nodiscard]] Visibility classify(const OrientedBox& box, const Frustum& frustum) noexcept;

} // namespace takedown::engine
