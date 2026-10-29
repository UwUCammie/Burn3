#include "takedown/engine/physics/mesh_probe.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <cstdint>
#include <span>

namespace takedown::engine {

MeshProbe probe_mesh(const std::span<const CollisionTriangle> triangles, const Vector3 start,
                     const Vector3 end, const MeshProbeRules& rules, const float marker) {
    MeshProbe result{};
    result.nearest = -1;
    result.marker = marker;
    result.pass(MeshProbeStep::Start);
    float nearest = 999.0F;
    for (std::size_t index = 0U; index < triangles.size(); ++index) {
        result.pass(MeshProbeStep::TriangleCheck);
        result.pass(MeshProbeStep::TriangleStart);
        const CollisionTriangle& triangle = triangles[index];
        bool test = false;
        if (!rules.raced) {
            result.pass(MeshProbeStep::AnySurface);
            test = true;
        } else if (rules.wrecked) {
            result.pass(MeshProbeStep::WreckedAnySurface);
            test = true;
        } else if (triangle.surface == kSpecialSurface) {
            result.pass(MeshProbeStep::SpecialSurface);
            test = true;
        } else if (triangle.surface <= 11U) {
            result.pass(MeshProbeStep::LowSurface);
            test = true;
        } else if (triangle.surface <= 20U) {
            result.pass(MeshProbeStep::MiddleSurface);
            test = true;
        } else {
            result.pass(MeshProbeStep::HighSurface);
        }
        result.pass(MeshProbeStep::WreckCheck);
        if (rules.wrecked) {
            result.pass(MeshProbeStep::Slope);
            const Vector3 a = triangle.normal;
            const Vector3 b = rules.up;
            if (game_less(chop_dot(a, b), 0.25F)) {
                result.pass(MeshProbeStep::Steep);
                test = false;
            }
        }
        result.pass(MeshProbeStep::TestCheck);
        if (test) {
            result.pass(MeshProbeStep::Test);
            result.last_test = test_segment_against_triangle(start, end, triangle.a, triangle.b, triangle.c);
            result.last_tested = index;
            if (result.last_test.tested) {
                result.solved = true;
                result.last_solved = result.last_test;
            }
            result.tested = true;
            if (result.last_test.hit) {
                result.pass(MeshProbeStep::Hit);
                if (game_less(result.last_test.t, nearest)) {
                    result.pass(MeshProbeStep::Nearer);
                    result.nearest = static_cast<std::int32_t>(index);
                    nearest = result.last_test.t;
                }
            } else {
                result.pass(MeshProbeStep::Miss);
            }
        }
        result.pass(MeshProbeStep::MarkerCheck);
        if (triangle.surface == kSpecialSurface) {
            result.pass(MeshProbeStep::MarkerSpecial);
            result.marker = rules.marker_set;
        } else {
            result.pass(MeshProbeStep::MarkerOther);
            if (game_equal(result.marker, rules.marker_set)) {
                result.pass(MeshProbeStep::MarkerCleared);
                result.marker = rules.marker_cleared;
            }
        }
        result.pass(MeshProbeStep::TriangleDone);
    }
    result.pass(MeshProbeStep::TriangleCheck);
    result.pass(MeshProbeStep::Finish);
    const Vector3 span = chop_sub(start, end);
    result.length_terms.x = chop_mul(span.x, span.x);
    result.length_terms.y = chop_mul(span.y, span.y);
    result.length_terms.z = chop_mul(span.z, span.z);
    result.length_squared = chop_dot(span, span);
    result.length = game_sqrt(result.length_squared);
    result.distance = chop_mul(result.length, nearest);
    return result;
}

} // namespace takedown::engine
