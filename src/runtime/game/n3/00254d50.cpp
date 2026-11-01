// 0x00254d50 (a0 = object O, a1 = vector V, a2 = quad Q): ScaledClampedVectorsAndDirection. A 448-byte VU0 leaf.
// The record P = u32[O+228] holds the floats s0 = P+80, s1 = P+84, s2 = P+88 and the byte g = P+92.
//   O+48 = min(V * s0, Vmin) and O+64 = min(V * s1, Vmin), both with w = 1.0. Vmin = (c, c, c, 1.0) when g is
//   non-zero (c = the float at gp-30332), otherwise (1, 1, 1, 1).
//   D = (Q.x, Q.y * s2, Q.z) with w = Q.w. When |D| is tiny (zero, or an exponent field of 0x6f or less) D becomes
//   (0, -1, 0). O+80.xyz = D * rsqrt(|D|^2) on the VU, and O+80.w keeps its value.
// The frame (sp-256 .. sp) holds spilled quads and words that the listing writes and reads back; they are
// reproduced in guest memory (the checker ignores the dead frame). v0 and f0 are temporaries: nothing is returned.
// Retired count, delay slots included: 29 + (6 for g == 0, 8 for g != 0) + 40 + (8 when |D| is tiny) + 21,
// i.e. 96, 104, 98 or 106.
// The interpreter faults on an unaligned or unmapped lqc2/sqc2. A PLAIN body cannot decline, so these quads follow
// the lq/sq rule (low four address bits dropped); the original faults only on states the game does not reach.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// Register numbers as the listing names them, and the VU0 destination masks (x is the high bit).
constexpr unsigned kAt = 1U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA1 = 5U;
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kW = n3::kMaskW;
constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kXyzw = n3::kMaskXyzw;

// Offsets in the object O, in the record P that O+228 points at, and the gp-relative float c.
constexpr GuestAddress kObjectPointer = 228U;
constexpr GuestAddress kScaleX = 80U;
constexpr GuestAddress kScaleY = 84U;
constexpr GuestAddress kScaleDirection = 88U;
constexpr GuestAddress kClampFlag = 92U;
constexpr std::int32_t kClampFloatOffset = -30332;
constexpr GuestAddress kClampedA = 48U;
constexpr GuestAddress kClampedB = 64U;
constexpr GuestAddress kDirection = 80U;
constexpr GuestAddress kFrameBytes = 256U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrefixCost = 29U;    // 0x00254d50..0x00254dc0
constexpr std::uint32_t kClampZeroCost = 6U;  // 0x00254de4..0x00254df8: g == 0
constexpr std::uint32_t kClampSetCost = 8U;   // 0x00254dc4..0x00254de0: g != 0, ends in the jump to 0x00254dfc
constexpr std::uint32_t kMiddleCost = 40U;    // 0x00254dfc..0x00254e98
constexpr std::uint32_t kTinyCost = 8U;       // 0x00254e9c..0x00254eb8: |D| is tiny
constexpr std::uint32_t kTailCost = 21U;      // 0x00254ebc..0x00254f0c

// qmtc2 / qmfc2 copy the whole 128-bit register, as the interpreter does.
void qmtc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) {
    cpu.write_vu0_vector(vf, cpu.read_gpr(gpr));
}
void qmfc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) {
    cpu.write_gpr(gpr, cpu.read_vu0_vector(vf));
}

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
template <class Memory>
void load_word(guest::R5900State& cpu, const Memory& m, const unsigned gpr, const GuestAddress address) {
    cpu.write_gpr_sign_extended_32(gpr, m.read_u32_le(address));
}
template <class Memory>
void load_byte(guest::R5900State& cpu, const Memory& m, const unsigned gpr, const GuestAddress address) {
    cpu.write_gpr_u64(gpr, m.read_u8(address));
}
template <class Memory>
void load_float(guest::R5900State& cpu, const Memory& m, const unsigned ft, const GuestAddress address) {
    cpu.write_fpr_bits(ft, m.read_u32_le(address));
}
template <class Memory>
void store_float(guest::R5900State& cpu, Memory& m, const unsigned ft, const GuestAddress address) {
    m.write_u32_le(address, cpu.read_fpr_bits(ft));
}

