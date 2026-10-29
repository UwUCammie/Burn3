#pragma once

// The game's rotation about an axis, built with its fast sine and cosine. It
// leaves out the (1 - cos) * a_i * a_j terms and uses a_i where Rodrigues'
// formula has a_i squared, so it is exact only for the coordinate axes (the
// ones the game passes).

#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/trigonometry.hpp"
#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct RotationRows {
    Vector3 rows[3];
};

[[nodiscard]] inline RotationRows axis_rotation(const Vector3 axis, const float angle) noexcept {
    const float turned = 0.0F + angle;
    const float sine = wave_cosine(turned - kHalfPi);
    const float one_minus_cosine = 1.0F - wave_cosine(turned);
    const Vector3 kept = ((Vector3{} - axis) + Vector3{1.0F, 1.0F, 1.0F}) * one_minus_cosine;
    const Vector3 swing = axis * sine;
    RotationRows result;
    result.rows[0] = {0.0F + (1.0F - kept.x), 0.0F + swing.z, 0.0F + (0.0F - swing.y)};
    result.rows[1] = {0.0F + (0.0F - swing.z), 0.0F + (1.0F - kept.y), 0.0F + swing.x};
    result.rows[2] = {0.0F + swing.y, 0.0F + (0.0F - swing.x), 0.0F + (1.0F - kept.z)};
    return result;
}

// The full rotation by `angle` about the unit `axis` (Rodrigues' formula,
// rows as the game stores them), with the vector unit's sine and cosine and
// its rounding.
[[nodiscard]] inline RotationRows axis_angle_rotation_chopped(const Vector3 axis, const float angle) noexcept {
    const float sine = wave_cosine_chopped(chop_sub(angle, kHalfPi));
    const float one_minus_cosine = chop_sub(1.0F, wave_cosine_chopped(angle));
    const Vector3 squares{chop_mul(axis.x, axis.x), chop_mul(axis.y, axis.y), chop_mul(axis.z, axis.z)};
    const Vector3 kept = chop_mul(Vector3{chop_add(-squares.x, 1.0F), chop_add(-squares.y, 1.0F),
                                          chop_add(-squares.z, 1.0F)},
                                  one_minus_cosine);
    const Vector3 cross_terms = chop_mul(
        Vector3{chop_mul(axis.y, axis.z), chop_mul(axis.z, axis.x), chop_mul(axis.x, axis.y)}, one_minus_cosine);
    const Vector3 swing = chop_mul(axis, sine);
    RotationRows result;
    result.rows[0] = {chop_sub(1.0F, kept.x), chop_add(cross_terms.z, swing.z), chop_sub(cross_terms.y, swing.y)};
    result.rows[1] = {chop_sub(cross_terms.z, swing.z), chop_sub(1.0F, kept.y), chop_add(cross_terms.x, swing.x)};
    result.rows[2] = {chop_add(cross_terms.y, swing.y), chop_sub(cross_terms.x, swing.x), chop_sub(1.0F, kept.z)};
    return result;
}

} // namespace takedown::engine
