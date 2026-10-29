#pragma once

// Measurements along the track: across a strip edge, ahead along a heading,
// and along a sampled curve.

#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/vector.hpp"

#include <cmath>
#include <cstdint>

namespace takedown::engine {

// How far `point` lies along the edge from `from` to `to`, as a fraction of
// the edge's horizontal length less a margin at each end: 0 at `margin` past
// `from`, 1 at `margin` before `to` (measured along the edge in 3D).
[[nodiscard]] inline float fraction_along_edge(const Vector3 from, const Vector3 to, const Vector3 point,
                                               const float margin) noexcept {
    Vector3 flat = from - to;
    flat.y = 0.0F;
    const float usable = (0.0F + std::sqrt(dot(flat, flat))) - 2.0F * margin;
    const Vector3 edge = to - from;
    const Vector3 direction = edge * static_cast<float>(1.0 / std::sqrt(static_cast<double>(dot(edge, edge))));
    return (dot(point - from, direction) - margin) / usable;
}

// How far `other` is ahead of `self` along `heading` taken flat (or along
// `fallback` when the heading has no horizontal length).
[[nodiscard]] inline float distance_ahead(const Vector3 self, const Vector3 other, const Vector3 heading,
                                          const Vector3 fallback) noexcept {
    Vector3 behind = self - other;
    behind.y = 0.0F;
    Vector3 flat = heading;
    flat.y = 0.0F;
    const float length_squared = dot(flat, flat);
    const Vector3 direction =
        length_squared < 0x1p-32F
            ? fallback
            : flat * static_cast<float>(1.0 / std::sqrt(static_cast<double>(length_squared)));
    return 0.0F - dot(behind, direction);
}

// A curve sampled at whole parameters: its value at p is sample(i - 1)
// (0 below the first) moved toward sample(i) by p - i, with i = p truncated
// and at most `count` (no move at i = count), as the original computes it.
template <class Sample>
[[nodiscard]] float sampled_curve(Sample&& sample, const std::int32_t count, const float parameter) {
    std::int32_t index = truncate_to_int(parameter);
    if (count < index) {
        index = count;
    }
    float value = index - 1 < 0 ? 0.0F : sample(index - 1);
    if (index < count) {
        value = (0.0F + value) + (parameter - static_cast<float>(index)) * (sample(index) - value);
    }
    return value;
}

} // namespace takedown::engine
