#pragma once

// A perspective camera's view frustum: its eight corners and six bounding
// planes, as the renderer's culling consumes them.

#include "takedown/engine/math/vector.hpp"

#include <cstdint>

namespace takedown::engine {

struct PerspectiveView {
    Vector3 right;
    Vector3 up;
    Vector3 at;
    Vector3 position;
    float window_x{};  // half extents of the view window at unit distance
    float window_y{};
    float offset_x{};  // view window offset (off-centre projection)
    float offset_y{};
    float near_distance{};
    float far_distance{};
};

struct FrustumPlane {
    Vector3 normal;   // unit length, pointing out of the frustum
    float distance{};
    // Per axis: 1 when the normal's component is not negative, which picks
    // the box corner closest to the plane.
    std::uint8_t closest[3]{};
};

struct ViewFrustum {
    // Near corners 0-3, then the far corners 4-7 in the same order:
    // +x+y, -x+y, -x-y, +x-y in view space.
    Vector3 corners[8];
    // Far, near, then the four sides.
    FrustumPlane planes[6];
};

// RenderWare's frustum (RwCamera): side plane normals normalised by division.
[[nodiscard]] ViewFrustum build_view_frustum(const PerspectiveView& view) noexcept;

// The game's own frustum for four-wide plane tests: the far and near planes,
// then the four side planes (same corners and orientation as above,
// normalised with a reciprocal square root) as columns.
// Four side planes as columns: plane i is (x[i], y[i], z[i], distance[i]).
struct SidePlanes {
    float x[4]{};
    float y[4]{};
    float z[4]{};
    float distance[4]{};
};

struct PackedFrustum {
    Vector3 far_normal;
    float far_distance{};
    Vector3 near_normal;
    float near_distance{};
    SidePlanes sides;
};

[[nodiscard]] PackedFrustum build_packed_frustum(const PerspectiveView& view) noexcept;

// The side planes of the frustum widened to the rasteriser's guard band: a
// screen of `width` by `height` pixels whose coordinates may reach
// `guard_limit` around its centre before clipping is needed.
[[nodiscard]] SidePlanes build_guard_band_planes(const PerspectiveView& view, float width, float height,
                                                 float guard_limit) noexcept;

} // namespace takedown::engine
