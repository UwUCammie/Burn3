// 0x004265d0 (a0 = A, a1 = B, a2 = C, a3 = D, t0 = pointer to two floats): writes one GIF strip packet at the
// write pointer held by the global at gp-24240, then stores the advanced pointer back. Returns nothing.
//
// The packet is a header quadword (GIF tag 0x84 in its top byte, NLOOP = N1 + T3 in its low word, the registers
// ST, XYZ2, ST, XYZ2, ST, XYZ2, ST, XYZ2 in its high word), then four quadwords for each iteration of a first loop
// (N1 iterations) and then of a second loop (T3 iterations). The loop counts and the retired count depend on the
// arguments, so the cost is the dry run's (N3_ENTRY).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCeiling = 20000U;

constexpr std::int32_t kWritePointerOffset = -24240;      // gp + offset: the packet write pointer
constexpr std::uint32_t kHalf = 0x3f000000U;              // 0.5f
constexpr std::uint32_t kTwoToMinus10 = 0x3a800000U;      // 2^-10
constexpr std::uint32_t kTwoToMinus14 = 0x38800000U;      // 2^-14
constexpr std::uint64_t kGifTag = 0x8400000000000000ULL;  // GIF tag: REGLIST, eight registers
constexpr std::uint64_t kGifRegisters = 0x52525252ULL;    // ST, XYZ2, ST, XYZ2, ST, XYZ2, ST, XYZ2
constexpr std::uint32_t kHeaderBytes = 16U;
constexpr std::uint32_t kIterationBytes = 64U;
constexpr std::uint32_t kStep = 512U;                     // the first loop's end moves 512 units per iteration

// Retired instruction counts from the listing, delay slots included. Each names the block it counts.
constexpr std::uint32_t kEntryCost = 46U;          // 004265d0..00426684: operands, float terms, bgtz and its delay slot
constexpr std::uint32_t kNotPositiveCost = 2U;     // 00426688..0042668c: A <= 0, so N1 = 0
constexpr std::uint32_t kPositiveCost = 3U;        // 00426690..00426698: slt, beq, nop
constexpr std::uint32_t kLessMinCost = 2U;         // 0042669c..004266a0: min(A, C) = A
constexpr std::uint32_t kNotLessMinCost = 1U;      // 004266a4: min(A, C) = C
constexpr std::uint32_t kRoundCost = 3U;           // 004266a8..004266b0: +31, bgez, delay sra
constexpr std::uint32_t kRoundAgainCost = 2U;      // 004266b4..004266b8: +31 again, sra (sum negative)
constexpr std::uint32_t kCompareCost = 3U;         // 004266bc..004266c4: slt A < C, bne, nop
constexpr std::uint32_t kNotLessCost = 2U;         // 004266c8..004266cc: T3 = 0 (A >= C)
constexpr std::uint32_t kLessCost = 2U;            // 004266d0..004266d4: blez, nop
constexpr std::uint32_t kLessShiftCost = 2U;       // 004266d8..004266dc or 004266e0..004266e4: a3 = A >> 5 or 0
constexpr std::uint32_t kLessBgezCost = 2U;        // 004266e8..004266ec: bgez (always taken), nop
constexpr std::uint32_t kLessCountCost = 3U;       // 004266f8..00426700: C + 31, bgez, delay sra
constexpr std::uint32_t kLessCountAgainCost = 2U;  // 00426704..00426708: C + 31 negative, +31 and sra
constexpr std::uint32_t kLessSubCost = 1U;         // 0042670c: T3 = v - a3
constexpr std::uint32_t kSetupCost = 31U;          // 00426710..00426788: header quadword, constants, loop 1 entry
constexpr std::uint32_t kLoopCheckCost = 3U;       // 0042681c..00426824 (and 004268f4..004268fc): test and decrement
constexpr std::uint32_t kLoopHeadCost = 2U;        // 0042678c..00426790 (and 00426864..00426868): bne and its nop
constexpr std::uint32_t kLoopLastCost = 1U;        // 00426794 (and 0042686c): the counter is zero, copy the end in
constexpr std::uint32_t kLoopBodyCost = 33U;       // 00426798..00426818 (and 00426870..004268f0)
constexpr std::uint32_t kMidCost = 15U;            // 00426828..00426860: loop 2 setup
constexpr std::uint32_t kEpilogueCost = 6U;        // 00426900..00426914: write pointer store, restore, return

constexpr std::int32_t as_signed(const std::uint32_t value) { return static_cast<std::int32_t>(value); }

// A 32-bit result as the 64-bit register holds it: sign-extended.
constexpr std::uint64_t sign_extend(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(as_signed(value)));
}

