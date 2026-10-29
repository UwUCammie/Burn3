#pragma once

// VU0 FMAC arithmetic, shared by the microprogram interpreter
// (src/guest/vu0_micro_interpreter.cpp) and the macro-mode COP2 instructions
// (src/guest/vu0_macro_fmac.cpp, src/guest/ee_interpreter.cpp).
//
// The scalar functions are the reference: one lane at a time, clamp-mode-3
// operands, a host-double add/subtract narrowed toward zero, and a truncated
// 24-bit significand multiply. The SIMD kernels compute all four lanes at
// once and are bit-identical to the scalar reference, flags included, under
// every host rounding mode and FTZ/DAZ setting (the reference's double
// add/subtract is rounded by the same MXCSR as the kernels' packed double
// add/subtract). tools/vu0_simd_arithmetic_diff.cpp checks that over
// billions of random and edge-case lanes, and tests/test_vu0_simd_arithmetic.cpp
// repeats a fast subset under ctest.
//
// TAKEDOWN_VU0_SIMD_ARITHMETIC=0 selects the scalar reference at run time.

#include "takedown/guest/ps2_float.hpp"
#include "takedown/guest/types.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace takedown::guest::vu0_detail {

// The process-wide selection. Differential tests flip it to run the same
// microprogram both ways; nothing else writes it.
//
// A namespace-scope inline variable rather than a function-local static: it is
// read once per COP2 operation in compiled EE code, where a function-local
// static would add a thread-safe-initialisation guard test to every read.
#if defined(__SSE2__)
inline bool g_simd_arithmetic_selection = [] {
    const auto* value = std::getenv("TAKEDOWN_VU0_SIMD_ARITHMETIC");
    return value == nullptr || std::string_view{value} != "0";
}();
#else
inline bool g_simd_arithmetic_selection = false;
#endif
[[nodiscard]] inline bool& simd_arithmetic_selection() noexcept { return g_simd_arithmetic_selection; }

[[nodiscard]] inline bool simd_arithmetic_enabled() noexcept {
#if defined(__SSE2__)
    return simd_arithmetic_selection();
#else
    return false;
#endif
}

enum class Vu0Fmac : std::uint8_t { Add, Subtract, Multiply, MultiplyAdd, MultiplySubtract };

// Lane masks use the VU's DEST order: X=8, Y=4, Z=2, W=1.
struct Vu0FmacResult {
    Gpr128 value{};
    std::uint32_t underflow_mask{};
    std::uint32_t overflow_mask{};
};

// ---- Scalar reference ---------------------------------------------------

