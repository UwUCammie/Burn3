#pragma once

// Directional light set-up: light colours scaled by intensity and limited
// per channel, and the light direction with its vertical part scaled.

#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cmath>

namespace takedown::engine {

// colour * intensity per channel, each at most `limit`; alpha is 1.
[[nodiscard]] inline Vector4 light_colour(const Vector4 colour, const float intensity, const float limit) noexcept {
    return {minimum(colour.x * intensity, limit), minimum(colour.y * intensity, limit),
            minimum(colour.z * intensity, limit), 1.0F};
}

// The unit direction of (x, y * vertical_scale, z); straight down when that
// is shorter than 2^-15.
[[nodiscard]] inline Vector3 light_direction(const Vector3 direction, const float vertical_scale) noexcept {
    Vector3 scaled = Vector3{} + Vector3{direction.x, direction.y * vertical_scale, direction.z};
    if (std::sqrt(dot(scaled, scaled)) < 0x1p-15F) {
        scaled = {0.0F, -1.0F, 0.0F};
    }
    return Vector3{} + scaled * reciprocal_sqrt(dot(scaled, scaled));
}

} // namespace takedown::engine
