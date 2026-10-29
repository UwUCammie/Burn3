#pragma once

// Vector operations in the game's rounding (engine/math/chopped.hpp): each
// lane rounds toward zero, sums run x, then y, then z, and a normalisation
// scales by the vector unit's reciprocal square root. For game code whose
// results feed back into themselves every frame (AI cars steering along the
// track), where host rounding would drift away from the original.

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

[[nodiscard]] inline Vector3 chop_add(const Vector3 a, const Vector3 b) noexcept {
    return {chop_add(a.x, b.x), chop_add(a.y, b.y), chop_add(a.z, b.z)};
}
[[nodiscard]] inline Vector3 chop_sub(const Vector3 a, const Vector3 b) noexcept {
    return {chop_sub(a.x, b.x), chop_sub(a.y, b.y), chop_sub(a.z, b.z)};
}
[[nodiscard]] inline Vector3 chop_mul(const Vector3 a, const float s) noexcept {
    return {chop_mul(a.x, s), chop_mul(a.y, s), chop_mul(a.z, s)};
}
[[nodiscard]] inline float chop_dot(const Vector3 a, const Vector3 b) noexcept {
    return chop_add(chop_add(chop_mul(a.x, b.x), chop_mul(a.y, b.y)), chop_mul(a.z, b.z));
}
[[nodiscard]] inline Vector3 chop_cross(const Vector3 a, const Vector3 b) noexcept {
    return {chop_sub(chop_mul(a.y, b.z), chop_mul(b.y, a.z)), chop_sub(chop_mul(a.z, b.x), chop_mul(b.z, a.x)),
            chop_sub(chop_mul(a.x, b.y), chop_mul(b.x, a.y))};
}
// `v` scaled to unit length (the largest float times `v` for a zero vector,
// as the vector unit gives).
[[nodiscard]] inline Vector3 chop_normalized(const Vector3 v) noexcept {
    return chop_mul(v, chop_reciprocal_sqrt(chop_dot(v, v)));
}

} // namespace takedown::engine
