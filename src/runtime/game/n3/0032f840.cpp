// 0x0032f840 (a0 = object O): LinkedPairCurveStep. Objects P1 = O+68 and P2 = O+72 each pick a 48-byte entry of the
// table at 0x0055efec by the signed byte at +6572; the entry points at a record whose quadword at +176 holds a weight
// in its w lane. X2 = CurveTableValue(P2 + 4288) + 0.25 * w2 and X1 = CurveTableValue(P1 + 4288) + 0.25 * w1. When
// G * w2 < T the original runs (its cold path calls 0x0032f550, which has no native). Otherwise D = X2 - X1 and
// f1 = 50 + |D|, the unit vector of P1 - P2 is taken on VU0, and the results go to O+16 (quadword), O+52 (word) and
// O+32 (flag byte, copied to P1+7137). Returns v0 = 1. The original's frame spills are not written: the quadwords it
// reads back are read from their sources. f20, f21 and f22 are restored at the end; s0, s1 and ra are never touched.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015ac00)
N3_CALLEE(001419b0)
N3_CALLEE(00156170)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 304U;
constexpr std::uint32_t kCeiling = 20000U;
constexpr GuestAddress kEntryTable = 0x0055EFECU;        // lui 0x56, addiu -4116: 48-byte entries
constexpr GuestAddress kThresholdAddress = 0x01E755B8U;  // lui 0x01e7, lwc1 21944: the threshold T
constexpr std::uint32_t kScaleOffset = static_cast<std::uint32_t>(-30980);  // lwc1 -30980($gp): the scale G
constexpr GuestAddress kConstantQuad = 0x004C9CF0U;      // lui 0x004d, lqc2 -25360: a constant quadword
constexpr GuestAddress kPointerA = 68U;                  // P1 (lw 68(s1))
constexpr GuestAddress kPointerB = 72U;                  // P2 (lw 72(s1))
constexpr GuestAddress kIndexByte = 6572U;               // signed entry index (lb 6572(P))
constexpr GuestAddress kRecordQuad = 176U;               // quadword of a record
constexpr std::uint32_t kCurveRecord = 4288U;            // CurveTableValue's record: P + 4288
constexpr GuestAddress kPosition = 48U;                  // lqc2 48(P)
constexpr GuestAddress kSecondQuad = 6352U;              // lqc2 6352(P)
constexpr GuestAddress kQuadOut = 16U;                   // sqc2 vf2, 16(s1)
constexpr GuestAddress kFlagByte = 32U;                  // sb v0, 32(s1)
constexpr GuestAddress kWordOut = 52U;                   // sw v0, 52(s1); lwc1 f0, 52(s1)
constexpr GuestAddress kCopyByte = 7137U;                // sb v1, 7137(P1)
constexpr GuestAddress kGateByte = 7170U;                // lbu 7170(P1)
constexpr GuestAddress kTableOfP1 = 6324U;               // lw 6324(P1): the record table
constexpr GuestAddress kTableIndex = 6328U;              // lhu 6328(P1): the record index

// Retired instructions of the original's straight-line blocks, delay slots included. The callees' counts come from
// their own runs; the count of a path adds its extra blocks below.
constexpr std::uint32_t kHeadCost = 34U;       // 0x0032f840..0x0032f8c4: entries, G, T and the cold test
constexpr std::uint32_t kCurvePairCost = 48U;  // 0x0032f8c8..0x0032f984: both curve values and the sign of D
constexpr std::uint32_t kOffsetCost = 4U;      // 0x0032f988..0x0032f9a4: f1 = 50 + |D| (either arm)
constexpr std::uint32_t kVectorCost = 34U;     // 0x0032f9a8..0x0032fa2c: the VU0 block and the first gate
constexpr std::uint32_t kZeroGateCost = 1U;    // 0x0032fa30: the gate returned 0
constexpr std::uint32_t kTailCost = 17U;       // 0x0032fa34..0x0032fa74
constexpr std::uint32_t kEpilogueCost = 8U;    // 0x0032fb08..0x0032fb24
constexpr std::uint32_t kRangeCost = 7U;       // 0x0032fa8c..0x0032faa4: the range call and its branch
constexpr std::uint32_t kRangeHitCost = 2U;    // 0x0032faf8, 0x0032fafc: the range call returned nonzero
constexpr std::uint32_t kRecordCost = 13U;     // 0x0032faa8..0x0032fad8: the record kind and its test
constexpr std::uint32_t kKindCost = 2U;        // 0x0032fb00, 0x0032fb04: kind == 255
constexpr std::uint32_t kGateByteCost = 5U;    // 0x0032fadc..0x0032fae4 and one of 0x0032faf0 / 0x0032fae8
constexpr std::uint32_t kCommonCost =
    kHeadCost + kCurvePairCost + kOffsetCost + kVectorCost + kTailCost + kEpilogueCost;