// Burnout 3's PCSX2 GameIndex entry selects VU clamp mode 3: exponent-zero
// operands become signed zero and exponent-255 operands become signed Fmax,
// preserving the source sign. microVU applies this clamp before every host
// ADD/SUB/MUL used by the FMAC families.
[[nodiscard]] constexpr std::uint32_t clamp3_operand_bits(const std::uint32_t bits) noexcept {
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7f800000U) {
        return (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return bits;
}

// Narrow an exact finite value to the VU's binary32 storage format using the
// default ChopZero mode. A host conversion normally rounds to nearest, so
// step one representable value toward zero only when that conversion moved
// away from the exact value. Directed host rounding modes also remain safe:
// a conversion already toward zero needs no adjustment. ChopZero overflow
// saturates to signed Fmax, and the VU flushes a nonzero denormal result to
// signed zero.
[[nodiscard]] inline std::uint32_t chop_exact_bits(const double exact, bool& underflow,
                                                   bool& overflow) noexcept {
    underflow = false;
    overflow = false;
    constexpr auto kMaximum = static_cast<double>(std::numeric_limits<float>::max());
    if (exact > kMaximum) {
        overflow = true;
        return 0x7f7fffffU;
    }
    if (exact < -kMaximum) {
        overflow = true;
        return 0xff7fffffU;
    }

    const auto bits = ps2_narrow_binary32_toward_zero_bits(exact);
    if ((bits & 0x7f800000U) == 0U) {
        underflow = exact != 0.0;
        return std::signbit(exact) ? 0x80000000U : 0U;
    }
    return bits;
}

[[nodiscard]] inline std::uint32_t add_subtract_bits(const std::uint32_t left_bits,
                                                     const std::uint32_t right_bits,
                                                     const bool subtract, bool& underflow,
                                                     bool& overflow) noexcept {
    const auto left = std::bit_cast<float>(clamp3_operand_bits(left_bits));
    const auto right = std::bit_cast<float>(clamp3_operand_bits(right_bits));
    const auto exact = subtract ? static_cast<double>(left) - static_cast<double>(right)
                                : static_cast<double>(left) + static_cast<double>(right);
    return chop_exact_bits(exact, underflow, overflow);
}

// VU-style truncated standalone multiply. Clamp-mode-3 operands are always
// finite. Normal operands keep a 24-bit significand (including the hidden bit)
// and truncate discarded product bits. A denormal input is already signed
// zero, an exact denormal product is flushed to signed zero, and an exponent
// past 254 saturates to signed Fmax. (A product between Fmax and 2^128
// truncates to Fmax without raising overflow.)
[[nodiscard]] inline std::uint32_t multiply_bits(const std::uint32_t left_bits,
                                                 const std::uint32_t right_bits,
                                                 bool& underflow, bool& overflow) noexcept {
    underflow = false;
    overflow = false;
    const auto left = clamp3_operand_bits(left_bits);
    const auto right = clamp3_operand_bits(right_bits);
    const auto left_exponent = static_cast<std::int32_t>((left >> 23U) & 0xffU);
    const auto right_exponent = static_cast<std::int32_t>((right >> 23U) & 0xffU);
    const auto result_sign = (left ^ right) & 0x80000000U;
    if (left_exponent == 0 || right_exponent == 0) {
        return result_sign;
    }
    const auto left_significand = std::uint64_t{0x00800000U} | (left & 0x007fffffU);
    const auto right_significand = std::uint64_t{0x00800000U} | (right & 0x007fffffU);
    const auto significand_product = left_significand * right_significand;
    const bool needs_normalization = (significand_product & (std::uint64_t{1} << 47U)) != 0U;
    const auto result_significand =
        static_cast<std::uint32_t>(significand_product >> (needs_normalization ? 24U : 23U));
    const auto result_exponent =
        left_exponent + right_exponent - 127 + (needs_normalization ? 1 : 0);
    if (result_exponent >= 0xff) {
        overflow = true;
        return result_sign | 0x7f7fffffU;
    }
    if (result_exponent <= 0) {
        // The exact product lies in the denormal range; flush to signed zero
        // and remember the underflow for the MAC flag computation.
        underflow = true;
        return result_sign;
    }
    return result_sign | (static_cast<std::uint32_t>(result_exponent) << 23U) |
           (result_significand & 0x007fffffU);
}

// One lane of an FMAC operation. MADD/MSUB are staged like PCSX2 microVU: a
// ChopZero/flushed/clamped binary32 product first, then the add or subtract.
// Only the final stage contributes flags.
template <Vu0Fmac Operation>
[[nodiscard]] inline std::uint32_t fmac_lane(const std::uint32_t left, const std::uint32_t right,
                                             const std::uint32_t accumulator, bool& underflow,
                                             bool& overflow) noexcept {
    if constexpr (Operation == Vu0Fmac::Add) {
        return add_subtract_bits(left, right, false, underflow, overflow);
    } else if constexpr (Operation == Vu0Fmac::Subtract) {
        return add_subtract_bits(left, right, true, underflow, overflow);
    } else if constexpr (Operation == Vu0Fmac::Multiply) {
        return multiply_bits(left, right, underflow, overflow);
    } else {
        bool product_underflow = false;
        bool product_overflow = false;
        const auto product = multiply_bits(left, right, product_underflow, product_overflow);
        return add_subtract_bits(accumulator, product, Operation == Vu0Fmac::MultiplySubtract,
                                 underflow, overflow);
    }
}

// The scalar reference over the lanes in `mask`. Lanes outside it keep `base`
// and report no flags.
template <Vu0Fmac Operation>
[[nodiscard]] inline Vu0FmacResult scalar_fmac(const Gpr128& left, const Gpr128& right,
                                               const Gpr128& accumulator, const Gpr128& base,
                                               const std::uint32_t mask) noexcept {
    Vu0FmacResult result{.value = base};
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
        if ((mask & lane_bit) == 0U) {
            continue;
        }
        bool underflow = false;
        bool overflow = false;
        result.value.words[lane] = fmac_lane<Operation>(left.words[lane], right.words[lane],
                                                        accumulator.words[lane], underflow,
                                                        overflow);
        result.underflow_mask |= underflow ? lane_bit : 0U;
        result.overflow_mask |= overflow ? lane_bit : 0U;
    }
    return result;
}

