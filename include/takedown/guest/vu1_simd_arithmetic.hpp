#pragma once

#include "takedown/guest/types.hpp"
#include <cstdlib>
#include <string_view>

namespace takedown::guest::vu1_detail {
inline bool native_float_arithmetic_enabled() noexcept {
#if defined(__SSE2__)
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_NATIVE_FLOAT_ARITHMETIC");
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
#else
    return false;
#endif
}
inline bool simd_arithmetic_enabled() noexcept {
#if defined(__SSE2__)
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_SIMD_ARITHMETIC");
#if defined(_WIN32)
        if (value == nullptr) return true;
#endif
        return value != nullptr && std::string_view{value} == "1";
    }();
    return enabled;
#else
    return false;
#endif
}
}

#if defined(__SSE2__)
#include <emmintrin.h>

namespace takedown::guest::vu1_detail {

// The floating-point environment of a VU1 launch. The VU truncates every
// result (its only rounding mode is toward zero), so the host rounds toward
// zero for the whole launch: a native binary32 add, subtract or multiply of
// clamped operands is then the VU's result, and the double-precision
// reference helpers chop the exact value instead of a value that was first
// rounded to nearest in double. Round-to-nearest here is not a harmless
// last-bit difference: Burnout's crash effects whited out the whole frame.
// With native float arithmetic the launch also flushes denormals (FTZ|DAZ),
// which is the VU's own behaviour and avoids hardware denormal assists.
// Every caller control bit is restored on exit.
class NativeFloatArithmeticScope {
public:
    explicit NativeFloatArithmeticScope(bool enabled = native_float_arithmetic_enabled()) noexcept
        : saved_(_mm_getcsr()) {
        _mm_setcsr(saved_ | kRoundTowardZero | (enabled ? 0x8040U : 0U));
    }
    ~NativeFloatArithmeticScope() { _mm_setcsr(saved_); }
    NativeFloatArithmeticScope(const NativeFloatArithmeticScope&) = delete;
    NativeFloatArithmeticScope& operator=(const NativeFloatArithmeticScope&) = delete;
private:
    static constexpr unsigned kRoundTowardZero = 0x6000U;
    unsigned saved_{};
};

enum class SimdArithmetic { Add, Subtract, Multiply, MultiplyAdd, MultiplySubtract };
struct SimdArithmeticResult {
    Gpr128 value{};
    std::uint32_t underflow_mask{};
    std::uint32_t overflow_mask{};
};

inline std::uint16_t simd_mac_flags(const Gpr128& value, std::uint32_t mask) noexcept {
    // Reverse lanes once so movemask directly yields the VU's XYZW bit order.
    const auto bits = _mm_shuffle_epi32(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.words.data())),
        _MM_SHUFFLE(0, 1, 2, 3));
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto zero = _mm_cmpeq_epi32(
        _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_setzero_si128());
    const auto underflow = _mm_andnot_si128(zero,
        _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
    const auto overflow = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    const auto lanes = [](__m128i v) { return _mm_movemask_ps(_mm_castsi128_ps(v)); };
    return static_cast<std::uint16_t>((lanes(zero) | (lanes(bits) << 4) |
        (lanes(underflow) << 8) | (lanes(overflow) << 12)) & ((mask & 15U) * 0x1111U));
}

inline __m128i simd_select(__m128i mask, __m128i yes, __m128i no) noexcept {
    return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
}

inline __m128 simd_clamp3(__m128i bits) noexcept {
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    bits = simd_select(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()), sign, bits);
    return _mm_castsi128_ps(simd_select(
        _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000)),
        _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)), bits));
}

// Rounding toward zero turns an overflowing sum or product into signed Fmax,
// the same bits as a result that is exactly Fmax (which the game does
// produce: it moves saturated values around). The kernels below keep the
// contract their callers were written for, "an overflowed raw result is an
// infinity", by putting the infinity back. Fmax lanes are rare, so this is
// one compare on the fast path and an out-of-line exact judgment, the same
// one the reference makes (the double-precision result exceeds Fmax), when
// one shows up.
enum class SimdNativeFinal { Add, Subtract, Multiply };

