#pragma once

// Where the sun appears: a point 50 units from the eye along the sun's
// direction, taken into view space; the lens flare draws there when it is in
// front of the near limit.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cmath>

namespace takedown::engine {

inline constexpr float kSunDistance = 50.0F;

struct SunProjection {
    Vector3 direction;   // the sun's direction, normalised
    Vector3 view_point;  // the sun point in view space
};

// `view` maps world to view space (rows applied as view = R x + U y + F z + T).
[[nodiscard]] inline SunProjection project_sun(const Vector3 direction, const Vector3 eye,
                                               const Transform& view) noexcept {
    SunProjection result;
    // One rounding, as the vector unit's reciprocal square root.
    result.direction = direction * static_cast<float>(1.0 / std::sqrt(static_cast<double>(dot(direction, direction))));
    const Vector3 point = eye + result.direction * kSunDistance;
    result.view_point =
        ((view.basis[0] * point.x + view.basis[1] * point.y) + view.basis[2] * point.z) + view.translation;
    return result;
}

} // namespace takedown::engine
