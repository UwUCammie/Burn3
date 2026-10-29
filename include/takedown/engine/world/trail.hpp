#pragma once

// A trail's points as the renderer takes them: each direction packed into
// four s16 lanes and each colour into four bytes.

#include "takedown/engine/math/scalar.hpp"
#include "takedown/engine/math/vector.hpp"

#include <array>
#include <cstdint>

namespace takedown::engine {

// Each lane truncated and saturated to s16.
[[nodiscard]] inline std::array<std::int16_t, 4> packed_trail_direction(const Vector4 lanes) noexcept {
    const auto pack = [](const float value) {
        const std::int32_t whole = truncate_to_int(value);
        return static_cast<std::int16_t>(whole < -32768 ? -32768 : whole > 32767 ? 32767 : whole);
    };
    return {pack(lanes.x), pack(lanes.y), pack(lanes.z), pack(lanes.w)};
}

// Each lane truncated and capped at 255 (not floored: a negative lane keeps
// its low byte), the first lane in the low byte.
[[nodiscard]] inline std::uint32_t packed_trail_colour(const Vector4 lanes) noexcept {
    const auto pack = [](const float value) {
        const std::int32_t whole = truncate_to_int(value);
        return static_cast<std::uint32_t>(whole > 255 ? 255 : whole) & 0xffU;
    };
    return pack(lanes.x) | (pack(lanes.y) << 8U) | (pack(lanes.z) << 16U) | (pack(lanes.w) << 24U);
}

} // namespace takedown::engine
