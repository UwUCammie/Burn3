// The pieces the generated VU0 native microprograms are built from
// (tools/vu0_aot_generator.py), included once for the SSE2 kernels and once,
// under a target("avx2") region, for the AVX2 kernels (TAKEDOWN_VU0_NATIVE_AVX2).
// Textually included inside src/guest/vu0_micro_interpreter.cpp's namespaces.

// STATUS's sticky Z/S/U/O as lane vectors: every MAC update ORs in its
// masked zero lanes, sign bits and underflow/overflow judgments.
struct Vu0NativeSticky {
    __m128i zero;
    __m128i sign;
    __m128i underflow;
    __m128i overflow;
};

struct Vu0NativeFmac {
    __m128i value;
    __m128i underflow;
    __m128i overflow;
};

template <std::uint32_t Mask>
[[gnu::always_inline]] inline __m128i vu0_native_select(const __m128i yes,
                                                         const __m128i no) noexcept {
    if constexpr (Mask == 0x0fU) {
        return yes;
    } else if constexpr (Mask == 0U) {
        return no;
    } else {
#if defined(TAKEDOWN_VU0_NATIVE_AVX2)
        // DEST bits X=8 .. W=1 are blend immediate bits for lanes 0 .. 3.
        constexpr int kLanes = ((Mask & 8U) != 0U ? 1 : 0) | ((Mask & 4U) != 0U ? 2 : 0) |
                               ((Mask & 2U) != 0U ? 4 : 0) | ((Mask & 1U) != 0U ? 8 : 0);
        return _mm_blend_epi32(no, yes, kLanes);
#else
        return vu0_detail::simd_select(vu0_detail::simd_lane_mask(Mask), yes, no);
#endif
    }
}

// simd_fmac_masked with the masked-off lanes taken from `base` (the
// destination register or ACC) and the judgments kept as lane masks.
template <vu0_detail::Vu0Fmac Operation, std::uint32_t Mask>
[[gnu::always_inline]] inline Vu0NativeFmac vu0_native_fmac(const __m128i left,
                                                            const __m128i right,
                                                            const __m128i accumulator,
                                                            const __m128i base) noexcept {
#if defined(TAKEDOWN_VU0_NATIVE_AVX2)
    const auto lanes = vu0_detail::simd_fmac_avx2<Operation>(left, right, accumulator);
#else
    const auto lanes = vu0_detail::simd_fmac<Operation>(left, right, accumulator);
#endif
    if constexpr (Mask == 0x0fU) {
        return {lanes.bits, lanes.underflow, lanes.overflow};
    } else {
        const auto lane_mask = vu0_detail::simd_lane_mask(Mask);
        return {vu0_native_select<Mask>(lanes.bits, base), _mm_and_si128(lanes.underflow, lane_mask),
                _mm_and_si128(lanes.overflow, lane_mask)};
    }
}

