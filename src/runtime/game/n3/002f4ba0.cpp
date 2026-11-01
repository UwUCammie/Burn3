// 0x002f4ba0 (a0 = tuning record P, f12 = x, f13 = y): InterpolateResponseCurve. A pure function: it stores
// nothing and returns one float in f0. It reads P[0], P[4], P[8] and P[20], the low clamp float at $gp - 32644,
// and three 4-entry tables: T0 (breakpoints, descending), T1 and T2 (values). Each block of the listing adds its
// retired count, delay slots included, as a path enters it, so every path retires exactly the original's
// instructions.
//
// A negative y negates x. x at or above T0[0] (NaN too) takes the top branch, which blends entry 0 of the value
// tables; x at or below T0[3] takes the bottom branch, which blends entry 3. Otherwise a scan finds the first
// a = 1..3 with x at or above T0[a] and interpolates between entries a - 1 and a. The scan only runs with x above
// T0[3], so it always stops at a <= 3 and its exit at a = 4 is unreachable. The listing's exit uses the incoming
// $a2 and $t0 as the entries; that is mirrored anyway, and it is why the declared cost is the 112 of that path
// rather than the 111 of the deepest reachable one.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kBreakpoints = 0x01e86d20U;  // T0 (lui 0x01e8, +27936); T0[3] is at +12
constexpr GuestAddress kTableOne = 0x01e86d30U;     // T1 (+27952)
constexpr GuestAddress kTableTwo = 0x01e86d40U;     // T2 (+27968)
constexpr std::uint32_t kLowClampOffset = 32644U;   // the float at $gp - 32644: the low clamp

// Float bit patterns the original builds with lui.
constexpr std::uint32_t kOne = 0x3f800000U;    // 1.0
constexpr std::uint32_t kTwo = 0x40000000U;    // 2.0
constexpr std::uint32_t kLimit = 0x44aa0000U;  // 1360.0, the top of the y range

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 5U;         // 0x2f4ba0: mtc1, nop, c.olt.s, bc1f and its delay slot
constexpr std::uint32_t kNegateCost = 1U;        // 0x2f4bb4: neg.s, only when y < 0
constexpr std::uint32_t kSetupCost = 14U;        // 0x2f4bb8: y / P[20], (1360 - y) / 1360, the T0[0] test
constexpr std::uint32_t kLowerTestCost = 5U;     // 0x2f4bf0: the T0[3] test
constexpr std::uint32_t kEdgeCost = 21U;         // 0x2f4cfc (top) and 0x2f4d50 (bottom)
constexpr std::uint32_t kScanStartCost = 4U;     // 0x2f4c04
constexpr std::uint32_t kScanTestCost = 3U;      // 0x2f4c30: slti, bne and its delay slot
constexpr std::uint32_t kScanStepCost = 6U;      // 0x2f4c14: one breakpoint compare
constexpr std::uint32_t kScanNextCost = 1U;      // 0x2f4c2c: a = a + 1
constexpr std::uint32_t kScanFoundCost = 3U;     // 0x2f4da4: lo = a - 1, hi = a
constexpr std::uint32_t kInterpolateCost = 48U;  // 0x2f4c3c..0x2f4cf8
constexpr std::uint32_t kReturnCost = 2U;        // jr $ra and its delay slot

// The longest path through the listing: entry with the negation, setup, the T0[3] test, the scan's three
// passes (3 + 6 + 1 each) and its a = 4 test (3), the interpolation and the return:
// 5 + 1 + 14 + 5 + 4 + 33 + 48 + 2 = 112. The a = 4 exit cannot run, so 111 is the deepest path that does.
constexpr std::uint32_t kCost = 112U;

// MAX.S and MIN.S on the raw sign/magnitude patterns: the ordering of guest::ee_cop1::max_s and min_s.
std::uint32_t ee_max(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}
std::uint32_t ee_min(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}

// The top (table entry 0) and bottom (entry 3) branches: the entry's T2 and T1 values, blended with the half term
// (P[0] * P[8] + (y / P[20]) * P[4]) / 2 and clamped to the low float at $gp - 32644 and 2.0.
std::uint32_t edge_value(const std::uint32_t t2, const std::uint32_t t1, const std::uint32_t half,
                         const std::uint32_t q, const std::uint32_t gp_clamp) {
    const std::uint32_t acc = fpu::add(0U, fpu::mul(t2, q));  // ADDA.S into a zeroed accumulator
    return ee_min(ee_max(fpu::madd(acc, t1, half), gp_clamp), kTwo);
}

