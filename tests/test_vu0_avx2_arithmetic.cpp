// Isolated proof only; root owns compiling/running it after the active batch.
// Compile actual production native support in both of its ISA namespaces.
#include "takedown/guest/vu0_avx2_arithmetic.hpp"
#include "tests/helpers/vu0_fmac_differential.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__FAST_MATH__) || !defined(TAKEDOWN_VU0_HAVE_AVX2)
#error This proof requires x86-64 GCC SSE2/AVX2 support without fast-math.
#endif

namespace {
using takedown::guest::Gpr128;
namespace vu0_detail = takedown::guest::vu0_detail;
namespace fixture = takedown::test::vu0_fmac;
using Operation = vu0_detail::Vu0Fmac;

// Names parsed by unrelated, uninstantiated conversion templates in support.
inline std::uint32_t to_bits(float value) { return std::bit_cast<std::uint32_t>(value); }
std::uint32_t vu0_float_to_int_bits(std::uint32_t, unsigned);

namespace sse {
#include "src/guest/vu0_native_support.inc"
}
#define TAKEDOWN_VU0_NATIVE_AVX2 1
#pragma GCC push_options
#pragma GCC target("avx2")
namespace avx {
#include "src/guest/vu0_native_support.inc"

// Keep the target boundary explicit when generic verification calls back
// into production helpers marked always_inline under target("avx2").
template <unsigned Mask>
[[gnu::noinline]] std::uint16_t control_mac(const Vu0NativeFmac& result) {
    return vu0_native_mac<Mask>(result);
}
template <unsigned Mask>
[[gnu::noinline]] std::uint16_t candidate_mac(const Vu0NativeFmac& result) {
    return static_cast<std::uint16_t>(
        vu0_detail::simd_mac_value_flags<false>(result.value, Mask) |
        vu0_detail::mac_judgment_flags(vu0_detail::simd_dest_bits(result.underflow) & Mask,
                                       vu0_detail::simd_dest_bits(result.overflow) & Mask));
}
template <unsigned Mask>
[[gnu::noinline]] void update_sticky(Vu0NativeSticky& sticky, const Vu0NativeFmac& result) {
    vu0_native_sticky<Mask>(sticky, result);
}
[[gnu::noinline]] std::uint32_t publish_status(unsigned status, bool touched, std::uint16_t mac,
                                             const Vu0NativeSticky& sticky) {
    return vu0_native_status(status, touched, mac, sticky);
}
}
#pragma GCC pop_options
#undef TAKEDOWN_VU0_NATIVE_AVX2

struct Counts {
    std::uint64_t environments{};
    std::uint64_t vectors{};
    std::uint64_t kernel_masks{};
    std::uint64_t explicit_masks{};
    std::uint64_t status_checks{};
    std::uint64_t underflow_lanes{};
    std::uint64_t overflow_lanes{};
    std::uint64_t negative_zero_lanes{};
    std::uint64_t positive_max_lanes{};
    std::uint64_t negative_max_lanes{};
};

constexpr std::array<std::uint32_t, 8> kPriorStatus{
    0U, 0x0fU, 0x30U, 0xc0U, 0x300U, 0xfc0U, 0xfffU, 0xdeadbeefU};

// The candidate's only semantic change: omit impossible value-based U/O.
template <unsigned Mask, class Result>
[[gnu::noinline]] std::uint16_t reduced_mac(const Result& result) {
    return static_cast<std::uint16_t>(
        vu0_detail::simd_mac_value_flags<false>(result.value, Mask) |
        vu0_detail::mac_judgment_flags(vu0_detail::simd_dest_bits(result.underflow) & Mask,
                                       vu0_detail::simd_dest_bits(result.overflow) & Mask));
}

[[gnu::noinline]] std::uint16_t reference_mac(const Gpr128& words, unsigned mask,
                                            unsigned underflow, unsigned overflow) {
    std::uint16_t flags = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const unsigned bit = 8U >> lane;
        if ((mask & bit) == 0) continue;
        const auto value = words.words[lane];
        if ((value & 0x7fffffffU) == 0) flags |= bit;
        if ((value & 0x80000000U) != 0) flags |= bit << 4U;
        if ((value & 0x7f800000U) == 0 && (value & 0x7fffffU) != 0) flags |= bit << 8U;
        if ((value & 0x7f800000U) == 0x7f800000U) flags |= bit << 12U;
    }
    return flags | ((underflow & mask) << 8U) | ((overflow & mask) << 12U);
}