// VU MAX/MINI are total signed-integer comparisons: NaN bit patterns and
// signed zero order by their 32-bit signed encoding, with the sign bit
// inverting the comparison when both operands are negative.
[[nodiscard]] constexpr std::uint32_t minimum_bits(const std::uint32_t left,
                                                   const std::uint32_t right) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0
                                          ? std::max(signed_left, signed_right)
                                          : std::min(signed_left, signed_right));
}

[[nodiscard]] constexpr std::uint32_t maximum_bits(const std::uint32_t left,
                                                   const std::uint32_t right) noexcept {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0
                                          ? std::min(signed_left, signed_right)
                                          : std::max(signed_left, signed_right));
}

template <bool Maximum>
[[nodiscard]] inline Gpr128 scalar_min_max(const Gpr128& left, const Gpr128& right,
                                           const Gpr128& base, const std::uint32_t mask) noexcept {
    Gpr128 result = base;
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if ((mask & (0x08U >> lane)) != 0U) {
            result.words[lane] = Maximum ? maximum_bits(left.words[lane], right.words[lane])
                                         : minimum_bits(left.words[lane], right.words[lane]);
        }
    }
    return result;
}

// MAC bits from the result values alone: 7..4 sign XYZW and 3..0 zero XYZW,
// plus (FullValueFlags) 11..8 for an exponent-0 nonzero value and 15..12 for
// an exponent-255 value. FMAC results never hold either encoding (they are
// flushed or saturated); the arithmetic's own judgments are the masks.
template <bool FullValueFlags>
[[nodiscard]] inline std::uint16_t scalar_mac_value_flags(const Gpr128& value,
                                                          const std::uint32_t mask) noexcept {
    std::uint32_t flags = 0;
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        const auto lane_bit = static_cast<std::uint32_t>(0x08U >> lane);
        if ((mask & lane_bit) == 0U) {
            continue;
        }
        const auto bits = value.words[lane];
        const auto exponent = bits & 0x7f800000U;
        flags |= (bits & 0x80000000U) != 0U ? lane_bit << 4U : 0U;
        flags |= (bits & 0x7fffffffU) == 0U ? lane_bit : 0U;
        if constexpr (FullValueFlags) {
            flags |= exponent == 0U && (bits & 0x007fffffU) != 0U ? lane_bit << 8U : 0U;
            flags |= exponent == 0x7f800000U ? lane_bit << 12U : 0U;
        }
    }
    return static_cast<std::uint16_t>(flags);
}

// The MAC layout puts lane bit L of each group at L << (4 * group).
[[nodiscard]] constexpr std::uint16_t mac_judgment_flags(const std::uint32_t underflow_mask,
                                                         const std::uint32_t overflow_mask) noexcept {
    return static_cast<std::uint16_t>(((underflow_mask & 0x0fU) << 8U) |
                                      ((overflow_mask & 0x0fU) << 12U));
}

#if defined(__SSE2__)

// ---- SSE2 kernels -------------------------------------------------------

