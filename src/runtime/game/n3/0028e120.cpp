// 0x0028e120 (a0 table, a1 float address, f12 input x): ScaleByCurveSegment. The table
// holds (x, y) float pairs, 8 bytes apiece. x is clamped to [x0, x1]; the segment is 0 when
// x0 <= x < x1 and 1 otherwise; the value is the line through that segment's two pairs at
// the clamped x, y_s + t * (y_s+1 - y_s) with t = (x - x_s) / (x_s+1 - x_s), or y_s when the
// segment has zero width. The float at a1 is multiplied in place by that value. Returns
// nothing: f0 and v0 are temporaries here, so they are left alone.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

template <class Context>
n3::Run scale_by_curve_segment(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress table = call.arg(0);
    const GuestAddress target = call.arg(1);
    const std::uint32_t input = call.cpu().read_fpr_bits(12U);

    // Both first abscissae are loaded before the first compare. The second load sits in
    // the delay slot of the bc1f, so it runs whichever way that branch goes.
    const std::uint32_t x0 = m.read_u32_le(table);
    const std::uint32_t x1 = m.read_u32_le(table + 8U);
    // lwc1, c.olt.s, bc1f and the delay-slot lwc1.
    std::uint32_t retired = 4U;
    std::uint32_t clamped = 0U;
    if (fpu::lt(input, x0)) {
        clamped = x0;
        // beq and its delay daddu, then the beq and nop into the segment search.
        retired += 4U;
    } else {
        // c.ole.s, bc1t and its delay nop.
        retired += 3U;
        clamped = fpu::le(input, x1) ? input : x1;
        // One of two move arms (mov.s pair, or beq and mov.s), then daddu, beq and nop.
        retired += 5U;
    }

    // The original's `while (segment == 0)` loop runs its body once. The entry test (beq
    // and nop) always jumps into the body, and the body either leaves the loop at once
    // (segment 0) or sets segment to 1, after which the re-test falls through.
    retired += 2U;
    // sll, addu, lwc1 x0, c.olt.s, bc1t and its nop.
    retired += 6U;
    std::uint32_t segment = 0U;
    if (fpu::lt(clamped, x0)) {
        segment = 1U;
        // addiu, then the re-test (beq and nop).
        retired += 3U;
    } else {
        // lwc1 x1, c.olt.s, bc1t and its nop.
        retired += 4U;
        if (!fpu::lt(clamped, x1)) {
            segment = 1U;
            // addiu, then the re-test (beq and nop).
            retired += 3U;
        }
    }

    // sll, addu, the two segment abscissae, mtc1 (+0 into f3), nop, sub.s, c.eq.s, bc1t
    // and its nop.
    retired += 10U;
    const GuestAddress pair = table + (segment << 3U);
    const std::uint32_t xa = m.read_u32_le(pair);
    const std::uint32_t xb = m.read_u32_le(pair + 8U);
    const std::uint32_t width = fpu::sub(xb, xa);
    if (fpu::eq(0U, width)) {
        // beq, with the lwc1 of y_s in its delay slot.
        const std::uint32_t factor = m.read_u32_le(pair + 4U);
        // lwc1 of the target, mul.s, beq and its delay swc1, then jr and its nop.
        const std::uint32_t current = m.read_u32_le(target);
        m.write_u32_le(target, fpu::mul(current, factor));
        retired += 8U;
    } else {
        // sub.s, nop, div.s, the two y values, adda.s, sub.s, madd.s, then the shared tail.
        const std::uint32_t t = fpu::div(fpu::sub(clamped, xa), width);
        const std::uint32_t ya = m.read_u32_le(pair + 4U);
        const std::uint32_t yb = m.read_u32_le(pair + 12U);
        // ADDA.S: ACC = 0 + y_s. MADD.S: ACC + t * (y_s+1 - y_s).
        const std::uint32_t acc = fpu::add(0U, ya);
        const std::uint32_t factor = fpu::madd(acc, t, fpu::sub(yb, ya));
        // lwc1 of the target, mul.s, beq and its delay swc1, then jr and its nop.
        const std::uint32_t current = m.read_u32_le(target);
        m.write_u32_le(target, fpu::mul(current, factor));
        retired += 14U;
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0028e120{0x0028e120U, 220U, 0x120ffcc007b40e4aULL,
                                                  "ScaleByCurveSegment", 51U, n3::live<&scale_by_curve_segment<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0028e120, scale_by_curve_segment)
