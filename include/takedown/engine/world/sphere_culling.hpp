#pragma once

// Sphere-versus-planes culling.

#include "takedown/engine/math/vector.hpp"
#include "takedown/engine/world/frustum_culling.hpp"

#include <cmath>
#include <span>

namespace takedown::engine {

// A plane as xyz = normal, w = distance: a point p lies at normal . p - w.
// Outside when the centre is more than the radius in front of any plane,
// Inside when it is more than the radius behind every plane.
[[nodiscard]] inline Visibility classify_sphere(const Vector3 centre, const float radius,
                                                const std::span<const Vector4> planes) noexcept {
    Visibility result = Visibility::Inside;
    for (const Vector4& plane : planes) {
        const float distance = dot(centre, plane.xyz()) - plane.w;
        if (!(distance <= radius)) {
            return Visibility::Outside;
        }
        if (!(distance <= -radius)) {
            result = Visibility::Intersecting;
        }
    }
    return result;
}

// How a line segment meets a sphere: Inside when both ends are in it,
// Intersecting when one end is or the segment's nearest point to the centre
// is, Outside otherwise.
[[nodiscard]] inline Visibility segment_sphere_overlap(const Vector3 start, const Vector3 end, const Vector3 centre,
                                                       const float radius) noexcept {
    const float radius_squared = radius * radius;
    const Vector3 from_end = centre - end;
    const Vector3 from_start = centre - start;
    const bool end_in = dot(from_end, from_end) <= radius_squared;
    const bool start_in = dot(from_start, from_start) <= radius_squared;
    if (end_in && start_in) {
        return Visibility::Inside;
    }
    if (end_in || start_in) {
        return Visibility::Intersecting;
    }
    const Vector3 along = end - start;
    const float t = dot(from_start, along) / dot(along, along);
    if (t < 0.0F || !(t <= 1.0F)) {
        return Visibility::Outside;
    }
    const Vector3 nearest = (start + along * t) - centre;
    return dot(nearest, nearest) <= radius_squared ? Visibility::Intersecting : Visibility::Outside;
}

// Four planes stored transposed, one per lane: a point p is inside plane i
// when distance[i] - normal_x[i] p.x - normal_y[i] p.y - normal_z[i] p.z is
// not negative.
struct TransposedPlanes {
    Vector4 normal_x;
    Vector4 normal_y;
    Vector4 normal_z;
    Vector4 distance;
};

// Whether any of the four planes has the point offset by `margin` outside
// (a negative zero counts as outside, like the original's sign test).
[[nodiscard]] inline bool any_plane_excludes(const TransposedPlanes& planes, const Vector3 point,
                                             const float margin) noexcept {
    const float normal_x[4] = {planes.normal_x.x, planes.normal_x.y, planes.normal_x.z, planes.normal_x.w};
    const float normal_y[4] = {planes.normal_y.x, planes.normal_y.y, planes.normal_y.z, planes.normal_y.w};
    const float normal_z[4] = {planes.normal_z.x, planes.normal_z.y, planes.normal_z.z, planes.normal_z.w};
    const float distance[4] = {planes.distance.x, planes.distance.y, planes.distance.z, planes.distance.w};
    for (int lane = 0; lane < 4; ++lane) {
        const float value =
            (((distance[lane] + 0.0F) - normal_x[lane] * point.x) - normal_y[lane] * point.y) -
            normal_z[lane] * point.z;
        if (std::signbit(value + margin)) {
            return true;
        }
    }
    return false;
}

// A sphere against the game's culling frustum: the four side planes, the
// near and far planes (as classify_sphere), and an inner set of side planes
// the sphere must clear to count as inside.
struct CullingFrustum {
    Vector4 depth_planes[2];
    TransposedPlanes sides;
    TransposedPlanes inner_sides;
};

[[nodiscard]] inline Visibility classify_sphere(const Vector3 centre, const float radius,
                                                const CullingFrustum& frustum) noexcept {
    if (any_plane_excludes(frustum.sides, centre, radius)) {
        return Visibility::Outside;
    }
    const Visibility depth = classify_sphere(centre, radius, frustum.depth_planes);
    if (depth != Visibility::Inside) {
        return depth;
    }
    return any_plane_excludes(frustum.inner_sides, centre, -radius) ? Visibility::Intersecting : Visibility::Inside;
}

} // namespace takedown::engine
