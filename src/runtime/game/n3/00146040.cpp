// 0x00146040 (a0 = strip object, a1 = quad index in its low halfword, a2 = point, 16-byte aligned): StripQuadProgress.
// Returns f0. The object's strip (u32 at +4) gives the index table (u32 at the strip's +0: halfwords, corner 0 of quad s
// at halfword 2s) and the record table (u32 at the strip's +4: 8-byte records with a float at +0). The length f20 is
// record s less record s-1, or record 0 alone when s is 0. StripQuadStep (0x00147d00) gives the direction of quad s,
// which is normalised. f0 = min(((point - vertex) . normalised) / f20, 1.0), where the vertex is the 16-byte entry at
// u32 0x0064d354 selected by corner 0 of quad s.
//
// The listing's stores are all to its own frame (the saved registers, the point, the normalised quad) and are not
// reproduced: the check ignores stack writes, and the normalised quad is carried in a register. The callee's quad at
// sp+80 is read back from the frame where the listing reads it. The temporaries the listing leaves behind (v0, v1, a0,
// a1, f1) are written as well. Retired counts: 70 when s is 0, 71 otherwise, plus the callee's.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00147d00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kStripOffset = 4U;          // object+4: the strip pointer (word)
constexpr GuestAddress kRecordsOffset = 4U;        // strip+4: the record table pointer (word)
constexpr GuestAddress kVertices = 0x0064d354U;    // lui 0x65, lw -11436: 16-byte vertex entries (word)
constexpr std::uint32_t kRecordBytes = 8U;         // a float at +0 of each record
constexpr std::uint32_t kFrameBytes = 112U;        // addiu sp, sp, -112
constexpr std::uint32_t kOutputOffset = 80U;       // the callee's quad at sp+80 (the jal's a2)
constexpr std::uint32_t kOneBits = 0x3f800000U;    // 1.0f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 10U;       // 0x146040..0x146064: frame, point, the blez and its delay slot
constexpr std::uint32_t kZeroCost = 6U;            // 0x146144..0x146158: index 0 (length = record 0), jump back
constexpr std::uint32_t kStepCost = 7U;            // 0x146068..0x146080: index > 0 (record s less record s-1)
constexpr std::uint32_t kJoinCost = 48U;           // 0x146084..0x146140: jal and delay slot, VU0, the quotient, min.s
constexpr std::uint32_t kEpilogueCost = 6U;        // 0x14615c..0x146170: reloads, jr ra and its delay slot

// The loads the listing makes, checked the way the interpreter checks them: lw and lwc1 need 4-byte alignment, lhu
// 2-byte, lqc2 16-byte, and each must lie in guest memory. A load that would fault declines the call, so the original
// runs and faults itself. (lq ignores the low four address bits and faults only outside guest memory.)
template <class Memory>
bool word_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 3U) == 0U && memory.contains(address, 4U);
}
template <class Memory>
bool half_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 1U) == 0U && memory.contains(address, 2U);
}
template <class Memory>
bool quad_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}
template <class Memory>
bool quad_maskable(const Memory& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}

