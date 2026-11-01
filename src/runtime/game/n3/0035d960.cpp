// 0x0035d960 (a0 = context C, a1 = group record R): GroupCentroidExtent.
//
// R holds its position vector at +0 (its w lane is a bound, read at +12), the element count (an int) at +36, a
// pointer at +16 and an intrusive list whose head is R+40. An element's link sits at element+20, so the element base
// is link-20: its position vector at +0, a pointer to a second vector at +16, a float output at +48.
//
// 1. The average of the element positions: the sum of their xyz lanes times 1 / count (the count converted to float).
// 2. For each element, extent = (position w + |average - position|) + the float at C+32. best (f20, from 0.0) is the
//    largest extent so far. An extent not above best is skipped. Otherwise best = extent, and when that extent is also
//    above R's bound the walk stops and R is left alone (the original returns without storing).
// 3. R's vector becomes (average xyz, best). Then each element's output becomes (second vector w - element w) squared.
//
// A non-zero R+16 sends the call to the original: its cold path calls 0x002bf530, which has no native.
// The VU0 sequence runs on the real VU0 state (VF1..VF4, ACC, Q, MAC and STATUS) in the original's order. The
// original's frame is never written; the stale quadword below the entry sp (its w lane reaches VF1) is read from memory.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kRecordTail = 16U;        // R+16: non-zero sends the call to the original
constexpr GuestAddress kRecordCount = 36U;       // R+36: the element count, an int
constexpr GuestAddress kRecordList = 40U;        // R+40: the list head; every element link points back to it
constexpr GuestAddress kContextAddend = 32U;     // C+32: the float added to each extent
constexpr GuestAddress kLinkOffset = 20U;        // element base = link - 20
constexpr GuestAddress kElementSecond = 16U;     // element+16: pointer to the second vector
constexpr GuestAddress kElementOutput = 48U;     // element+48: the float output
constexpr std::uint32_t kFrameBytes = 368U;      // the original's frame: [sp - 368, sp) at the call
constexpr std::uint32_t kStaleQuad = 352U;       // frame+352: the quadword the original reads before writing it
constexpr std::uint32_t kOneBits = 0x3f800000U;  // 1.0f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 21U;        // 0x35d960..0x35d9b0: frame, VU0 setup, the first test's jump
constexpr std::uint32_t kSumTestCost = 2U;       // 0x35d9c8: the sum loop's test and its delay slot
constexpr std::uint32_t kSumBodyCost = 5U;       // 0x35d9b4..0x35d9c4: one element's position added to the sum
constexpr std::uint32_t kAverageCost = 18U;      // 0x35d9d0..0x35da14: the average, the centroid and the first step
constexpr std::uint32_t kExtentTestCost = 2U;    // 0x35daa0..0x35daa4: the extent loop's test
constexpr std::uint32_t kExtentBodyCost = 26U;   // 0x35da18..0x35da7c: one element's extent and its first test
constexpr std::uint32_t kExtentRaiseCost = 6U;   // 0x35da80..0x35da94: an extent above best becomes the best
constexpr std::uint32_t kAdvanceCost = 2U;       // 0x35db18..0x35db1c: the link advance and its jump
constexpr std::uint32_t kExtentStopCost = 8U;    // 0x35da98..0x35da9c and 0x35db98..0x35dbac: early return
constexpr std::uint32_t kTailTestCost = 3U;      // 0x35daa8..0x35dab0: the tail pointer test
constexpr std::uint32_t kStoreCost = 11U;        // 0x35dab4..0x35dadc: the centroid store and the spread loop's start
constexpr std::uint32_t kSpreadTestCost = 2U;    // 0x35db08..0x35db0c: the spread loop's test
constexpr std::uint32_t kSpreadBodyCost = 10U;   // 0x35dae0..0x35db04: one element's output
constexpr std::uint32_t kExitCost = 9U;          // 0x35db10..0x35db14, 0x35db94, 0x35db98..0x35dbac: normal return

