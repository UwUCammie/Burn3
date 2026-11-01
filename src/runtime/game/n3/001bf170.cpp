// 0x001bf170 (a0 = object O): UpdateObjectWheelContactModes. O holds a signed byte n at +4457 (rows) and rows of 192
// bytes at O+192*i (row record R = O+192*i+2080). Returns nothing (v0/v1/f0 are scratch).
//
// Prologue: the quad at O+176 is copied to the frame (sp+224); its w word is f26. P = u32[O+5108] (a pointer);
// the word u32[P+6028] chooses the path.
//
// IDLE path (u32[P+6028] == 0): O+5417 = 0; for each row i < n whose |float(row+2172) * float(row+2160)| is not
// <= 2.0: u32[row+2200] = 1 and O+5417 = 1.
//
// ACTIVE path: s0 = (f26 < (1609.0 * float(O+5036)) / 3600.0) || u32[O+5320] == 0xffffffff. For each row i < n:
//   u32[row+2200] = 0; when the byte at row+2259 is set the row gets a mode word at R+120 (R = row+2080) from
//   a VU0 computation (cross product of the quad at O+208 with R[16..] - quad at u32[O+516]+48, plus the quad
//   at O+176, its length through vsqrt, and for rows < 2 a normalised direction dotted with the quad at O+5376)
//   and the float tests below, then calls AddScaledDirectionByMode (0x001bf030) with a1 = byte R+176.
//   The row is skipped (no mode, no call) when that byte is clear.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001bf030)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

constexpr std::uint32_t kFrameBytes = 272U;
constexpr GuestAddress kSaveQuad = 224U;      // sq a1, 224(sp): the quad from O+176
constexpr GuestAddress kSumQuad = 256U;       // sqc2 vf2, 256(sp)
constexpr GuestAddress kPosQuad = 144U;       // sqc2 vf3, 144(sp)

constexpr GuestAddress kRowStride = 192U;
constexpr GuestAddress kRowCountByte = 4457U;
constexpr GuestAddress kQuadO176 = 176U;
constexpr GuestAddress kQuadO208 = 208U;
constexpr GuestAddress kQuadO5376 = 5376U;
constexpr GuestAddress kRowBase = 2080U;      // row record R = O + 192*i + 2080
constexpr GuestAddress kRowFlagWord = 2200U;  // u32 cleared/set per row
constexpr GuestAddress kRowActiveByte = 2259U;
constexpr GuestAddress kModeWord = 120U;      // R + 120

constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kTwentyNine = 0x41e80000U;  // 29.0
constexpr std::uint32_t kDivisor = 0x45610000U;     // 3600.0
constexpr std::int32_t kScaleInt = 1609;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 20U;          // 0x1bf170..0x1bf1bc
constexpr std::uint32_t kActiveHeadCost = 15U;     // 0x1bf1c0..0x1bf1f8
constexpr std::uint32_t kHeadClearCost = 1U;       // 0x1bf1fc
constexpr std::uint32_t kHeadTestCost = 2U;        // 0x1bf200..204
constexpr std::uint32_t kHeadWordCost = 3U;        // 0x1bf208..210
constexpr std::uint32_t kHeadInitCost = 10U;       // 0x1bf214..238
constexpr std::uint32_t kLoopTestCost = 4U;        // 0x1bf23c..248
constexpr std::uint32_t kExitCost = 1U + 12U;      // 0x1bf54c + 0x1bf550..57c
constexpr std::uint32_t kRowHeadCost = 9U;         // 0x1bf24c..268
constexpr std::uint32_t kRowSkipCost = 2U;         // 0x1bf428..42c
constexpr std::uint32_t kVuCost = 30U;             // 0x1bf270..2e4
constexpr std::uint32_t kS0SetCost = 8U;           // 0x1bf2e8..304
constexpr std::uint32_t kS0SetStoreCost = 3U;      // 0x1bf308..310
constexpr std::uint32_t kPlainCost = 4U;           // 0x1bf314..320
constexpr std::uint32_t kPlainSixCost = 3U;        // 0x1bf430..438
constexpr std::uint32_t kModeFourCost = 5U;        // 0x1bf324..334
constexpr std::uint32_t kBlock43cCost = 6U;        // 0x1bf43c..450
constexpr std::uint32_t kBlock4b4Cost = 3U;        // 0x1bf4b4..4bc
constexpr std::uint32_t kBlock454Cost = 3U;        // 0x1bf454..45c
constexpr std::uint32_t kBlock498Cost = 4U;        // 0x1bf498..4a4
constexpr std::uint32_t kBlock4a8Cost = 3U;        // 0x1bf4a8..4b0
constexpr std::uint32_t kBlock460Cost = 4U;        // 0x1bf460..46c
constexpr std::uint32_t kBlock470Cost = 1U;        // 0x1bf470
constexpr std::uint32_t kBlock474Cost = 2U;        // 0x1bf474..478
constexpr std::uint32_t kBlock47cCost = 4U;        // 0x1bf47c..488
constexpr std::uint32_t kBlock48cCost = 3U;        // 0x1bf48c..494
constexpr std::uint32_t kBlock4c0Cost = 4U;        // 0x1bf4c0..4cc
constexpr std::uint32_t kBlock4d0Cost = 2U;        // 0x1bf4d0..4d4
constexpr std::uint32_t kBlock338Cost = 9U;        // 0x1bf338..358
constexpr std::uint32_t kBlock35cCost = 8U;        // 0x1bf35c..378
constexpr std::uint32_t kBlock37cCost = 3U;        // 0x1bf37c..384
constexpr std::uint32_t kBlock388Cost = 3U;        // 0x1bf388..390
constexpr std::uint32_t kBlock394Cost = 3U;        // 0x1bf394..39c
constexpr std::uint32_t kBlock3a0Cost = 29U;       // 0x1bf3a0..410
constexpr std::uint32_t kBlock414Cost = 2U;        // 0x1bf414..418
constexpr std::uint32_t kCallCost = 3U;            // 0x1bf41c..424: lbu, jal, delay slot
constexpr std::uint32_t kRowNextCost = 2U;         // 0x1bf428..42c
constexpr std::uint32_t kIdleSetupCost = 6U;       // 0x1bf4e0..4f4
constexpr std::uint32_t kIdleTestCost = 4U;        // 0x1bf528..534
constexpr std::uint32_t kIdleRowCost = 11U;        // 0x1bf4f8..520
constexpr std::uint32_t kIdleSetCost = 3U;         // 0x1bf540..548
constexpr std::uint32_t kIdleNextCost = 1U;        // 0x1bf524
constexpr std::uint32_t kIdleEndCost = 2U + 2U + 12U;  // 0x1bf538..53c, 0x1bf4d8..4dc, 0x1bf550..57c

