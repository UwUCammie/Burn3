// 0x002f5d40 (a0 = output quad, a1 = pointer to the pair table's address, a2 = segment (low 16 bits), a3 = pair
// offset, f12 = t): RoadLinkEntryPoint. Two halfwords are read from the pair table at 4 * segment + 2 * a3, the second
// one 4 bytes on. Each selects a 16-byte entry of the table whose address is the u32 at 0x0064d354: P0 for the
// first, P1 for the second.
// The result is P0 + t * (P1 - P0) over xyz, run on VU0 with the listing's vsub.xyz, vmulx.xyz and vadd.xyz, and
// stored at a0 with sqc2. Its w lane is bits 127..96 of $v1 as the caller left it: mfc1 moves t into the low
// doubleword and keeps the upper one, and qmtc2 copies the whole register into VF1. The original's two stores to its
// own frame (below the entry $sp) are not reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kEntryTable = 0x0064d354U;  // lui 0x65, lw -11436($v1): holds the address of the 16-byte entries
constexpr std::uint32_t kRetired = 31U;            // 0x002f5d40..0x002f5db8: one path, the final jr and its delay slot included

template <class Context>
n3::Run road_link_entry_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lw t0,0(a1); andi v1,a2,0xffff; sll a2,v1,2; sll t1,a3,1; addu, addu: the pair, then its two halfwords.
    const GuestAddress pair_table = m.read_u32_le(call.arg(1));
    const std::uint32_t segment = call.arg(2) & 0xffffU;
    const GuestAddress pair = pair_table + (segment << 2U) + (call.arg(3) << 1U);
    const std::uint32_t first = m.read_u16_le(pair);            // lhu a2,0(v1)
    const std::uint32_t second = m.read_u16_le(pair + 4U);      // lhu v1,4(v1)

    // lw a3,-11436(v1) with v1 = 0x650000, then sll 4 / addu / lq for each entry.
    const GuestAddress table = m.read_u32_le(kEntryTable);
    const guest::Gpr128 p0 = n3::load_quad_any(m, table + (first << 4U));    // lq a2,0(a2)
    const guest::Gpr128 p1 = n3::load_quad_any(m, table + (second << 4U));   // lq v1,0(v1)

    // mfc1 v1,f12 then qmtc2 v1,vf1: VF1.x is t. mfc1 writes the low doubleword (sign-extended) and keeps the
    // upper one, which qmtc2 then copies into VF1.z and VF1.w.
    const std::uint32_t t = cpu.read_fpr_bits(12U);
    guest::Gpr128 scalar = cpu.read_gpr(3U);
    scalar.words[0] = t;
    scalar.words[1] = (t & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(1U, scalar);                           // qmtc2 v1,vf1
    cpu.write_vu0_vector(2U, p0);                               // qmtc2 a2,vf2
    cpu.write_vu0_vector(3U, p1);                               // qmtc2 v1,vf3
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, 14U, true);           // vsub.xyz vf3,vf3,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, 14U, 0U);          // vmulx.xyz vf1,vf3,vf1x
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);          // vadd.xyz vf1,vf2,vf1
    n3::store_quad_any(m, call.arg(0), cpu.read_vu0_vector(1U)); // sqc2 vf1,0(a0)
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002f5d40, 124U, 0x3ec70e9f2e9f517bULL, "RoadLinkEntryPoint", 31U, road_link_entry_point,
               EngineReturn::Nothing)
