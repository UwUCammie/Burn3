// 0x001b3b40 (a1 = contact pair record, a2 = velocity quad of object A, a3 = velocity quad of object B):
// ProjectContactVelocities. Object A's body is the u32 at +12 of the object at the record's +36, object B's the one at
// +40. A body whose flag byte at +530 is non-zero takes part: its velocity quad is slid along the contact surface
// (normal = the body's quad at +368) and keeps its speed. A velocity that is zero, or not moving into the contact
// (dot with the normal not below the float at $gp-29740), is left alone. Otherwise the tangent
// t = v - n*dot is taken, and the direction is t normalised, or the negated quad at +192 when |t|^2 < 2^-32; the new
// velocity is direction * |v|. The VU0 sequence follows the original's instruction order, so VF1..VF3, ACC, Q, the
// FPU registers and the frame words end as the original leaves them. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kT0 = 8U;
constexpr std::uint8_t kMaskX = 8U;

constexpr GuestAddress kObjectAOffset = 36U;           // lw a0,36(a1)
constexpr GuestAddress kObjectBOffset = 40U;           // lw v1,40(a1)
constexpr GuestAddress kBodyOffset = 12U;              // lw ...,12(object)
constexpr GuestAddress kFlagOffset = 530U;             // lbu ...,530(body): the body takes part when non-zero
constexpr GuestAddress kNormalOffset = 368U;           // lqc2 ...,368(body): the contact normal
constexpr GuestAddress kDirectionOffset = 192U;        // lqc2 ...,192(body): the direction for a zero tangent
constexpr GuestAddress kFrameBytes = 192U;             // addiu sp,sp,-192
constexpr GuestAddress kLimitOffset = 0U - 29740U;     // lwc1 f0,-29740(gp): the dot product limit
constexpr std::uint32_t kShortLengthBits = 0x2f800000U;  // 2^-32: the squared-length threshold

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;               // 0x001b3b40..0x001b3b5c: frame, four loads, lbu, beq, nop
constexpr std::uint32_t kFlagCost = 3U;                // 0x001b3cf4..0x001b3cfc: lbu, beq, nop (object B's flag)
constexpr std::uint32_t kLengthCost = 16U;             // 0x001b3b60..0x001b3b9c: |v|^2 and its bc1t with delay slot
constexpr std::uint32_t kFallThroughCost = 1U;         // daddu: only when the bc1t does not branch
constexpr std::uint32_t kSkipTestCost = 3U;            // andi, bne, nop: the short test that follows
constexpr std::uint32_t kDotCost = 15U;                // 0x001b3bb0..0x001b3be8: dot product and its bc1f
constexpr std::uint32_t kTangentCost = 37U;            // 0x001b3bec..0x001b3c7c: speed, tangent, |t|^2 and its bc1t
constexpr std::uint32_t kNormalCost = 17U;             // 0x001b3c90..0x001b3cd0: normalised direction, through beq, sq
constexpr std::uint32_t kShortTangentCost = 3U;        // 0x001b3cd4..0x001b3cdc: direction from the body
constexpr std::uint32_t kJoinCost = 5U;                // 0x001b3ce0..0x001b3cf0: scale by the speed and store
constexpr std::uint32_t kReturnCost = 2U;              // 0x001b3e94..0x001b3e98: jr and addiu sp

