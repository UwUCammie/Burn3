#include "takedown/engine/world/convex_volume.hpp"

#include <cstddef>

namespace takedown::engine {

void build_face_planes(ConvexVolume& volume, const std::array<FaceCorners, 6>& faces) noexcept {
    for (std::size_t face = 0; face < faces.size(); ++face) {
        // Corner indices are masked to the eight corners, as the game does.
        const Vector3 origin = volume.corners[static_cast<std::size_t>(faces[face].origin & 7)].xyz();
        const Vector3 second = volume.corners[static_cast<std::size_t>(faces[face].second & 7)].xyz();
        const Vector3 third = volume.corners[static_cast<std::size_t>(faces[face].third & 7)].xyz();
        const Vector3 perpendicular = cross(origin - second, third - second);
        const Vector3 direction = -perpendicular;
        const float inverse_length = reciprocal_sqrt(dot(perpendicular, perpendicular));
        const Vector3 normal = direction * inverse_length;
        volume.planes[face] = Vector4{normal.x, normal.y, normal.z, dot(normal, origin)};
    }
}

} // namespace takedown::engine
