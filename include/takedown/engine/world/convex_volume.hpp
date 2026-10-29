#pragma once

// A convex volume with eight corners and six faces (a box, possibly skewed or
// tapered like a view frustum), as the world visibility code builds it for
// each region it tests.

#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstdint>

namespace takedown::engine {

// The three corners (indices into ConvexVolume::corners) that span one face;
// the face plane passes through `origin`.
struct FaceCorners {
    std::int8_t origin{};
    std::int8_t second{};
    std::int8_t third{};
};

// Corners followed by the face planes, the layout the game stores (224 bytes).
// A plane is (unit normal, distance): a point p is on the plane when
// normal . p == distance.
struct ConvexVolume {
    std::array<Vector4, 8> corners{};
    std::array<Vector4, 6> planes{};
};

// Fills in `volume.planes` from `volume.corners`. Each face's normal is the
// unit vector of (third - second) x (origin - second), i.e. perpendicular to
// the face, and its distance is normal . corners[origin].
void build_face_planes(ConvexVolume& volume, const std::array<FaceCorners, 6>& faces) noexcept;

} // namespace takedown::engine
