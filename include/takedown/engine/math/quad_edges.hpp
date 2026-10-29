#pragma once

// Point-in-quad tests for quads taken from a strip of vertex pairs (corners
// 0 and 1 start the quad, 2 and 3 end it), seen along an up axis.

#include "takedown/engine/math/vector.hpp"

#include <cstdint>

namespace takedown::engine {

enum QuadEdge : std::uint32_t {
    kQuadEdgeStartSide = 1U,  // corner 0 to corner 2
    kQuadEdgeEndSide = 2U,    // corner 1 to corner 3
    kQuadEdgeEnd = 4U,        // corner 2 to corner 3
    kQuadEdgeStart = 8U,      // corner 0 to corner 1
};

struct StripQuad {
    Vector3 corners[4];
    bool has_start{true};  // false when corners 0 and 1 are the same vertex
    bool has_end{true};    // false when corners 2 and 3 are the same vertex
};

// The QuadEdge bits of the edges `point` lies outside of (zero inside).
[[nodiscard]] std::uint32_t quad_outside_edges(const StripQuad& quad, Vector3 point, Vector3 up) noexcept;

// The QuadEdge bits of the edges `point` lies more than `margin` beyond
// (a negative margin: less than -margin inside), measured along each edge's
// unit outward normal in the game's rounding. A missing start or end edge
// (two equal corners) is never reported.
[[nodiscard]] std::uint32_t quad_edges_beyond(const StripQuad& quad, Vector3 point, Vector3 up, float margin) noexcept;

} // namespace takedown::engine