[[gnu::always_inline]] inline __m128i simd_load(const Gpr128& value) noexcept {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(value.words.data()));
}

[[gnu::always_inline]] inline void simd_store(Gpr128& value, const __m128i bits) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(value.words.data()), bits);
}

[[gnu::always_inline]] inline __m128i simd_select(const __m128i mask, const __m128i yes,
                                                  const __m128i no) noexcept {
    return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
}

// All-ones in each lane whose DEST bit (X=8 .. W=1) is set.
[[gnu::always_inline]] inline __m128i simd_lane_mask(const std::uint32_t mask) noexcept {
    const auto bits = _mm_setr_epi32(8, 4, 2, 1);
    return _mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32(static_cast<int>(mask)), bits), bits);
}

// Lane-wise all-ones/zero vector to a DEST-ordered 4-bit mask.
[[gnu::always_inline]] inline std::uint32_t simd_dest_bits(const __m128i lanes) noexcept {
    return static_cast<std::uint32_t>(_mm_movemask_ps(
        _mm_castsi128_ps(_mm_shuffle_epi32(lanes, _MM_SHUFFLE(0, 1, 2, 3)))));
}

// clamp3_operand_bits, four lanes, with integer compares only (a signaling
// NaN pattern never reaches a host floating-point instruction).
[[gnu::always_inline]] inline __m128 simd_clamp3(__m128i bits) noexcept {
    const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    const auto sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    bits = simd_select(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()), sign, bits);
    return _mm_castsi128_ps(simd_select(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000)),
                                        _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)), bits));
}

[[gnu::always_inline]] inline __m128i simd_pack_double_masks(const __m128d lo,
                                                             const __m128d hi) noexcept {
    return _mm_unpacklo_epi64(
        _mm_shuffle_epi32(_mm_castpd_si128(lo), _MM_SHUFFLE(2, 0, 2, 0)),
        _mm_shuffle_epi32(_mm_castpd_si128(hi), _MM_SHUFFLE(2, 0, 2, 0)));
}

struct SimdLanes {
    __m128i bits;
    __m128i underflow;
    __m128i overflow;
};

// chop_exact_bits on four doubles (lanes X,Y in `lo`, Z,W in `hi`). Clearing
// a double's low 29 significand bits truncates it toward zero to binary32
// precision, so for every result in the normal binary32 range the
// conversion that follows is exact and gives the reference's chopped value
// under any MXCSR. Results below 2^-126 flush to signed zero (underflow when
// nonzero) and results past the overflow threshold saturate to signed Fmax:
// a product (Multiply) overflows at 2^128, where the reference's exponent
// reaches 255, and a sum past Fmax. The flags come straight from the doubles,
// off the value's dependency chain.
template <bool Multiply>
[[gnu::always_inline]] inline SimdLanes simd_chop(const __m128d lo, const __m128d hi) noexcept {
    const auto keep = _mm_castsi128_pd(
        _mm_set1_epi64x(static_cast<long long>(0xffffffffe0000000ULL)));
    const auto bits = _mm_castps_si128(_mm_movelh_ps(_mm_cvtpd_ps(_mm_and_pd(lo, keep)),
                                                     _mm_cvtpd_ps(_mm_and_pd(hi, keep))));
    // The doubles' high words, in lane order, carry each result's sign.
    const auto high = _mm_castps_si128(
        _mm_shuffle_ps(_mm_castpd_ps(lo), _mm_castpd_ps(hi), _MM_SHUFFLE(3, 1, 3, 1)));
    const auto sign = _mm_and_si128(high, _mm_set1_epi32(static_cast<int>(0x80000000U)));
    const auto sign64 = _mm_set1_pd(-0.0);
    const auto abs_lo = _mm_andnot_pd(sign64, lo);
    const auto abs_hi = _mm_andnot_pd(sign64, hi);
    __m128i overflow;
    if constexpr (Multiply) {
        const auto limit = _mm_set1_pd(0x1p128);
        overflow = simd_pack_double_masks(_mm_cmpge_pd(abs_lo, limit),
                                          _mm_cmpge_pd(abs_hi, limit));
    } else {
        const auto limit = _mm_set1_pd(0x1.fffffep127);
        overflow = simd_pack_double_masks(_mm_cmpgt_pd(abs_lo, limit),
                                          _mm_cmpgt_pd(abs_hi, limit));
    }
    const auto smallest = _mm_set1_pd(0x1p-126);
    const auto tiny = simd_pack_double_masks(_mm_cmplt_pd(abs_lo, smallest),
                                             _mm_cmplt_pd(abs_hi, smallest));
    const auto nonzero = simd_pack_double_masks(_mm_cmpneq_pd(lo, _mm_setzero_pd()),
                                                _mm_cmpneq_pd(hi, _mm_setzero_pd()));
    const auto special = _mm_or_si128(overflow, tiny);
    const auto special_value =
        _mm_or_si128(sign, _mm_and_si128(overflow, _mm_set1_epi32(0x7f7fffff)));
    return {simd_select(special, special_value, bits), _mm_and_si128(tiny, nonzero), overflow};
}

