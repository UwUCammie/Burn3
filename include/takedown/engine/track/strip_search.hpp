#pragma once

// Finding the track strip quad nearest a point: by the squared distance to
// each quad's centre (the mean of its four corners), over every strip object.

#include "takedown/engine/math/vector.hpp"

#include <cstdint>

namespace takedown::engine {

struct NearestStripQuad {
    std::int32_t object{-1};   // -1: none closer than the limit
    std::int32_t quad{};
    float distance_squared{};
};

// `object_count()` and `quad_count(object)` are read again on every step and
// `corner(object, quad, k)` returns corner k (0 to 3) of a quad, as the
// original reads them. A point exactly at the origin (either sign of zero)
// finds nothing.
template <class ObjectCount, class QuadCount, class Corner>
[[nodiscard]] NearestStripQuad find_nearest_strip_quad(const Vector3 point, const float limit_squared,
                                                       ObjectCount&& object_count, QuadCount&& quad_count,
                                                       Corner&& corner) {
    NearestStripQuad nearest;
    nearest.distance_squared = limit_squared;
    if (point.x == 0.0F && point.y == 0.0F && point.z == 0.0F) {
        return nearest;
    }
    for (std::int32_t object = 0; object < object_count(); ++object) {
        for (std::int32_t quad = 0; quad < quad_count(object); ++quad) {
            const Vector3 centre =
                (((corner(object, quad, 0) + corner(object, quad, 1)) + corner(object, quad, 2)) +
                 corner(object, quad, 3)) *
                0.25F;
            const Vector3 offset = centre - point;
            const float distance_squared = dot(offset, offset);
            if (distance_squared < nearest.distance_squared) {
                nearest = {object, quad, distance_squared};
            }
        }
    }
    return nearest;
}

} // namespace takedown::engine