template <class Context>
n3::Run scaled_clamped_vectors_and_direction(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress vector_addr = call.arg(1U);
    const GuestAddress quad_addr = call.arg(2U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 0x00254d54..0x00254dbc: O+48 = V * s0 and O+64 = V * s1, each with w = 1.0.
    load_vu(cpu, m, 2U, vector_addr);                                                // lqc2 vf2,0(a1)
    load_word(cpu, m, kV1, object + kObjectPointer);                                 // lw v1,228(a0)
    load_gpr_quad(cpu, m, kA1, quad_addr);                                           // lq a1,0(a2)
    load_word(cpu, m, kV1, cpu.read_gpr_u32(kV1) + kScaleX);                         // lw v1,80(v1)
    store_gpr_quad(cpu, m, kA1, frame + 224U);                                       // sq a1,224(sp)
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, kXyzw, 0U);                             // vmulx.xyzw vf1,vf2,vf1x
    store_vu(cpu, m, 1U, object + kClampedA);                                        // sqc2 vf1,48(a0)
    load_word(cpu, m, kV1, object + kObjectPointer);                                 // lw v1,228(a0)
    load_word(cpu, m, kA1, cpu.read_gpr_u32(kV1) + kScaleY);                         // lw a1,84(v1)
    qmtc2(cpu, kA1, 1U);                                                             // qmtc2 a1,vf1
    cpu.write_gpr_sign_extended_32(kV1, 0x3f800000U);                                // lui v1,0x3f80
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f0
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, kXyzw, 0U);                             // vmulx.xyzw vf1,vf2,vf1x
    store_vu(cpu, m, 1U, object + kClampedB);                                        // sqc2 vf1,64(a0)
    load_vu(cpu, m, 1U, object + kClampedA);                                         // lqc2 vf1,48(a0)
    qmtc2(cpu, kV1, 3U);                                                             // qmtc2 v1,vf3
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 3U, kW, 0U);                                // vmulx.w vf1,vf0,vf3x
    store_vu(cpu, m, 1U, object + kClampedA);                                        // sqc2 vf1,48(a0)
    load_vu(cpu, m, 1U, object + kClampedB);                                         // lqc2 vf1,64(a0)
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 3U, kW, 0U);                                // vmulx.w vf1,vf0,vf3x
    store_vu(cpu, m, 1U, object + kClampedB);                                        // sqc2 vf1,64(a0)
    load_word(cpu, m, kV1, object + kObjectPointer);                                 // lw v1,228(a0)
    load_byte(cpu, m, kV1, cpu.read_gpr_u32(kV1) + kClampFlag);                      // lbu v1,92(v1)

    std::uint32_t retired = kPrefixCost;
    if (cpu.read_gpr_u64(kV1) == 0U) {
        // 0x00254de4..0x00254df8 (g == 0): the spill quad is (1.0, 1.0, 1.0, 1.0); f0 is still 1.0 here.
        retired += kClampZeroCost;
        store_float(cpu, m, 0U, frame + 32U);                                        // swc1 f0,32(sp)
        store_float(cpu, m, 0U, frame + 36U);                                        // swc1 f0,36(sp)
        store_float(cpu, m, 0U, frame + 40U);                                        // swc1 f0,40(sp)
        store_float(cpu, m, 0U, frame + 44U);                                        // swc1 f0,44(sp)
        load_gpr_quad(cpu, m, kV1, frame + 32U);                                     // lq v1,32(sp)
        store_gpr_quad(cpu, m, kV1, frame + 208U);                                   // sq v1,208(sp)
    } else {
        // 0x00254dc4..0x00254de0 (g != 0): f0 = c and the spill quad is (c, c, c, 1.0).
        retired += kClampSetCost;
        store_float(cpu, m, 0U, frame + 60U);                                        // swc1 f0,60(sp)
        load_float(cpu, m, 0U, gp + static_cast<GuestAddress>(kClampFloatOffset));   // lwc1 f0,-30332(gp)
        store_float(cpu, m, 0U, frame + 48U);                                        // swc1 f0,48(sp)
        store_float(cpu, m, 0U, frame + 52U);                                        // swc1 f0,52(sp)
        store_float(cpu, m, 0U, frame + 56U);                                        // swc1 f0,56(sp)
        load_gpr_quad(cpu, m, kV1, frame + 48U);                                     // lq v1,48(sp)
        store_gpr_quad(cpu, m, kV1, frame + 208U);                                   // sq v1,208(sp)
    }

    // 0x00254dfc..0x00254e98: the clamp of O+48 and O+64, then D = (Q.x, Q.y * s2, Q.z) and |D|^2 on the VU.
    load_vu(cpu, m, 1U, object + kClampedA);                                         // lqc2 vf1,48(a0)
    load_vu(cpu, m, 3U, frame + 208U);                                               // lqc2 vf3,208(sp)
    load_vu(cpu, m, 2U, frame + 224U);                                               // lqc2 vf2,224(sp)
    ee_cop2::vmini(cpu, 1U, 1U, 3U, kXyzw);                                          // vmini.xyzw vf1,vf1,vf3
    store_vu(cpu, m, 1U, object + kClampedA);                                        // sqc2 vf1,48(a0)
    load_vu(cpu, m, 1U, object + kClampedB);                                         // lqc2 vf1,64(a0)
    ee_cop2::vmini(cpu, 1U, 1U, 3U, kXyzw);                                          // vmini.xyzw vf1,vf1,vf3
    store_vu(cpu, m, 1U, object + kClampedB);                                        // sqc2 vf1,64(a0)
    load_word(cpu, m, kV1, object + kObjectPointer);                                 // lw v1,228(a0)
    load_word(cpu, m, kA1, cpu.read_gpr_u32(kV1) + kScaleDirection);                 // lw a1,88(v1)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                         // vaddw.x vf1,vf0,vf0w
    load_vu(cpu, m, 3U, frame + 224U);                                               // lqc2 vf3,224(sp)
    qmfc2(cpu, kV1, 1U);                                                             // qmfc2 v1,vf1
    qmtc2(cpu, kA1, 1U);                                                             // qmtc2 a1,vf1
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, kY, 0U);                                // vmulx.y vf1,vf2,vf1x
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kX, 0U, false);                         // vaddx.x vf3,vf0,vf2x
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, kY, 1U, false);                         // vaddy.y vf3,vf0,vf1y
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kZ, 2U, false);                         // vaddz.z vf3,vf0,vf2z
    store_vu(cpu, m, 3U, frame + 192U);                                              // sqc2 vf3,192(sp)
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);                                            // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                                   // vadday.x acc,vf2,vf2y
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);          // vmaddz.x vf1,vf1,vf2z
    qmfc2(cpu, kV1, 1U);                                                             // qmfc2 v1,vf1
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                     // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                         // vaddw.x vf1,vf0,vf0w
    qmfc2(cpu, kA1, 1U);                                                             // qmfc2 a1,vf1
    qmtc2(cpu, kA1, 1U);                                                             // vwaitq (nothing to wait for); qmtc2 a1,vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, kX, ee_cop2::kControlQ);                      // vmulq.x vf1,vf1,q
    qmfc2(cpu, kA1, 1U);                                                             // qmfc2 a1,vf1
    cpu.write_gpr_sign_extended_32(kV1, 0x7f800000U);                                // lui v1,0x7f80
    cpu.write_gpr_sign_extended_32(kAt, 0x37800000U);                                // lui at,0x3780
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kA1) & cpu.read_gpr_u64(kV1));           // and v1,a1,v1
    cpu.write_gpr_u64(kAt, cpu.read_gpr_u64(kAt) | 1U);                              // ori at,at,0x1
    cpu.write_gpr_u64(kAt, cpu.read_gpr_u64(kV1) < cpu.read_gpr_u64(kAt) ? 1U : 0U); // sltu at,v1,at
    retired += kMiddleCost;

    if (cpu.read_gpr_u64(kAt) != 0U) {
        // 0x00254e9c..0x00254eb8: |D| is tiny, so D = (0, -1, 0) with w = Q.w.
        retired += kTinyCost;
        cpu.write_gpr_u64(kA1, 0U);                                                  // daddu a1,zero,zero
        cpu.write_gpr_sign_extended_32(kV1, 0xbf800000U);                            // lui v1,0xbf80
        qmtc2(cpu, kA1, 2U);                                                         // qmtc2 a1,vf2
        qmtc2(cpu, kV1, 1U);                                                         // qmtc2 v1,vf1
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kX, 0U, false);                     // vaddx.x vf3,vf0,vf2x
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, kY, 0U, false);                     // vaddx.y vf3,vf0,vf1x
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kZ, 0U, false);                     // vaddx.z vf3,vf0,vf2x
        store_vu(cpu, m, 3U, frame + 192U);                                          // sqc2 vf3,192(sp)
    }

    // 0x00254ebc..0x00254f0c: O+80.xyz = D * rsqrt(|D|^2); O+80.w keeps its value.
    load_vu(cpu, m, 2U, frame + 192U);                                               // lqc2 vf2,192(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                         // vaddw.x vf1,vf0,vf0w
    qmfc2(cpu, kV1, 1U);                                                             // qmfc2 v1,vf1
    ee_cop2::vmul(cpu, 2U, 2U, 2U, kXyz);                                            // vmul.xyz vf2,vf2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                                   // vadday.x acc,vf2,vf2y
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);          // vmaddz.x vf1,vf1,vf2z
    qmfc2(cpu, kV1, 1U);                                                             // qmfc2 v1,vf1
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    load_gpr_quad(cpu, m, kV1, frame + 192U);                                        // lq v1,192(sp)
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                        // vrsqrt q,vf0w,vf1x
    qmtc2(cpu, kV1, 1U);                                                             // vwaitq; qmtc2 v1,vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, kXyz, ee_cop2::kControlQ);                    // vmulq.xyz vf1,vf1,q
    qmfc2(cpu, kV1, 1U);                                                             // qmfc2 v1,vf1
    load_vu(cpu, m, 2U, object + kDirection);                                        // lqc2 vf2,80(a0)
    qmtc2(cpu, kV1, 1U);                                                             // qmtc2 v1,vf1
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);                              // vadd.xyz vf2,vf0,vf1
    store_vu(cpu, m, 2U, object + kDirection);                                       // sqc2 vf2,80(a0)
    return call.finish(retired + kTailCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00254d50, 448U, 0x4c6761ce3a138f7bULL, "ScaledClampedVectorsAndDirection", 106U,
               scaled_clamped_vectors_and_direction, EngineReturn::Nothing)
