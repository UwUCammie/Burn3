#pragma once

// EE COP1 (scalar FPU) arithmetic as the reference interpreter models it.
// Shared by src/guest/ee_interpreter.cpp and generated EE code so both use
// one definition of every operand clamp, rounding and FCR31 flag.

#include "takedown/guest/ps2_float.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace takedown::guest::ee_cop1 {

inline constexpr std::uint32_t kFcr31CauseOverflow = 0x00008000U;
inline constexpr std::uint32_t kFcr31CauseUnderflow = 0x00004000U;
inline constexpr std::uint32_t kFcr31CauseInvalid = 0x00020000U;
inline constexpr std::uint32_t kFcr31CauseDivideByZero = 0x00010000U;
inline constexpr std::uint32_t kFcr31StickyInvalid = 0x00000040U;
inline constexpr std::uint32_t kFcr31StickyDivideByZero = 0x00000020U;
inline constexpr std::uint32_t kFcr31StickyOverflow = 0x00000010U;
inline constexpr std::uint32_t kFcr31StickyUnderflow = 0x00000008U;
inline constexpr std::uint32_t kFcr31Condition = 0x00800000U;

[[nodiscard]] inline bool is_cop1_denormal(const std::uint32_t bits) noexcept {
    return (bits & 0x7f800000U) == 0U && (bits & 0x007fffffU) != 0U;
}

[[nodiscard]] inline std::uint32_t normalize_cop1_operand(const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

inline std::uint32_t finish_cop1_arithmetic(R5900State& state, const std::uint32_t bits) noexcept {
    auto flags = state.fcr31_bits() & ~(kFcr31CauseOverflow | kFcr31CauseUnderflow);
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        flags |= kFcr31CauseOverflow | kFcr31StickyOverflow;
        state.set_fcr31_bits(flags);
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    if (is_cop1_denormal(bits)) {
        flags |= kFcr31CauseUnderflow | kFcr31StickyUnderflow;
        state.set_fcr31_bits(flags);
        return bits & 0x80000000U;
    }
    state.set_fcr31_bits(flags);
    return bits;
}

[[nodiscard]] inline std::uint32_t cop1_narrow_chop_zero(const double exact) noexcept {
    // PCSX2 runs the ordinary EE scalar-FPU pipeline in the PS2 default
    // ChopZero mode. Binary32 ADD/SUB inputs and binary32 products are exact
    // in a host double, so narrow once and undo a host conversion only when
    // it rounded away from zero. This also converts a rounded infinity to
    // signed Fmax for a finite overflowing exact result, matching ChopZero.
    return ps2_narrow_binary32_toward_zero_bits(exact);
}

[[nodiscard]] inline std::uint32_t cop1_add_normal_finite(const std::uint32_t left,
                                                          const std::uint32_t right) noexcept {
    const auto exact = static_cast<double>(std::bit_cast<float>(left)) +
                       static_cast<double>(std::bit_cast<float>(right));
    return cop1_narrow_chop_zero(exact);
}

[[nodiscard]] inline std::uint32_t cop1_div_normal_finite(const std::uint32_t left,
                                                          const std::uint32_t right) noexcept {
    const auto result = std::bit_cast<float>(left) / std::bit_cast<float>(right);
    return std::bit_cast<std::uint32_t>(result);
}

[[nodiscard]] inline std::uint32_t cop1_sub_normal_finite(const std::uint32_t left,
                                                          const std::uint32_t right) noexcept {
    const auto exact = static_cast<double>(std::bit_cast<float>(left)) -
                       static_cast<double>(std::bit_cast<float>(right));
    return cop1_narrow_chop_zero(exact);
}

[[nodiscard]] inline std::uint32_t cop1_mul_normal_finite(const std::uint32_t left,
                                                          const std::uint32_t right) noexcept {
    const auto exact = static_cast<double>(std::bit_cast<float>(left)) *
                       static_cast<double>(std::bit_cast<float>(right));
    return cop1_narrow_chop_zero(exact);
}

[[nodiscard]] inline bool is_cop1_nonnegative(const std::uint32_t bits) noexcept {
    return (bits & 0x80000000U) == 0U;
}

[[nodiscard]] inline std::uint32_t cop1_sqrt_normal_finite(const std::uint32_t value) noexcept {
    const auto result = std::sqrt(std::bit_cast<float>(value));
    return std::bit_cast<std::uint32_t>(result);
}

// ---- Whole instructions (fd, fs, ft are FPR indices) --------------------------

inline void add_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.write_fpr_bits(fd, finish_cop1_arithmetic(state, cop1_add_normal_finite(left, right)));
}