unsigned status_nibble(const unsigned mac) {
    return ((mac & 0xfU) ? 1U : 0U) | ((mac & 0xf0U) ? 2U : 0U) |
           ((mac & 0xf00U) ? 4U : 0U) | ((mac & 0xf000U) ? 8U : 0U);
}

template <class Sticky, class Result, class StickyFn, class StatusFn>
void check_status(const Result& result, unsigned control, unsigned candidate, unsigned independent,
                  unsigned old_flags, StickyFn update_sticky, StatusFn publish_status, Counts& counts) {
    const unsigned prior_sticky = (old_flags >> 6U) & 15U;
    const auto prior = [](bool set) { return _mm_set1_epi32(set ? -1 : 0); };
    Sticky sticky{prior(prior_sticky & 1U), prior(prior_sticky & 2U),
                  prior(prior_sticky & 4U), prior(prior_sticky & 8U)};
    update_sticky(sticky, result);
    for (const bool touched : {false, true}) {
        const auto expected = (touched ? ((old_flags & ~15U) | status_nibble(independent)) : old_flags) |
                              ((prior_sticky | status_nibble(independent)) << 6U);
        if (publish_status(old_flags, touched, control, sticky) != expected ||
            publish_status(old_flags, touched, candidate, sticky) != expected)
            throw std::runtime_error("STATUS/sticky state differs after MAC reduction");
        ++counts.status_checks;
    }
}

template <unsigned Mask, class Result, class Sticky, class MacFn, class StickyFn, class StatusFn>
void check_result(const Result& result, const Gpr128& expected_words,
                  unsigned expected_underflow, unsigned expected_overflow,
                  MacFn control_mac, StickyFn update_sticky, StatusFn publish_status,
                  Counts& counts) {
    Gpr128 words;
    vu0_detail::simd_store(words, result.value);
    const auto underflow = vu0_detail::simd_dest_bits(result.underflow);
    const auto overflow = vu0_detail::simd_dest_bits(result.overflow);
    if (words != expected_words || underflow != expected_underflow || overflow != expected_overflow)
        throw std::runtime_error("production native FMAC differs from scalar oracle");
    for (unsigned lane = 0; lane < 4; ++lane) {
        if ((Mask & (8U >> lane)) == 0) continue;
        const auto value = words.words[lane], magnitude = value & 0x7fffffffU;
        const auto exponent = magnitude & 0x7f800000U;
        if ((exponent == 0 && magnitude != 0) || exponent == 0x7f800000U)
            throw std::runtime_error("active native FMAC lane violates canonical-value premise");
        counts.negative_zero_lanes += value == 0x80000000U;
        counts.positive_max_lanes += value == 0x7f7fffffU;
        counts.negative_max_lanes += value == 0xff7fffffU;
    }
    counts.underflow_lanes += std::popcount(underflow);
    counts.overflow_lanes += std::popcount(overflow);
    const auto before = _mm_getcsr();
    const auto control = control_mac(result);
    const auto after_control = _mm_getcsr();
    const auto candidate = reduced_mac<Mask>(result);
    const auto after_candidate = _mm_getcsr();
    const auto independent = reference_mac(words, Mask, underflow, overflow);
    if (control != candidate || control != independent)
        throw std::runtime_error("native control/candidate/independent MAC mismatch");
    if (before != after_control || before != after_candidate)
        throw std::runtime_error("integer MAC computation modified MXCSR");

    // Seed every kind of prior sticky flag, and retain unrelated/FDIV bits.
    const auto old_flags = kPriorStatus[counts.kernel_masks % kPriorStatus.size()];
    check_status<Sticky>(result, control, candidate, independent, old_flags,
                         update_sticky, publish_status, counts);
    // The same word enters the existing four-slot delayed MAC pipeline.
    std::array<unsigned, 4> before_pipeline{0xffffU, 0x00f0U, 0xf00fU, 0xaa55U};
    const std::array<unsigned, 4> control_pipeline{before_pipeline[1], before_pipeline[2], before_pipeline[3], control};
    const std::array<unsigned, 4> candidate_pipeline{before_pipeline[1], before_pipeline[2], before_pipeline[3], candidate};
    if (control_pipeline != candidate_pipeline) throw std::runtime_error("delayed MAC differs");
}