template <class Context>
n3::Run linked_pair_curve_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0U);
    const std::uint32_t saved_f20 = cpu.read_fpr_bits(20U);
    const std::uint32_t saved_f21 = cpu.read_fpr_bits(21U);
    const std::uint32_t saved_f22 = cpu.read_fpr_bits(22U);

    // 0x0032f86c..0x0032f8a8: the entries and the records they point at.
    const GuestAddress object_a = memory.read_u32_le(object + kPointerA);
    const GuestAddress object_b = memory.read_u32_le(object + kPointerB);
    cpu.write_fpr_bits(0U, memory.read_u32_le(kThresholdAddress));
    cpu.write_fpr_bits(1U, memory.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kScaleOffset));
    const auto index_a = static_cast<std::int8_t>(memory.read_u8(object_a + kIndexByte));
    const auto index_b = static_cast<std::int8_t>(memory.read_u8(object_b + kIndexByte));
    const GuestAddress record_a = memory.read_u32_le(kEntryTable + ((static_cast<std::uint32_t>(index_a) * 3U) << 4U));
    const GuestAddress record_b = memory.read_u32_le(kEntryTable + ((static_cast<std::uint32_t>(index_b) * 3U) << 4U));

    // 0x0032f8ac..0x0032f8e0: V2 is the quadword of P2's record; its w lane is the weight used by the cold test.
    const Gpr128 quad_b = n3::load_quad_any(memory, record_b + kRecordQuad);
    cpu.write_gpr(2U, quad_b);                                    // lq v0, 176(a1)
    cpu.write_gpr(5U, quad_b);                                    // lq a1, 176(a1)
    cpu.write_fpr_bits(2U, quad_b.words[3]);                      // lwc1 f2, 140(sp)
    ee_cop1::mul_s(cpu, 1U, 1U, 2U);                              // mul.s f1, f1, f2
    ee_cop1::compare_s(cpu, ee_cop1::Compare::Less, 1U, 0U);      // c.olt.s f1, f0
    if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {
        return call.decline();                                    // bc1t 0x0032fa78: the cold path
    }

    // 0x0032f8c8..0x0032f8e4: P2's curve value C2; f20 = w2 for the sum below.
    cpu.write_fpr_bits(22U, 0x3E800000U);                         // mtc1 v1, f22: 0.25
    cpu.write_fpr_bits(20U, quad_b.words[3]);                     // lwc1 f20, 124(sp)
    cpu.write_gpr_sign_extended_32(2U, object_b);                 // lw v0, 72(s1)
    call.set_arg(0U, object_b + kCurveRecord);                    // addiu a0, v0, 4288
    const n3::Run curve_b = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
    if (curve_b.declined) {
        return call.decline();
    }

    // 0x0032f8ec..0x0032f910: V1; X2 = C2 + 0.25 * w2 in f21; then P1's curve value C1 in f0.
    const Gpr128 quad_a = n3::load_quad_any(memory, record_a + kRecordQuad);
    cpu.write_gpr(4U, quad_a);                                    // lq a0, 176(s0)
    cpu.write_fpr_bits(1U, 0U);                                   // mtc1 $zero, $f1
    cpu.write_gpr_sign_extended_32(2U, object_a);                 // lw v0, 68(s1)
    ee_cop1::adda_s(cpu, 1U, 0U);                                 // adda.s f1, f0
    ee_cop1::madd_s(cpu, 21U, 22U, 20U);                          // madd.s f21, f22, f20
    cpu.write_fpr_bits(20U, quad_a.words[3]);                     // lwc1 f20, 108(sp)
    call.set_arg(0U, object_a + kCurveRecord);                    // addiu a0, v0, 4288
    const n3::Run curve_a = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
    if (curve_a.declined) {
        return call.decline();
    }

    // 0x0032f914..0x0032f92c: VF1.x = 1; ACC = C1; f0 = X1 = C1 + 0.25 * w1.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);     // vaddw.x vf1, vf0, vf0w
    cpu.write_gpr_sign_extended_32(6U, object_a);                 // lw a2, 68(s1)
    cpu.write_fpr_bits(2U, 0U);                                   // mtc1 $zero, $f2
    cpu.write_gpr_sign_extended_32(5U, object_b);                 // lw a1, 72(s1)
    ee_cop1::adda_s(cpu, 2U, 0U);                                 // adda.s f2, f0
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                   // qmfc2 v0, vf1
    ee_cop1::madd_s(cpu, 0U, 22U, 20U);                           // madd.s f0, f22, f20

    // 0x0032f930..0x0032f96c: VF3.xyz = the unit vector of P1 - P2, scaled by Q = 1 / |P1 - P2|.
    cpu.write_vu0_vector(1U, n3::load_quad_any(memory, object_b + kPosition));  // lqc2 vf1, 48(a1)
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, object_a + kPosition));  // lqc2 vf2, 48(a2)
    ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, 14U, true);                           // vsub.xyz vf3, vf2, vf1
    ee_cop2::vmul(cpu, 2U, 3U, 3U, 14U);                                        // vmul.xyz vf2, vf3, vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                                 // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                                 // qmfc2 v0, vf1
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                                 // qmtc2 v0, vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                   // vrsqrt q, vf0w, vf1x
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                                 // qmfc2 v0, vf1
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(2U));                               // mtc1 v0, f1
    ee_cop1::sqrt_s(cpu, 1U, 1U);                                               // sqrt.s f1, f1
    cpu.write_gpr_sign_extended_32(2U, cpu.read_fpr_bits(1U));                  // mfc1 v0, f1
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                                 // qmtc2 v0, vf1
    // vwaitq: the synchronous VU0 model has nothing to wait for.
    ee_cop2::vmul_control(cpu, 3U, 3U, 14U, ee_cop2::kControlQ);                // vmulq.xyz vf3, vf3, q
    cpu.write_gpr(2U, cpu.read_vu0_vector(3U));                                 // qmfc2 v0, vf3
    // The original spills v0 (this unit vector) to 272(sp) and reloads it after the gates; it is kept here.
    const Gpr128 unit = cpu.read_vu0_vector(3U);

    // 0x0032f978..0x0032f9a4: D = X2 - X1 in f1; f1 = 50 - D when D <= 0, else 50 + D, so f1 = 50 + |D|.
    ee_cop1::sub_s(cpu, 1U, 21U, 0U);                                           // sub.s f1, f21, f0
    ee_cop1::compare_s(cpu, ee_cop1::Compare::LessEqual, 1U, 2U);               // c.ole.s f1, f2
    const bool d_at_most_zero = (cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U;
    cpu.write_gpr_sign_extended_32(2U, 0x42480000U);                            // lui v0, 0x4248 (50.0)
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(2U));                               // mtc1 v0, f0
    if (d_at_most_zero) {
        ee_cop1::sub_s(cpu, 1U, 0U, 1U);                                        // sub.s f1, f0, f1
    } else {
        ee_cop1::add_s(cpu, 1U, 0U, 1U);                                        // add.s f1, f0, f1
    }

    // 0x0032f9a8..0x0032f9d4: f0 = +-1.0 with the sign of S, the float at O+52.
    cpu.write_fpr_bits(0U, memory.read_u32_le(object + kWordOut));              // lwc1 f0, 52(s1)
    cpu.write_gpr_sign_extended_32(2U, 0x004D0000U);                            // lui v0, 0x004d
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, kConstantQuad));         // lqc2 vf2, -25360(v0)
    cpu.write_gpr_sign_extended_32(4U, 0x80000000U);                            // lui a0, 0x8000
    cpu.write_gpr_sign_extended_32(3U, 0x3F800000U);                            // lui v1, 0x3f80
    cpu.write_vu0_vector(1U, n3::load_quad_any(memory, object_b + kSecondQuad));  // lqc2 vf1, 6352(a1)
    cpu.write_gpr_sign_extended_32(2U, cpu.read_fpr_bits(0U));                  // lw v0, 300(sp)
    cpu.write_gpr_u64(2U, cpu.read_gpr_u64(2U) & cpu.read_gpr_u64(4U));        // and v0, v0, a0
    cpu.write_gpr_u64(2U, cpu.read_gpr_u64(2U) | cpu.read_gpr_u64(3U));        // or v0, v0, v1
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(2U));                               // lwc1 f0, 296(sp)

    // 0x0032f9d8..0x0032fa1c: the outer product, then VF2 = (VF3 + VF1 * s) + VF4 * 5.0, stored at O+16.
    ee_cop2::vopmula(cpu, 2U, 1U);                                              // vopmula.xyz acc, vf2, vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                                          // vopmsub.xyz vf1, vf1, vf2
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                                 // qmfc2 v0, vf1
    cpu.write_gpr_sign_extended_32(4U, cpu.read_fpr_bits(0U));                  // mfc1 a0, f0
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                                 // qmtc2 v0, vf1
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(1U));                  // mfc1 v1, f1
    cpu.write_vu0_vector(2U, cpu.read_gpr(4U));                                 // qmtc2 a0, vf2
    cpu.write_gpr_sign_extended_32(2U, 0x40A00000U);                            // lui v0, 0x40a0 (5.0)
    ee_cop2::vmul_broadcast(cpu, 4U, 1U, 2U, 14U, 0U);                          // vmulx.xyz vf4, vf1, vf2x
    cpu.write_vu0_vector(1U, n3::load_quad_any(memory, object_a + kSecondQuad));  // lqc2 vf1, 6352(a2)
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));                                 // qmtc2 v1, vf2
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, object_b + kPosition));   // lqc2 vf3, 48(a1)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);                          // vmulx.xyz vf1, vf1, vf2x
    ee_cop2::vadd_vector(cpu, 2U, 3U, 1U, 14U, false);                          // vadd.xyz vf2, vf3, vf1
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                                 // qmtc2 v0, vf1
    ee_cop2::vmul_broadcast(cpu, 1U, 4U, 1U, 14U, 0U);                          // vmulx.xyz vf1, vf4, vf1x
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 14U, false);                          // vadd.xyz vf2, vf2, vf1
    n3::store_quad_any(memory, object + kQuadOut, cpu.read_vu0_vector(2U));     // sqc2 vf2, 16(s1)

    // 0x0032fa20..0x0032fa2c: GetLinkedRecordByte(P1); the flag comes from the path taken below.
    call.set_arg(0U, object_a);                                                 // lw a0, 68(s1) (delay slot)
    const n3::Run gate = call.invoke([](auto& machine) { return n3::fn_001419b0(machine); }, kFrameBytes);
    if (gate.declined) {
        return call.decline();
    }
    std::uint32_t callee_retired = curve_a.retired + curve_b.retired + gate.retired;
    std::uint32_t extra = kZeroGateCost;
    std::uint32_t flag = 0U;
    if (call.result() != 0U) {
        // 0x0032fa8c..0x0032fa9c: CheckRemainingRangeFits(P1 + 4288) with f12 = 2.0.
        cpu.write_gpr_sign_extended_32(3U, object_a);                           // lw v1, 68(s1)
        cpu.write_gpr_sign_extended_32(2U, 0x40000000U);                        // lui v0, 0x4000
        cpu.write_fpr_bits(12U, cpu.read_gpr_u32(2U));                          // mtc1 v0, f12
        call.set_arg(0U, object_a + kCurveRecord);                              // addiu a0, v1, 4288
        const n3::Run range_result =
            call.invoke([](auto& machine) { return n3::fn_00156170(machine); }, kFrameBytes);
        if (range_result.declined) {
            return call.decline();
        }
        callee_retired += range_result.retired;
        extra = kRangeCost;
        if (call.result() != 0U) {
            extra += kRangeHitCost;                                             // 0x0032faf8: v0 = 0
            flag = 0U;
        } else {
            // 0x0032faa8..0x0032fad4: the kind byte (+2) of P1's table record.
            const GuestAddress table = memory.read_u32_le(object_a + kTableOfP1);
            const std::uint32_t slot = memory.read_u16_le(object_a + kTableIndex);
            const GuestAddress block = memory.read_u32_le(memory.read_u32_le(table + 4U) + 8U);
            const std::uint8_t kind = memory.read_u8(block + slot * 10U + 2U);
            extra += kRecordCost;
            if (kind == 255U) {
                extra += kKindCost;                                             // 0x0032fb00: v0 = 1
                flag = 1U;
            } else {
                // 0x0032fadc..0x0032faf4: v0 = 1 when P1's gate byte is zero, else 0.
                const std::uint8_t gate_byte = memory.read_u8(object_a + kGateByte);
                extra += kGateByteCost;
                flag = gate_byte == 0U ? 1U : 0U;
            }
        }
    }

    // 0x0032fa34..0x0032fa74: the flag byte; VF1.x = 1; P1's copy byte; the dot product of P1 with the unit vector
    // (VF1.x) stored at O+52; v0 = 1 is the return value.
    cpu.write_gpr_u64(2U, flag);                                                // v0 = flag
    memory.write_u8(object + kFlagByte, static_cast<std::uint8_t>(flag));       // sb v0, 32(s1)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1, vf0, vf0w
    const std::uint8_t flag_read = memory.read_u8(object + kFlagByte);          // lbu v1, 32(s1)
    cpu.write_gpr_u64(3U, flag_read);
    cpu.write_gpr_sign_extended_32(2U, object_a);                               // lw v0, 68(s1)
    cpu.write_gpr(4U, cpu.read_vu0_vector(1U));                                 // qmfc2 a0, vf1
    memory.write_u8(object_a + kCopyByte, flag_read);                           // sb v1, 7137(v0)
    cpu.write_gpr_sign_extended_32(2U, object_a);                               // lw v0, 68(s1)
    cpu.write_vu0_vector(2U, unit);                                             // lqc2 vf2, 272(sp)
    cpu.write_vu0_vector(1U, n3::load_quad_any(memory, object_a));              // lqc2 vf1, 0(v0)
    ee_cop2::vmul(cpu, 2U, 2U, 1U, 14U);                                        // vmul.xyz vf2, vf2, vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(4U));                                 // qmtc2 a0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                                 // qmfc2 v0, vf1
    memory.write_u32_le(object + kWordOut, cpu.read_gpr_u32(2U));               // sw v0, 52(s1)
    cpu.write_gpr_sign_extended_32(2U, 1U);                                     // addiu v0, zero, 1

    // 0x0032fb08..0x0032fb24: the saved f20..f22 come back as they were on entry.
    cpu.write_fpr_bits(20U, saved_f20);
    cpu.write_fpr_bits(21U, saved_f21);
    cpu.write_fpr_bits(22U, saved_f22);
    return call.finish(kCommonCost + extra + callee_retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0032f840, 744U, 0x14f9bd0bef6a6cd7ULL, "LinkedPairCurveStep", kCeiling, linked_pair_curve_step,
         EngineReturn::Integer)