// One object's half from its length test to its join, or to the branch that skips the rest. The caller has tested the
// body's flag. Returns the instructions retired in that span.
std::uint32_t run_half(guest::R5900State& cpu, auto& m, const GuestAddress body,
                       const GuestAddress velocity, const GuestAddress tangent_slot,
                       const GuestAddress direction_slot) {
    const auto qmfc2 = [&](const unsigned gpr, const unsigned vf) { cpu.write_gpr(gpr, cpu.read_vu0_vector(vf)); };
    const auto qmtc2 = [&](const unsigned gpr, const unsigned vf) { cpu.write_vu0_vector(vf, cpu.read_gpr(gpr)); };
    std::uint32_t retired = kLengthCost;

    // Length test: vf1.x is 1 while the velocity travels through v1 (qmfc2/qmtc2 keep vf1's other lanes).
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    cpu.write_gpr(kA0, n3::load_quad_any(m, velocity));                                  // lq a0,0(velocity)
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    qmtc2(kA0, 1U);                                                                  // qmtc2 a0,vf1
    ee_cop2::vmul(cpu, 2U, 1U, 1U, n3::kMaskXyz);                                    // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f1
    cpu.write_gpr_sign_extended_32(kV1, kShortLengthBits);                           // lui v1,0x2f80
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f0
    const bool short_length = fpu::lt(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U));  // c.olt.s f1,f0; bc1t
    cpu.write_gpr_sign_extended_32(kV1, 1U);                                         // addiu v1,zero,1 (delay slot)
    if (!short_length) {
        cpu.write_gpr_u64(kV1, 0U);                                                  // daddu v1,zero,zero
        retired += kFallThroughCost;
    }
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kV1) & 0xffU);                           // andi v1,v1,0xff
    retired += kSkipTestCost;                                                        // bne v1,zero; nop
    if (short_length) {
        return retired;
    }

    // Dot product of the velocity with the normal: vf2 = v * n, ACC.x = x + y, vf1.x = ACC.x + z.
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, body + kNormalOffset));                 // lqc2 vf2,368(body)
    qmtc2(kA0, 3U);                                                                  // qmtc2 a0,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);                                    // vmul.xyz vf2,vf3,vf2
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    cpu.write_fpr_bits(0U, m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kLimitOffset));  // lwc1 f0,-29740(gp)
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f1
    const bool below_limit = fpu::lt(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U));  // c.olt.s f1,f0; bc1f
    retired += kDotCost;
    if (!below_limit) {
        return retired;
    }

    // Speed |v| = sqrt(|v|^2) in f2, then the tangent t = v - n * dot, stored to the frame.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                                    // vmul.xyz vf2,vf3,vf3
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                     // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1 (after vwaitq)
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);                  // vmulq.x vf1,vf1,q
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    cpu.write_fpr_bits(2U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f2
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, body + kNormalOffset));                // lqc2 vf1,368(body)
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_fpr_bits(1U));                      // mfc1 v1,f1
    qmtc2(kV1, 2U);                                                                  // qmtc2 v1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                      // vmulx.xyz vf1,vf1,vf2x
    ee_cop2::vadd_vector(cpu, 3U, 3U, 1U, n3::kMaskXyz, true);                       // vsub.xyz vf3,vf3,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                     // vaddw.x vf1,vf0,vf0w
    n3::store_quad_any(m, tangent_slot, cpu.read_vu0_vector(3U));                        // sqc2 vf3,0(sp)

    // |t|^2 in vf1.x: below 2^-32 the direction comes from the body, otherwise t is normalised.
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                                    // vmul.xyz vf2,vf3,vf3
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                                  // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV1, 1U);                                                                  // qmfc2 v1,vf1
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f1
    cpu.write_gpr_sign_extended_32(kV1, kShortLengthBits);                           // lui v1,0x2f80
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kV1));                                   // mtc1 v1,f0
    const bool short_tangent = fpu::lt(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U));  // c.olt.s f1,f0; bc1t
    retired += kTangentCost;
    cpu.write_gpr_sign_extended_32(kV1, 1U);                                         // addiu v1,zero,1 (delay slot)
    if (!short_tangent) {
        cpu.write_gpr_u64(kV1, 0U);                                                  // daddu v1,zero,zero
        retired += kFallThroughCost;
    }
    cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kV1) & 0xffU);                           // andi v1,v1,0xff
    retired += kSkipTestCost;                                                        // bne v1,zero; nop

    if (short_tangent) {
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, body + kDirectionOffset));         // lqc2 vf1,192(body)
        ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, n3::kMaskXyz, true);                   // vsub.xyz vf1,vf0,vf1
        n3::store_quad_any(m, direction_slot, cpu.read_vu0_vector(1U));                  // sqc2 vf1,16(sp)
        retired += kShortTangentCost;
    } else {
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, tangent_slot));                    // lqc2 vf2,0(sp)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                 // vaddw.x vf1,vf0,vf0w
        qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
        ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);                                // vmul.xyz vf2,vf2,vf2
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                           // vadday.x acc,vf2,vf2y
        qmtc2(kV1, 1U);                                                              // qmtc2 v1,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
        qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
        qmtc2(kV1, 1U);                                                              // qmtc2 v1,vf1
        cpu.write_gpr(kV1, n3::load_quad_any(m, tangent_slot));                          // lq v1,0(sp)
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                    // vrsqrt q,vf0w,vf1x
        qmtc2(kV1, 1U);                                                              // qmtc2 v1,vf1 (after vwaitq)
        ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);        // vmulq.xyz vf1,vf1,q
        qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
        n3::store_quad_any(m, direction_slot, cpu.read_gpr(kV1));                        // sq v1,16(sp) (delay slot)
        retired += kNormalCost;
    }

    // Join: the direction scaled by the speed (f2), stored to the velocity.
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_fpr_bits(2U));                      // mfc1 v1,f2
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, direction_slot));                      // lqc2 vf1,16(sp)
    qmtc2(kV1, 2U);                                                                  // qmtc2 v1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                      // vmulx.xyz vf1,vf1,vf2x
    n3::store_quad_any(m, velocity, cpu.read_vu0_vector(1U));                            // sqc2 vf1,0(velocity)
    retired += kJoinCost;
    return retired;
}

