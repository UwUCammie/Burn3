#pragma once

// The game's two random number generators.

#include <bit>
#include <cstdint>

namespace takedown::engine {

// Effects keep a pair of words: each step rotates the first by half a word
// (the low half sign-extended), adds the second to it, then adds the new
// first to the second. The new first word is the value.
struct EffectRandom {
    std::uint32_t first{};
    std::uint32_t second{};

    std::uint32_t step() noexcept {
        const auto rotated = (first << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(first) >> 16);
        first = rotated + second;
        second += first;
        return first;
    }
};

// The vector unit's generator: a 23-bit shift register read as a float in
// [1, 2). Seeding keeps a float's mantissa bits.
struct VectorRandom {
    std::uint32_t bits{0x3f800000U};

    void seed(const float value) noexcept { bits = (std::bit_cast<std::uint32_t>(value) & 0x007fffffU) | 0x3f800000U; }

    float next() noexcept {
        const auto feedback = ((bits >> 4U) ^ (bits >> 22U)) & 1U;
        bits = (((bits << 1U) ^ feedback) & 0x007fffffU) | 0x3f800000U;
        return std::bit_cast<float>(bits);
    }
};

} // namespace takedown::engine
