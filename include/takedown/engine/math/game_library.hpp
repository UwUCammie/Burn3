#pragma once

// The C library's float functions as the game uses them: computed in double
// precision and rounded once. Arguments are read as the game's processor
// holds them (denormals are zero, an all-ones exponent is an ordinary huge
// number) and results are kept in its float range (overflow saturates, an
// invalid result is the largest float), as src/runtime/game/math_library.cpp
// replaces the originals.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace takedown::engine {

[[nodiscard]] inline double game_float_value(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xffU);
    const double sign = (bits & 0x80000000U) != 0U ? -1.0 : 1.0;
    if (exponent == 0) {
        return sign * 0.0;
    }
    const double mantissa = 1.0 + static_cast<double>(bits & 0x7fffffU) / 8388608.0;
    return sign * std::ldexp(mantissa, exponent - 127);
}

[[nodiscard]] inline float game_float_result(const double value) noexcept {
    constexpr float kLargest = std::numeric_limits<float>::max();
    if (std::isnan(value)) {
        return kLargest;
    }
    if (std::fabs(value) >= static_cast<double>(kLargest)) {
        return std::copysign(kLargest, static_cast<float>(value));
    }
    const auto result = static_cast<float>(value);
    return std::fabs(result) < std::numeric_limits<float>::min() ? std::copysign(0.0F, result) : result;
}

[[nodiscard]] inline float game_asin(const float x) noexcept {
    return game_float_result(std::asin(game_float_value(x)));
}
[[nodiscard]] inline float game_acos(const float x) noexcept {
    return game_float_result(std::acos(game_float_value(x)));
}
[[nodiscard]] inline float game_tan(const float x) noexcept {
    return game_float_result(std::tan(game_float_value(x)));
}
[[nodiscard]] inline float game_pow(const float x, const float y) noexcept {
    return game_float_result(std::pow(game_float_value(x), game_float_value(y)));
}

} // namespace takedown::engine