inline void sub_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.write_fpr_bits(fd, finish_cop1_arithmetic(state, cop1_sub_normal_finite(left, right)));
}

inline void mul_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.write_fpr_bits(fd, finish_cop1_arithmetic(state, cop1_mul_normal_finite(left, right)));
}

inline void div_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto numerator = state.read_fpr_bits(fs);
    const auto denominator = state.read_fpr_bits(ft);
    // The EE has no IEEE infinity/NaN result for a zero divisor. Both signed
    // zero and denormals count as zero, and DIV.S saturates to the signed
    // maximum finite bit pattern while recording either Invalid (0/0) or
    // Divide-by-zero (nonzero/0) in FCR31.
    if ((denominator & 0x7f800000U) == 0U) {
        const bool zero_numerator = (numerator & 0x7f800000U) == 0U;
        state.write_fpr_bits(fd, ((numerator ^ denominator) & 0x80000000U) | 0x7f7fffffU);
        state.set_fcr31_bits(state.fcr31_bits() |
                             (zero_numerator ? kFcr31CauseInvalid | kFcr31StickyInvalid
                                             : kFcr31CauseDivideByZero | kFcr31StickyDivideByZero));
        return;
    }
    // PCSX2's FPU interpreter applies fpuDouble() to both nonzero-divisor
    // operands. Exponent-zero numerators therefore become signed zero,
    // exponent-255 operands become signed Fmax, and a denormal/overflow
    // quotient is flushed/clamped without setting DIV.S O/U flags.
    const auto normalized_numerator = normalize_cop1_operand(numerator);
    const auto normalized_denominator = normalize_cop1_operand(denominator);
    state.write_fpr_bits(
        fd, normalize_cop1_operand(cop1_div_normal_finite(normalized_numerator, normalized_denominator)));
}

inline void max_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(state.read_fpr_bits(fs));
    const auto signed_right = std::bit_cast<std::int32_t>(state.read_fpr_bits(ft));
    // MAX.S uses the EE FPU's raw sign/magnitude ordering, including its
    // defined handling for signed zero and exponent-255 bit patterns.
    // Comparing signed encodings in the opposite direction when both values
    // are negative reproduces that ordering without host NaNs.
    const auto result = signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                            : std::max(signed_left, signed_right);
    state.write_fpr_bits(fd, std::bit_cast<std::uint32_t>(result));
    state.set_fcr31_bits(state.fcr31_bits() & ~(kFcr31CauseOverflow | kFcr31CauseUnderflow));
}

inline void min_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(state.read_fpr_bits(fs));
    const auto signed_right = std::bit_cast<std::int32_t>(state.read_fpr_bits(ft));
    // MIN.S is the inverse of the EE's raw sign/magnitude MAX.S order. In
    // particular it chooses -0 over +0 and reverses signed-integer ordering
    // when both operands carry a negative sign bit.
    const auto result = signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                            : std::min(signed_left, signed_right);
    state.write_fpr_bits(fd, std::bit_cast<std::uint32_t>(result));
    state.set_fcr31_bits(state.fcr31_bits() & ~(kFcr31CauseOverflow | kFcr31CauseUnderflow));
}

