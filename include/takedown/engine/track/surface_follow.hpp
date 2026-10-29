#pragma once

// Keeping an object on the road surface (traffic and other things that ride
// the track strips), in the game's rounding (engine/math/chopped.hpp).
//
// The object has a position, a height above the surface and an up
// direction. A segment 10 either side of its position along that direction
// is tested against the strip quad it is on (corners a, b, c, d: triangles
// a, b, c then c, b, d). When it crosses, the object takes the quad's mean
// up direction (the cross of its diagonals, or straight up when the quad
// is degenerate) and moves along its old up direction onto the surface
// plus its height.

#include "takedown/engine/math/segment_triangle.hpp"
#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace takedown::engine {

struct SurfaceQuad {
    Vector3 a;
    Vector3 b;
    Vector3 c;
    Vector3 d;
};

struct SurfaceSettle {
    SegmentTriangleTest first;         // against a, b, c
    SegmentTriangleTest second;        // against c, b, d, when the first missed
    bool second_tested{};
    bool hit{};
    // When it hit:
    Vector3 across;                    // (c - a) + (d - b)
    Vector3 along;                     // (a - b) + (c - d)
    Vector3 cross;                     // across x along
    float cross_length_squared{};
    float inverse_length{};
    bool degenerate{};                 // the cross has no length: up is straight up
    Vector3 up;
    float rise{};                      // the move along the old up direction
    Vector3 position;
};

[[nodiscard]] SurfaceSettle settle_on_quad(Vector3 position, float height, Vector3 up, const SurfaceQuad& quad) noexcept;

// Lines a point up with a quad's centre line (from the middle of edge a, b
// to the middle of edge c, d) while keeping its distance from a pivot: the
// point goes to the pivot plus that distance toward the point's projection
// on the line. Traffic does this for its front and rear axles, the rear
// pivoting on the front.
struct CentreLineSwing {
    Vector3 near_middle;      // (a + b) / 2
    Vector3 far_middle;       // (c + d) / 2
    Vector3 line;             // unit, near to far
    Vector3 along_terms;      // line * (point - near_middle), per lane
    float along{};
    Vector3 projected;        // the point on the line
    float distance{};         // from the pivot
    Vector3 toward;           // unit, pivot to the projection
    float toward_length_squared{};
    float toward_inverse_length{};
    Vector3 position;
};

[[nodiscard]] CentreLineSwing swing_onto_centre_line(Vector3 point, Vector3 pivot, const SurfaceQuad& quad) noexcept;

// A strip's rows: the vertex indices of each row's two sides. A quad spans
// two rows (a, b on the first, c, d on the next).
struct StripRow {
    std::uint16_t left{};
    std::uint16_t right{};
};

struct StripRider {
    Vector3 position;
    float height{};
    Vector3 up;
    std::array<std::uint16_t, 4> corners{};  // a, b, c, d
    std::int32_t row{-1};                    // of a and b; -1 when off the strip
};

struct StripAttempt {
    std::array<std::uint16_t, 4> corners{};
    SurfaceSettle settle;
};

inline constexpr std::size_t kStripSearchRows = 11U;  // the start row and ten behind it

struct StripFollow {
    std::array<StripAttempt, kStripSearchRows> attempts;
    std::size_t attempt_count{};
    bool advanced{};    // tracking: the rider moved on a row
    bool at_end{};      // tracking: missed on the strip's last quad
    bool on_strip{};
};

// Tracking: settles the rider on its quad, or on the next one along when it
// has run off it (lost at the strip's end or when that misses too).
// `rows(i)` gives row i, `vertex(index)` a vertex, `row_count` the rows.
template <typename Rows, typename Vertices>
[[nodiscard]] StripFollow keep_on_strip(StripRider& rider, const std::int32_t row_count, Rows&& rows,
                                        Vertices&& vertex) {
    StripFollow follow;
    const auto attempt = [&] {
        StripAttempt& next = follow.attempts[follow.attempt_count++];
        next.corners = rider.corners;
        next.settle = settle_on_quad(rider.position, rider.height, rider.up,
                                     {vertex(rider.corners[0]), vertex(rider.corners[1]), vertex(rider.corners[2]),
                                      vertex(rider.corners[3])});
        return next.settle;
    };
    if (attempt().hit) {
        follow.on_strip = true;
        return follow;
    }
    if (rider.row >= row_count - 2) {
        follow.at_end = true;
        rider.row = -1;
        return follow;
    }
    follow.advanced = true;
    ++rider.row;
    const StripRow next = rows(rider.row + 1);
    rider.corners = {rider.corners[2], rider.corners[3], next.left, next.right};
    follow.on_strip = attempt().hit;
    if (!follow.on_strip) {
        rider.row = -1;
    }
    return follow;
}

// Searching: tries the quads from `start_row` back over ten rows.
template <typename Rows, typename Vertices>
[[nodiscard]] StripFollow find_on_strip(StripRider& rider, const std::int32_t start_row, Rows&& rows,
                                        Vertices&& vertex) {
    StripFollow follow;
    const StripRow first = rows(start_row + 1);
    rider.corners[0] = first.left;
    rider.corners[1] = first.right;
    for (std::int32_t row = start_row; row >= 0 && row >= start_row - 10; --row) {
        const StripRow here = rows(row);
        rider.corners = {here.left, here.right, rider.corners[0], rider.corners[1]};
        StripAttempt& next = follow.attempts[follow.attempt_count++];
        next.corners = rider.corners;
        next.settle = settle_on_quad(rider.position, rider.height, rider.up,
                                     {vertex(rider.corners[0]), vertex(rider.corners[1]), vertex(rider.corners[2]),
                                      vertex(rider.corners[3])});
        if (next.settle.hit) {
            follow.on_strip = true;
            rider.row = row;
            return follow;
        }
    }
    rider.row = -1;
    return follow;
}

} // namespace takedown::engine
