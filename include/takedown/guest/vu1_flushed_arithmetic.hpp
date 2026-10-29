#pragma once

// Experimental scoped arithmetic, separate from the qualified implementation.
#include "takedown/guest/vu1_simd_arithmetic.hpp"

#if defined(__SSE2__)
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace takedown::guest::vu1_detail {

// Requires the caller's DAZ/FTZ scope. Hardware consumes tiny operands as
// signed zero and flushes tiny arithmetic results; only exceptional large
// encodings need software saturation. Use integer comparisons so signaling
// NaNs do not introduce host floating-point exceptions during clamping.
inline __m128 simd_clamp_flushed(__m128i bits) noexcept {
    const auto absolute = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    const auto maximum = _mm_set1_epi32(0x7f7fffff);
#if defined(__SSE4_1__)
    const auto clamped = _mm_min_epu32(absolute, maximum);
#else
    const auto clamped = simd_select(_mm_cmpgt_epi32(absolute, maximum), maximum, absolute);
#endif
    return _mm_castsi128_ps(_mm_or_si128(sign, clamped));
}

template <SimdArithmetic Operation, bool LeftNormalized = false,
          bool RightNormalized = false, bool AccumulatorNormalized = false>
#if defined(__GNUC__) || defined(__clang__)
// Keep values and exception masks visible to native-block liveness analysis.
// An out-of-line aggregate return forces calculation of unused flag masks.
[[gnu::always_inline]]
#endif
inline SimdArithmeticResult native_float_flushed_arithmetic(
    const Gpr128& left, const Gpr128& right, const Gpr128& accumulator = {}) noexcept {
    const auto clamp = [](__m128i bits) {
        return simd_clamp_flushed(bits);
    };
    const auto load = [&]<bool Normalized>(const Gpr128& value) {
        const auto bits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.words.data()));
        if constexpr (Normalized) return _mm_castsi128_ps(bits);
        else return clamp(bits);
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
            const auto product = clamp(_mm_castps_si128(result));
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
                    _mm_castps_si128(clamp(bits)));
    output.underflow_mask = lane_mask(underflow);
    output.overflow_mask = lane_mask(overflow);
    return output;
}

} // namespace takedown::guest::vu1_detail
#endif
