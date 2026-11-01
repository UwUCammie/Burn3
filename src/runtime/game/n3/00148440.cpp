// 0x00148440 (a0 = strip object, a1 = the point as a quadword, a2 = quad index in its low 16 bits):
// ClassifyPointInStripQuad. The quad's four corner indices P0..P3 are the halfwords at +0, +2, +4 and +6 of entry
// (a2 & 0xffff) * 4 in the table at u32[u32[a0 + 4]]. The corners are 16-byte vectors in the table at u32[0x0064d354],
// and the vector at 0x004c9cf0 (the up vector) is the axis the quad is seen along. The result (v0) is the set of the
// quad's edges the point lies outside of: bit 2 for the P2-P3 end (tested only when P2 != P3), bit 0 for the P0-P2
// side, bit 1 for the P1-P3 side and bit 3 for the P0-P1 start (tested only when P0 != P1). Each test is the VU0
// dot product of the point less the edge's base corner with a cross product of the edge and the up vector, computed
// by the listing's own VU0 sequence; the bit is set when that product is above zero (c.ole.s fails). The point is
// spilled to 192(sp) and reloaded for the tests, as the original does (the spill is in the callee's own frame).
// f1 and f0 end as the last test leaves them. The VU0 registers, the accumulator and the MAC flags follow the
// original's sequence, and the scratch GPRs (a0-a3, t0-t3, v1) are not reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kStripQuads = 4U;           // lw 4(object), then lw 0(that): the quad index table
constexpr GuestAddress kVertices = 0x0064d354U;    // lui 0x65, lw -11436: pointer to the 16-byte vertices
constexpr GuestAddress kUp = 0x004c9cf0U;          // lui 0x4d, lq / lqc2 -25360: the up vector
constexpr std::uint32_t kFrameBytes = 208U;        // addiu $sp,$sp,-208
constexpr std::uint32_t kSpillOffset = 192U;       // sq $t3,192($sp), reloaded by lqc2 vf1,192($sp)
constexpr std::uint8_t kMaskX = 8U;                // the x lane alone (lane_selected: 8 >> lane)

// Retired instruction counts of the blocks of the listing, delay slots included. A path adds the blocks it runs.
constexpr std::uint32_t kHeadCost = 14U;           // 0x148440..0x148474: the reads, the spill, beq P2 == P3
constexpr std::uint32_t kEndTestCost = 31U;        // 0x148478..0x1484f0: the P2-P3 test, bc1t
constexpr std::uint32_t kEndSetCost = 2U;          // 0x1484f4..0x1484f8: bit 2, then beq to the next test
constexpr std::uint32_t kLeftTestCost = 34U;       // 0x1485f4..0x148678: the P0-P2 test, bc1t
constexpr std::uint32_t kLeftSetCost = 2U;         // 0x14867c..0x148680: bit 0, then beq to the next test
constexpr std::uint32_t kRightTestCost = 29U;      // 0x148500..0x148570: the P1-P3 test, bc1t
constexpr std::uint32_t kRightSetCost = 1U;        // 0x148574: bit 1
constexpr std::uint32_t kStartSkipCost = 4U;       // 0x148578..0x148584: beq P1 == P0 and its nop
constexpr std::uint32_t kStartTestCost = 24U;      // 0x148588..0x1485e4: the P0-P1 test, bc1t
constexpr std::uint32_t kStartSetCost = 3U;        // 0x1485e8..0x1485f0: bit 3, beq to the return, addiu $sp
constexpr std::uint32_t kReturnFrameCost = 1U;     // 0x148684: addiu $sp, on the branches into the return
constexpr std::uint32_t kReturnCost = 2U;          // 0x148688..0x14868c: jr $ra and its nop

// mtc1 of the x lane into f1, mtc1 $zero into f0, then c.ole.s f1,f0: true when the product is at most zero.
bool product_at_most_zero(guest::R5900State& cpu) {
    const std::uint32_t product = cpu.read_vu0_vector(1U).words[0];
    cpu.write_fpr_bits(1U, product);
    cpu.write_fpr_bits(0U, 0U);
    return fpu::le(product, 0U);
}

