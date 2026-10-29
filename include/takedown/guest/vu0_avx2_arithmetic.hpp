#pragma once

// AVX2 forms of the VU0 FMAC kernels of vu0_simd_arithmetic.hpp, for the native
// microprograms (src/guest/vu0_native_support.inc). They perform the same
// operations on the same values - the clamp-mode-3 operands, the host-double
// add/subtract/multiply, the chop to binary32 and the underflow/overflow
// judgments taken from the doubles - and return bit-identical results under
// every host rounding mode and FTZ/DAZ setting; only the instruction count
// differs: four doubles per 256-bit operation instead of two pairs, and
// variable blends instead of and/andnot/or. tests/test_vu0_avx2_arithmetic.cpp
// compares them with the SSE2 kernels over random and edge operands.
//
// TAKEDOWN_VU0_AVX2=0 keeps the native microprograms on the SSE2 kernels.

#include "takedown/guest/vu0_simd_arithmetic.hpp"

#if defined(__GNUC__) && defined(__x86_64__) && defined(__SSE2__)
#define TAKEDOWN_VU0_HAVE_AVX2 1

#include <immintrin.h>

namespace takedown::guest::vu0_detail {

[[nodiscard]] inline bool avx2_supported() noexcept {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

// Whether the native microprograms use these kernels: the CPU has AVX2 and
// TAKEDOWN_VU0_AVX2 is not "0".
[[nodiscard]] inline bool avx2_arithmetic_enabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("TAKEDOWN_VU0_AVX2");
        return (value == nullptr || std::string_view{value} != "0") && avx2_supported();
    }();
    return enabled;
}

// clamp3_operand_bits, four lanes (simd_clamp3 with variable blends).
[[gnu::target("avx2"), gnu::always_inline]] inline __m128 simd_clamp3_avx2(const __m128i bits) noexcept {
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    auto result = _mm_blendv_epi8(bits, sign, _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
    result = _mm_blendv_epi8(result, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)),
                             _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000)));
    return _mm_castsi128_ps(result);
}

// Four 64-bit compare masks to four 32-bit lane masks.
[[gnu::target("avx2"), gnu::always_inline]] inline __m128i simd_pack_masks_avx2(const __m256d mask) noexcept {
    return _mm256_castsi256_si128(
        _mm256_permutevar8x32_epi32(_mm256_castpd_si256(mask), _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6)));
}

// simd_chop on four doubles. The chopped binary32 keeps the sign of the double
// (the narrowing never changes it), so the sign is read from the result.
template <bool Multiply>
[[gnu::target("avx2"), gnu::always_inline]] inline SimdLanes simd_chop_avx2(const __m256d value) noexcept {
    const auto keep = _mm256_castsi256_pd(_mm256_set1_epi64x(static_cast<long long>(0xffffffffe0000000ULL)));
    const auto bits = _mm_castps_si128(_mm256_cvtpd_ps(_mm256_and_pd(value, keep)));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    const auto magnitude = _mm256_andnot_pd(_mm256_set1_pd(-0.0), value);
    __m256d overflow64;
    if constexpr (Multiply) {
        overflow64 = _mm256_cmp_pd(magnitude, _mm256_set1_pd(0x1p128), _CMP_GE_OQ);
    } else {
        overflow64 = _mm256_cmp_pd(magnitude, _mm256_set1_pd(0x1.fffffep127), _CMP_GT_OQ);
    }
    const auto tiny64 = _mm256_cmp_pd(magnitude, _mm256_set1_pd(0x1p-126), _CMP_LT_OQ);
    const auto nonzero64 = _mm256_cmp_pd(value, _mm256_setzero_pd(), _CMP_NEQ_UQ);
    const auto overflow = simd_pack_masks_avx2(overflow64);
    const auto special = simd_pack_masks_avx2(_mm256_or_pd(overflow64, tiny64));
    const auto underflow = simd_pack_masks_avx2(_mm256_and_pd(tiny64, nonzero64));
    const auto special_value =
        _mm_or_si128(sign, _mm_and_si128(overflow, _mm_set1_epi32(0x7f7fffff)));
    return {_mm_blendv_epi8(bits, special_value, special), underflow, overflow};
}

// simd_fmac with the same contract.
template <Vu0Fmac Operation>
[[gnu::target("avx2"), gnu::always_inline]] inline SimdLanes simd_fmac_avx2(
    const __m128i left, const __m128i right, const __m128i accumulator) noexcept {
    const auto a = _mm256_cvtps_pd(simd_clamp3_avx2(left));
    const auto b = _mm256_cvtps_pd(simd_clamp3_avx2(right));
    if constexpr (Operation == Vu0Fmac::Add) {
        return simd_chop_avx2<false>(_mm256_add_pd(a, b));
    } else if constexpr (Operation == Vu0Fmac::Subtract) {
        return simd_chop_avx2<false>(_mm256_sub_pd(a, b));
    } else {
        const auto product = simd_chop_avx2<true>(_mm256_mul_pd(a, b));
        if constexpr (Operation == Vu0Fmac::Multiply) {
            return product;
        } else {
            // A product is never denormal or exponent-255, so it needs no
            // second clamp before the add/subtract stage.
            const auto p = _mm256_cvtps_pd(_mm_castsi128_ps(product.bits));
            const auto c = _mm256_cvtps_pd(simd_clamp3_avx2(accumulator));
            if constexpr (Operation == Vu0Fmac::MultiplyAdd) {
                return simd_chop_avx2<false>(_mm256_add_pd(c, p));
            } else {
                return simd_chop_avx2<false>(_mm256_sub_pd(c, p));
            }
        }
    }
}

} // namespace takedown::guest::vu0_detail
#endif
