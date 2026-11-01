// 0x00145ef0 (a0 = strip object, a1 = edge index (low 16 bits), a2 = point as an aligned quadword, f12 = margin):
// StripEdgeFraction. The object's road (u32 at +4) gives a pair table (u32 at its +0) with two halfwords per edge
// index: the first (+0) and the second (+2) select 16-byte vertices of the table at u32[0x0064d354]. Equal vertices
// give 0.5. Otherwise the result is (t - margin) / (L - 2 * margin): L is the length of the edge from the first
// vertex to the second, and t is the projection of (point - first vertex) onto that edge. The vectors, the length
// and the projection run on VU0 (VF1..VF4, ACC, Q); the COP1 accumulator and f0/f1 carry the rest. Returns f0.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kVertexTable = 0x0064d354U;  // lui 0x65, lw -11436: 16-byte vertices
constexpr std::uint32_t kHalfBits = 0x3f000000U;    // 0.5f
constexpr std::uint32_t kTwoBits = 0x40000000U;     // 2.0f

template <class Context>
n3::Run strip_edge_fraction(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint32_t margin = cpu.read_fpr_bits(12U);

    const GuestAddress edge = m.read_u32_le(m.read_u32_le(call.arg(0) + 4U)) + ((call.arg(1) & 0xffffU) << 2U);
    const std::uint32_t to_index = m.read_u16_le(edge + 2U);
    const std::uint32_t from_index = m.read_u16_le(edge);
    if (from_index == to_index) {
        cpu.write_fpr_bits(0U, kHalfBits);  // lui 0x3f00, mtc1 v0,f0
        return call.finish(17U);
    }

    const GuestAddress entries = m.read_u32_le(kVertexTable);
    const GuestAddress to_vertex = entries + (to_index << 4U);
    const GuestAddress from_vertex = entries + (from_index << 4U);
    const guest::Gpr128 point = n3::load_quad_any(m, call.arg(2));  // lq a2,0(a2)

    // daddu a1,zero,zero clears the low doubleword of a1; qmtc2 a1,vf2 then copies the whole register.
    guest::Gpr128 cleared_a1 = call.arg128(1U);
    cleared_a1.words[0] = 0U;
    cleared_a1.words[1] = 0U;
    cpu.write_vu0_vector(2U, cleared_a1);                                     // qmtc2 a1,vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved_vf1 = cpu.read_vu0_vector(1U);                  // qmfc2 v1,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, to_vertex));                // lqc2 vf1,0(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, from_vertex));              // lqc2 vf3,0(v0)
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, 14U, true);                         // vsub.xyz vf1,vf3,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 4U, 0U, false);                  // vaddx.y vf1,vf0,vf2x
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                      // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved_vf1);                                      // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                              // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);               // vmulq.x vf1,vf1,q
    const std::uint32_t length = cpu.read_vu0_vector(1U).words[0];            // qmfc2 v1,vf1 (x lane)
    const std::uint32_t accumulator = fpu::add(0U, length);                   // adda.s f0,f1 (f0 is +0)
    cpu.set_fpu_accumulator_bits(accumulator);
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, from_vertex));              // lqc2 vf3,0(v0)
    cpu.write_fpr_bits(1U, length);                                           // mtc1 v1,f1
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, to_vertex));                // lqc2 vf2,0(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const std::uint32_t inset = fpu::msub(accumulator, kTwoBits, margin);     // msub.s f0,f0,f12 (f0 = 2.0)
    ee_cop2::vadd_vector(cpu, 4U, 2U, 3U, 14U, true);                         // vsub.xyz vf4,vf2,vf3
    ee_cop2::vmul(cpu, 2U, 4U, 4U, 14U);                                      // vmul.xyz vf2,vf4,vf4
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                 // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 4U, 4U, 14U, ee_cop2::kControlQ);              // vmulq.xyz vf4,vf4,q
    const guest::Gpr128 unit = cpu.read_vu0_vector(4U);                       // qmfc2 v0,vf4
    cpu.write_vu0_vector(1U, point);                                          // qmtc2 a2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, 14U, true);                         // vsub.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(2U, unit);                                           // qmtc2 v0,vf2
    ee_cop2::vmul(cpu, 2U, 1U, 2U, 14U);                                      // vmul.xyz vf2,vf1,vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    cpu.write_gpr_sign_extended_32(2U, cpu.read_fpr_bits(12U));               // mfc1 v0,f12
    cpu.write_vu0_vector(2U, cpu.read_gpr(2U));                               // qmtc2 v0,vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, 8U, 0U, true);                   // vsubx.x vf1,vf1,vf2x
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(2U));                             // mtc1 v0,f1
    cpu.write_fpr_bits(0U, fpu::div(cpu.read_fpr_bits(1U), inset));          // div.s f0,f1,f0
    return call.finish(80U);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00145ef0, 336U, 0x3a8b2c8cdf620db7ULL, "StripEdgeFraction", 80U, strip_edge_fraction, EngineReturn::Float)
