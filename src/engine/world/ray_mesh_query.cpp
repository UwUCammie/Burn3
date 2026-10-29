#include "takedown/engine/world/ray_mesh_query.hpp"

#include "takedown/engine/math/chopped_vector.hpp"

namespace takedown::engine {

Vector3 mesh_origin(const std::int8_t x, const std::int8_t y, const std::int8_t z) noexcept {
    return {chop_mul(static_cast<float>(x), kMeshOffsetStep), chop_mul(static_cast<float>(y), kMeshOffsetStep),
            chop_mul(static_cast<float>(z), kMeshOffsetStep)};
}

Vector3 mesh_vertex(const Vector3& origin, const MeshVertexCode code) noexcept {
    return {chop_add(chop_mul(static_cast<float>(code.x), kMeshVertexStep), origin.x),
            chop_add(chop_mul(static_cast<float>(code.y), kMeshVertexStep), origin.y),
            chop_add(chop_mul(static_cast<float>(code.z), kMeshVertexStep), origin.z)};
}

Vector3 triangle_unit_normal(const Vector3 a, const Vector3 b, const Vector3 c) noexcept {
    return chop_normalized(chop_cross(chop_sub(b, a), chop_sub(c, a)));
}

Vector3 segment_point(const Vector3 start, const Vector3 end, const float t) noexcept {
    // ACC = start + 0, ACC -= start * t, then end * t added.
    const Vector3 accumulator = chop_sub(chop_add(start, Vector3{}), chop_mul(start, t));
    return chop_add(accumulator, chop_mul(end, t));
}

} // namespace takedown::engine
