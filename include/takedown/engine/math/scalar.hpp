#pragma once

// Scalar helpers with fully defined results, for game code that relies on
// them: conversions that saturate instead of overflowing, and min/max that
// order -0 below +0.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace takedown::engine {

// Truncates toward zero; values beyond the int32 range saturate.
[[nodiscard]] inline std::int32_t truncate_to_int(const float value) noexcept {
    if (value >= 2147483648.0F) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= -2147483648.0F) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] inline float minimum(const float a, const float b) noexcept {
    if (a == b) {
        return std::bit_cast<std::int32_t>(a) < std::bit_cast<std::int32_t>(b) ? a : b;
    }
    return a < b ? a : b;
}

[[nodiscard]] inline float maximum(const float a, const float b) noexcept {
    if (a == b) {
        return std::bit_cast<std::int32_t>(a) < std::bit_cast<std::int32_t>(b) ? b : a;
    }
    return a < b ? b : a;
}

[[nodiscard]] inline float clamp(const float value, const float low, const float high) noexcept {
    return maximum(minimum(value, high), low);
}

// A float as the game's processor reads it for a comparison: no denormals
// (they are zero) and no infinities or NaNs (the largest float instead).
[[nodiscard]] inline float game_operand(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000U) == 0U) {
        return std::bit_cast<float>(bits & 0x80000000U);
    }
    if ((bits & 0x7f800000U) == 0x7f800000U) {
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f7fffffU);
    }
    return value;
}

// The processor's "less than" of two floats.
[[nodiscard]] inline bool game_less(const float a, const float b) noexcept {
    return game_operand(a) < game_operand(b);
}

[[nodiscard]] inline bool game_less_equal(const float a, const float b) noexcept {
    return game_operand(a) <= game_operand(b);
}

[[nodiscard]] inline bool game_equal(const float a, const float b) noexcept {
    return game_operand(a) == game_operand(b);
}

// The scalar unit's square root (of the magnitude; zero for zero and
// denormals), rounded to nearest.
[[nodiscard]] inline float game_sqrt(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000U) == 0U) {
        return std::bit_cast<float>(bits & 0x80000000U);
    }
    return std::sqrt(std::bit_cast<float>(std::bit_cast<std::uint32_t>(game_operand(value)) & 0x7fffffffU));
}

// The scalar unit's divide: rounds to nearest, and a zero divisor gives the
// largest float signed as the quotient would be.
[[nodiscard]] inline float game_div(const float numerator, const float denominator) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(denominator);
    if ((bits & 0x7f800000U) == 0U) {
        return std::bit_cast<float>(((std::bit_cast<std::uint32_t>(numerator) ^ bits) & 0x80000000U) | 0x7f7fffffU);
    }
    return game_operand(game_operand(numerator) / game_operand(denominator));
}

} // namespace takedown::engine