constexpr std::uint32_t kCeiling = 20000U;

// lqc2/sqc2 fault unless the address is 16-byte aligned and inside guest memory.
template <class Memory>
bool quad_ok(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

void qmfc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) { cpu.write_gpr(gpr, cpu.read_vu0_vector(vf)); }
void qmtc2(guest::R5900State& cpu, const unsigned gpr, const unsigned vf) { cpu.write_vu0_vector(vf, cpu.read_gpr(gpr)); }

template <class Context>
n3::Run update_object_wheel_contact_modes(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint64_t entry_a0 = call.arg64(0);
    const GuestAddress object = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    constexpr unsigned kV0 = 2U;

    // lq a1,176(s2); sq a1,224(sp); lwc1 f26,236(sp)
    const Gpr128 saved_quad = n3::load_quad_any(m, object + kQuadO176);
    n3::store_quad_any(m, frame + kSaveQuad, saved_quad);
    const std::uint32_t f26 = m.read_u32_le(frame + kSaveQuad + 12U);
    const GuestAddress pointer = m.read_u32_le(object + 5108U);
    const std::uint32_t gate = m.read_u32_le(pointer + 6028U);
    std::uint32_t retired = kEntryCost;

    if (gate == 0U) {
        // IDLE path.
        m.write_u8(object + 5417U, 0U);
        retired += kIdleSetupCost;
        std::int32_t i = 0;
        for (;;) {
            retired += kIdleTestCost;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            if (!(i < static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kRowCountByte))))) {
                break;
            }
            const GuestAddress row = object + kRowStride * static_cast<std::uint32_t>(i);
            retired += kIdleRowCost;
            const std::uint32_t product = fpu::mul(m.read_u32_le(row + 2172U), m.read_u32_le(row + 2160U));
            if (!fpu::le(fpu::abs(product), kTwo)) {
                m.write_u32_le(row + kRowFlagWord, 1U);
                m.write_u8(object + 5417U, 1U);
                retired += kIdleSetCost;
            }
            retired += kIdleNextCost;
            ++i;
        }
        return call.finish(retired + kIdleEndCost);
    }

    // ACTIVE path.
    retired += kActiveHeadCost;
    const std::uint32_t scaled = fpu::mul(fpu::from_int(kScaleInt), m.read_u32_le(object + 5036U));
    const std::uint32_t limit = fpu::div(scaled, kDivisor);
    bool s0 = fpu::lt(f26, limit);
    if (!s0) {
        retired += kHeadClearCost;
    }
    retired += kHeadTestCost;
    if (!s0) {
        retired += kHeadWordCost;
        s0 = m.read_u32_le(object + 5320U) == 0xffffffffU;
    }
    const std::uint32_t f24 = m.read_u32_le(gp - 31792U);
    const std::uint32_t f22 = m.read_u32_le(gp - 30980U);
    const std::uint32_t f20 = m.read_u32_le(gp - 32704U);
    retired += kHeadInitCost;

    for (std::int32_t i = 0;; ++i) {
        retired += kLoopTestCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (!(i < static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kRowCountByte))))) {
            break;
        }
        const GuestAddress row_base = object + kRowStride * static_cast<std::uint32_t>(i);
        const GuestAddress row = row_base + kRowBase;
        retired += kRowHeadCost;
        m.write_u32_le(row_base + kRowFlagWord, 0U);
        if (m.read_u8(row_base + kRowActiveByte) == 0U) {
            retired += kRowNextCost;
            continue;
        }
        retired += kVuCost;

        // VU0 sequence 0x1bf270..0x1bf2dc.
        const GuestAddress v0_pointer = m.read_u32_le(object + 516U);
        if (!quad_ok(m, row + 16U) || !quad_ok(m, object + kQuadO208) || !quad_ok(m, v0_pointer + 48U) ||
            !quad_ok(m, object + kQuadO176) || !quad_ok(m, frame + kSumQuad)) {
            return call.decline();
        }
        cpu.write_gpr_sign_extended_32(kV0, v0_pointer);
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, row + 16U));                 // lqc2 vf3,16(v1)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kQuadO208));        // lqc2 vf1,208(s2)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, v0_pointer + 48U));          // lqc2 vf2,48(v0)
        ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, kXyz, true);                         // vsub.xyz vf2,vf3,vf2
        ee_cop2::vopmula(cpu, 1U, 2U);                                             // vopmula.xyz acc,vf1,vf2
        ee_cop2::vopmsub(cpu, 1U, 2U, 1U);                                         // vopmsub.xyz vf1,vf2,vf1
        qmfc2(cpu, kV0, 1U);
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, object + kQuadO176));        // lqc2 vf3,176(s2)
        qmtc2(cpu, kV0, 2U);
        ee_cop2::vadd_vector(cpu, 2U, 2U, 3U, kXyz, false);                        // vadd.xyz vf2,vf2,vf3
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                   // vaddw.x vf1,vf0,vf0w
        n3::store_quad_any(m, frame + kSumQuad, cpu.read_vu0_vector(2U));          // sqc2 vf2,256(sp)
        qmfc2(cpu, kV0, 1U);
        ee_cop2::vmul(cpu, 2U, 2U, 2U, kXyz);                                      // vmul.xyz vf2,vf2,vf2
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                             // vadday.x acc,vf2,vf2y
        qmtc2(cpu, kV0, 1U);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);    // vmaddz.x vf1,vf1,vf2z
        qmfc2(cpu, kV0, 1U);
        qmtc2(cpu, kV0, 1U);
        ee_cop2::vsqrt(cpu, 1U, 0U);                                               // vsqrt q,vf1x
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                   // vaddw.x vf1,vf0,vf0w
        qmfc2(cpu, kV0, 1U);
        qmtc2(cpu, kV0, 1U);                                                       // after vwaitq
        ee_cop2::vmul_control(cpu, 1U, 1U, kX, ee_cop2::kControlQ);                // vmulq.x vf1,vf1,q
        qmfc2(cpu, kV0, 1U);
        const std::uint32_t f3 = cpu.read_gpr_u32(kV0);                            // mtc1 v0,f3

        const std::uint32_t row_product = fpu::mul(m.read_u32_le(row + 92U), m.read_u32_le(row + 80U));
        const auto excess = [&]() { return fpu::sub(fpu::abs(row_product), f3); };
        if (s0) {
            retired += kS0SetCost;
            if (!fpu::le(excess(), kTwo)) {
                retired += kS0SetStoreCost;
                m.write_u32_le(row + kModeWord, 2U);
            }
        } else {
            retired += kPlainCost;
            const GuestAddress flag_pointer = m.read_u32_le(object + 5108U);
            if (m.read_u8(flag_pointer + 6555U) != 0U) {
                retired += kPlainSixCost;
                m.write_u32_le(row + kModeWord, 6U);
            } else {
                retired += kModeFourCost;
                const std::uint32_t mode_word = m.read_u32_le(object + 5412U);
                const std::uint32_t mode_minus = mode_word - 1U;
                if (mode_minus < 2U) {
                    // 0x1bf43c
                    retired += kBlock43cCost;
                    m.write_u32_le(row + kModeWord, 4U);
                    if (!fpu::le(m.read_u32_le(object + 5152U), f24)) {
                        retired += kBlock4b4Cost;
                        m.write_u32_le(row + kModeWord, 3U);
                    } else {
                        retired += kBlock454Cost;
                        if (!(i < 2)) {
                            // 0x1bf498
                            retired += kBlock498Cost;
                            if (!fpu::le(m.read_u32_le(object + 5148U), f24)) {
                                retired += kBlock4a8Cost;
                                m.write_u32_le(row + kModeWord, 3U);
                            }
                        } else {
                            // 0x1bf460
                            retired += kBlock460Cost;
                            const std::uint32_t a0 = m.read_u32_le(object + 5412U);
                            bool to_48c = false;
                            bool at_474 = false;
                            if (a0 == 1U) {
                                // 0x1bf4c0
                                retired += kBlock4c0Cost;
                                if (fpu::lt(m.read_u32_le(object + 5156U), 0U)) {
                                    to_48c = true;
                                } else {
                                    retired += kBlock4d0Cost;
                                    at_474 = true;
                                }
                            } else {
                                retired += kBlock470Cost;
                                at_474 = true;
                            }
                            if (at_474) {
                                retired += kBlock474Cost;
                                if (a0 == 2U) {
                                    retired += kBlock47cCost;
                                    if (!fpu::le(m.read_u32_le(object + 5156U), 0U)) {
                                        to_48c = true;
                                    }
                                }
                            }
                            if (to_48c) {
                                retired += kBlock48cCost;
                                m.write_u32_le(row + kModeWord, 3U);
                            }
                        }
                    }
                } else {
                    // 0x1bf338
                    retired += kBlock338Cost;
                    if (!quad_ok(m, frame + kPosQuad)) {
                        return call.decline();
                    }
                    n3::store_quad_any(m, frame + kPosQuad, cpu.read_vu0_vector(3U));   // sqc2 vf3,144(v0)
                    const std::uint32_t f0 = m.read_u32_le(object + 5076U);
                    const std::uint32_t f2 = m.read_u32_le(frame + kPosQuad + 12U);
                    const std::uint32_t f1 = fpu::sub(f0, kTwo);
                    const std::uint32_t scaled_w = fpu::mul(f22, f2);
                    bool to_388 = !fpu::lt(scaled_w, f1);
                    if (!to_388) {
                        retired += kBlock35cCost;
                        if (fpu::le(excess(), kTwo)) {
                            to_388 = true;
                        } else {
                            retired += kBlock37cCost;
                            m.write_u32_le(row + kModeWord, 3U);
                        }
                    }
                    if (to_388) {
                        retired += kBlock388Cost;
                        if (!fpu::le(f26, kTwentyNine)) {
                            retired += kBlock394Cost;
                            if (i < 2) {
                                retired += kBlock3a0Cost;
                                if (!quad_ok(m, frame + kSumQuad) || !quad_ok(m, object + kQuadO5376)) {
                                    return call.decline();
                                }
                                // 0x1bf3a0..410: normalised sum vector dotted with the quad at O+5376.
                                cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame + kSumQuad));
                                ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);
                                qmfc2(cpu, kV0, 1U);
                                ee_cop2::vmul(cpu, 2U, 2U, 2U, kXyz);
                                ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
                                qmtc2(cpu, kV0, 1U);
                                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);
                                qmfc2(cpu, kV0, 1U);
                                qmtc2(cpu, kV0, 1U);
                                cpu.write_gpr(kV0, n3::load_quad_any(m, frame + kSumQuad));   // lq v0,256(sp)
                                ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                      // vrsqrt q,vf0w,vf1x
                                qmtc2(cpu, kV0, 1U);
                                ee_cop2::vmul_control(cpu, 1U, 1U, kXyz, ee_cop2::kControlQ);  // vmulq.xyz
                                qmfc2(cpu, kV0, 1U);
                                cpu.write_vu0_vector(3U, n3::load_quad_any(m, object + kQuadO5376));
                                qmtc2(cpu, kV0, 2U);
                                ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);
                                ee_cop2::vmul(cpu, 2U, 3U, 2U, kXyz);                          // vmul.xyz vf2,vf3,vf2
                                qmfc2(cpu, kV0, 1U);
                                ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
                                qmtc2(cpu, kV0, 1U);
                                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);
                                qmfc2(cpu, kV0, 1U);
                                const std::uint32_t dot = cpu.read_gpr_u32(kV0);               // mtc1 v0,f0
                                if (fpu::lt(dot, f20)) {
                                    retired += kBlock414Cost;
                                    m.write_u32_le(row + kModeWord, 4U);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 0x1bf41c: lbu a1,176(v1); jal AddScaledDirectionByMode with a0 = s2.
        retired += kCallCost;
        call.set_arg64(0U, entry_a0);
        call.set_arg(1U, m.read_u8(row + 176U));
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_001bf030(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += callee.retired + kRowNextCost;
    }
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001bf170, 1040U, 0x200fd232c07f272fULL, "UpdateObjectWheelContactModes", kCeiling,
         update_object_wheel_contact_modes, EngineReturn::Nothing)