template <class Context>
n3::Run classify_point_in_strip_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lq $t3,0($a1); lw 4($a0); lw 0($v1); the four corner halfwords; then the spill (sq $t3,192($sp)). Every read
    // comes before the spill, as in the original; the frame is open, so 192($sp) is the entry stack pointer - 16.
    const Gpr128 point = n3::load_quad_any(m, call.arg(1));
    const GuestAddress corners = m.read_u32_le(m.read_u32_le(call.arg(0) + kStripQuads)) + ((call.arg(2) & 0xffffU) << 2U);
    const std::uint32_t p0 = m.read_u16_le(corners + 0U);
    const std::uint32_t p1 = m.read_u16_le(corners + 2U);
    const std::uint32_t p2 = m.read_u16_le(corners + 4U);
    const std::uint32_t p3 = m.read_u16_le(corners + 6U);
    const GuestAddress spill = call.stack_pointer() - kFrameBytes + kSpillOffset;
    n3::store_quad_any(m, spill, point);

    // The vertex table pointer and the up vector are read after the spill, in the listing's blocks.
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const Gpr128 up = n3::load_quad_any(m, kUp);

    std::uint32_t flags = 0U;
    std::uint32_t retired = kHeadCost;

    if (p2 != p3) {
        // 0x148478..0x1484f0 (bit 2): base P2 in vf3, edge P2 -> P3 in vf2, the cross product negated into vf2.
        cpu.write_vu0_vector(1U, up);                                                    // lqc2 vf1,-25360($a1)
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertices + (p2 << 4U)));           // lqc2 vf3,0($a3)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertices + (p3 << 4U)));           // lqc2 vf2,0($a1)
        ee_cop2::vadd_vector(cpu, 2U, 2U, 3U, n3::kMaskXyz, true);                       // vsub.xyz vf2,vf2,vf3
        ee_cop2::vopmula(cpu, 2U, 1U);                                                   // vopmula.xyz acc,vf2,vf1
        ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                                               // vopmsub.xyz vf1,vf1,vf2
        ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, true);                       // vsub.xyz vf2,vf0,vf1
        cpu.write_vu0_vector(1U, point);                                                 // qmtc2 $t3,vf1
        ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, true);                       // vsub.xyz vf1,vf1,vf3
        ee_cop2::vmul(cpu, 2U, 1U, 2U, n3::kMaskXyz);                                    // vmul.xyz vf2,vf1,vf2
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                               // vadday.x acc,vf2,vf2y
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
        retired += kEndTestCost;
        if (!product_at_most_zero(cpu)) {
            flags |= 4U;
            retired += kEndSetCost;
        }
    }

    // 0x1485f4..0x148678 (bit 0): base P0 in vf3, edge P0 -> P2 with the cross product negated into vf2. The
    // point is seeded through vf4, whose x lane is set here and whose other lanes keep their old values.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertices + (p0 << 4U)));               // lqc2 vf3,0($a3)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertices + (p2 << 4U)));               // lqc2 vf2,0($a2)
    cpu.write_vu0_vector(1U, up);                                                        // qmtc2 $a1,vf1
    ee_cop2::vadd_vector(cpu, 2U, 2U, 3U, n3::kMaskXyz, true);                           // vsub.xyz vf2,vf2,vf3
    ee_cop2::vopmula(cpu, 2U, 1U);                                                       // vopmula.xyz acc,vf2,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                                                   // vopmsub.xyz vf1,vf1,vf2
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, true);                           // vsub.xyz vf2,vf0,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, spill));                               // lqc2 vf1,192($sp)
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, kMaskX, 3U, false);                         // vaddw.x vf4,vf0,vf0w
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, true);                           // vsub.xyz vf1,vf1,vf3
    ee_cop2::vmul(cpu, 2U, 1U, 2U, n3::kMaskXyz);                                        // vmul.xyz vf2,vf1,vf2
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(4U));                                   // qmfc2 $a2,vf4; qmtc2 $a2,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                                   // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);          // vmaddz.x vf1,vf1,vf2z
    retired += kLeftTestCost;
    if (!product_at_most_zero(cpu)) {
        flags |= 1U;
        retired += kLeftSetCost;
    }

    // 0x148500..0x148570 (bit 1): base P1 in vf2, edge P1 -> P3 in vf3, the cross product kept (not negated) in vf4.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertices + (p3 << 4U)));               // lqc2 vf3,0($a2)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertices + (p1 << 4U)));               // lqc2 vf2,0($a3)
    cpu.write_vu0_vector(1U, up);                                                        // qmtc2 $a1,vf1
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, n3::kMaskXyz, true);                           // vsub.xyz vf3,vf3,vf2
    ee_cop2::vopmula(cpu, 3U, 1U);                                                       // vopmula.xyz acc,vf3,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 3U);                                                   // vopmsub.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(4U, cpu.read_vu0_vector(1U));                                   // qmfc2 $a2,vf1; qmtc2 $a2,vf4
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, spill));                               // lqc2 vf1,192($sp)
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, kMaskX, 3U, false);                         // vaddw.x vf3,vf0,vf0w
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, true);                           // vsub.xyz vf1,vf1,vf2
    const Gpr128 right_seed = cpu.read_vu0_vector(3U);                                   // qmfc2 $a2,vf3
    ee_cop2::vmul(cpu, 2U, 1U, 4U, n3::kMaskXyz);                                        // vmul.xyz vf2,vf1,vf4
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                                   // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, right_seed);                                                // qmtc2 $a2,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);          // vmaddz.x vf1,vf1,vf2z
    retired += kRightTestCost;
    if (!product_at_most_zero(cpu)) {
        flags |= 2U;
        retired += kRightSetCost;
    }

    // 0x148578..0x148584: a start edge exists only when P1 != P0.
    retired += kStartSkipCost;
    if (p1 == p0) {
        retired += kReturnFrameCost + kReturnCost;
        call.return_value(flags);
        return call.finish(retired);
    }

    // 0x148588..0x1485e4 (bit 3): base P0 in vf2, edge P0 -> P1 in vf3, the cross product kept in vf4.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertices + (p1 << 4U)));               // lqc2 vf3,0($a3)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertices + (p0 << 4U)));               // lqc2 vf2,0($v1)
    cpu.write_vu0_vector(1U, up);                                                        // qmtc2 $a1,vf1
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, n3::kMaskXyz, true);                           // vsub.xyz vf3,vf3,vf2
    ee_cop2::vopmula(cpu, 3U, 1U);                                                       // vopmula.xyz acc,vf3,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 3U);                                                   // vopmsub.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(4U, cpu.read_vu0_vector(1U));                                   // qmfc2 $v1,vf1; qmtc2 $v1,vf4
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, spill));                               // lqc2 vf1,192($sp)
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, kMaskX, 3U, false);                         // vaddw.x vf3,vf0,vf0w
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, true);                           // vsub.xyz vf1,vf1,vf2
    const Gpr128 start_seed = cpu.read_vu0_vector(3U);                                   // qmfc2 $v1,vf3
    ee_cop2::vmul(cpu, 2U, 1U, 4U, n3::kMaskXyz);                                        // vmul.xyz vf2,vf1,vf4
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                                   // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, start_seed);                                                // qmtc2 $v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);          // vmaddz.x vf1,vf1,vf2z
    retired += kStartTestCost;
    if (product_at_most_zero(cpu)) {
        retired += kReturnFrameCost + kReturnCost;
    } else {
        flags |= 8U;
        retired += kStartSetCost + kReturnCost;
    }
    call.return_value(flags);
    return call.finish(retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY_PLAIN(00148440, 592U, 0x5b33c143df96797eULL, "ClassifyPointInStripQuad", 146U, classify_point_in_strip_quad,
               EngineReturn::Integer)
