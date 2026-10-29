#pragma once

// The game's float arithmetic as its processor performs it: every add,
// subtract and multiply rounds toward zero, a multiply-add rounds the
// product before adding, and results below the smallest normal float become
// zero. For game code whose results depend on that rounding (random number
// tricks that keep only the rounding residue, counts truncated from sums).

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace takedown::engine {

// Rounds to nearest, then steps the magnitude down once when that moved
// away from zero (without a branch on the data: the step is taken about
// half the time, unpredictably).
[[nodiscard]] inline float chop(const double exact) noexcept {
    constexpr auto kLargest = static_cast<double>(std::numeric_limits<float>::max());
    if (std::fabs(exact) > kLargest) [[unlikely]] {
        return exact > 0.0 ? std::numeric_limits<float>::max() : -std::numeric_limits<float>::max();
    }
    const auto rounded = static_cast<float>(exact);
    auto bits = std::bit_cast<std::uint32_t>(rounded);
    bits -= static_cast<std::uint32_t>(std::fabs(static_cast<double>(rounded)) > std::fabs(exact));
    if ((bits & 0x7f800000U) == 0U) [[unlikely]] {
        bits &= 0x80000000U;
    }
    return std::bit_cast<float>(bits);
}

[[nodiscard]] inline float chop_add(const float a, const float b) noexcept {
    return chop(static_cast<double>(a) + static_cast<double>(b));
}
[[nodiscard]] inline float chop_sub(const float a, const float b) noexcept {
    return chop(static_cast<double>(a) - static_cast<double>(b));
}
[[nodiscard]] inline float chop_mul(const float a, const float b) noexcept {
    return chop(static_cast<double>(a) * static_cast<double>(b));
}
// accumulator + a * b, the product rounded first.
[[nodiscard]] inline float chop_madd(const float accumulator, const float a, const float b) noexcept {
    return chop_add(accumulator, chop_mul(a, b));
}
// The vector unit's square root (of the magnitude), divide and reciprocal
// square root also round toward zero. (The scalar unit's divide does not.)
[[nodiscard]] inline float chop_sqrt(const float value) noexcept {
    return chop(std::sqrt(static_cast<double>(value < 0.0F ? -value : value)));
}
[[nodiscard]] inline float chop_div(const float numerator, const float denominator) noexcept {
    if (denominator == 0.0F) {
        const bool negative = std::signbit(numerator) != std::signbit(denominator);
        return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return chop(static_cast<double>(numerator) / static_cast<double>(denominator));
}
[[nodiscard]] inline float chop_reciprocal_sqrt(const float value) noexcept {
    if (value == 0.0F) {
        return std::signbit(value) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return chop_div(1.0F, chop_sqrt(value));
}
[[nodiscard]] inline float chop_from_int(const std::int32_t value) noexcept {
    return chop(static_cast<double>(value));
}
// Truncates toward zero; out of range values saturate by sign.
[[nodiscard]] inline std::int32_t truncate_word(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7f800000U) > 0x4e800000U) {
        return (bits & 0x80000000U) == 0U ? std::numeric_limits<std::int32_t>::max()
                                          : std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}
// An unsigned word as the game converts it: through a signed conversion of
// half the value (keeping the low bit) doubled when the top bit is set.
[[nodiscard]] inline float chop_from_unsigned(const std::uint32_t value) noexcept {
    if (static_cast<std::int32_t>(value) >= 0) {
        return chop_from_int(static_cast<std::int32_t>(value));
    }
    const float half = chop_from_int(static_cast<std::int32_t>((value >> 1U) | (value & 1U)));
    return chop_add(half, half);
}

} // namespace takedown::engine
