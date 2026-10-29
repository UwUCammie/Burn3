#include "takedown/engine/math/segment_triangle.hpp"

#include "takedown/engine/math/chopped_vector.hpp"

#include <cmath>

namespace takedown::engine {

bool test_above_zero(const float value) noexcept { return !std::signbit(value) && value != 0.0F; }

SegmentTriangleTest test_segment_against_triangle(const Vector3 start, const Vector3 end, const Vector3 a,
                                                  const Vector3 b, const Vector3 c) noexcept {
    SegmentTriangleTest result;
    result.direction = chop_sub(end, start);
    result.edge_c = chop_sub(c, a);
    result.from_a = chop_sub(start, a);
    result.edge_b = chop_sub(b, a);
    result.p = chop_cross(result.direction, result.edge_c);
    result.q = chop_cross(result.from_a, result.edge_b);
    result.determinant = chop_dot(result.edge_b, result.p);
    result.across_b = chop_dot(result.from_a, result.p);
    result.along = chop_dot(result.edge_c, result.q);
    result.across_c = chop_dot(result.direction, result.q);
    result.inverse_determinant = chop_div(1.0F, result.determinant);
    result.high = chop_mul(result.determinant, 1.00001F);
    result.low = chop_mul(result.determinant, -1.0e-5F);
    result.tested = test_above_zero(chop_sub(result.determinant, 1.0e-8F));
    if (!result.tested) {
        return result;
    }
    result.hit = test_above_zero(chop_sub(result.across_b, result.low)) &&
                 test_above_zero(chop_sub(result.high, result.across_b)) &&
                 test_above_zero(chop_sub(result.across_c, result.low)) &&
                 test_above_zero(chop_sub(result.high, chop_add(result.across_b, result.across_c))) &&
                 test_above_zero(chop_sub(result.along, result.low)) &&
                 test_above_zero(chop_sub(result.high, result.along));
    result.t = chop_mul(result.along, result.inverse_determinant);
    result.u = chop_mul(result.across_b, result.inverse_determinant);
    result.v = chop_mul(result.across_c, result.inverse_determinant);
    result.point = chop_add(chop_add(start, Vector3{}), chop_mul(result.direction, result.t));
    return result;
}

} // namespace takedown::engine
