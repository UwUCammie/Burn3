#pragma once

// Piecewise linear curves as the game stores them: (x, y) points with x
// rising.

#include <cstdint>

namespace takedown::engine {

struct CurvePoint {
    float x{};
    float y{};
};

// The curve's value at x, clamped to its first and last x. `point_at(i)`
// returns point i of `count`. As in the original, an x at (or clamped to)
// the last point interpolates towards point `count`, the data after the
// table, so point_at must accept indices -1 to count.
template <class PointAt>
[[nodiscard]] float evaluate_curve(PointAt&& point_at, const std::int32_t count, const float input) noexcept {
    const float first = point_at(0).x;
    const float last = point_at(count - 1).x;
    float x = first;
    if (!(input < first)) {
        x = input <= last ? input : last;
    }
    std::int32_t segment = 0;
    for (; segment < count - 1; ++segment) {
        if (!(x < point_at(segment).x) && x < point_at(segment + 1).x) {
            break;
        }
    }
    const CurvePoint from = point_at(segment);
    const CurvePoint to = point_at(segment + 1);
    const float width = to.x - from.x;
    if (0.0F == width) {
        return from.y;
    }
    return (0.0F + from.y) + ((x - from.x) / width) * (to.y - from.y);
}

} // namespace takedown::engine
