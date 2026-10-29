#pragma once

// Axis-aligned bounding boxes and the game's 3x4 object transforms.

#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct Bounds {
    Vector3 minimum;
    Vector3 maximum;
};

// An object's placement as the game stores it: three basis rows, then the
// translation (each a 16-byte vector, w unused). A local point p lands at
// basis[0] * p.x + basis[1] * p.y + basis[2] * p.z + translation.
struct Transform {
    Vector3 basis[3];
    Vector3 translation;
};

// The world-space box enclosing a transformed local box (each axis takes the
// smaller and larger of every basis term, then adds the translation).
[[nodiscard]] Bounds transform_bounds(const Transform& transform, const Bounds& local) noexcept;

} // namespace takedown::engine
