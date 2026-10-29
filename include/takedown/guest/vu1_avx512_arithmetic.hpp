#pragma once

#include "takedown/guest/vu1_simd_arithmetic.hpp"

namespace takedown::guest::vu1_detail {
inline bool avx512_arithmetic_supported() noexcept {
#if defined(__GNUC__) && defined(__x86_64__)
    return __builtin_cpu_supports("avx512f");
#else
    return false;
#endif
}
inline bool avx512_arithmetic_enabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU1_AVX512_ARITHMETIC");
#if defined(_WIN32)
        if (value == nullptr) return avx512_arithmetic_supported();
#endif
        return value != nullptr && std::string_view{value} == "1" &&
            avx512_arithmetic_supported();
    }();
    return enabled;
}
}

#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
namespace takedown::guest::vu1_detail {

// Embedded rounding changes only this arithmetic operation, never MXCSR.
// Fall back for values where rounding an intermediate double can differ from
// direct binary32 chopping, or where saturation hides an overflow judgment.
template <SimdArithmetic Operation>
[[gnu::target("avx512f"), gnu::always_inline]]
inline bool avx512_binary(__m128 a, __m128 b, SimdNarrowResult& output) noexcept {
    const auto zero = _mm_setzero_si128();
    const auto exponent_mask = _mm_set1_epi32(0x7f800000);
    const auto ea = _mm_and_si128(_mm_castps_si128(a), exponent_mask);
    const auto eb = _mm_and_si128(_mm_castps_si128(b), exponent_mask);
    if constexpr (Operation != SimdArithmetic::Multiply) {
        const auto gap = _mm_sub_epi32(ea, eb);
        const auto wide_gap = _mm_or_si128(
            _mm_cmpgt_epi32(gap, _mm_set1_epi32(29 * 0x800000)),
            _mm_cmplt_epi32(gap, _mm_set1_epi32(-29 * 0x800000)));
        const auto nonzero_gap = _mm_andnot_si128(
            _mm_or_si128(_mm_cmpeq_epi32(ea, zero), _mm_cmpeq_epi32(eb, zero)), wide_gap);
        if (_mm_movemask_ps(_mm_castsi128_ps(nonzero_gap))) return false;
    }
    constexpr auto rounding = _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC;
    __m512 result;
    if constexpr (Operation == SimdArithmetic::Multiply)
        result = _mm512_maskz_mul_round_ps(0x0f, _mm512_castps128_ps512(a),
                                          _mm512_castps128_ps512(b), rounding);
    else if constexpr (Operation == SimdArithmetic::Add)
        result = _mm512_maskz_add_round_ps(0x0f, _mm512_castps128_ps512(a),
                                          _mm512_castps128_ps512(b), rounding);
    else
        result = _mm512_maskz_sub_round_ps(0x0f, _mm512_castps128_ps512(a),
                                          _mm512_castps128_ps512(b), rounding);
    const auto bits = _mm_castps_si128(_mm512_castps512_ps128(result));
    const auto absolute = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    const auto saturated = _mm_cmpgt_epi32(absolute, _mm_set1_epi32(0x7f7ffffe));
    if (_mm_movemask_ps(_mm_castsi128_ps(saturated))) return false;
    const auto denormal = _mm_cmpeq_epi32(_mm_and_si128(bits, exponent_mask), zero);
    __m128i nonzero;
    if constexpr (Operation == SimdArithmetic::Multiply)
        nonzero = _mm_andnot_si128(_mm_or_si128(_mm_cmpeq_epi32(ea, zero),
                                               _mm_cmpeq_epi32(eb, zero)),
                                  _mm_set1_epi32(-1));
    else if constexpr (Operation == SimdArithmetic::Add)
        nonzero = _mm_castps_si128(_mm_cmpneq_ps(a, _mm_xor_ps(b, _mm_set1_ps(-0.0F))));
    else
        nonzero = _mm_castps_si128(_mm_cmpneq_ps(a, b));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    output = {simd_select(denormal, sign, bits), _mm_and_si128(denormal, nonzero), zero};
    return true;
}

template <SimdArithmetic Operation>
[[gnu::target("avx512f")]]
inline SimdArithmeticResult avx512_arithmetic(const Gpr128& left, const Gpr128& right,
                                             const Gpr128& accumulator = {}) noexcept {
    // Preserve the reference's dynamically rounded double cancellation sign:
    // an exact zero is +0 toward zero (a VU1 launch) and to nearest alike.
    // FTZ/DAZ are also left entirely to the established reference path.
    if (const auto environment = _mm_getcsr() & (_MM_ROUND_MASK | 0x8040U);
        environment != _MM_ROUND_TOWARD_ZERO && environment != _MM_ROUND_NEAREST)
        return simd_arithmetic<Operation>(left, right, accumulator);
    const auto a = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(left.words.data())));
    const auto b = simd_clamp3(_mm_loadu_si128(reinterpret_cast<const __m128i*>(right.words.data())));
    SimdNarrowResult result;
    bool okay;
    if constexpr (Operation == SimdArithmetic::Add || Operation == SimdArithmetic::Subtract)
        okay = avx512_binary<Operation>(a, b, result);
    else {
        okay = avx512_binary<SimdArithmetic::Multiply>(a, b, result);
        if constexpr (Operation != SimdArithmetic::Multiply) {
            if (okay) {
                const auto acc = simd_clamp3(_mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(accumulator.words.data())));
                if constexpr (Operation == SimdArithmetic::MultiplyAdd)
                    okay = avx512_binary<SimdArithmetic::Add>(acc, _mm_castsi128_ps(result.bits), result);
                else
                    okay = avx512_binary<SimdArithmetic::Subtract>(acc, _mm_castsi128_ps(result.bits), result);
            }
        }
    }
    if (!okay) return simd_arithmetic<Operation>(left, right, accumulator);
    SimdArithmeticResult output;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output.value.words.data()), result.bits);
    output.underflow_mask = static_cast<std::uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(
        _mm_shuffle_epi32(result.underflow, _MM_SHUFFLE(0, 1, 2, 3)))));
    return output;
}
}
#endif