template <Operation Op, unsigned Mask>
void check_sse(const Gpr128& a, const Gpr128& b, const Gpr128& acc, const Gpr128& base, Counts& counts) {
    const auto expected = vu0_detail::scalar_fmac<Op>(a, b, acc, base, Mask);
    const auto actual = sse::vu0_native_fmac<Op, Mask>(vu0_detail::simd_load(a), vu0_detail::simd_load(b),
                                                    vu0_detail::simd_load(acc), vu0_detail::simd_load(base));
    check_result<Mask, sse::Vu0NativeFmac, sse::Vu0NativeSticky>(actual, expected.value,
        expected.underflow_mask, expected.overflow_mask, sse::vu0_native_mac<Mask>,
        sse::vu0_native_sticky<Mask>, sse::vu0_native_status, counts);
    ++counts.kernel_masks;
}

template <Operation Op, unsigned Mask>
[[gnu::target("avx2")]] void check_avx(const Gpr128& a, const Gpr128& b, const Gpr128& acc,
                                      const Gpr128& base, Counts& counts) {
    const auto expected = vu0_detail::scalar_fmac<Op>(a, b, acc, base, Mask);
    const auto actual = avx::vu0_native_fmac<Op, Mask>(vu0_detail::simd_load(a), vu0_detail::simd_load(b),
                                                    vu0_detail::simd_load(acc), vu0_detail::simd_load(base));
    const auto before = _mm_getcsr();
    if (avx::candidate_mac<Mask>(actual) != reduced_mac<Mask>(actual) || _mm_getcsr() != before)
        throw std::runtime_error("AVX2-compiled candidate differs or modifies MXCSR");
    check_result<Mask, avx::Vu0NativeFmac, avx::Vu0NativeSticky>(actual, expected.value,
        expected.underflow_mask, expected.overflow_mask, avx::control_mac<Mask>,
        avx::update_sticky<Mask>, avx::publish_status, counts);
    ++counts.kernel_masks;
}

template <Operation Op, std::size_t... Mask>
void check_masks(const Gpr128& a, const Gpr128& b, const Gpr128& acc, const Gpr128& base,
                 Counts& counts, std::index_sequence<Mask...>) {
    (check_sse<Op, Mask>(a, b, acc, base, counts), ...);
    (check_avx<Op, Mask>(a, b, acc, base, counts), ...);
}

void check_vector(const Gpr128& a, const Gpr128& b, const Gpr128& acc, const Gpr128& base, Counts& counts) {
    const auto masks = std::make_index_sequence<16>{};
    check_masks<Operation::Add>(a, b, acc, base, counts, masks);
    check_masks<Operation::Subtract>(a, b, acc, base, counts, masks);
    check_masks<Operation::Multiply>(a, b, acc, base, counts, masks);
    check_masks<Operation::MultiplyAdd>(a, b, acc, base, counts, masks);
    check_masks<Operation::MultiplySubtract>(a, b, acc, base, counts, masks);
    ++counts.vectors;
}

template <unsigned Mask, class Result, class Sticky, class MacFn, class StickyFn, class StatusFn>
void explicit_judgment_masks(MacFn control_mac, StickyFn update_sticky, StatusFn publish_status,
                            Counts& counts) {
    // Unselected passthrough lanes deliberately contain denormal and NaN bits.
    const Gpr128 base{{0x00000001U, 0xff800001U, 0x807fffffU, 0x7f800000U}};
    const Gpr128 active{{0x80000000U, 0x7f7fffffU, 0xff7fffffU, 0x00800000U}};
    const auto value = sse::vu0_native_select<Mask>(vu0_detail::simd_load(active), vu0_detail::simd_load(base));
    Gpr128 words;
    vu0_detail::simd_store(words, value);
    for (unsigned underflow = 0; underflow < 16; ++underflow) {
        for (unsigned overflow = 0; overflow < 16; ++overflow) {
            const Result result{value, vu0_detail::simd_lane_mask(underflow),
                                      vu0_detail::simd_lane_mask(overflow)};
            const auto original = control_mac(result);
            const auto candidate = reduced_mac<Mask>(result);
            const auto independent = reference_mac(words, Mask, underflow, overflow);
            if (original != candidate || original != independent)
                throw std::runtime_error("explicit U/O mask or inactive-lane leakage");
            // Native FMAC masks these judgments before sticky accumulation.
            // The unmasked synthetic result above also tests MAC's own mask.
            const Result masked_result{value, vu0_detail::simd_lane_mask(underflow & Mask),
                                             vu0_detail::simd_lane_mask(overflow & Mask)};
            for (const auto prior_status : kPriorStatus) {
                check_status<Sticky>(masked_result, original, candidate, independent, prior_status,
                                     update_sticky, publish_status, counts);
            }
            ++counts.explicit_masks;
        }
    }
}