inline void abs_s(R5900State& state, const unsigned fd, const unsigned fs) noexcept {
    // ABS.S is a bitwise sign clear on the EE. It preserves zero, denormal and
    // exponent-255 payloads, while clearing the current O/U cause bits (the
    // sticky history remains intact).
    state.write_fpr_bits(fd, state.read_fpr_bits(fs) & 0x7fffffffU);
    state.set_fcr31_bits(state.fcr31_bits() & ~(kFcr31CauseOverflow | kFcr31CauseUnderflow));
}

inline void neg_s(R5900State& state, const unsigned fd, const unsigned fs) noexcept {
    state.write_fpr_bits(fd, state.read_fpr_bits(fs) ^ 0x80000000U);
    state.set_fcr31_bits(state.fcr31_bits() & ~(kFcr31CauseOverflow | kFcr31CauseUnderflow));
}

// Unlike ordinary MIPS encodings, EE SQRT.S reads FT and requires the FS field
// to be zero. This permits the reached in-place sqrt f1,f1.
inline void sqrt_s(R5900State& state, const unsigned fd, const unsigned ft) noexcept {
    const auto source = state.read_fpr_bits(ft);
    auto next_fcr31 = state.fcr31_bits() & ~(kFcr31CauseInvalid | kFcr31CauseDivideByZero);
    if ((source & 0x7f800000U) == 0U) {
        // Denormals are signed zero inputs on the EE, just like explicit
        // +/-0. Neither negative zero nor a signed denormal is Invalid.
        state.write_fpr_bits(fd, source & 0x80000000U);
        state.set_fcr31_bits(next_fcr31);
        return;
    }
    const bool negative = !is_cop1_nonnegative(source);
    const auto magnitude = normalize_cop1_operand(source) & 0x7fffffffU;
    if (negative) {
        next_fcr31 |= kFcr31CauseInvalid | kFcr31StickyInvalid;
    }
    state.write_fpr_bits(fd, cop1_sqrt_normal_finite(magnitude));
    state.set_fcr31_bits(next_fcr31);
}

// EE RSQRT.S divides FS by sqrt(FT). Both inputs are read before the write,
// because the reached normalization form writes back to its FS register.
inline void rsqrt_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto numerator = state.read_fpr_bits(fs);
    const auto radicand = state.read_fpr_bits(ft);
    auto next_fcr31 = state.fcr31_bits() & ~(kFcr31CauseInvalid | kFcr31CauseDivideByZero);
    // Signed zero and denormals are zero divisors on the EE. PCSX2's
    // authoritative RSQRT.S path always records Divide here and takes the
    // saturated result sign from FT, including for a zero numerator.
    if ((radicand & 0x7f800000U) == 0U) {
        state.write_fpr_bits(fd, (radicand & 0x80000000U) | 0x7f7fffffU);
        state.set_fcr31_bits(next_fcr31 | kFcr31CauseDivideByZero | kFcr31StickyDivideByZero);
        return;
    }
    const bool negative_radicand = !is_cop1_nonnegative(radicand);
    const auto normalized_numerator = normalize_cop1_operand(numerator);
    const auto magnitude = normalize_cop1_operand(radicand) & 0x7fffffffU;
    const auto root = std::sqrt(std::bit_cast<float>(magnitude));
    const auto quotient = std::bit_cast<float>(normalized_numerator) / root;
    const auto result = normalize_cop1_operand(std::bit_cast<std::uint32_t>(quotient));
    if (negative_radicand) {
        next_fcr31 |= kFcr31CauseInvalid | kFcr31StickyInvalid;
    }
    state.write_fpr_bits(fd, result);
    state.set_fcr31_bits(next_fcr31);
}

inline void cvt_w_s(R5900State& state, const unsigned fd, const unsigned fs) noexcept {
    const auto source_bits = state.read_fpr_bits(fs);
    std::uint32_t result{};
    // EE CVT.W.S truncates toward zero. Inputs outside the signed-word range
    // (including exponent-255 bit patterns) saturate according to their sign
    // instead of relying on undefined host float-to-int casts.
    if ((source_bits & 0x7f800000U) <= 0x4e800000U) {
        result = std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(source_bits)));
    } else {
        result = (source_bits & 0x80000000U) == 0U ? 0x7fffffffU : 0x80000000U;
    }
    state.write_fpr_bits(fd, result);
}

