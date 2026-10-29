#include "takedown/engine/math/clipping.hpp"

namespace takedown::engine {

namespace {

enum class Side { kInside, kBelow, kAbove };

[[nodiscard]] float axis_value(const Vector4& vertex, const int axis) noexcept {
    return axis == 0 ? vertex.x : (axis == 1 ? vertex.y : vertex.z);
}

[[nodiscard]] Side classify(const Vector4& vertex, const int axis, const float low, const float high) noexcept {
    const float value = axis_value(vertex, axis);
    if (value < low) {
        return Side::kBelow;
    }
    return value <= high ? Side::kInside : Side::kAbove;
}

} // namespace

std::size_t clip_polygon_to_slab(const std::span<const Vector4> input, const int axis, const float low,
                                 const float high, const std::span<Vector4> output) noexcept {
    const std::size_t count = input.size();
    if (count == 0U) {
        return 0U;
    }
    std::size_t written = 0U;
    const auto emit = [&](const Vector4& vertex) {
        if (written < output.size()) {
            output[written] = vertex;
        }
        ++written;
    };
    Side previous_side = classify(input[count - 1U], axis, low, high);
    for (std::size_t index = 0U; index < count; ++index) {
        const Vector4& current = input[index];
        const Vector4& previous = input[index == 0U ? count - 1U : index - 1U];
        const Side side = classify(current, axis, low, high);
        if (side != previous_side) {
            const float start = axis_value(previous, axis);
            const float inverse_length = 1.0F / (axis_value(current, axis) - start);
            const auto crossing = [&](const float plane) {
                const float t = inverse_length * (plane - start);
                const float rest = 1.0F - t;
                emit(Vector4{t * current.x + rest * previous.x, t * current.y + rest * previous.y,
                             t * current.z + rest * previous.z, current.w});
            };
            if (previous_side == Side::kBelow) {
                crossing(low);
            } else if (previous_side == Side::kAbove) {
                crossing(high);
            }
            if (side == Side::kBelow) {
                crossing(low);
            } else if (side == Side::kAbove) {
                crossing(high);
            }
        }
        if (side == Side::kInside) {
            emit(current);
        }
        previous_side = side;
    }
    return written;
}

} // namespace takedown::engine