[[gnu::noinline, gnu::cold]] inline __m128 simd_restore_native_overflow(
    const __m128 raw, const __m128 left, const __m128 right,
    const SimdNativeFinal operation) noexcept {
    const auto wide = [operation](const __m128d a, const __m128d b) {
        return operation == SimdNativeFinal::Add        ? _mm_add_pd(a, b)
               : operation == SimdNativeFinal::Subtract ? _mm_sub_pd(a, b)
                                                        : _mm_mul_pd(a, b);
    };
    const auto magnitude = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
    const auto maximum = _mm_set1_pd(0x1.fffffep127);
    const auto exceeds = [&](const __m128d exact) {
        return _mm_castpd_si128(_mm_cmpgt_pd(_mm_and_pd(exact, magnitude), maximum));
    };
    const auto low = exceeds(wide(_mm_cvtps_pd(left), _mm_cvtps_pd(right)));
    const auto high = exceeds(wide(_mm_cvtps_pd(_mm_movehl_ps(left, left)),
                                   _mm_cvtps_pd(_mm_movehl_ps(right, right))));
    const auto overflowed = _mm_unpacklo_epi64(_mm_shuffle_epi32(low, _MM_SHUFFLE(2, 0, 2, 0)),
                                               _mm_shuffle_epi32(high, _MM_SHUFFLE(2, 0, 2, 0)));
    const auto bits = _mm_castps_si128(raw);
    const auto infinity = _mm_or_si128(
        _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U))),
        _mm_set1_epi32(0x7f800000));
    return _mm_castsi128_ps(simd_select(overflowed, infinity, bits));
}

[[gnu::always_inline]] inline __m128 simd_native_result(
    const __m128 raw, const __m128 left, const __m128 right,
    const SimdNativeFinal operation) noexcept {
    const auto saturated = _mm_cmpgt_epi32(
        _mm_and_si128(_mm_castps_si128(raw), _mm_set1_epi32(0x7fffffff)),
        _mm_set1_epi32(0x7f7ffffe));
    if (_mm_movemask_ps(_mm_castsi128_ps(saturated)) != 0) [[unlikely]] {
        return simd_restore_native_overflow(raw, left, right, operation);
    }
    return raw;
}

// Native binary32 arithmetic in place of the scalar reference's chopped double
// intermediate. Inside a NativeFloatArithmeticScope the host rounds toward
// zero, so each add, subtract and multiply is already the chopped result.
// Keep exceptional input clamping, signed-zero flushing, and separate
// multiply/add stages.
// Normalized inputs are a compiler proof, never a runtime assumption: every
// observed lane already passed this same clamp. Unobserved result/flag lanes
// must remain masked by the caller when it supplies a partial-lane proof.
template <SimdArithmetic Operation, bool LeftNormalized = false,
          bool RightNormalized = false, bool AccumulatorNormalized = false>
#if defined(__GNUC__) || defined(__clang__)
// Keep values and exception masks visible to native-block liveness analysis.
// An out-of-line aggregate return forces calculation of unused flag masks.
[[gnu::always_inline]]
#endif
inline SimdArithmeticResult native_float_arithmetic(
    const Gpr128& left, const Gpr128& right, const Gpr128& accumulator = {}) noexcept {
    const auto load = []<bool Normalized>(const Gpr128& value) {
        const auto bits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.words.data()));
        if constexpr (Normalized) return _mm_castsi128_ps(bits);
        else return simd_clamp3(bits);
    };
    const auto a = load.template operator()<LeftNormalized>(left);
    const auto b = load.template operator()<RightNormalized>(right);
    __m128 result;
    if constexpr (Operation == SimdArithmetic::Add)
        result = simd_native_result(_mm_add_ps(a, b), a, b, SimdNativeFinal::Add);
    else if constexpr (Operation == SimdArithmetic::Subtract)
        result = simd_native_result(_mm_sub_ps(a, b), a, b, SimdNativeFinal::Subtract);
    else {
        result = _mm_mul_ps(a, b);
        if constexpr (Operation == SimdArithmetic::Multiply) {
            result = simd_native_result(result, a, b, SimdNativeFinal::Multiply);
        } else {
            // An overflowed product is already the signed Fmax the clamp
            // between the two stages would make of it.
            const auto product = simd_clamp3(_mm_castps_si128(result));
            const auto c = load.template operator()<AccumulatorNormalized>(accumulator);
            if constexpr (Operation == SimdArithmetic::MultiplyAdd)
                result = simd_native_result(_mm_add_ps(c, product), c, product,
                                            SimdNativeFinal::Add);
            else
                result = simd_native_result(_mm_sub_ps(c, product), c, product,
                                            SimdNativeFinal::Subtract);
        }
    }
    const auto bits = _mm_castps_si128(result);
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto absolute = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    const auto underflow = _mm_andnot_si128(_mm_cmpeq_epi32(absolute, _mm_setzero_si128()),
        _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
    const auto overflow = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    const auto lane_mask = [](__m128i lanes) {
        return static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(
            _mm_shuffle_epi32(lanes, _MM_SHUFFLE(0, 1, 2, 3)))));
    };
    SimdArithmeticResult output;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output.value.words.data()),
                    _mm_castps_si128(simd_clamp3(bits)));
    output.underflow_mask = lane_mask(underflow);
    output.overflow_mask = lane_mask(overflow);
    return output;
}