// A MADD/MSUB whose accumulator lanes are all clamp3 fixed points (a finite
// normal or a signed zero). Every native FMAC result is a chop output, so that
// is exactly what an accumulator written by an earlier FMAC holds. clamp3 is
// then the identity on all four physically evaluated lanes, so the accumulator
// clamp can be dropped. The Multiply kernel still clamps both operands and
// produces the identical chopped product, and the same final chop follows, so
// the value, the underflow/overflow judgments and the DEST selection match
// vu0_native_fmac bit for bit.
template <vu0_detail::Vu0Fmac Operation, std::uint32_t Mask>
[[gnu::always_inline]] inline Vu0NativeFmac vu0_native_fmac_canonical_acc(
    const __m128i left, const __m128i right, const __m128i accumulator,
    const __m128i base) noexcept {
    static_assert(Operation == vu0_detail::Vu0Fmac::MultiplyAdd ||
                  Operation == vu0_detail::Vu0Fmac::MultiplySubtract);
#if defined(TAKEDOWN_VU0_NATIVE_AVX2)
    const auto product =
        vu0_detail::simd_fmac_avx2<vu0_detail::Vu0Fmac::Multiply>(left, right, accumulator);
    const auto p = _mm256_cvtps_pd(_mm_castsi128_ps(product.bits));
    const auto c = _mm256_cvtps_pd(_mm_castsi128_ps(accumulator));
    const auto sum = Operation == vu0_detail::Vu0Fmac::MultiplyAdd
                         ? vu0_detail::simd_chop_avx2<false>(_mm256_add_pd(c, p))
                         : vu0_detail::simd_chop_avx2<false>(_mm256_sub_pd(c, p));
#else
    const auto product =
        vu0_detail::simd_fmac<vu0_detail::Vu0Fmac::Multiply>(left, right, accumulator);
    const auto c = _mm_castsi128_ps(accumulator);
    const auto p = _mm_castsi128_ps(product.bits);
    const auto cl = _mm_cvtps_pd(c);
    const auto ch = _mm_cvtps_pd(_mm_movehl_ps(c, c));
    const auto pl = _mm_cvtps_pd(p);
    const auto ph = _mm_cvtps_pd(_mm_movehl_ps(p, p));
    const auto sum = Operation == vu0_detail::Vu0Fmac::MultiplyAdd
                         ? vu0_detail::simd_chop<false>(_mm_add_pd(cl, pl), _mm_add_pd(ch, ph))
                         : vu0_detail::simd_chop<false>(_mm_sub_pd(cl, pl), _mm_sub_pd(ch, ph));
#endif
    if constexpr (Mask == 0x0fU) {
        return {sum.bits, sum.underflow, sum.overflow};
    } else {
        const auto lane_mask = vu0_detail::simd_lane_mask(Mask);
        return {vu0_native_select<Mask>(sum.bits, base), _mm_and_si128(sum.underflow, lane_mask),
                _mm_and_si128(sum.overflow, lane_mask)};
    }
}

// The 16-bit MAC value run_upper_form reports. Active lanes come directly
// from native FMAC: chop has flushed/saturated them to signed zero or finite
// normals. Only the explicit arithmetic judgments can contribute U/O flags.
template <std::uint32_t Mask>
[[gnu::always_inline]] inline std::uint16_t vu0_native_mac(const Vu0NativeFmac& result) noexcept {
    return static_cast<std::uint16_t>(
        vu0_detail::simd_mac_value_flags<false>(result.value, Mask) |
        vu0_detail::mac_judgment_flags(vu0_detail::simd_dest_bits(result.underflow) & Mask,
                                       vu0_detail::simd_dest_bits(result.overflow) & Mask));
}

// FMAC results are never exponent-0 nonzero or exponent-255 (simd_chop
// flushes and saturates them), so value-based U/O bits never join the
// sticky accumulation; the differential checker confirms it.
template <std::uint32_t Mask>
[[gnu::always_inline]] inline void vu0_native_sticky(Vu0NativeSticky& sticky,
                                                     const Vu0NativeFmac& result) noexcept {
    auto zero = _mm_cmpeq_epi32(_mm_and_si128(result.value, _mm_set1_epi32(0x7fffffff)),
                                _mm_setzero_si128());
    auto sign = result.value;
    if constexpr (Mask != 0x0fU) {
        const auto lane_mask = vu0_detail::simd_lane_mask(Mask);
        zero = _mm_and_si128(zero, lane_mask);
        sign = _mm_and_si128(sign, lane_mask);
    }
    sticky.zero = _mm_or_si128(sticky.zero, zero);
    sticky.sign = _mm_or_si128(sticky.sign, sign);
    sticky.underflow = _mm_or_si128(sticky.underflow, result.underflow);
    sticky.overflow = _mm_or_si128(sticky.overflow, result.overflow);
}

// STATUS as vu0_step leaves it: Z/S/U/O from the newest MAC update (if any),
// sticky copies of every update, and the FDIV bits already in `status`.
[[nodiscard]] inline std::uint32_t vu0_native_status(std::uint32_t status, const bool mac_touched,
                                                     const std::uint16_t mac,
                                                     const Vu0NativeSticky& sticky) noexcept {
    if (mac_touched) {
        const auto current = static_cast<std::uint32_t>(
            ((mac & 0x000fU) != 0U ? 0x1U : 0U) | ((mac & 0x00f0U) != 0U ? 0x2U : 0U) |
            ((mac & 0x0f00U) != 0U ? 0x4U : 0U) | ((mac & 0xf000U) != 0U ? 0x8U : 0U));
        status = (status & ~0x000fU) | current;
    }
    const auto any = [](const __m128i lanes) {
        return _mm_movemask_ps(_mm_castsi128_ps(lanes)) != 0;
    };
    const auto accumulated = static_cast<std::uint32_t>(
        (any(sticky.zero) ? 0x1U : 0U) | (any(sticky.sign) ? 0x2U : 0U) |
        (any(sticky.underflow) ? 0x4U : 0U) | (any(sticky.overflow) ? 0x8U : 0U));
    return status | (accumulated << 6U);
}

