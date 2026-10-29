#pragma once

// The closest crossing of a segment with a compressed triangle mesh, as the
// game's collision and visibility queries find it (a list of fixed-size
// records, each naming three or four vertices of a shared table; a record
// with a fourth vertex holds two triangles). Vertices are 16-bit
// fixed-point; a signed byte per axis in the mesh's header places the table
// in the world. Every triangle is tested with the segment and triangle test
// (engine/math/segment_triangle.hpp), in record order, and a crossing
// replaces the best only when its t is strictly smaller.

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstdint>

namespace takedown::engine {

struct MeshVertexCode {
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint16_t z{};
};

// Scale of a vertex code, in world units per step, and of a header byte.
inline constexpr float kMeshVertexStep = 0.0152587890625F;  // 1000 / 65536
inline constexpr float kMeshOffsetStep = 500.0F;

[[nodiscard]] Vector3 mesh_origin(std::int8_t x, std::int8_t y, std::int8_t z) noexcept;
[[nodiscard]] Vector3 mesh_vertex(const Vector3& origin, MeshVertexCode code) noexcept;

// The unit normal of the triangle (a, b, c): (b - a) x (c - a) normalised.
[[nodiscard]] Vector3 triangle_unit_normal(Vector3 a, Vector3 b, Vector3 c) noexcept;

// start + (end - start) * t as the game's vector unit evaluates it for a hit.
[[nodiscard]] Vector3 segment_point(Vector3 start, Vector3 end, float t) noexcept;

struct MeshHit {
    bool found{};
    float t{};
    float u{};
    float v{};
    std::uint32_t record{};  // the record address handed to the iteration
    std::uint32_t second{};  // 1 for a record's second triangle
    std::uint32_t tests{};   // triangles tested
};

// The records run from `first` in steps of `stride` while below `end`; the
// first record is always tested. `indices(record)` gives its four vertex
// indices (the fourth is 255 for a record with one triangle), `vertex(index)`
// a vertex position. A crossing counts only when t is below `best`.
template <typename Indices, typename Vertex>
[[nodiscard]] MeshHit closest_mesh_hit(const Vector3 start, const Vector3 end, const float best,
                                       const std::uint32_t first, const std::uint32_t end_record,
                                       const std::uint32_t stride, Indices&& indices, Vertex&& vertex) {
    MeshHit hit;
    float limit = best;
    std::uint32_t record = first;
    std::uint32_t second = 0U;
    for (;;) {
        const std::array<std::uint8_t, 4> index = indices(record);
        const auto test = second == 0U
                              ? test_segment_against_triangle(start, end, vertex(index[0]), vertex(index[1]),
                                                              vertex(index[2]))
                              : test_segment_against_triangle(start, end, vertex(index[2]), vertex(index[1]),
                                                              vertex(index[3]));
        ++hit.tests;
        // The game's compare reads denormals as zero and huge values as the largest float.
        if (test.hit && game_less(test.t, limit)) {
            limit = test.t;
            hit.found = true;
            hit.t = test.t;
            hit.u = chop_add(0.0F, test.u);
            hit.v = chop_add(0.0F, test.v);
            hit.record = record;
            hit.second = second;
        }
        if (second == 0U && index[3] < 255U) {
            second = 1U;
        } else {
            second = 0U;
            record += stride;
        }
        if (!(record < end_record)) {
            break;
        }
    }
    return hit;
}

} // namespace takedown::engine
