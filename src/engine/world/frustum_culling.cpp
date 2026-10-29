#include "takedown/engine/world/frustum_culling.hpp"

namespace takedown::engine {

namespace {

// distance - normal . corner for four planes at once (normals transposed, one
// plane per lane). A negative lane means the corner is outside that plane.
[[nodiscard]] Vec4 plane_distances(const Vec4 corner, const Vec4 distance, const Vec4 normal_x,
                                   const Vec4 normal_y, const Vec4 normal_z) noexcept {
    Vec4 result = _mm_sub_ps(distance, _mm_mul_ps(normal_x, _mm_shuffle_ps(corner, corner, 0x00)));
    result = _mm_sub_ps(result, _mm_mul_ps(normal_y, _mm_shuffle_ps(corner, corner, 0x55)));
    return _mm_sub_ps(result, _mm_mul_ps(normal_z, _mm_shuffle_ps(corner, corner, 0xAA)));
}

} // namespace

// RECOVERY: statements, operands, types and control flow match the original
// GIMPLE exactly; only the internal SSA version order of a few vector temporaries
// differs (e.g. the origin+edge_c / origin+edge_a sums and the near/far transpose
// temporaries), which the checker still reports as a difference. Several
// restructurings (nested initializers, extra named corners, inlined plane maths,
// brace initialisation) were tried without reproducing the original numbering.
Visibility classify(const OrientedBox& box, const Frustum& frustum) noexcept {
    const Vec4 origin_c = _mm_add_ps(box.origin, box.edge_c);
    const Vec4 origin_a = _mm_add_ps(box.origin, box.edge_a);
    const Vec4 corners[8] = {
        box.origin,
        origin_c,
        _mm_add_ps(origin_c, box.edge_a),
        origin_a,
        _mm_add_ps(box.origin, box.edge_b),
        _mm_add_ps(origin_c, box.edge_b),
        _mm_add_ps(_mm_add_ps(origin_c, box.edge_a), box.edge_b),
        _mm_add_ps(origin_a, box.edge_b),
    };

    // One bit per plane: set in outside_every while every corner so far is
    // outside that plane, set in outside_some once any corner is.
    int outside_every = 0xF;
    int outside_some = 0;
    for (const Vec4 corner : corners) {
        const int mask = _mm_movemask_ps(plane_distances(corner, frustum.side_distance, frustum.side_normal_x,
                                                         frustum.side_normal_y, frustum.side_normal_z));
        outside_every &= mask;
        outside_some |= mask;
    }
    if (outside_every != 0) {
        return Visibility::Outside;
    }

    // Transpose the near and far planes into lanes 0 and 1.
    const Vec4 depth_xy = _mm_unpacklo_ps(frustum.near_plane, frustum.far_plane);
    const Vec4 depth_zw = _mm_unpackhi_ps(frustum.near_plane, frustum.far_plane);
    const Vec4 depth_y = _mm_movehl_ps(depth_xy, depth_xy);
    const Vec4 depth_w = _mm_movehl_ps(depth_zw, depth_zw);
    outside_every = 0x3;
    for (const Vec4 corner : corners) {
        const int mask = _mm_movemask_ps(plane_distances(corner, depth_w, depth_xy, depth_y, depth_zw)) & 0x3;
        outside_every &= mask;
        outside_some |= mask;
    }
    if (outside_every != 0) {
        return Visibility::Outside;
    }
    return outside_some == 0 ? Visibility::Inside : Visibility::Intersecting;
}

} // namespace takedown::engine