// The quadword with `hi` in its upper doubleword and `lo` in its lower one.
inline Gpr128 quad(const std::uint64_t hi, const std::uint64_t lo) {
    return Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32U),
                   static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32U)}};
}

template <class Context>
n3::Run emit_strip_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();

    const std::uint32_t a = call.arg(0);
    const std::uint32_t b = call.arg(1);
    const std::uint32_t c = call.arg(2);
    const std::uint32_t d = call.arg(3);
    const GuestAddress floats = call.arg(4);
    const GuestAddress pointer_address =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kWritePointerOffset);

    // Float terms (0x004265d4..0x00426684, 0x00426640..0x00426678).
    const std::uint32_t p0 = m.read_u32_le(floats);
    const std::uint32_t p5 = m.read_u32_le(floats + 4U);
    const std::uint32_t float_a = fpu::from_int(as_signed(a));
    const std::uint32_t float_b = fpu::from_int(as_signed(b));
    const std::uint32_t f9 = fpu::mul(kTwoToMinus10, fpu::add(kHalf, float_a));
    const std::uint32_t acc_b = fpu::add(0U, fpu::add(kHalf, float_b));

    // L0 = min(max(B, 0), D): pmaxw and pminw on the low word, kept by pextlw (0x004265f4..0x00426618).
    const std::int32_t clamped_b = as_signed(b) > 0 ? as_signed(b) : 0;
    const std::int32_t l0_signed = clamped_b < as_signed(d) ? clamped_b : as_signed(d);
    const std::uint32_t l0 = static_cast<std::uint32_t>(l0_signed);
    const std::uint64_t t0 = sign_extend(l0 << 4U) << 16U;
    const std::uint64_t a3_wide = sign_extend(d << 4U) << 16U;
    const std::uint32_t float_b_minus_l0 = fpu::from_int(as_signed(b - l0));
    const std::uint32_t float_b_minus_d = fpu::from_int(as_signed(b - d));

    // The three per-row terms, stored as stack words 76, 72 and 68 by the original.
    const std::uint32_t term76 = fpu::mul(fpu::msub(acc_b, float_b, p5), kTwoToMinus10);
    const std::uint32_t term72 = fpu::mul(fpu::msub(acc_b, float_b_minus_l0, p5), kTwoToMinus10);
    const std::uint32_t term68 = fpu::mul(fpu::msub(acc_b, float_b_minus_d, p5), kTwoToMinus10);
    const std::uint32_t scale = fpu::mul(p0, kTwoToMinus14);
    const std::uint32_t acc = fpu::add(0U, f9);
    const std::uint64_t wide76 = static_cast<std::uint64_t>(term76) << 32U;
    const std::uint64_t wide72 = static_cast<std::uint64_t>(term72) << 32U;
    const std::uint64_t wide68 = static_cast<std::uint64_t>(term68) << 32U;

    // Loop counts: N1 from A and C (0x00426680..0x004266b8), T3 from C and A (0x004266bc..0x0042670c).
    std::uint32_t retired = kEntryCost;
    const std::int32_t sa = as_signed(a);
    const std::int32_t sc = as_signed(c);
    std::int64_t n1 = 0;
    if (sa <= 0) {
        retired += kNotPositiveCost;
    } else {
        retired += kPositiveCost;
        const std::int32_t lesser = sa < sc ? sa : sc;
        retired += sa < sc ? kLessMinCost : kNotLessMinCost;
        std::uint32_t sum = static_cast<std::uint32_t>(lesser) + 31U;
        retired += kRoundCost;
        if (as_signed(sum) < 0) {
            sum += 31U;
            retired += kRoundAgainCost;
        }
        n1 = as_signed(sum) >> 5;
    }
    std::int64_t t3 = 0;
    retired += kCompareCost;
    if (sa >= sc) {
        retired += kNotLessCost;
    } else {
        retired += kLessCost;
        const std::int32_t a_shift = sa > 0 ? (sa >> 5) : 0;
        retired += kLessShiftCost;
        retired += kLessBgezCost;
        const std::uint32_t c_plus = c + 31U;
        retired += kLessCountCost;
        std::int32_t v = as_signed(c_plus) >> 5;
        if (as_signed(c_plus) < 0) {
            v = as_signed(c_plus + 31U) >> 5;
            retired += kLessCountAgainCost;
        }
        retired += kLessSubCost;
        t3 = as_signed(static_cast<std::uint32_t>(v) - static_cast<std::uint32_t>(a_shift));
    }
    retired += kSetupCost;

    // Header quadword at the write pointer (0x00426724..0x00426764).
    const GuestAddress packet = m.read_u32_le(pointer_address);
    const std::uint32_t loops = static_cast<std::uint32_t>(n1) + static_cast<std::uint32_t>(t3);
    n3::store_quad_any(m, packet, quad(kGifRegisters, kGifTag | loops));

    // Loop state (0x00426770..0x0042677c, 0x00426828..0x0042685c).
    const std::uint32_t t1_lo = a << 4U;
    const std::int32_t lane_a = as_signed(t1_lo);
    const std::int32_t lane_c = as_signed(c << 4U);
    const std::uint64_t min_wide = static_cast<std::uint32_t>(lane_a < lane_c ? lane_a : lane_c);
    const std::uint64_t max_wide = static_cast<std::uint32_t>(lane_a > 0 ? lane_a : 0);
    const std::uint32_t c_shift = c << 4U;
    std::uint64_t x = sign_extend(c_shift - 1U) & ~std::uint64_t{0x1FFU};
    std::uint64_t y = sign_extend(c_shift);
    GuestAddress t2 = packet + kHeaderBytes;
    const std::uint32_t acc_loop = acc;

    // First loop (0x0042678c..0x00426824): the last iteration's end is min(A << 4, C << 4) (pminw, lane 0).
    std::uint64_t t4 = 512U;
    std::uint64_t t5 = 0U;
    std::int64_t a1 = n1;
    for (;;) {
        retired += kLoopCheckCost;
        if (retired > kCeiling) {
            return call.decline();
        }
        const bool more = a1 != 0;
        a1 -= 1;
        if (!more) {
            break;
        }
        retired += kLoopHeadCost;
        if (a1 == 0) {
            t4 = min_wide;
            retired += kLoopLastCost;
        }
        const std::uint32_t start_lo = static_cast<std::uint32_t>(t5);
        const std::uint32_t end_lo = static_cast<std::uint32_t>(t4);
        const std::uint64_t start_wide = sign_extend(start_lo);
        const std::uint64_t end_term = sign_extend(end_lo) | t0;
        const std::uint32_t dy = fpu::msub(acc_loop, scale, fpu::from_int(as_signed(t1_lo - start_lo)));
        const std::uint32_t dx = fpu::msub(acc_loop, scale, fpu::from_int(as_signed(t1_lo - end_lo)));
        t5 = t4;
        t4 = sign_extend(end_lo + kStep);
        n3::store_quad_any(m, t2, quad(start_wide, dy | wide76));
        n3::store_quad_any(m, t2 + kHeaderBytes, quad(end_term, dx | wide72));
        n3::store_quad_any(m, t2 + 2U * kHeaderBytes, quad(start_wide | a3_wide, dy | wide68));
        n3::store_quad_any(m, t2 + 3U * kHeaderBytes, quad(end_term, dx | wide72));
        t2 += kIterationBytes;
        retired += kLoopBodyCost;
    }
    retired += kMidCost;

    // Second loop (0x00426864..0x004268fc): the last iteration's x is max(A << 4, 0) (pmaxw, lane 0).
    for (;;) {
        retired += kLoopCheckCost;
        if (retired > kCeiling) {
            return call.decline();
        }
        const bool more = t3 != 0;
        t3 -= 1;
        if (!more) {
            break;
        }
        retired += kLoopHeadCost;
        if (t3 == 0) {
            x = max_wide;
            retired += kLoopLastCost;
        }
        const std::uint32_t x_lo = static_cast<std::uint32_t>(x);
        const std::uint32_t y_lo = static_cast<std::uint32_t>(y);
        const std::uint64_t x_wide = sign_extend(x_lo);
        const std::uint64_t y_term = sign_extend(y_lo) | t0;
        const std::uint64_t x_term = x_wide | a3_wide;
        const std::uint32_t dy = fpu::madd(acc_loop, scale, fpu::from_int(as_signed(y_lo - t1_lo)));
        const std::uint32_t dx = fpu::madd(acc_loop, scale, fpu::from_int(as_signed(x_lo - t1_lo)));
        n3::store_quad_any(m, t2, quad(x_wide, dx | wide76));
        n3::store_quad_any(m, t2 + kHeaderBytes, quad(y_term, dy | wide72));
        n3::store_quad_any(m, t2 + 2U * kHeaderBytes, quad(x_term, dx | wide68));
        n3::store_quad_any(m, t2 + 3U * kHeaderBytes, quad(y_term, dy | wide72));
        t2 += kIterationBytes;
        y = x;
        x = sign_extend(x_lo - 512U);
        retired += kLoopBodyCost;
    }

    retired += kEpilogueCost;
    m.write_u32_le(pointer_address, t2);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(004265d0, 840U, 0x84d1f9f84dd53dfeULL, "EmitStripPacket", kCeiling, emit_strip_packet, EngineReturn::Nothing)
