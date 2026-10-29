#include "takedown/engine/world/view_frustum.hpp"

#include <bit>


namespace takedown::engine {

namespace {

// scalar * vector, with the scalar as the left operand like the original's maths.
[[nodiscard]] Vector3 scaled(const float s, const Vector3 a) noexcept { return {s * a.x, s * a.y, s * a.z}; }

// Cross product of two edges, taken by reference so the temporaries stay unnamed.
[[nodiscard]] Vector3 cross_edges(const Vector3& a, const Vector3& b) noexcept {
    return {a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x, a.x * b.y - b.x * a.y};
}

// Directions through the window corners, then the near and far corners along
// them from the (offset) eye.
void build_corners(const PerspectiveView& view, Vector3 (&corners)[8]) noexcept {
    const Vector3 offset = scaled(-view.offset_x, view.right) + scaled(view.offset_y, view.up);
    const Vector3 half_right = scaled(view.window_x, view.right);
    const Vector3 half_up = scaled(view.window_y, view.up);
    const Vector3 full_right = half_right * 2.0F;
    const Vector3 full_up = half_up * 2.0F;
    Vector3 directions[4];
    directions[0] = (view.at + half_right) + half_up;
    directions[1] = directions[0] - full_right;
    directions[2] = directions[1] - full_up;
    directions[3] = directions[2] + full_right;
    const Vector3 origin = offset + view.position;
    for (int corner = 0; corner != 4; ++corner) {
        const Vector3 direction = directions[corner] - offset;
        corners[corner] = origin + scaled(view.near_distance, direction);
        corners[corner + 4] = origin + scaled(view.far_distance, direction);
    }
}

// The four side planes (normalised with a reciprocal square root) as columns.
SidePlanes build_side_planes(const Vector3 (&corners)[8]) {
    const Vector3& near_left = corners[1];
    const Vector3& near_right = corners[3];
    const Vector3& far_left = corners[4];
    const Vector3& far_right = corners[6];
    const Vector3& left_base = corners[5];
    const Vector3& right_base = corners[7];
    const Vector3 left_edge = near_left - left_base;
    const Vector3 right_edge = near_right - right_base;
    const Vector3 normals[4] = {
        cross_edges(left_edge, far_right - left_base),
        cross_edges(far_left - left_base, left_edge),
        cross_edges(right_edge, far_left - right_base),
        cross_edges(far_right - right_base, right_edge),
    };
    const Vector3 points[4] = {near_left, near_left, near_right, near_right};
    SidePlanes planes{};
    for (int side = 0; side != 4; ++side) {
        const Vector3 normal = normals[side] * reciprocal_sqrt(dot(normals[side], normals[side]));
        planes.x[side] = normal.x;
        planes.y[side] = normal.y;
        planes.z[side] = normal.z;
        planes.distance[side] = dot(points[side], normal);
    }
    return planes;
}

// Same corners as build_corners, written for the packed frustum (called once,
// so it is folded into build_packed_frustum).
void fill_packed_corners(const PerspectiveView& view, Vector3 (&corners)[8]) noexcept {
    const Vector3 up_shift = scaled(view.offset_y, view.up);
    const Vector3 right_shift = scaled(-view.offset_x, view.right);
    const Vector3 offset = up_shift + right_shift;
    const Vector3 half_right = view.right * view.window_x;
    const Vector3 half_up = view.up * view.window_y;
    const Vector3 full_right = half_right * 2.0F;
    const Vector3 full_up = half_up * 2.0F;
    Vector3 directions[4];
    directions[0] = half_up + (view.at + half_right);
    directions[1] = directions[0] - full_right;
    directions[2] = directions[1] - full_up;
    directions[3] = full_right + directions[2];
    const Vector3 origin = offset + view.position;
    for (int corner = 0; corner != 4; ++corner) {
        const Vector3 direction = directions[corner] - offset;
        corners[corner] = origin + direction * view.near_distance;
        corners[corner + 4] = origin + direction * view.far_distance;
    }
}

[[nodiscard]] Vector3 normalised(const Vector3 v) noexcept {
    const float length = std::sqrt(dot(v, v));
    const float scale = length > 0.0F ? 1.0F / length : length;
    return v * scale;
}

[[nodiscard]] unsigned char non_negative(const float value) noexcept { return (std::bit_cast<std::uint32_t>(value) & 0x80000000U) == 0U ? 1U : 0U; }

[[nodiscard]] FrustumPlane make_plane(const Vector3 normal, const Vector3 point) noexcept {
    return {normal, dot(normal, point), {non_negative(normal.x), non_negative(normal.y), non_negative(normal.z)}};
}

} // namespace

ViewFrustum build_view_frustum(const PerspectiveView& view) noexcept {
    ViewFrustum frustum{};
    build_corners(view, frustum.corners);
    const Vector3* c = frustum.corners;
    frustum.planes[0] = make_plane(view.at, c[4]);
    frustum.planes[1] = make_plane(-view.at, c[0]);
    const Vector3& near_left = c[1];
    const Vector3& near_right = c[3];
    const Vector3& far_left = c[4];
    const Vector3& far_right = c[6];
    const Vector3& left_base = c[5];
    const Vector3& right_base = c[7];
    const Vector3 left_edge = near_left - left_base;
    frustum.planes[2] = make_plane(normalised(cross_edges(left_edge, far_right - left_base)), near_left);
    frustum.planes[3] = make_plane(normalised(cross_edges(far_left - left_base, left_edge)), near_left);
    const Vector3 right_edge = near_right - right_base;
    frustum.planes[4] = make_plane(normalised(cross_edges(right_edge, far_left - right_base)), near_right);
    frustum.planes[5] = make_plane(normalised(cross_edges(far_right - right_base, right_edge)), near_right);
    return frustum;
}

PackedFrustum build_packed_frustum(const PerspectiveView& view) noexcept {
    Vector3 corners[8]{};
    fill_packed_corners(view, corners);
    const Vector3 zero{};
    const Vector3& near_corner = corners[0];
    const Vector3& far_corner = corners[4];
    PackedFrustum frustum;
    frustum.far_normal = zero + view.at;
    frustum.far_distance = dot(far_corner, view.at);
    frustum.near_normal = (zero - view.at) + zero;
    frustum.near_distance = dot(near_corner, zero - view.at);
    frustum.sides = build_side_planes(corners);
    return frustum;
}

SidePlanes build_guard_band_planes(const PerspectiveView& view, const float width, const float height,
                                   const float guard_limit) noexcept {
    Vector3 corners[8]{};
    build_corners(view, corners);
    const float half_width = width * 0.5F;
    const float half_height = height * 0.5F;
    const float grow_y = ((guard_limit - half_height) / half_height - 1.0F) * 0.5F;
    const float grow_x = ((guard_limit - half_width) / half_width - 1.0F) * 0.5F;
    Vector3 widened[8]{};
    for (int end = 0; end != 8; end += 4) {
        const Vector3* c = corners + end;
        const Vector3 up_edge = (c[1] - c[2]) * grow_y;
        const Vector3 right_edge = (c[3] - c[2]) * grow_x;
        const Vector3 sum = right_edge + up_edge;
        const Vector3 diff = up_edge - right_edge;
        widened[end] = c[0] + sum;
        widened[end + 1] = c[1] + diff;
        widened[end + 2] = c[2] - sum;
        widened[end + 3] = c[3] - diff;
    }
    return build_side_planes(widened);
}

} // namespace takedown::engine
