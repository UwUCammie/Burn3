#pragma once

// A cubic curve segment through four control points, shaped by a basis
// matrix: the weights of the points at t are the matrix's rows times t^3,
// t^2, t and 1, summed in that order.

#include "takedown/engine/math/vector.hpp"

#include <array>

namespace takedown::engine {

using CubicBasis = std::array<Vector4, 4>;

[[nodiscard]] inline Vector4 cubic_weights(const CubicBasis& basis, const float t) noexcept {
    const float t2 = 0.0F + t * t;
    const float powers[4] = {0.0F + t2 * t, t2, 0.0F + t, 1.0F};
    const auto lane = [&](float Vector4::*component) {
        return ((basis[0].*component * powers[0] + basis[1].*component * powers[1]) +
                basis[2].*component * powers[2]) +
               basis[3].*component * powers[3];
    };
    return {lane(&Vector4::x), lane(&Vector4::y), lane(&Vector4::z), lane(&Vector4::w)};
}

// The point of the segment through `points` with `weights`.
[[nodiscard]] inline Vector3 cubic_point(const std::array<Vector3, 4>& points, const Vector4 weights) noexcept {
    return ((points[0] * weights.x + points[1] * weights.y) + points[2] * weights.z) + points[3] * weights.w;
}

} // namespace takedown::engine
