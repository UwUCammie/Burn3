#pragma once

// The game's single-precision sine and cosine building blocks: the C
// library's argument-reduced polynomial kernels it was linked with.

namespace takedown::engine {

// sin(x + tail) for |x| <= pi/4 by its odd polynomial. `tail` is the low part
// of an argument split in two by range reduction and is used only when
// `has_tail`; tiny arguments return x unchanged.
[[nodiscard]] float sine_kernel(float x, float tail, bool has_tail) noexcept;

// The game's fast cosine (vehicle code computes four at once on the vector
// unit): the angle is folded to a quarter turn by its fractional number of
// turns, then a degree-9 odd polynomial gives sin(2 pi x). Accurate to about
// 1e-4. The sine is wave_cosine(angle - pi/2).
[[nodiscard]] float wave_cosine(float angle) noexcept;

// wave_cosine as the vector unit computes it, every step rounded toward zero
// and the turns rounded off by adding and removing 1.5 * 2^23: bit for bit
// what the game gets.
[[nodiscard]] float wave_cosine_chopped(float angle) noexcept;

// The first step of wave_cosine_chopped: the angle folded to the polynomial's
// argument, a quarter turn either side of zero (in turns).
[[nodiscard]] float wave_fold_chopped(float angle) noexcept;

inline constexpr float kHalfPi = 1.57079637F;  // 0x3fc90fdb

} // namespace takedown::engine