template <bool Maximum, std::uint32_t Mask>
[[gnu::always_inline]] inline __m128i vu0_native_min_max(const __m128i left, const __m128i right,
                                                         const __m128i base) noexcept {
    return vu0_native_select<Mask>(vu0_detail::simd_min_max<Maximum>(left, right), base);
}

[[nodiscard]] inline Gpr128 vu0_native_gpr(const __m128i value) noexcept {
    Gpr128 result;
    vu0_detail::simd_store(result, value);
    return result;
}

template <unsigned Lane>
[[gnu::always_inline]] inline std::uint32_t vu0_native_lane(const __m128i value) noexcept {
    return static_cast<std::uint32_t>(
        _mm_cvtsi128_si32(_mm_shuffle_epi32(value, _MM_SHUFFLE(Lane, Lane, Lane, Lane))));
}

// ITOF/FTOI lane by lane, with the interpreter's own expressions.
template <unsigned Fraction, std::uint32_t Mask>
[[gnu::always_inline]] inline __m128i vu0_native_itof(const __m128i source,
                                                      const __m128i destination) noexcept {
    auto words = vu0_native_gpr(source);
    const auto scale =
        Fraction == 0U ? 1.0F : std::bit_cast<float>(0x3f800000U - (Fraction << 23U));
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if ((Mask & (0x08U >> lane)) != 0U) {
            words.words[lane] =
                to_bits(static_cast<float>(static_cast<std::int32_t>(words.words[lane])) * scale);
        }
    }
    return vu0_native_select<Mask>(vu0_detail::simd_load(words), destination);
}

template <unsigned Fraction, std::uint32_t Mask>
[[gnu::always_inline]] inline __m128i vu0_native_ftoi(const __m128i source,
                                                      const __m128i destination) noexcept {
    auto words = vu0_native_gpr(source);
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if ((Mask & (0x08U >> lane)) != 0U) {
            words.words[lane] = vu0_float_to_int_bits(words.words[lane], Fraction);
        }
    }
    return vu0_native_select<Mask>(vu0_detail::simd_load(words), destination);
}

// VU0 data memory at a 16-byte-aligned offset below 4 KiB.
template <std::uint32_t Mask>
[[gnu::always_inline]] inline __m128i vu0_native_load(const std::byte* const address,
                                                      const __m128i destination) noexcept {
    if constexpr (Mask == 0U) {
        static_cast<void>(address);
        return destination;
    } else {
        return vu0_native_select<Mask>(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(address)), destination);
    }
}

template <std::uint32_t Mask>
[[gnu::always_inline]] inline void vu0_native_store(std::byte* const address,
                                                    const __m128i value) noexcept {
    if constexpr (Mask == 0x0fU) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(address), value);
    } else {
        const auto words = vu0_native_gpr(value);
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            if ((Mask & (0x08U >> lane)) != 0U) {
                std::memcpy(address + lane * 4U, &words.words[lane], sizeof(std::uint32_t));
            }
        }
    }
}

// ISW: the 16-bit value and a zero upper half in each selected word.
template <std::uint32_t Mask>
[[gnu::always_inline]] inline void vu0_native_store_words(std::byte* const address,
                                                          const std::uint32_t value) noexcept {
    for (std::size_t lane = 0; lane < 4U; ++lane) {
        if ((Mask & (0x08U >> lane)) != 0U) {
            std::memcpy(address + lane * 4U, &value, sizeof(value));
        }
    }
}

[[gnu::always_inline]] inline std::uint16_t vu0_native_u16(const std::byte* const address) noexcept {
    std::uint16_t value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}