template <class Context>
n3::Run strip_quad_progress(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t index = call.arg(1) & 0xffffU;     // andi s0, a1, 0xffff
    const GuestAddress point_address = call.arg(2);

    // lq v0, 0(a2): the point, kept for the VU0 sequence that reloads it from the frame.
    if (!quad_maskable(m, point_address)) {
        return call.decline();
    }
    const Gpr128 point = n3::load_quad_any(m, point_address);

    // Prologue: f20 is the length of quad s's segment, from the strip's record table.
    if (!word_loadable(m, object + kStripOffset)) {
        return call.decline();
    }
    const GuestAddress strip = m.read_u32_le(object + kStripOffset);
    if (!word_loadable(m, strip + kRecordsOffset)) {
        return call.decline();
    }
    const GuestAddress records = m.read_u32_le(strip + kRecordsOffset);
    const GuestAddress record = records + index * kRecordBytes;
    std::uint32_t retired = kPrologueCost;
    std::uint32_t length = 0U;
    if (index == 0U) {
        if (!word_loadable(m, record)) {
            return call.decline();
        }
        length = m.read_u32_le(record);
        retired += kZeroCost;
    } else {
        if (!word_loadable(m, record) || !word_loadable(m, record - kRecordBytes)) {
            return call.decline();
        }
        length = fpu::sub(m.read_u32_le(record), m.read_u32_le(record - kRecordBytes));
        retired += kStepCost;
    }

    // jal StripQuadStep, with a2 = sp+80 in its delay slot. The caller's frame is sp less 112.
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    if (!quad_loadable(m, frame + kOutputOffset)) {
        return call.decline();
    }
    call.set_arg(2, frame + kOutputOffset);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00147d00(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }

    // The listing reads the strip again, then the index table, the vertex table base and corner 0 of quad s.
    const GuestAddress strip_again = m.read_u32_le(object + kStripOffset);
    if (!word_loadable(m, strip_again)) {
        return call.decline();
    }
    const GuestAddress indices = m.read_u32_le(strip_again);
    const GuestAddress corner_address = indices + index * 4U;   // addu v1, v1, a0 with a0 = 4 * s
    if (!half_loadable(m, corner_address) || !word_loadable(m, kVertices)) {
        return call.decline();
    }
    const std::uint32_t corner = m.read_u16_le(corner_address);  // lhu
    const GuestAddress vertex = m.read_u32_le(kVertices) + (corner << 4U);
    if (!quad_loadable(m, vertex)) {
        return call.decline();
    }

    // The VU0 sequence in the listing's order: VF2 is the callee's quad, VF1 gets the squared length, Q its reciprocal
    // root, and the quad is normalised in xyz (vwaitq has nothing to wait for).
    const Gpr128 direction = n3::load_quad_any(m, frame + kOutputOffset);     // lqc2 vf2, 80(sp)
    cpu.write_vu0_vector(2U, direction);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1, vf0, vf0w
    const Gpr128 length_seed = cpu.read_vu0_vector(1U);                       // qmfc2 v0, vf1
    ee_cop2::vmul(cpu, 2U, 2U, 2U, 14U);                                      // vmul.xyz vf2, vf2, vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, length_seed);                                    // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1, vf1, vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                 // vrsqrt q, vf0w, vf1x
    cpu.write_vu0_vector(1U, direction);                                      // lq v0, 80(sp); qmtc2 v0, vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, 14U, ee_cop2::kControlQ);              // vmulq.xyz vf1, vf1, q
    const Gpr128 normalized = cpu.read_vu0_vector(1U);                        // qmfc2 v0, vf1 (stored at sp+80)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1, vf0, vf0w
    const Gpr128 dot_seed = cpu.read_vu0_vector(1U);                          // qmfc2 a1, vf1
    cpu.write_vu0_vector(2U, point);                                          // lqc2 vf2, 96(sp): the point
    cpu.write_vu0_vector(3U, normalized);                                     // lqc2 vf3, 80(sp): normalised quad
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex));                   // lqc2 vf1, 0(v0): the vertex
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                         // vsub.xyz vf1, vf2, vf1
    ee_cop2::vmul(cpu, 2U, 1U, 3U, 14U);                                      // vmul.xyz vf2, vf1, vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, dot_seed);                                       // qmtc2 a1, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1, vf1, vf2z
    const Gpr128 dot_vec = cpu.read_vu0_vector(1U);                           // qmfc2 v0, vf1

    // div.s f1, f1, f20; lui v0, 0x3f80; mtc1 v0, f0; min.s f0, f1, f0
    const std::uint32_t quotient = fpu::div(dot_vec.words[0], length);
    cpu.write_fpr_bits(1U, quotient);
    cpu.write_fpr_bits(0U, kOneBits);
    ee_cop1::min_s(cpu, 0U, 1U, 0U);

    // The temporaries the listing leaves: v0 is the VF1 result with its low doubleword set to 1.0 by the lui, v1 is
    // corner << 4, a0 is 4 * s, and a1 is the VF1 value taken before the second vmaddz.
    Gpr128 v0 = dot_vec;
    v0.words[0] = kOneBits;
    v0.words[1] = 0U;
    cpu.write_gpr(2U, v0);
    cpu.write_gpr_sign_extended_32(3U, corner << 4U);
    cpu.write_gpr_sign_extended_32(4U, index << 2U);
    cpu.write_gpr(5U, dot_seed);
    return call.finish(retired + kJoinCost + callee.retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00146040, 308U, 0xee6a6a176d53875fULL, "StripQuadProgress", kCeiling, strip_quad_progress,
         EngineReturn::Float)
