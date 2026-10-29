#pragma once

// SSE2 integer expansion for VIF1 UNPACK.
// Process complete groups of four V2-16 / V4-8 vectors. The existing scalar
// decoder must handle the remaining vectors and every overlapping-input call.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

namespace vif1_packed_expand {

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
template <unsigned Type, bool Zero>
inline void expand_groups(std::uint32_t* output, const std::uint8_t* input,
                          const std::size_t groups) noexcept {
    static_assert(Type == 5U || Type == 14U);
    const auto zero = _mm_setzero_si128();
    for (std::size_t group = 0U; group < groups; ++group) {
        // Exactly four source vectors remain: both formats consume16bytes.
        const auto raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
        __m128i a, b, c, d;
        if constexpr (Type == 5U) {
            const auto extension = Zero ? zero : _mm_srai_epi16(raw, 15);
            const auto low = _mm_unpacklo_epi16(raw, extension);
            const auto high = _mm_unpackhi_epi16(raw, extension);
            a = _mm_shuffle_epi32(low, _MM_SHUFFLE(1, 0, 1, 0));
            b = _mm_shuffle_epi32(low, _MM_SHUFFLE(3, 2, 3, 2));
            c = _mm_shuffle_epi32(high, _MM_SHUFFLE(1, 0, 1, 0));
            d = _mm_shuffle_epi32(high, _MM_SHUFFLE(3, 2, 3, 2));
        } else {
            const auto byte_extension = Zero ? zero : _mm_cmpgt_epi8(zero, raw);
            const auto low = _mm_unpacklo_epi8(raw, byte_extension);
            const auto high = _mm_unpackhi_epi8(raw, byte_extension);
            const auto low_extension = Zero ? zero : _mm_srai_epi16(low, 15);
            const auto high_extension = Zero ? zero : _mm_srai_epi16(high, 15);
            a = _mm_unpacklo_epi16(low, low_extension);
            b = _mm_unpackhi_epi16(low, low_extension);
            c = _mm_unpacklo_epi16(high, high_extension);
            d = _mm_unpackhi_epi16(high, high_extension);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4U), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 8U), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 12U), d);
        input += 16U;
        output += 16U;
    }
}
#endif

// Call ONLY after the unchanged format/source-size checks and lanes.resize.
// output contains vector_count*4 valid uint32_t elements. No destination is
// touched unless the whole supplied input span and output range are disjoint.
// This preserves the original sequential scalar behavior for valid aliasing
// calls. No pointer-end addition is used in the overlap check.
[[nodiscard]] inline std::size_t expand_prefix(
    std::uint32_t* output, const unsigned type, const std::uint32_t vector_count,
    const bool zero_extension, const std::span<const std::uint8_t> input) noexcept {
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    if ((type != 5U && type != 14U) || vector_count < 4U) return 0U;
    // The outer API already checked the full input span before resizing. This
    // local guard also makes accidental standalone use refuse short input.
    if (static_cast<std::size_t>(vector_count) > input.size() / 4U) return 0U;
    // The project x64 baseline permits this integer address-range comparison.
    // Decline on platforms where destination-byte arithmetic is not bounded.
    if (static_cast<std::size_t>(vector_count) > std::numeric_limits<std::size_t>::max() / 16U) return 0U;
    const auto output_bytes = static_cast<std::size_t>(vector_count) * 16U;
    const auto destination_address = reinterpret_cast<std::uintptr_t>(output);
    const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
    const bool disjoint = destination_address >= input_address
                              ? destination_address - input_address >= input.size()
                              : input_address - destination_address >= output_bytes;
    if (!disjoint) return 0U;
    const auto groups = static_cast<std::size_t>(vector_count) / 4U;
    if (type == 5U) {
        if (zero_extension) expand_groups<5U, true>(output, input.data(), groups);
        else expand_groups<5U, false>(output, input.data(), groups);
    } else {
        if (zero_extension) expand_groups<14U, true>(output, input.data(), groups);
        else expand_groups<14U, false>(output, input.data(), groups);
    }
    return groups * 4U;
#else
    static_cast<void>(output);
    static_cast<void>(type);
    static_cast<void>(vector_count);
    static_cast<void>(zero_extension);
    static_cast<void>(input);
    return 0U;
#endif
}

}  // namespace vif1_packed_expand
