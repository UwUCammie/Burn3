// 0x00147c10 (a0 = object, a1 = edge index in the low 16 bits): StripEdgeLength, the horizontal length of edge a1
// of the strip of object a0. The object's pointer at +4 gives the strip's edge table (u32 at +0 points at it): two
// u16 vertex indices per 4-byte edge, from at +0 and to at +2. The vertices are 16-byte entries at u32[0x0064d354].
// The length is the VU0 sequence below: daddu clears a2's low doubleword, so VF3.x is zero and vaddx.y clears the y
// lane; the squared length is x*x + z*z, and VSQRT of it is returned in f0. The original writes no memory.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kVertices = 0x0064d354U;   // lui 0x65, lw -11436: 16-byte vertex entries
constexpr std::uint32_t kRetired = 39U;           // 0x00147c10..0x00147ca8: one path, no loops

template <class Context>
n3::Run strip_edge_length(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lw v1,4(a0); lw v0,0(v1): the edge table. andi v0,a1,0xffff; sll a1,v0,2: the edge's byte offset.
    const GuestAddress strip = m.read_u32_le(call.arg(0) + 4U);
    const GuestAddress pairs = m.read_u32_le(strip);
    const std::uint32_t edge_offset = (call.arg(1) & 0xffffU) << 2U;
    const GuestAddress edge = pairs + edge_offset;
    // lhu v0,0(v0) and lhu v1,2(v0): the edge's vertex indices. lw a0 gives the vertex table.
    const std::uint32_t from_index = m.read_u16_le(edge);
    const std::uint32_t to_index = m.read_u16_le(edge + 2U);
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const GuestAddress from_address = vertices + (from_index << 4U);
    const GuestAddress to_address = vertices + (to_index << 4U);

    // daddu a2,zero,zero clears only the low doubleword of a2; qmtc2 a2,vf3 then copies the whole register,
    // so VF3's high doubleword is a2's own value from entry.
    guest::Gpr128 cleared_a2 = call.arg128(2U);
    cleared_a2.words[0] = 0U;
    cleared_a2.words[1] = 0U;

    // The VU0 sequence of the listing. Each helper is the one the interpreter uses for that instruction.
    cpu.write_vu0_vector(3U, cleared_a2);                                        // qmtc2 a2,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                         // qmfc2 a2,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, to_address));                  // lqc2 vf1,0(v1)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, from_address));                // lqc2 vf2,0(v0)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                            // vsub.xyz vf1,vf2,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 3U, 4U, 0U, false);                     // vaddx.y vf1,vf0,vf3x
    ee_cop2::vmul(cpu, 1U, 1U, 1U, 14U);                                         // vmul.xyz vf1,vf1,vf1
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));                           // qmfc2 v0,vf1; qmtc2 v0,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                               // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved);                                             // qmtc2 a2,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                 // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                  // vmulq.x vf1,vf1,q
    const guest::Gpr128 result = cpu.read_vu0_vector(1U);                        // qmfc2 v0,vf1

    // The registers the listing leaves behind: v1 = the to-vertex's address, a0 = the vertex table, a1 = the
    // edge's byte offset, a2 = the vf1 snapshot, v0 = the result (mtc1 v0,f0 reads its low word).
    cpu.write_gpr_sign_extended_32(3U, to_address);
    cpu.write_gpr_sign_extended_32(4U, vertices);
    cpu.write_gpr_sign_extended_32(5U, edge_offset);
    cpu.write_gpr(6U, saved);
    cpu.write_gpr(2U, result);
    cpu.write_fpr_bits(0U, result.words[0]);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00147c10, 156U, 0xe804eb4eede70c47ULL, "StripEdgeLength", 39U, strip_edge_length, EngineReturn::Float)