inline __m128i simd_pack_double_masks(__m128d lo, __m128d hi) noexcept {
    return _mm_unpacklo_epi64(
        _mm_shuffle_epi32(_mm_castpd_si128(lo), _MM_SHUFFLE(2, 0, 2, 0)),
        _mm_shuffle_epi32(_mm_castpd_si128(hi), _MM_SHUFFLE(2, 0, 2, 0)));
}

struct SimdNarrowResult { __m128i bits, underflow, overflow; };

// Two doubles represent every product of two finite binary32 operands exactly.
// Add/subtract use the same host-double operation as the scalar reference.
// Correct an away-from-zero conversion by one binary32 ULP, without changing
// MXCSR or relying on a different floating-point environment for other code.
inline SimdNarrowResult simd_chop(__m128d lo, __m128d hi) noexcept {
    const auto sign64 = _mm_set1_pd(-0.0);
    const auto abs_lo = _mm_andnot_pd(sign64, lo);
    const auto abs_hi = _mm_andnot_pd(sign64, hi);
    const auto rounded_lo = _mm_cvtpd_ps(lo);
    const auto rounded_hi = _mm_cvtpd_ps(hi);
    auto bits = _mm_castps_si128(_mm_movelh_ps(rounded_lo, rounded_hi));
    const auto away = simd_pack_double_masks(
        _mm_cmpgt_pd(_mm_andnot_pd(sign64, _mm_cvtps_pd(rounded_lo)), abs_lo),
        _mm_cmpgt_pd(_mm_andnot_pd(sign64, _mm_cvtps_pd(rounded_hi)), abs_hi));
    bits = _mm_add_epi32(bits, away); // all-ones mask subtracts one ULP.
    const auto overflow = simd_pack_double_masks(
        _mm_cmpgt_pd(abs_lo, _mm_set1_pd(0x1.fffffep127)),
        _mm_cmpgt_pd(abs_hi, _mm_set1_pd(0x1.fffffep127)));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    bits = simd_select(overflow, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)), bits);
    const auto denormal = _mm_cmpeq_epi32(
        _mm_and_si128(bits, _mm_set1_epi32(0x7f800000)), _mm_setzero_si128());
    const auto nonzero = simd_pack_double_masks(
        _mm_cmpneq_pd(lo, _mm_setzero_pd()), _mm_cmpneq_pd(hi, _mm_setzero_pd()));
    return {simd_select(denormal, sign, bits), _mm_and_si128(denormal, nonzero), overflow};
}