// One FMAC operation on all four lanes. Every binary32 product of clamped
// operands is exact in a double, so the truncated reference multiply is the
// chop of the packed double product; add/subtract perform the reference's
// own double operation.
template <Vu0Fmac Operation>
[[gnu::always_inline]] inline SimdLanes simd_fmac(const __m128i left, const __m128i right,
                                                  const __m128i accumulator) noexcept {
    const auto a = simd_clamp3(left);
    const auto b = simd_clamp3(right);
    const auto al = _mm_cvtps_pd(a);
    const auto ah = _mm_cvtps_pd(_mm_movehl_ps(a, a));
    const auto bl = _mm_cvtps_pd(b);
    const auto bh = _mm_cvtps_pd(_mm_movehl_ps(b, b));
    if constexpr (Operation == Vu0Fmac::Add) {
        return simd_chop<false>(_mm_add_pd(al, bl), _mm_add_pd(ah, bh));
    } else if constexpr (Operation == Vu0Fmac::Subtract) {
        return simd_chop<false>(_mm_sub_pd(al, bl), _mm_sub_pd(ah, bh));
    } else {
        const auto product = simd_chop<true>(_mm_mul_pd(al, bl), _mm_mul_pd(ah, bh));
        if constexpr (Operation == Vu0Fmac::Multiply) {
            return product;
        } else {
            // A product is never denormal or exponent-255, so it needs no
            // second clamp before the add/subtract stage.
            const auto p = _mm_castsi128_ps(product.bits);
            const auto c = simd_clamp3(accumulator);
            const auto cl = _mm_cvtps_pd(c);
            const auto ch = _mm_cvtps_pd(_mm_movehl_ps(c, c));
            const auto pl = _mm_cvtps_pd(p);
            const auto ph = _mm_cvtps_pd(_mm_movehl_ps(p, p));
            if constexpr (Operation == Vu0Fmac::MultiplyAdd) {
                return simd_chop<false>(_mm_add_pd(cl, pl), _mm_add_pd(ch, ph));
            } else {
                return simd_chop<false>(_mm_sub_pd(cl, pl), _mm_sub_pd(ch, ph));
            }
        }
    }
}

// scalar_fmac's contract on four lanes held in registers. Callers that form
// a broadcast or permuted operand should build it with a shuffle: a vector
// assembled from four 32-bit stores and read back whole stalls on store
// forwarding for longer than the arithmetic takes.
struct SimdFmacResult {
    __m128i value;
    std::uint32_t underflow_mask;
    std::uint32_t overflow_mask;
};

template <Vu0Fmac Operation>
[[gnu::always_inline]] inline SimdFmacResult simd_fmac_masked(
    const __m128i left, const __m128i right, const __m128i accumulator, const __m128i base,
    const std::uint32_t mask) noexcept {
    const auto lanes = simd_fmac<Operation>(left, right, accumulator);
    return {simd_select(simd_lane_mask(mask), lanes.bits, base),
            simd_dest_bits(lanes.underflow) & mask & 0x0fU,
            simd_dest_bits(lanes.overflow) & mask & 0x0fU};
}

