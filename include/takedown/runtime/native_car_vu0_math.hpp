#pragma once

// Exact VU0 float arithmetic for plain C++ code (P1 native car constants).
//
// The PS2 vector unit does not follow IEEE-754: FMAC results are rounded toward zero, denormals read and write as signed zero, an
// operand with exponent 255 reads as signed Fmax (clamp mode 3), results past Fmax saturate, MADD/MSUB form a separately chopped
// product before the add, and RSQRT/SQRT chop the root before the divide. The car constants are bit-for-bit what the VU0 microcode
// produced, so the few places that compute them with floats use these scalar helpers, in the microcode's operation order.
// They are thin wrappers over the project's reference arithmetic (guest/vu0_simd_arithmetic.hpp); the EE FPU variants below are
// the COP1 rules (StartEffect runs on the EE, not the VU).

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

namespace takedown::runtime::native_car::vu {

namespace detail = guest::vu0_detail;

[[nodiscard]] inline std::uint32_t bits(const float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
[[nodiscard]] inline float flt(const std::uint32_t value) noexcept { return std::bit_cast<float>(value); }

// ---- VU0 FMAC ------------------------------------------------------------------------------------------------------------------

[[nodiscard]] inline float vadd(const float a, const float b) noexcept {
    bool underflow = false, overflow = false;
    return flt(detail::add_subtract_bits(bits(a), bits(b), false, underflow, overflow));
}
[[nodiscard]] inline float vsub(const float a, const float b) noexcept {
    bool underflow = false, overflow = false;
    return flt(detail::add_subtract_bits(bits(a), bits(b), true, underflow, overflow));
}
[[nodiscard]] inline float vmul(const float a, const float b) noexcept {
    bool underflow = false, overflow = false;
    return flt(detail::multiply_bits(bits(a), bits(b), underflow, overflow));
}
// ACC + a * b (VMADD / VMADDA): the product is chopped to binary32 first.
[[nodiscard]] inline float vmadd(const float acc, const float a, const float b) noexcept {
    bool underflow = false, overflow = false;
    const auto product = detail::multiply_bits(bits(a), bits(b), underflow, overflow);
    return flt(detail::add_subtract_bits(bits(acc), product, false, underflow, overflow));
}
// ACC - a * b (VMSUB / VMSUBA / VOPMSUB).
[[nodiscard]] inline float vmsub(const float acc, const float a, const float b) noexcept {
    bool underflow = false, overflow = false;
    const auto product = detail::multiply_bits(bits(a), bits(b), underflow, overflow);
    return flt(detail::add_subtract_bits(bits(acc), product, true, underflow, overflow));
}
// x + 0: the value as the VU leaves it after a plain copy through an FMAC (denormals and -0 become +-0 / +0).
[[nodiscard]] inline float vcanon(const float a) noexcept { return vadd(a, 0.0F); }
// 0 - x
[[nodiscard]] inline float vneg(const float a) noexcept { return vsub(0.0F, a); }
// MAC sign flag of a result: the sign bit.
[[nodiscard]] inline bool vsign(const float a) noexcept { return (bits(a) & 0x80000000U) != 0U; }

// ---- VU0 divide unit -----------------------------------------------------------------------------------------------------------

// Q = numerator / sqrt(|radicand|) (VRSQRT): a zero radicand gives a signed Fmax (signed zero for 0/0).
[[nodiscard]] inline float vrsqrt(const float numerator, const float radicand) noexcept {
    const auto n = detail::clamp3_operand_bits(bits(numerator));
    const auto r = detail::clamp3_operand_bits(bits(radicand));
    if ((r & 0x7fffffffU) == 0U) {
        const auto sign = (n ^ r) & 0x80000000U;
        return flt((n & 0x7fffffffU) != 0U ? (sign | 0x7f7fffffU) : sign);
    }
    bool underflow = false, overflow = false;
    const auto root = detail::chop_exact_bits(std::sqrt(std::fabs(static_cast<double>(flt(r)))), underflow, overflow);
    return flt(detail::chop_exact_bits(static_cast<double>(flt(n)) / static_cast<double>(flt(root)), underflow, overflow));
}
// Q = sqrt(|x|) (VSQRT).
[[nodiscard]] inline float vsqrt(const float a) noexcept {
    const auto r = detail::clamp3_operand_bits(bits(a));
    bool underflow = false, overflow = false;
    return flt(detail::chop_exact_bits(std::sqrt(std::fabs(static_cast<double>(flt(r)))), underflow, overflow));
}

// VFTOI0: truncate toward zero, saturating by sign past +-2^31.
[[nodiscard]] inline std::uint32_t ftoi0(const float a) noexcept {
    const auto b = bits(a);
    if ((b & 0x7f800000U) > 0x4effffffU) {
        return (b & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(a));
}

// ---- the runtime's StartEffect native (host float arithmetic, round to nearest even) ------------------------------------------------
//
// The runtime does not run the guest's StartEffect (0x220040): the engine native in src/runtime/game/render.cpp replaces it and computes
// the W quads and W+832 with plain host `float` operations, i.e. rounded to NEAREST EVEN, where the guest's EE FPU and VU0 would chop.
// The two differ by one ulp in a third of the cases that matter (W+12 = 38.4 * f12, W+28 = 19.2 * f12, W+48 = 128.1 * f12, 128.1 * f13,
// and position - camera); measured on the W the live game leaves in the captures' scratchpad. The native car path has to carry what
// the live game carries, so it uses these two helpers; they do not depend on the current MXCSR rounding mode.

// The float nearest to `exact` (ties to even), as bits. `exact` is a double holding an exact binary32 product or difference.
[[nodiscard]] inline std::uint32_t nearest_even_bits(const double exact) noexcept {
    const std::uint32_t sign = std::signbit(exact) ? 0x80000000U : 0U;
    const double magnitude = std::fabs(exact);
    if (magnitude == 0.0) {
        return sign;
    }
    if (magnitude >= 340282356779733661637539395458142568448.0) {      // 2^128 - 2^103: rounds to infinity
        return sign | 0x7f800000U;
    }
    int exponent = 0;
    static_cast<void>(std::frexp(magnitude, &exponent));               // magnitude in [2^(e-1), 2^e)
    int binade = exponent - 1;
    if (binade < -126) {
        binade = -126;                                                 // the subnormal grid
    }
    const double ulp = std::ldexp(1.0, binade - 23);
    const double scaled = magnitude / ulp;                             // a power-of-two scaling: exact
    double whole = std::floor(scaled);
    const double remainder = scaled - whole;
    if (remainder > 0.5 || (remainder == 0.5 && std::fmod(whole, 2.0) != 0.0)) {
        whole += 1.0;
    }
    // whole <= 2^24 and ulp is a power of two, so the product is exactly a binary32: the conversion cannot round.
    return sign | std::bit_cast<std::uint32_t>(static_cast<float>(whole * ulp));
}

// a * b and a - b of two binary32 bit patterns as the host's float operators give them (finite operands; otherwise the host result).
[[nodiscard]] inline std::uint32_t host_mul(const std::uint32_t a, const std::uint32_t b) noexcept {
    if ((a & 0x7f800000U) == 0x7f800000U || (b & 0x7f800000U) == 0x7f800000U) {
        return bits(flt(a) * flt(b));
    }
    return nearest_even_bits(static_cast<double>(flt(a)) * static_cast<double>(flt(b)));
}
[[nodiscard]] inline std::uint32_t host_sub(const std::uint32_t a, const std::uint32_t b) noexcept {
    if ((a & 0x7f800000U) == 0x7f800000U || (b & 0x7f800000U) == 0x7f800000U) {
        return bits(flt(a) - flt(b));
    }
    return nearest_even_bits(static_cast<double>(flt(a)) - static_cast<double>(flt(b)));
}

// ---- EE FPU (COP1) -------------------------------------------------------------------------------------------------------------

[[nodiscard]] inline std::uint32_t ee_finish(const std::uint32_t b) noexcept {
    if ((b & 0x7fffffffU) == 0x7f800000U) {
        return (b & 0x80000000U) | 0x7f7fffffU;
    }
    return guest::ee_cop1::is_cop1_denormal(b) ? b & 0x80000000U : b;
}
// MUL.S on raw bits.
[[nodiscard]] inline std::uint32_t ee_mul(const std::uint32_t a, const std::uint32_t b) noexcept {
    return ee_finish(guest::ee_cop1::cop1_mul_normal_finite(guest::ee_cop1::normalize_cop1_operand(a),
                                                            guest::ee_cop1::normalize_cop1_operand(b)));
}

} // namespace takedown::runtime::native_car::vu
