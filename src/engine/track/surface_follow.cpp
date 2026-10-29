#include "takedown/engine/track/surface_follow.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

namespace takedown::engine {

SurfaceSettle settle_on_quad(const Vector3 position, const float height, const Vector3 up,
                             const SurfaceQuad& quad) noexcept {
    // The segment runs 10 above to 10 below the position along the old up direction.
    SurfaceSettle settle;
    const Vector3 start = chop_add(position, chop_mul(up, 10.0F));
    const Vector3 end = chop_add(position, chop_mul(up, -10.0F));
    settle.first = test_segment_against_triangle(start, end, quad.a, quad.b, quad.c);
    const SegmentTriangleTest* crossing = &settle.first;
    if (!settle.first.hit) {
        settle.second_tested = true;
        settle.second = test_segment_against_triangle(start, end, quad.c, quad.b, quad.d);
        crossing = &settle.second;
    }
    settle.hit = crossing->hit;
    if (!settle.hit) {
        return settle;
    }
    settle.rise = chop_madd(10.0F, 2.0F, chop_mul(crossing->t, -10.0F));
    settle.across = chop_add(chop_sub(quad.c, quad.a), chop_sub(quad.d, quad.b));
    settle.along = chop_add(chop_sub(quad.a, quad.b), chop_sub(quad.c, quad.d));
    settle.cross = chop_cross(settle.across, settle.along);
    settle.cross_length_squared = chop_dot(settle.cross, settle.cross);
    settle.inverse_length = chop_reciprocal_sqrt(settle.cross_length_squared);
    settle.degenerate = game_equal(0.0F, game_sqrt(settle.cross_length_squared));
    settle.up = settle.degenerate ? Vector3{0.0F, 1.0F, 0.0F} : chop_mul(settle.cross, settle.inverse_length);
    const float lift = chop_add(height, settle.rise);
    // Only the height changes; the other lanes pass through the vector add unchanged.
    const float y = chop_add(chop_add(position.y, chop_mul(up.y, lift)), 0.0F);
    settle.position = chop_add(Vector3{}, Vector3{position.x, y, position.z});
    return settle;
}

CentreLineSwing swing_onto_centre_line(const Vector3 point, const Vector3 pivot, const SurfaceQuad& quad) noexcept {
    CentreLineSwing swing;
    swing.far_middle = chop_mul(chop_add(quad.c, quad.d), 0.5F);
    swing.near_middle = chop_mul(chop_add(quad.a, quad.b), 0.5F);
    swing.line = chop_normalized(chop_sub(swing.far_middle, swing.near_middle));
    const Vector3 from_near = chop_sub(point, swing.near_middle);
    swing.along_terms = {chop_mul(swing.line.x, from_near.x), chop_mul(swing.line.y, from_near.y),
                         chop_mul(swing.line.z, from_near.z)};
    swing.along = chop_add(chop_add(swing.along_terms.x, swing.along_terms.y), swing.along_terms.z);
    swing.projected = chop_add(swing.near_middle, chop_mul(swing.line, swing.along));
    const Vector3 offset = chop_sub(point, pivot);
    swing.distance = chop_sqrt(chop_dot(offset, offset));
    const Vector3 toward = chop_sub(swing.projected, pivot);
    swing.toward_length_squared = chop_dot(toward, toward);
    swing.toward_inverse_length = chop_reciprocal_sqrt(swing.toward_length_squared);
    swing.toward = chop_mul(toward, swing.toward_inverse_length);
    swing.position = chop_add(pivot, chop_mul(swing.toward, swing.distance));
    return swing;
}

} // namespace takedown::engine