template <Vu0Fmac Operation>
[[gnu::always_inline]] inline Vu0FmacResult simd_fmac(const Gpr128& left, const Gpr128& right,
                                                      const Gpr128& accumulator,
                                                      const Gpr128& base,
                                                      const std::uint32_t mask) noexcept {
    const auto lanes = simd_fmac_masked<Operation>(simd_load(left), simd_load(right),
                                                   simd_load(accumulator), simd_load(base),
                                                   mask);
    Vu0FmacResult result;
    simd_store(result.value, lanes.value);
    result.underflow_mask = lanes.underflow_mask;
    result.overflow_mask = lanes.overflow_mask;
    return result;
}

template <bool Maximum>
[[gnu::always_inline]] inline __m128i simd_min_max(const __m128i a, const __m128i b) noexcept {
    const auto greater = _mm_cmpgt_epi32(a, b);
    const auto larger = simd_select(greater, a, b);
    const auto smaller = simd_select(greater, b, a);
    const auto both_negative = _mm_srai_epi32(_mm_and_si128(a, b), 31);
    return Maximum ? simd_select(both_negative, smaller, larger)
                   : simd_select(both_negative, larger, smaller);
}

template <bool Maximum>
[[gnu::always_inline]] inline Gpr128 simd_min_max(const Gpr128& left, const Gpr128& right,
                                                  const Gpr128& base,
                                                  const std::uint32_t mask) noexcept {
    Gpr128 result;
    simd_store(result, simd_select(simd_lane_mask(mask),
                                   simd_min_max<Maximum>(simd_load(left), simd_load(right)),
                                   simd_load(base)));
    return result;
}

template <bool FullValueFlags>
[[gnu::always_inline]] inline std::uint16_t simd_mac_value_flags(const __m128i bits,
                                                                 const std::uint32_t mask) noexcept {
    const auto zero = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)),
                                      _mm_setzero_si128());
    auto flags = simd_dest_bits(zero) | (simd_dest_bits(bits) << 4U);
    if constexpr (FullValueFlags) {
        const auto exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
        const auto denormal = _mm_andnot_si128(
            zero, _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
        const auto infinite = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
        flags |= (simd_dest_bits(denormal) << 8U) | (simd_dest_bits(infinite) << 12U);
    }
    return static_cast<std::uint16_t>(flags & ((mask & 0x0fU) * 0x1111U));
}

template <bool FullValueFlags>
[[gnu::always_inline]] inline std::uint16_t simd_mac_value_flags(const Gpr128& value,
                                                                 const std::uint32_t mask) noexcept {
    return simd_mac_value_flags<FullValueFlags>(simd_load(value), mask);
}

// One lane of `value` in all four lanes. The lane is a run-time value in the
// interpreter and a constant in compiled pairs.
[[gnu::always_inline]] inline __m128i simd_broadcast_lane(const __m128i value,
                                                          const unsigned lane) noexcept {
    switch (lane & 3U) {
    case 0U: return _mm_shuffle_epi32(value, _MM_SHUFFLE(0, 0, 0, 0));
    case 1U: return _mm_shuffle_epi32(value, _MM_SHUFFLE(1, 1, 1, 1));
    case 2U: return _mm_shuffle_epi32(value, _MM_SHUFFLE(2, 2, 2, 2));
    default: return _mm_shuffle_epi32(value, _MM_SHUFFLE(3, 3, 3, 3));
    }
}

#endif

// One scalar in all four lanes, for the scalar reference's operands.
[[nodiscard]] inline Gpr128 broadcast(const std::uint32_t bits) noexcept {
    return Gpr128{.words = {bits, bits, bits, bits}};
}

} // namespace takedown::guest::vu0_detail