// The high word of a 32-bit value written to a 64-bit register (MFC1 and LW sign-extend it).
constexpr std::uint32_t sign_word(const std::uint32_t bits) noexcept {
    return (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Context>
n3::Run group_centroid_extent(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress context_address = call.arg(0U);
    const GuestAddress record = call.arg(1U);
    const GuestAddress head = record + kRecordList;
    const GuestAddress first = m.read_u32_le(head);       // lw t0,40(a1): the first element link
    std::uint32_t retired = kEntryCost;

    // daddu v1,zero,zero clears only the low doubleword, and qmtc2 v1,vf2 copies the whole register.
    Gpr128 cleared = cpu.read_gpr(3U);
    cleared.words[0] = 0U;
    cleared.words[1] = 0U;
    cpu.write_vu0_vector(2U, cleared);
    // lqc2 vf1,352(sp): the stale frame quadword. Three adds with vf2.x = 0 zero its x, y and z.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, call.stack_pointer() - kFrameBytes + kStaleQuad));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 8U, 0U, false);   // vaddx.x vf1,vf0,vf2x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 4U, 0U, false);   // vaddx.y vf1,vf0,vf2x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 2U, 0U, false);   // vaddx.z vf1,vf0,vf2x
    Gpr128 sum = cpu.read_vu0_vector(1U);                      // sqc2 vf1,352(sp)

    // Sum of the element positions (xyz). VF2 carries the running sum, as the original's loop does.
    GuestAddress link = first;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kSumTestCost;
        if (link == head) {
            break;
        }
        retired += kSumBodyCost;
        const GuestAddress next = m.read_u32_le(link);                                  // lw a0,0(a0)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, link - kLinkOffset));             // lqc2 vf1,-20(a0)
        cpu.write_vu0_vector(2U, sum);                                                  // lqc2 vf2,352(sp)
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);                     // vadd.xyz vf2,vf2,vf1
        sum = cpu.read_vu0_vector(2U);                                                  // sqc2 vf2,352(sp)
        link = next;
    }

    // Average = sum * (1 / count). The reciprocal reaches VF3 through mfc1 and qmtc2 (its high doubleword is a0's).
    retired += kAverageCost;
    const std::uint32_t count = fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(record + kRecordCount)));
    const std::uint32_t reciprocal = fpu::div(kOneBits, count);
    const Gpr128 entry_a0 = cpu.read_gpr(4U);
    cpu.write_vu0_vector(3U, Gpr128{{reciprocal, sign_word(reciprocal), entry_a0.words[2], entry_a0.words[3]}});
    cpu.write_vu0_vector(1U, sum);                             // lqc2 vf1,352(sp)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, n3::kMaskXyz, 0U);   // vmulx.xyz vf1,vf1,vf3x
    cpu.write_vu0_vector(4U, cpu.read_vu0_vector(1U));          // sqc2 vf1,80(sp); lqc2 vf4,80(sp)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf2,vf0,vf0w: vf2.x = 1

    // Extent loop. best is f20 (0.0 at entry).
    std::uint32_t best = 0U;
    link = first;
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kExtentTestCost;
        if (link == head) {
            break;
        }
        retired += kExtentBodyCost;
        const GuestAddress base = link - kLinkOffset;                                    // addu a0,t0,a3
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, base));                            // lqc2 vf3,0(a0)
        ee_cop2::vadd_vector(cpu, 1U, 4U, 3U, n3::kMaskXyz, true);                       // vsub.xyz vf1,vf4,vf3
        ee_cop2::vmul(cpu, 1U, 1U, 1U, n3::kMaskXyz);                                    // vmul.xyz vf1,vf1,vf1
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                                   // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 2U, false, false, false);          // vmaddz.x vf1,vf2,vf1z
        ee_cop2::vsqrt(cpu, 1U, 0U);                                                     // vsqrt q,vf1x
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                         // vaddw.x vf1,vf0,vf0w
        ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                      // vmulq.x vf1,vf1,q
        ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, n3::kMaskW, 0U, false);                 // vaddx.w vf3,vf3,vf1x
        // lw a0,32(C) and qmtc2 a0,vf1: the addend becomes x and its sign word y; z and w stay as they are.
        const std::uint32_t addend = m.read_u32_le(context_address + kContextAddend);
        Gpr128 with_addend = cpu.read_vu0_vector(1U);
        with_addend.words[0] = addend;
        with_addend.words[1] = sign_word(addend);
        cpu.write_vu0_vector(1U, with_addend);
        ee_cop2::vadd_broadcast(cpu, 1U, 3U, 1U, n3::kMaskW, 0U, false);                 // vaddx.w vf1,vf3,vf1x
        const std::uint32_t extent = cpu.read_vu0_vector(1U).words[3];                   // lwc1 f1,172(sp)
        if (fpu::le(extent, best)) {                                                     // c.ole.s f1,f20; bc1t
            retired += kAdvanceCost;
        } else {
            retired += kExtentRaiseCost;
            best = extent;                                                               // mov.s f20,f1
            if (!fpu::le(extent, n3::load_quad_any(m, record).words[3])) {               // c.ole.s f1,f0 (R+12)
                return call.finish(retired + kExtentStopCost);
            }
            retired += kAdvanceCost;
        }
        link = m.read_u32_le(link);                                                      // lw t0,0(t0)
    }

    // The cold test (lw v1,16(s1); bne): the original's cold path calls 0x002bf530, which has no native.
    retired += kTailTestCost;
    if (m.read_u32_le(record + kRecordTail) != 0U) {
        return call.decline();
    }

    // Centroid store. VF2 = the average (lqc2 vf2,80(sp)). VF1 = (best, its sign word, a1's high doubleword), from
    // mfc1 a1,f20 and qmtc2 a1,vf1. VF3 = (average xyz, best) is written to R's vector.
    retired += kStoreCost;
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(4U));
    Gpr128 best_vector = cpu.read_gpr(5U);
    best_vector.words[0] = best;
    best_vector.words[1] = sign_word(best);
    cpu.write_vu0_vector(1U, best_vector);
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, n3::kMaskXyz, false);       // vadd.xyz vf3,vf0,vf2
    ee_cop2::vmul_broadcast(cpu, 3U, 0U, 1U, n3::kMaskW, 0U);         // vmulx.w vf3,vf0,vf1x
    n3::store_quad_any(m, record, cpu.read_vu0_vector(3U));           // sqc2 vf3,0(s1)

    // Spread loop: each element's output becomes (second vector w - element w) squared.
    GuestAddress element = m.read_u32_le(head);                       // lw a3,40(s1)
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kSpreadTestCost;
        if (element == head) {
            break;
        }
        retired += kSpreadBodyCost;
        const GuestAddress base = element - kLinkOffset;              // addu a2,a3,s0
        const GuestAddress second = m.read_u32_le(base + kElementSecond);   // lw a1,16(a2)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, base));         // lqc2 vf1,0(a2)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, second));       // lqc2 vf2,0(a1)
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, n3::kMaskW, 3U, true);   // vsubw.w vf1,vf2,vf1w
        const std::uint32_t difference = cpu.read_vu0_vector(1U).words[3];   // lwc1 f0,108(sp): vf1.w
        m.write_u32_le(base + kElementOutput, fpu::mul(difference, difference));   // mul.s f0; swc1 f0,48(a2)
        element = m.read_u32_le(element);                             // lw a3,0(a3)
    }
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0035d960, 592U, 0x83785c225d1bb16aULL, "GroupCentroidExtent", kCeiling, group_centroid_extent,
         EngineReturn::Nothing)
