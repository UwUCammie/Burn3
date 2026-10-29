#pragma once

// Where a segment crosses a triangle, as the game tests it for wheel probes
// and traffic (Moller-Trumbore in the game's vector unit rounding,
// engine/math/chopped.hpp). The crossing counts when the determinant is
// above 1e-8 and t, u, v and u + v lie strictly inside their ranges, which
// are widened by 1e-5 at the low end and 0.001% at the high end.

#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct SegmentTriangleTest {
    Vector3 direction;          // end - start
    Vector3 edge_b;             // b - a
    Vector3 edge_c;             // c - a
    Vector3 from_a;             // start - a
    Vector3 p;                  // direction x edge_c
    Vector3 q;                  // from_a x edge_b
    float determinant{};        // edge_b . p
    float along{};              // edge_c . q: t times the determinant
    float across_b{};           // from_a . p: u times the determinant
    float across_c{};           // direction . q: v times the determinant
    float inverse_determinant{};
    float high{};               // the determinant widened above
    float low{};                // and below zero
    bool tested{};              // the determinant is above 1e-8
    bool hit{};
    // When tested:
    float t{};
    float u{};
    float v{};
    Vector3 point;              // start + direction * t
};

[[nodiscard]] SegmentTriangleTest test_segment_against_triangle(Vector3 start, Vector3 end, Vector3 a, Vector3 b,
                                                                Vector3 c) noexcept;

// Whether a result of the test's arithmetic counts as above zero (not zero,
// not negative, not flushed to zero).
[[nodiscard]] bool test_above_zero(float value) noexcept;

} // namespace takedown::engine
