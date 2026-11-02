// 0x002fb640 ApplyCurveModifier (a0 = curve record C, a1 = input float array, a2 = output float array; returns nothing).
// Record C: +0 pointer P to n points {float x, float y} (8 bytes each), +4 u8 n, +5 s8 input index, +6 s8 output index.
// x = in[C5] is clamped to the curve's x range: below P[0].x it becomes P[0].x; otherwise it stays x when x is not above
// P[n-1].x, and becomes P[n-1].x when it is (a NaN also becomes P[n-1].x). The segment that holds the clamped value is
// found by a scan from the first point: a point whose x lies above the value is passed over, and the first pair whose
// upper x lies above it ends the scan. y is interpolated along that segment (a flat segment, with equal x ends, takes
// its start y) and stored at out[C6]; a zero output index multiplies out[0] by y instead.
//
// The scan runs once per point, so the retired count depends on the data. The body is the N3_ENTRY dry run: it does its
// one store on whichever memory it runs on, and each path counts the listing's blocks (delay slots included).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The scan is bounded by the point count (at most 255 passes), so no path comes near this.
constexpr std::uint32_t kCeiling = 20000U;

constexpr GuestAddress kPointsField = 0U;   // +0: P
constexpr GuestAddress kCountField = 4U;    // +4: n (u8)
constexpr GuestAddress kInputField = 5U;    // +5: input index (s8)
constexpr GuestAddress kOutputField = 6U;   // +6: output index (s8)
constexpr GuestAddress kPointSize = 8U;     // one {x, y} point
constexpr GuestAddress kYOffset = 4U;       // y within a point

// Retired instructions per part of the listing.
constexpr std::uint32_t kBelowFirstCost = 14U;     // the 12-instruction head, then beq and its delay slot
constexpr std::uint32_t kInRangeCost = 18U;        // the head, then c.ole.s, its bc1t and delay slot, a move pair or
                                                   // beq and a move, and the lbu
constexpr std::uint32_t kScanSetupCost = 3U;       // daddu, beq, addiu (n - 1)
constexpr std::uint32_t kScanTestCost = 3U;        // slt, bne and its delay slot
constexpr std::uint32_t kPointTestCost = 6U;       // sll, addu, lwc1, c.olt.s, bc1t and its delay slot
constexpr std::uint32_t kNextTestCost = 4U;        // lwc1, c.olt.s, bc1t and its delay slot
constexpr std::uint32_t kStepCost = 1U;            // addiu (the next point)
constexpr std::uint32_t kSegmentTestCost = 10U;    // sll, addu, lwc1 x2, mtc1, nop, sub.s, c.eq.s, bc1t and its delay slot
constexpr std::uint32_t kFlatSegmentCost = 2U;     // beq and its delay slot (y = the start y)
constexpr std::uint32_t kSpanSegmentCost = 8U;     // sub.s, nop, div.s, lwc1 x2, adda.s, sub.s, madd.s
constexpr std::uint32_t kStoreAtIndexCost = 10U;   // lb, bne and its delay slot; sll, addu, swc1; beq and its delay slot;
                                                   // jr and its delay slot
constexpr std::uint32_t kScaleFirstCost = 13U;     // lb, bne and its delay slot; sll, addu, lwc1, mul.s, beq and its delay
                                                   // slot swc1; beq and its delay slot; jr and its delay slot

// lb: a signed byte as a 32-bit word, so the address arithmetic wraps the way the original's does.
template <class Memory>
std::uint32_t signed_byte(Memory& m, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(address))));
}

template <class Context>
n3::Run apply_curve_modifier(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress curve = call.arg(0);
    const GuestAddress inputs = call.arg(1);
    const GuestAddress outputs = call.arg(2);

    const GuestAddress points = m.read_u32_le(curve + kPointsField);
    const std::uint32_t count = m.read_u8(curve + kCountField);
    const std::uint32_t x = m.read_u32_le(inputs + (signed_byte(m, curve + kInputField) << 2U));
    const std::uint32_t first = m.read_u32_le(points);

    // Clamp to [first, last]. A NaN is not below first and not at most last, so it takes last.
    std::uint32_t clamped = first;
    std::uint32_t retired = kBelowFirstCost;
    if (!fpu::lt(x, first)) {
        const std::uint32_t last = m.read_u32_le(points + kPointSize * count - kPointSize);
        clamped = fpu::le(x, last) ? x : last;
        retired = kInRangeCost;
    }

    // Scan from the first point. Each pass that goes on is charged the loop test that follows it (the first test is
    // charged before the scan); the pass that finds the segment is followed by no test.
    const std::int32_t last_segment = static_cast<std::int32_t>(count) - 1;
    retired += kScanSetupCost + kScanTestCost;
    std::int32_t segment = 0;
    for (; segment < last_segment; ++segment) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress point = points + kPointSize * static_cast<std::uint32_t>(segment);
        if (fpu::lt(clamped, m.read_u32_le(point))) {
            retired += kPointTestCost + kStepCost + kScanTestCost;
            continue;
        }
        if (fpu::lt(clamped, m.read_u32_le(point + kPointSize))) {
            retired += kPointTestCost + kNextTestCost;
            break;
        }
        retired += kPointTestCost + kNextTestCost + kStepCost + kScanTestCost;
    }

    // The segment's start and end x. A flat segment takes its start y; otherwise y = y0 + t * (y1 - y0), with
    // t = (clamped - x0) / (x1 - x0), accumulated the way adda.s followed by madd.s does it.
    const GuestAddress start = points + kPointSize * static_cast<std::uint32_t>(segment);
    const std::uint32_t x0 = m.read_u32_le(start);
    const std::uint32_t span = fpu::sub(m.read_u32_le(start + kPointSize), x0);
    retired += kSegmentTestCost;
    std::uint32_t y = 0U;
    if (fpu::eq(0U, span)) {
        y = m.read_u32_le(start + kYOffset);
        retired += kFlatSegmentCost;
    } else {
        const std::uint32_t t = fpu::div(fpu::sub(clamped, x0), span);
        const std::uint32_t y_start = m.read_u32_le(start + kYOffset);
        const std::uint32_t y_end = m.read_u32_le(start + kPointSize + kYOffset);
        y = fpu::madd(fpu::add(0U, y_start), t, fpu::sub(y_end, y_start));
        retired += kSpanSegmentCost;
    }

    // out[C6] = y, except that index 0 multiplies the value already there.
    const std::uint32_t out_index = signed_byte(m, curve + kOutputField);
    if (out_index != 0U) {
        m.write_u32_le(outputs + (out_index << 2U), y);
        retired += kStoreAtIndexCost;
    } else {
        m.write_u32_le(outputs, fpu::mul(m.read_u32_le(outputs), y));
        retired += kScaleFirstCost;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002fb640, 300U, 0xb4d4cc1c4b3cde05ULL, "ApplyCurveModifier", kCeiling, apply_curve_modifier,
         EngineReturn::Nothing)
