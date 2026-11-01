// 0x00256fd0 (a0 = particle list L, a1 = vector V, a2 = vector W, a3 = quad Q, t0 = quad T, f12 = scale S):
// AppendFacingParticle, a 176-byte VU0 leaf that returns nothing.
//
// The dot product d = V.xyz . W.xyz is formed on the VU: VF2 = W * V (xyz), ACC.x = VF2.x + VF2.y, VF1.x =
// ACC.x + 1.0 * VF2.z. d stays in f1. When d <= 0 (c.ole.s, then bc1t) nothing is written. When the write pointer
// L+44 equals the end pointer L+48 the list is full and nothing is written either. Otherwise one 32-byte particle
// is written at the write pointer B (a2), in the listing's order:
//   B+0..B+15    quad Q, its fourth word then overwritten by 400.0 (swc1 f0 at B+12)
//   B+16..B+31   VF1 = (T.xyz * d * 64, T.w), its fourth word then overwritten by S (swc1 f12 at B+28)
// and L+44 advances by 32. The overlapping stores run in the listing's order, so the later word wins.
// Retired counts, delay slots included: 21 (d <= 0), 29 (list full), 44 (particle written); declared cost 44.
// The VU0 registers, the MAC/STATUS flags and the temporaries (a1..a3, v1, f0, f1) end as the listing leaves them:
// the same primitives run in the same order, with the interpreter's sign-extending and whole-register moves.
// The interpreter faults on an unaligned or unmapped lqc2/sqc2. A PLAIN body cannot decline, so these quads drop
// the low four address bits like lq/sq, and that fault is not reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

// Register numbers as the listing names them.
constexpr unsigned kF0 = 0U;
constexpr unsigned kF1 = 1U;
constexpr unsigned kF12 = 12U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;

// The VU0 destination masks (x is the high bit).
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

// The particle list at a0: the write pointer and the end pointer, and the particle size.
constexpr GuestAddress kWrite = 44U;
constexpr GuestAddress kEnd = 48U;
constexpr std::uint32_t kParticleBytes = 32U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kDotCost = 19U;      // 0x00256fd0..0x00257018: the dot product and the facing test
constexpr std::uint32_t kFullTestCost = 8U;  // 0x0025701c..0x00257038: T scaled by d, the pointers, the full test
constexpr std::uint32_t kWriteCost = 15U;    // 0x0025703c..0x00257074: the particle and the pointer
constexpr std::uint32_t kReturnCost = 2U;    // 0x00257078..0x0025707c: jr ra and its delay slot
constexpr std::uint32_t kNotFacingCost = kDotCost + kReturnCost;                       // 21
constexpr std::uint32_t kFullCost = kDotCost + kFullTestCost + kReturnCost;            // 29
constexpr std::uint32_t kWrittenCost = kDotCost + kFullTestCost + kWriteCost + kReturnCost;  // 44

// qmtc2 / qmfc2 copy the whole 128-bit register, as the interpreter does.
void qmtc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) {
    cpu.write_vu0_vector(vf, cpu.read_gpr(gpr));
}
void qmfc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) {
    cpu.write_gpr(gpr, cpu.read_vu0_vector(vf));
}

// lqc2/sqc2 and lq/sq: quads drop the low four address bits (see the note above).
template <class Memory>
void load_vu(guest::R5900State& cpu, const Memory& m, const unsigned vf, const GuestAddress address) {
    cpu.write_vu0_vector(vf, n3::load_quad_any(m, address));
}
template <class Memory>
void store_vu(guest::R5900State& cpu, Memory& m, const unsigned vf, const GuestAddress address) {
    n3::store_quad_any(m, address, cpu.read_vu0_vector(vf));
}
template <class Memory>
void load_gpr_quad(guest::R5900State& cpu, const Memory& m, const unsigned gpr, const GuestAddress address) {
    cpu.write_gpr(gpr, n3::load_quad_any(m, address));
}
template <class Memory>
void store_gpr_quad(guest::R5900State& cpu, Memory& m, const unsigned gpr, const GuestAddress address) {
    n3::store_quad_any(m, address, cpu.read_gpr(gpr));
}
// lw: the low 64 bits of the register, sign-extended.
template <class Memory>
void load_word(guest::R5900State& cpu, const Memory& m, const unsigned gpr, const GuestAddress address) {
    cpu.write_gpr_sign_extended_32(gpr, m.read_u32_le(address));
}
template <class Memory>
void store_float(guest::R5900State& cpu, Memory& m, const unsigned ft, const GuestAddress address) {
    m.write_u32_le(address, cpu.read_fpr_bits(ft));
}