template <class Context>
n3::Run project_contact_velocities(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress pair = call.arg(1);
    const GuestAddress velocity_a = call.arg(2);
    const GuestAddress velocity_b = call.arg(3);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // Entry (0x001b3b40..0x001b3b5c). Both objects and their bodies are read from the record before a1 is overwritten
    // with object B's body. a0 takes object A, t0 body A, v1 object B and then body A's flag.
    const GuestAddress object_a = m.read_u32_le(pair + kObjectAOffset);
    const GuestAddress object_b = m.read_u32_le(pair + kObjectBOffset);
    const GuestAddress body_a = m.read_u32_le(object_a + kBodyOffset);
    const GuestAddress body_b = m.read_u32_le(object_b + kBodyOffset);
    cpu.write_gpr_sign_extended_32(kA0, object_a);                                  // lw a0,36(a1)
    cpu.write_gpr_sign_extended_32(kV1, object_b);                                  // lw v1,40(a1)
    cpu.write_gpr_sign_extended_32(kT0, body_a);                                    // lw t0,12(a0)
    cpu.write_gpr_sign_extended_32(kA1, body_b);                                    // lw a1,12(v1)
    const std::uint8_t flag_a = m.read_u8(body_a + kFlagOffset);
    cpu.write_gpr_u64(kV1, flag_a);                                                 // lbu v1,530(t0)
    std::uint32_t retired = kEntryCost;
    if (flag_a != 0U) {
        retired += run_half(cpu, m, body_a, velocity_a, frame, frame + 16U);
    }

    // Object B's flag (0x001b3cf4..0x001b3cfc). A zero flag leaves through the return at once.
    const std::uint8_t flag_b = m.read_u8(body_b + kFlagOffset);
    cpu.write_gpr_u64(kV1, flag_b);                                                 // lbu v1,530(a1)
    retired += kFlagCost;
    if (flag_b != 0U) {
        retired += run_half(cpu, m, body_b, velocity_b, frame + 48U, frame + 64U);
    }
    retired += kReturnCost;                                                         // jr ra; addiu sp,sp,192
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001b3b40{0x001b3b40U, 860U, 0xfec0d3c1756fce7fULL,
                                                  "ProjectContactVelocities", 209U, n3::live<&project_contact_velocities<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001b3b40, project_contact_velocities)