template <SimdArithmetic Operation>
inline SimdArithmeticResult simd_arithmetic(const Gpr128& left, const Gpr128& right,
                                           const Gpr128& accumulator = {}) noexcept {
    const auto a = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(left.words.data())));
    const auto b = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(right.words.data())));
    const auto al = _mm_cvtps_pd(a), ah = _mm_cvtps_pd(_mm_movehl_ps(a, a));
    const auto bl = _mm_cvtps_pd(b), bh = _mm_cvtps_pd(_mm_movehl_ps(b, b));
    SimdNarrowResult result;
    if constexpr (Operation == SimdArithmetic::Add)
        result = simd_chop(_mm_add_pd(al, bl), _mm_add_pd(ah, bh));
    else if constexpr (Operation == SimdArithmetic::Subtract)
        result = simd_chop(_mm_sub_pd(al, bl), _mm_sub_pd(ah, bh));
    else {
        result = simd_chop(_mm_mul_pd(al, bl), _mm_mul_pd(ah, bh));
        if constexpr (Operation != SimdArithmetic::Multiply) {
            const auto acc = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(accumulator.words.data())));
            const auto product = _mm_castsi128_ps(result.bits);
            const auto cl = _mm_cvtps_pd(acc), ch = _mm_cvtps_pd(_mm_movehl_ps(acc, acc));
            const auto pl = _mm_cvtps_pd(product), ph = _mm_cvtps_pd(_mm_movehl_ps(product, product));
            if constexpr (Operation == SimdArithmetic::MultiplyAdd)
                result = simd_chop(_mm_add_pd(cl, pl), _mm_add_pd(ch, ph));
            else result = simd_chop(_mm_sub_pd(cl, pl), _mm_sub_pd(ch, ph));
        }
    }
    const auto lane_mask = [](__m128i mask) {
        const auto m = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(mask)));
        return ((m & 1U) << 3U) | ((m & 2U) << 1U) | ((m & 4U) >> 1U) | ((m & 8U) >> 3U);
    };
    SimdArithmeticResult output;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output.value.words.data()), result.bits);
    output.underflow_mask = lane_mask(result.underflow);
    output.overflow_mask = lane_mask(result.overflow);
    return output;
}

// VF0 is architectural (0,0,0,1). Retail microprograms frequently encode
// register moves as ADDx/MULw using it. Those operations are exact after
// operand clamping, including signed zeros under every host rounding mode.
inline __m128 simd_add_signed_zero(__m128 value, __m128 signed_zero,
                                  bool subtract) noexcept {
    const auto sign_mask = _mm_set1_epi32(static_cast<int>(0x80000000U));
    const auto bits = _mm_castps_si128(value);
    const auto zero_lanes = _mm_cmpeq_epi32(
        _mm_andnot_si128(sign_mask, bits), _mm_setzero_si128());
    auto zero_sign = _mm_castps_si128(signed_zero);
    if (subtract) zero_sign = _mm_xor_si128(zero_sign, sign_mask);
    const auto value_sign = _mm_and_si128(bits, sign_mask);
    // Compiler folding of x - 0 may ignore dynamic round-down signed zeros.
    // Construct the exact-zero sign explicitly instead of relying on it.
    const auto result_sign = (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_DOWN
        ? _mm_or_si128(value_sign, zero_sign) : _mm_and_si128(value_sign, zero_sign);
    return _mm_castsi128_ps(simd_select(zero_lanes, result_sign, bits));
}

template <SimdArithmetic Operation>
inline SimdArithmeticResult simd_arithmetic_vf0_lane(
    const Gpr128& left, const Gpr128& accumulator, unsigned lane) noexcept {
    if (lane == 3U) {
        if constexpr (Operation == SimdArithmetic::MultiplyAdd)
            return simd_arithmetic<SimdArithmetic::Add>(accumulator, left);
        else if constexpr (Operation == SimdArithmetic::MultiplySubtract)
            return simd_arithmetic<SimdArithmetic::Subtract>(accumulator, left);
        else if constexpr (Operation != SimdArithmetic::Multiply) {
            const Gpr128 one{.words = {0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U}};
            return simd_arithmetic<Operation>(left, one, accumulator);
        }
    }
    const auto a = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(left.words.data())));
    __m128 value;
    if constexpr (Operation == SimdArithmetic::Add)
        value = simd_add_signed_zero(a, _mm_setzero_ps(), false);
    else if constexpr (Operation == SimdArithmetic::Subtract)
        value = simd_add_signed_zero(a, _mm_setzero_ps(), true);
    else {
        value = lane == 3U ? a : _mm_and_ps(a, _mm_set1_ps(-0.0F));
        if constexpr (Operation != SimdArithmetic::Multiply) {
            const auto acc = simd_clamp3(_mm_loadu_si128(
                reinterpret_cast<const __m128i*>(accumulator.words.data())));
            value = simd_add_signed_zero(acc, value,
                Operation == SimdArithmetic::MultiplySubtract);
        }
    }
    SimdArithmeticResult result;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(result.value.words.data()), _mm_castps_si128(value));
    return result;
}

} // namespace takedown::guest::vu1_detail
#endif
