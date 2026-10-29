#pragma once

// Making a drifting rotation (three rows: right, up, at) orthonormal again,
// as the game does for AI cars after moving them: every row is normalised,
// then the two rows closest to perpendicular are kept and the third rebuilt
// from them (a zero-length row is always rebuilt). In the game's rounding.

#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <cmath>
#include <cstdint>

namespace takedown::engine {

struct RotationBasis {
    Vector3 right;
    Vector3 up;
    Vector3 at;
};

// Which row the rebuild started from. KeptRightUndecided is the fallback
// when no pair is strictly closer to perpendicular than the others (callers
// that carry a fourth lane per row see it handled differently there).
enum class KeptRow : std::uint8_t { Up, At, Right, RightUndecided };

struct Orthonormalized {
    RotationBasis basis;
    KeptRow kept{};
};

[[nodiscard]] inline Orthonormalized orthonormalized(RotationBasis b) noexcept {
    const float right_length = game_sqrt(chop_dot(b.right, b.right));
    const float up_length = game_sqrt(chop_dot(b.up, b.up));
    const float at_length = game_sqrt(chop_dot(b.at, b.at));
    b.right = chop_normalized(b.right);
    b.up = chop_normalized(b.up);
    b.at = chop_normalized(b.at);
    KeptRow kept;
    if (game_less_equal(right_length, 0.0F)) {
        kept = KeptRow::Up;
    } else if (game_less_equal(up_length, 0.0F)) {
        kept = KeptRow::At;
    } else if (game_less_equal(at_length, 0.0F)) {
        kept = KeptRow::Right;
    } else {
        // How far each pair is from perpendicular.
        const float up_at = std::fabs(chop_dot(b.up, b.at));
        const float at_right = std::fabs(chop_dot(b.at, b.right));
        const float right_up = std::fabs(chop_dot(b.right, b.up));
        if (game_less(up_at, at_right)) {
            kept = game_less(up_at, right_up) ? KeptRow::Up : KeptRow::Right;
        } else {
            kept = game_less(at_right, right_up) ? KeptRow::At : KeptRow::RightUndecided;
        }
    }
    switch (kept) {
    case KeptRow::Up:  // right from up and at, then at
        b.right = chop_normalized(chop_cross(b.up, b.at));
        b.at = chop_normalized(chop_cross(b.right, b.up));
        break;
    case KeptRow::At:  // up from at and right, then right
        b.up = chop_normalized(chop_cross(b.at, b.right));
        b.right = chop_normalized(chop_cross(b.up, b.at));
        break;
    case KeptRow::Right:
    case KeptRow::RightUndecided:  // at from right and up, then up
        b.at = chop_normalized(chop_cross(b.right, b.up));
        b.up = chop_normalized(chop_cross(b.at, b.right));
        break;
    }
    return {b, kept};
}

} // namespace takedown::engine