inline void cvt_s_w(R5900State& state, const unsigned fd, const unsigned fs) noexcept {
    const auto source = std::bit_cast<std::int32_t>(state.read_fpr_bits(fs));
    state.write_fpr_bits(fd, cop1_narrow_chop_zero(static_cast<double>(source)));
}

inline void adda_s(R5900State& state, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.set_fpu_accumulator_bits(finish_cop1_arithmetic(state, cop1_add_normal_finite(left, right)));
}

inline void suba_s(R5900State& state, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.set_fpu_accumulator_bits(finish_cop1_arithmetic(state, cop1_sub_normal_finite(left, right)));
}

inline void mula_s(R5900State& state, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    state.set_fpu_accumulator_bits(finish_cop1_arithmetic(state, cop1_mul_normal_finite(left, right)));
}

// The EE rounds the product to a single-precision intermediate before
// combining it with ACC; these are not host fused multiply-adds.
inline void madd_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto accumulator = normalize_cop1_operand(state.fpu_accumulator_bits());
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    const auto product = normalize_cop1_operand(cop1_mul_normal_finite(left, right));
    state.write_fpr_bits(fd, finish_cop1_arithmetic(state, cop1_add_normal_finite(accumulator, product)));
}

inline void msub_s(R5900State& state, const unsigned fd, const unsigned fs, const unsigned ft) noexcept {
    const auto accumulator = normalize_cop1_operand(state.fpu_accumulator_bits());
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    const auto product = normalize_cop1_operand(cop1_mul_normal_finite(left, right));
    state.write_fpr_bits(fd, finish_cop1_arithmetic(state, cop1_sub_normal_finite(accumulator, product)));
}

inline void madda_s(R5900State& state, const unsigned fs, const unsigned ft) noexcept {
    const auto accumulator = normalize_cop1_operand(state.fpu_accumulator_bits());
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    const auto product = normalize_cop1_operand(cop1_mul_normal_finite(left, right));
    state.set_fpu_accumulator_bits(finish_cop1_arithmetic(state, cop1_add_normal_finite(accumulator, product)));
}

inline void msuba_s(R5900State& state, const unsigned fs, const unsigned ft) noexcept {
    const auto accumulator = normalize_cop1_operand(state.fpu_accumulator_bits());
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    const auto product = normalize_cop1_operand(cop1_mul_normal_finite(left, right));
    state.set_fpu_accumulator_bits(finish_cop1_arithmetic(state, cop1_sub_normal_finite(accumulator, product)));
}

// C.EQ.S / C.LT.S / C.LE.S. The EE does not have IEEE NaNs or denormals.
// PCSX2's C_cond_S compares fpuDouble() values, which flushes exponent-zero
// operands to signed zero and clamps every exponent-255 payload to signed
// Fmax. Raw host comparisons would incorrectly make qNaN-shaped contact
// values unordered and can reject the road predicates at 0x263cf0, 0x263d00,
// 0x263d44, and 0x263d70.
enum class Compare : std::uint8_t { Equal, Less, LessEqual };
inline void compare_s(R5900State& state, const Compare kind, const unsigned fs, const unsigned ft) noexcept {
    const auto left = normalize_cop1_operand(state.read_fpr_bits(fs));
    const auto right = normalize_cop1_operand(state.read_fpr_bits(ft));
    bool condition{};
    if (kind == Compare::Equal) {
        condition = std::bit_cast<float>(left) == std::bit_cast<float>(right);
    } else if (kind == Compare::LessEqual) {
        condition = std::bit_cast<float>(left) <= std::bit_cast<float>(right);
    } else {
        condition = std::bit_cast<float>(left) < std::bit_cast<float>(right);
    }
    state.set_fcr31_bits(condition ? state.fcr31_bits() | kFcr31Condition
                                   : state.fcr31_bits() & ~kFcr31Condition);
}

} // namespace takedown::guest::ee_cop1
