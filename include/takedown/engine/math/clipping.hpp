#pragma once

// Polygon clipping.

#include "takedown/engine/math/vector.hpp"

#include <cstddef>
#include <span>

namespace takedown::engine {

// Clips a convex polygon to the slab low <= p[axis] <= high (axis 0 = x,
// 1 = y, 2 = z), walking its edges in order (Sutherland-Hodgman). Each output
// vertex is an input vertex or an edge's crossing of a slab plane; its w is
// that edge's end vertex's w. Writes at most 2 * input.size() vertices and
// returns how many.
std::size_t clip_polygon_to_slab(std::span<const Vector4> input, int axis, float low, float high,
                                 std::span<Vector4> output) noexcept;

// Keeps the parts of line segments behind a plane (normal . p <= distance,
// the plane as xyz = normal, w = distance). For each segment it emits, in
// order: the crossing point when exactly one end is behind (w from the second
// end), then each end that is behind. `segment(n)` returns segment n's ends.
template <class Segment, class Emit>
std::size_t clip_segments_to_plane(const std::size_t segments, const Vector4 plane, Segment&& segment,
                                   Emit&& emit) {
    std::size_t emitted = 0U;
    for (std::size_t index = 0U; index < segments; ++index) {
        const auto [from, to] = segment(index);
        const float from_distance = dot(plane.xyz(), from.xyz());
        const float to_distance = dot(plane.xyz(), to.xyz());
        const bool from_behind = from_distance <= plane.w;
        const bool to_behind = to_distance <= plane.w;
        if (from_behind != to_behind) {
            const float t = (plane.w - from_distance) / (to_distance - from_distance);
            const Vector3 crossing = ((from.xyz() + Vector3{}) - from.xyz() * t) + to.xyz() * t;
            emit(Vector4{crossing.x, crossing.y, crossing.z, to.w});
            ++emitted;
        }
        if (from_behind) {
            emit(from);
            ++emitted;
        }
        if (to_behind) {
            emit(to);
            ++emitted;
        }
    }
    return emitted;
}

} // namespace takedown::engine
