#pragma once

// Small vector types for engine code. Vec4 matches the game's 16-byte
// vectors (x, y, z, w floats) so engine data can share the game's layouts.

#include <cmath>
#include <cstdint>
#include <limits>

namespace takedown::engine {

struct Vector3 {
    float x{};
    float y{};
    float z{};
};

struct alignas(16) Vector4 {
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] Vector3 xyz() const noexcept { return {x, y, z}; }
};

[[nodiscard]] constexpr Vector3 operator+(const Vector3 a, const Vector3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vector3 operator-(const Vector3 a, const Vector3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vector3 operator-(const Vector3 a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vector3 operator*(const Vector3 a, const float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}

// Summed x, then y, then z, like the original's multiply-accumulate.
[[nodiscard]] constexpr float dot(const Vector3 a, const Vector3 b) noexcept {
    return (a.x * b.x + a.y * b.y) + a.z * b.z;
}

[[nodiscard]] constexpr Vector3 cross(const Vector3 a, const Vector3 b) noexcept {
    return {a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x, a.x * b.y - b.x * a.y};
}

// 1 / sqrt(value), with the original's result for zero (the largest float
// instead of infinity) so degenerate input stays finite.
[[nodiscard]] inline float reciprocal_sqrt(const float value) noexcept {
    if (!(value > 0.0F)) {
        return std::numeric_limits<float>::max();
    }
    return 1.0F / std::sqrt(value);
}

} // namespace takedown::engine
