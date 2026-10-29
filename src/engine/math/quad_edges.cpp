#include "takedown/engine/math/quad_edges.hpp"

#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

namespace takedown::engine {

namespace {

// The edge from -> to crossed with the up axis: the normal pointing out of a
// quad wound the way the strip is.
[[nodiscard]] Vector3 edge_normal(const Vector3 from, const Vector3 to, const Vector3 up) noexcept {
    const Vector3 edge = to - from;
    return {up.z * edge.y - up.y * edge.z, up.x * edge.z - up.z * edge.x, up.y * edge.x - up.x * edge.y};
}

// Positive when `point` lies on the outward side of the edge from -> to.
[[nodiscard]] float edge_side(const Vector3 from, const Vector3 to, const Vector3 point, const Vector3 up) noexcept {
    const Vector3 normal = edge_normal(from, to, up);
    return dot(normal, point - from);
}

} // namespace

std::uint32_t quad_outside_edges(const StripQuad& quad, const Vector3 point, const Vector3 up) noexcept {
    const Vector3(&corners)[4] = quad.corners;
    std::uint32_t outside = 0U;
    if (quad.has_end && edge_side(corners[2], corners[3], point, up) < 0.0F) {
        outside = kQuadEdgeEnd;
    }
    if (edge_side(corners[0], corners[2], point, up) < 0.0F) {
        outside |= kQuadEdgeStartSide;
    }
    if (edge_side(corners[1], corners[3], point, up) > 0.0F) {
        outside |= kQuadEdgeEndSide;
    }
    if (quad.has_start && dot(point - corners[0], edge_normal(corners[0], corners[1], up)) > 0.0F) {
        outside |= kQuadEdgeStart;
    }
    return outside;
}

std::uint32_t quad_edges_beyond(const StripQuad& quad, const Vector3 point, const Vector3 up,
                                const float margin) noexcept {
    // Whether `point` is farther than `margin` outside the edge from -> to,
    // measured from `origin` along the edge's unit normal.
    const auto beyond = [&](const Vector3 from, const Vector3 to, const Vector3 origin) {
        const Vector3 normal = chop_normalized(chop_cross(chop_sub(from, to), up));
        return !game_less_equal(chop_dot(chop_sub(point, origin), normal), margin);
    };
    const Vector3* c = quad.corners;
    std::uint32_t edges = 0U;
    if (quad.has_end && beyond(c[2], c[3], c[2])) {
        edges = kQuadEdgeEnd;
    }
    if (beyond(c[0], c[2], c[2])) {
        edges |= kQuadEdgeStartSide;
    }
    if (beyond(c[3], c[1], c[1])) {
        edges |= kQuadEdgeEndSide;
    }
    if (quad.has_start && beyond(c[1], c[0], c[1])) {
        edges |= kQuadEdgeStart;
    }
    return edges;
}

} // namespace takedown::engine