template <class Context>
n3::Run interpolate_response_curve(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress tuning = call.arg(0);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t y = call.cpu().read_fpr_bits(13U);
    std::uint32_t x = call.cpu().read_fpr_bits(12U);

    std::uint32_t retired = kEntryCost;
    if (fpu::lt(y, 0U)) {
        x = fpu::neg(x);
        retired += kNegateCost;
    }

    // Every branch below reads these values, so they are loaded before the first test.
    retired += kSetupCost;
    const std::uint32_t p0 = m.read_u32_le(tuning);
    const std::uint32_t p4 = m.read_u32_le(tuning + 4U);
    const std::uint32_t p8 = m.read_u32_le(tuning + 8U);
    const std::uint32_t y_over_p20 = fpu::div(y, m.read_u32_le(tuning + 20U));
    const std::uint32_t q = fpu::div(fpu::sub(kLimit, y), kLimit);
    const std::uint32_t half = fpu::div(fpu::madd(fpu::mul(p0, p8), y_over_p20, p4), kTwo);
    const std::uint32_t gp_clamp = m.read_u32_le(gp - kLowClampOffset);

    std::uint32_t result = 0U;
    if (!fpu::lt(x, m.read_u32_le(kBreakpoints))) {
        // 0x2f4cfc: the top branch (x at or above T0[0], NaN included).
        retired += kEdgeCost;
        result = edge_value(m.read_u32_le(kTableTwo), m.read_u32_le(kTableOne), half, q, gp_clamp);
        retired += kReturnCost;
    } else {
        retired += kLowerTestCost;
        if (fpu::le(x, m.read_u32_le(kBreakpoints + 12U))) {
            // 0x2f4d50: the bottom branch (x at or below T0[3]).
            retired += kEdgeCost;
            result = edge_value(m.read_u32_le(kTableTwo + 12U), m.read_u32_le(kTableOne + 12U), half, q, gp_clamp);
            retired += kReturnCost;
        } else {
            // 0x2f4c04: scan a = 1, 2, 3 for the first breakpoint that x is at or above. Each pass retires the
            // loop test (slti, bne, delay slot) and the compare block, then the step to the next pass or the found block.
            retired += kScanStartCost;
            std::uint32_t a = 1U;
            std::uint32_t hi = 0U;
            std::uint32_t lo = 0U;
            for (;;) {
                retired += kScanTestCost;
                if (a >= 4U) {
                    // Unreachable (see above): the listing then reads the incoming $a2 and $t0.
                    hi = call.arg(2);
                    lo = call.arg(4);
                    break;
                }
                retired += kScanStepCost;
                if (!fpu::lt(x, m.read_u32_le(kBreakpoints + 4U * a))) {
                    retired += kScanFoundCost;
                    hi = a;
                    lo = a - 1U;
                    break;
                }
                retired += kScanNextCost;
                ++a;
            }

            // 0x2f4c3c: interpolate between entries lo and hi. The indices are byte offsets (index << 2, 32-bit).
            retired += kInterpolateCost + kReturnCost;
            const std::uint32_t hi_offset = hi << 2U;
            const std::uint32_t lo_offset = lo << 2U;
            const std::uint32_t t0_hi = m.read_u32_le(kBreakpoints + hi_offset);
            const std::uint32_t t0_lo = m.read_u32_le(kBreakpoints + lo_offset);
            const std::uint32_t distance = fpu::sub(x, t0_hi);
            const std::uint32_t span = fpu::sub(t0_lo, t0_hi);
            const std::uint32_t t = fpu::div(fpu::sub(span, distance), span);
            const std::uint32_t t2_lo = m.read_u32_le(kTableTwo + lo_offset);
            const std::uint32_t t2_hi = m.read_u32_le(kTableTwo + hi_offset);
            const std::uint32_t t1_lo = m.read_u32_le(kTableOne + lo_offset);
            const std::uint32_t t1_hi = m.read_u32_le(kTableOne + hi_offset);
            // Each ADDA.S overwrites the accumulator, so each chain is built in full before the next starts.
            const std::uint32_t lower = fpu::madd(fpu::add(0U, fpu::mul(q, t2_lo)), half, t1_lo);
            const std::uint32_t upper = fpu::madd(fpu::add(0U, fpu::mul(q, t2_hi)), half, t1_hi);
            const std::uint32_t blended = fpu::madd(fpu::mul(upper, t), lower, fpu::sub(kOne, t));
            result = ee_min(ee_max(blended, gp_clamp), kTwo);
        }
    }

    call.cpu().write_fpr_bits(0U, result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002f4ba0{0x002f4ba0U, 536U, 0xd7de3d2789856e6cULL, "InterpolateResponseCurve",
                                                  kCost, n3::live<&interpolate_response_curve<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f4ba0, interpolate_response_curve)