template <class Context>
n3::Run append_facing_particle(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    // The argument registers as they are on entry: the listing overwrites a1..a3 before some of them are read again.
    const GuestAddress list = call.arg(0U);
    const GuestAddress vector_v = call.arg(1U);
    const GuestAddress vector_w = call.arg(2U);
    const GuestAddress quad_q = call.arg(3U);
    const GuestAddress quad_t = call.arg(4U);

    // 0x00256fd0..0x00257018: VF2 = W * V, ACC.x = VF2.x + VF2.y, VF1.x = ACC.x + VF2.z = d; f1 = d, f0 = 0.
    load_vu(cpu, m, 2U, vector_v);                                                       // lqc2 vf2,0(a1)
    load_vu(cpu, m, 3U, vector_w);                                                       // lqc2 vf3,0(a2)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                             // vaddw.x vf1,vf0,vf0w
    load_gpr_quad(cpu, m, kA3, quad_q);                                                  // lq a3,0(a3)
    load_gpr_quad(cpu, m, kV1, quad_t);                                                  // lq v1,0(t0)
    ee_cop2::vmul(cpu, 2U, 3U, 2U, kXyz);                                                // vmul.xyz vf2,vf3,vf2
    qmfc2(cpu, kA2, 2U);                                                                 // qmfc2 a2,vf2
    qmfc2(cpu, kA1, 1U);                                                                 // qmfc2 a1,vf1
    qmtc2(cpu, kA2, 2U);                                                                 // qmtc2 a2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                                       // vadday.x acc,vf2,vf2y
    qmtc2(cpu, kA1, 1U);                                                                 // qmtc2 a1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);              // vmaddz.x vf1,vf1,vf2z
    qmfc2(cpu, kA1, 1U);                                                                 // qmfc2 a1,vf1
    cpu.write_fpr_bits(kF1, cpu.read_gpr_u32(kA1));                                      // mtc1 a1,f1
    cpu.write_fpr_bits(kF0, 0U);                                                         // mtc1 zero,f0
    ee_cop1::compare_s(cpu, ee_cop1::Compare::LessEqual, kF1, kF0);                      // c.ole.s f1,f0
    if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {                           // bc1t: d <= 0, nothing added
        return call.finish(kNotFacingCost);
    }

    // 0x0025701c..0x00257038: VF1 = T.xyz * d (VF2.x), then the write pointer (a2) and the end pointer (v1).
    cpu.write_gpr_sign_extended_32(kA1, cpu.read_fpr_bits(kF1));                         // mfc1 a1,f1
    qmtc2(cpu, kV1, 1U);                                                                 // qmtc2 v1,vf1
    qmtc2(cpu, kA1, 2U);                                                                 // qmtc2 a1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kXyz, 0U);                                  // vmulx.xyz vf1,vf1,vf2x
    load_word(cpu, m, kA2, list + kWrite);                                               // lw a2,44(a0)
    load_word(cpu, m, kV1, list + kEnd);                                                 // lw v1,48(a0)
    qmfc2(cpu, kA1, 1U);                                                                 // qmfc2 a1,vf1 (beq's delay slot)
    if (cpu.read_gpr_u64(kA2) == cpu.read_gpr_u64(kV1)) {                                // beq a2,v1: the list is full
        return call.finish(kFullCost);
    }

    // 0x0025703c..0x00257074: VF1.xyz *= 64 (VF2.x), then the particle at B = a2 and the write pointer += 32.
    qmtc2(cpu, kA1, 1U);                                                                 // qmtc2 a1,vf1
    cpu.write_gpr_sign_extended_32(kV1, 0x43c80000U);                                    // lui v1,0x43c8 (400.0)
    cpu.write_fpr_bits(kF0, cpu.read_gpr_u32(kV1));                                      // mtc1 v1,f0
    cpu.write_gpr_sign_extended_32(kA1, 0x42800000U);                                    // lui a1,0x4280 (64.0)
    qmtc2(cpu, kA1, 2U);                                                                 // qmtc2 a1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kXyz, 0U);                                  // vmulx.xyz vf1,vf1,vf2x
    qmfc2(cpu, kV1, 1U);                                                                 // qmfc2 v1,vf1
    const GuestAddress particle = cpu.read_gpr_u32(kA2);
    store_gpr_quad(cpu, m, kA3, particle);                                               // sq a3,0(a2)
    store_float(cpu, m, kF0, particle + 12U);                                            // swc1 f0,12(a2)
    qmtc2(cpu, kV1, 1U);                                                                 // qmtc2 v1,vf1
    store_vu(cpu, m, 1U, particle + 16U);                                                // sqc2 vf1,16(a2)
    store_float(cpu, m, kF12, particle + 28U);                                           // swc1 f12,28(a2)
    load_word(cpu, m, kV1, list + kWrite);                                               // lw v1,44(a0)
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kV1) + kParticleBytes);         // addiu v1,v1,32
    m.write_u32_le(list + kWrite, cpu.read_gpr_u32(kV1));                                // sw v1,44(a0)
    return call.finish(kWrittenCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00256fd0, 176U, 0x0cf04d6d33f29877ULL, "AppendFacingParticle", 44U, append_facing_particle,
               EngineReturn::Nothing)