template <unsigned Mask>
[[gnu::target("avx2")]] void explicit_avx_judgment_masks(Counts& counts) {
    explicit_judgment_masks<Mask, avx::Vu0NativeFmac, avx::Vu0NativeSticky>(
        avx::control_mac<Mask>, avx::update_sticky<Mask>, avx::publish_status, counts);
}

class Environment {
    unsigned saved = _mm_getcsr();
public:
    explicit Environment(unsigned environment) { _mm_setcsr(0x1f80U | environment); }
    ~Environment() { _mm_setcsr(saved); }
};

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("usage: prove_native_mac [random-vectors-per-environment]");
        const unsigned random_vectors = argc == 2 ? static_cast<unsigned>(std::stoul(argv[1])) : 1024U;
        if (random_vectors > 1048576U) throw std::runtime_error("random vector count exceeds bound");
        if (!vu0_detail::avx2_supported()) throw std::runtime_error("AVX2 required: both actual native paths must execute");
        Counts counts;
        [&]<std::size_t... Mask>(std::index_sequence<Mask...>) {
            (explicit_judgment_masks<Mask, sse::Vu0NativeFmac, sse::Vu0NativeSticky>(
                sse::vu0_native_mac<Mask>, sse::vu0_native_sticky<Mask>, sse::vu0_native_status, counts), ...);
            (explicit_avx_judgment_masks<Mask>(counts), ...);
        }(std::make_index_sequence<16>{});
        fixture::Random random{0x67a436f397a0933bULL};
        for (unsigned rounding = 0; rounding < 4; ++rounding) {
            for (unsigned flush = 0; flush < 4; ++flush) {
                for (const unsigned sticky : {0U, 0x3fU}) {
                    Environment scope((rounding << 13U) | ((flush & 1U) ? 0x8000U : 0U) |
                                      ((flush & 2U) ? 0x0040U : 0U) | sticky);
                    ++counts.environments;
                    for (unsigned left = 0; left < fixture::kEdges.size(); ++left) {
                        for (unsigned right = 0; right < fixture::kEdges.size(); ++right) {
                            Gpr128 a{}, b{}, acc{}, base{};
                            for (unsigned lane = 0; lane < 4; ++lane) {
                                a.words[lane] = fixture::kEdges[(left + lane) % fixture::kEdges.size()];
                                b.words[lane] = fixture::kEdges[(right + lane * 3U) % fixture::kEdges.size()];
                                acc.words[lane] = fixture::kEdges[(left + right + lane * 7U) % fixture::kEdges.size()];
                                base.words[lane] = fixture::kEdges[(left * 3U + right + lane) % fixture::kEdges.size()];
                            }
                            check_vector(a, b, acc, base, counts);
                        }
                    }
                    for (unsigned index = 0; index < random_vectors; ++index) {
                        Gpr128 a{}, b{}, acc{}, base{};
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            a.words[lane] = fixture::operand(random, random.next32());
                            b.words[lane] = fixture::operand(random, a.words[lane]);
                            acc.words[lane] = fixture::operand(random, b.words[lane]);
                            base.words[lane] = random.next32();
                        }
                        check_vector(a, b, acc, base, counts);
                    }
                }
            }
        }
        if (!counts.underflow_lanes || !counts.overflow_lanes || !counts.negative_zero_lanes ||
            !counts.positive_max_lanes || !counts.negative_max_lanes)
            throw std::runtime_error("required signed-zero/saturation/judgment coverage missing");
        std::cout << "PASS native MAC reduction: environments=" << counts.environments
                  << " vectors=" << counts.vectors << " actual-kernel-mask-cases=" << counts.kernel_masks
                  << " explicit-judgment-mask-cases=" << counts.explicit_masks
                  << " status-checks=" << counts.status_checks << " underflow-lanes=" << counts.underflow_lanes
                  << " overflow-lanes=" << counts.overflow_lanes << " negative-zero-lanes=" << counts.negative_zero_lanes
                  << " positive-max-lanes=" << counts.positive_max_lanes << " negative-max-lanes=" << counts.negative_max_lanes
                  << " production-unmodified=true performance-qualified=false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native MAC reduction: " << error.what() << '\n';
        return 1;
    }
}
