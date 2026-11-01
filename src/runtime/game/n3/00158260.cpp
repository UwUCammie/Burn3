// 0x00158260 (a0 = object B, a1 = object P, a2 = two quads whose z lanes are P's segment end offsets,
// a3 = slot index): VehicleProximityMinDistance. A proximity test on the XZ plane between object P and
// the object A that B points at (u32 at B+200). The first stage compares the P-A distance with R + 20.0
// (R = float at $gp-29136) and requires the unit P-A direction to lie within the direction quad of the
// def selected by A's byte +6572 (a 48-byte table at 0x0055efec); the second tests the end points of
// A's and P's segments against R. The first test that succeeds wins: the float at B+1184+4*a3 (a running
// minimum, -1.0 = unset) takes the distance when it is unset or larger, and the result is 1. Otherwise 0.
// The VU0 sequences run on the real VU0 state in the listing's order; the general registers that carry
// quadwords through QMFC2/QMTC2 are tracked as full 128-bit values (mfc1/daddu/sll write only the low
// doubleword, so a later QMTC2 copies the old high doubleword along).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr unsigned kV0 = 2U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;

constexpr std::int32_t kRangeOffset = -29136;          // $gp-relative: R
constexpr std::uint32_t kRangeMargin = 0x41a00000U;    // 20.0f
constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5f
constexpr std::uint32_t kUnset = 0xbf800000U;          // -1.0f
constexpr GuestAddress kDefTable = 0x0055efecU;        // lui 0x56, addiu -4116: 48-byte entries
constexpr GuestAddress kSlotBase = 1184U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;

// Retired counts from the listing (delay slots included).
constexpr std::uint32_t kStageA = 37U;       // 0x158260..0x1582f0
constexpr std::uint32_t kStageB = 40U;       // 0x1582f4..0x158390
constexpr std::uint32_t kNearTest = 3U;      // 0x158394
constexpr std::uint32_t kStageC = 42U;       // 0x1583a0..0x158444
constexpr std::uint32_t kStageD2 = 25U;      // 0x158448..0x1584a8
constexpr std::uint32_t kStageD3 = 25U;      // 0x1584ac..0x15850c
constexpr std::uint32_t kStageD4 = 24U;      // 0x158510..0x15856c
constexpr std::uint32_t kFailExit = 4U;      // set v0 = 0 and return (two blocks of two)
constexpr std::uint32_t kFailD4 = 3U;        // 0x1586c0 and the return block
constexpr std::uint32_t kTailStore = 15U;    // slot unset: store, return 1
constexpr std::uint32_t kTailReplace = 18U;  // slot larger than the distance: store, return 1
constexpr std::uint32_t kTailKeep = 16U;     // slot already smaller: return 1

template <class Context>
n3::Run proximity(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const auto reg32 = [&](const unsigned r) { return cpu.read_gpr_u32(r); };
    const auto set32 = [&](const unsigned r, const std::uint32_t v) { cpu.write_gpr_sign_extended_32(r, v); };
    const auto qmfc2 = [&](const unsigned r, const unsigned vf) { cpu.write_gpr(r, cpu.read_vu0_vector(vf)); };
    const auto qmtc2 = [&](const unsigned r, const unsigned vf) { cpu.write_vu0_vector(vf, cpu.read_gpr(r)); };
    const auto lqc2 = [&](const unsigned vf, const GuestAddress a) { cpu.write_vu0_vector(vf, n3::load_quad_any(m, a)); };
    const auto vaddw_x = [&] { ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false); };  // vaddw.x vf1,vf0,vf0w
    const auto fail = [&](const std::uint32_t retired) {
        cpu.write_gpr_u64(kV0, 0U);                                                           // daddu v0,zero,zero
        return call.finish(retired);
    };

    // The tail of a successful test: the slot at B+1184+4*a3 keeps the smaller distance (-1.0 = unset).
    // `before` counts everything up to the branch to the tail; the tail adds its own path's count.
    const auto record = [&](const unsigned x_reg, const std::uint32_t before) {
        set32(kV1, (reg32(kA3) << 2U) + reg32(kA0));                             // sll/addu v1
        set32(kV0, kUnset);                                                      // lui v0,0xbf80
        const GuestAddress slot = reg32(kV1) + kSlotBase;
        const std::uint32_t current = m.read_u32_le(slot);                       // lwc1 f1,1184(v1)
        cpu.write_fpr_bits(1U, current);
        cpu.write_fpr_bits(0U, kUnset);                                          // mtc1 v0,f0
        set32(kV0, slot);                                                        // addiu v0,v1,1184
        const std::uint32_t x = cpu.read_fpr_bits(x_reg);
        std::uint32_t tail = kTailKeep;
        if (fpu::eq(kUnset, current)) {                                          // c.eq.s f0,f1
            m.write_u32_le(slot, x);                                             // swc1
            tail = kTailStore;
        } else if (!fpu::le(current, x)) {                                       // c.ole.s f1,x
            m.write_u32_le(slot, x);
            tail = kTailReplace;
        }
        set32(kV0, 1U);                                                          // addiu v0,zero,1
        return call.finish(before + tail);
    };
    // vsqrt q,vf1x; vaddw.x; qmfc2 v0; vwaitq; qmtc2 v0,vf1; vmulq.x vf1,vf1,q; qmfc2 v0; mtc1 v0,f<dest>.
    const auto finish_distance = [&](const unsigned dest) {
        ee_cop2::vsqrt(cpu, 1U, 0U);                                             // vsqrt q,vf1x
        vaddw_x();
        qmfc2(kV0, 1U);                                                          // qmfc2 v0,vf1
        qmtc2(kV0, 1U);                                                          // qmtc2 v0,vf1
        ee_cop2::vmul_control(cpu, 1U, 1U, kX, ee_cop2::kControlQ);              // vmulq.x vf1,vf1,q
        qmfc2(kV0, 1U);                                                          // qmfc2 v0,vf1
        cpu.write_fpr_bits(dest, reg32(kV0));                                    // mtc1 v0,f<dest>
    };
    // After "vsub.xyz vf1,..." with v0 = f2: the rest of d2/d3/d4.
    const auto full_distance = [&](const unsigned dest) {
        qmtc2(kV0, 2U);                                                          // qmtc2 v0,vf2
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, kY, 0U, false);                 // vaddx.y vf1,vf0,vf2x
        ee_cop2::vmul(cpu, 2U, 1U, 1U, kXyz);                                    // vmul.xyz vf2,vf1,vf1
        vaddw_x();
        qmfc2(kV0, 1U);                                                          // qmfc2 v0,vf1
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                           // vadday.x acc,vf2,vf2y
        qmtc2(kV0, 1U);                                                          // qmtc2 v0,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
        qmfc2(kV0, 1U);                                                          // qmfc2 v0,vf1
        qmtc2(kV0, 1U);                                                          // qmtc2 v0,vf1
        finish_distance(dest);
    };

    // 0x158260: P - A on the XZ plane (VF4.x is the low word of the cleared v0, zero).
    set32(kT1, m.read_u32_le(reg32(kA0) + 200U));                                // lw t1,200(a0)
    cpu.write_gpr_u64(kV0, 0U);                                                  // daddu v0,zero,zero
    lqc2(7U, reg32(kA1) + 48U);                                                  // lqc2 vf7,48(a1)
    cpu.write_fpr_bits(2U, 0U);                                                  // mtc1 zero,f2
    qmtc2(kV0, 4U);                                                              // qmtc2 v0,vf4
    lqc2(2U, reg32(kT1) + 48U);                                                  // lqc2 vf2,48(t1)
    vaddw_x();
    ee_cop2::vadd_vector(cpu, 3U, 7U, 2U, kXyz, true);                           // vsub.xyz vf3,vf7,vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 4U, kY, 0U, false);                     // vaddx.y vf3,vf0,vf4x
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    ee_cop2::vmul(cpu, 1U, 3U, 3U, kXyz);                                        // vmul.xyz vf1,vf3,vf3
    qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
    qmtc2(kV1, 2U);                                                              // qmtc2 v1,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                    // vrsqrt q,vf0w,vf1x
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    cpu.write_fpr_bits(0U, reg32(kV0));                                          // mtc1 v0,f0
    cpu.write_fpr_bits(0U, fpu::sqrt(cpu.read_fpr_bits(0U)));                    // sqrt.s f0,f0
    set32(kV0, cpu.read_fpr_bits(0U));                                           // mfc1 v0,f0
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    ee_cop2::vmul_control(cpu, 3U, 3U, kXyz, ee_cop2::kControlQ);                // vmulq.xyz vf3,vf3,q
    qmfc2(kT2, 3U);                                                              // qmfc2 t2,vf3
    cpu.write_fpr_bits(1U, m.read_u32_le(reg32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kRangeOffset)));  // lwc1 f1
    cpu.write_fpr_bits(4U, reg32(kV0));                                          // mtc1 v0,f4
    set32(kV0, kRangeMargin);                                                    // lui v0,0x41a0
    cpu.write_fpr_bits(0U, reg32(kV0));                                          // mtc1 v0,f0
    cpu.write_fpr_bits(0U, fpu::add(cpu.read_fpr_bits(0U), cpu.read_fpr_bits(1U)));  // add.s f0,f0,f1
    std::uint32_t retired = kStageA;
    if (!fpu::le(cpu.read_fpr_bits(4U), cpu.read_fpr_bits(0U))) {                // c.ole.s f4,f0; bc1f
        return fail(retired + 2U + 2U);
    }

    // 0x1582f4: the P-A direction against the def's direction quad.
    set32(kV1, cpu.read_fpr_bits(2U));                                           // mfc1 v1,f2
    set32(kT0, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(
                   m.read_u8(reg32(kT1) + 6572U)))));                            // lb t0,6572(t1)
    set32(kV0, kDefTable);                                                       // lui/addiu v0
    qmtc2(kV1, 2U);                                                              // qmtc2 v1,vf2
    set32(kV1, reg32(kT0) << 1U);                                                // sll v1,t0,1
    set32(kV1, reg32(kV1) + reg32(kT0));                                         // addu v1,v1,t0
    set32(kV1, reg32(kV1) << 4U);                                                // sll v1,v1,4
    set32(kV0, reg32(kV0) + reg32(kV1));                                         // addu v0,v0,v1
    set32(kV0, m.read_u32_le(reg32(kV0)));                                       // lw v0,0(v0)
    vaddw_x();
    qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
    lqc2(3U, reg32(kV0) + 192U);                                                 // lqc2 vf3,192(v0)
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kY, 0U, false);                     // vaddx.y vf3,vf0,vf2x
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kXyz);                                        // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                              // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                    // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 3U, 3U, kXyz, ee_cop2::kControlQ);                // vmulq.xyz vf3,vf3,q
    qmfc2(kV0, 3U);                                                              // qmfc2 v0,vf3
    qmtc2(kT2, 2U);                                                              // qmtc2 t2,vf2
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    ee_cop2::vmul(cpu, 2U, 2U, 1U, kXyz);                                        // vmul.xyz vf2,vf2,vf1
    vaddw_x();
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    cpu.write_fpr_bits(3U, reg32(kV0));                                          // mtc1 v0,f3
    set32(kV0, kHalf);                                                           // lui v0,0x3f00
    cpu.write_fpr_bits(0U, reg32(kV0));                                          // mtc1 v0,f0
    retired += kStageB;
    if (fpu::lt(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(0U))) {                 // c.olt.s f3,f0; bc1t
        return fail(retired + 2U + 2U);
    }

    // 0x158394: the P-A distance itself against R.
    retired += kNearTest;
    if (fpu::lt(cpu.read_fpr_bits(4U), cpu.read_fpr_bits(1U))) {                 // c.olt.s f4,f1; bc1t
        return record(4U, retired);
    }

    // 0x1583a0: end points of A's segment (t0 = vf1, vf6) and of P's segment (a1 = vf1, vf5).
    set32(kV0, cpu.read_fpr_bits(2U));                                           // mfc1 v0,f2
    lqc2(4U, reg32(kT1) + 32U);                                                  // lqc2 vf4,32(t1)
    qmtc2(kV0, 8U);                                                              // qmtc2 v0,vf8
    vaddw_x();
    set32(kV0, m.read_u32_le(reg32(kT1) + 64U));                                 // lw v0,64(t1)
    qmfc2(kV1, 1U);                                                              // qmfc2 v1,vf1
    lqc2(1U, reg32(kV0) + 3712U);                                                // lqc2 vf1,3712(v0)
    lqc2(2U, reg32(kV0) + 3728U);                                                // lqc2 vf2,3728(v0)
    lqc2(5U, reg32(kT1) + 48U);                                                  // lqc2 vf5,48(t1)
    ee_cop2::vmul_broadcast(cpu, 1U, 4U, 1U, kXyz, 2U);                          // vmulz.xyz vf1,vf4,vf1z
    ee_cop2::vadd_vector(cpu, 1U, 5U, 1U, kXyz, false);                          // vadd.xyz vf1,vf5,vf1
    ee_cop2::vmul_broadcast(cpu, 2U, 4U, 2U, kXyz, 2U);                          // vmulz.xyz vf2,vf4,vf2z
    qmfc2(kT0, 1U);                                                              // qmfc2 t0,vf1
    ee_cop2::vadd_vector(cpu, 6U, 5U, 2U, kXyz, false);                          // vadd.xyz vf6,vf5,vf2
    lqc2(3U, reg32(kA1) + 32U);                                                  // lqc2 vf3,32(a1)
    lqc2(1U, reg32(kA2));                                                        // lqc2 vf1,0(a2)
    lqc2(2U, reg32(kA2) + 16U);                                                  // lqc2 vf2,16(a2)
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, kXyz, 2U);                          // vmulz.xyz vf1,vf3,vf1z
    ee_cop2::vmul_broadcast(cpu, 2U, 3U, 2U, kXyz, 2U);                          // vmulz.xyz vf2,vf3,vf2z
    ee_cop2::vadd_vector(cpu, 1U, 7U, 1U, kXyz, false);                          // vadd.xyz vf1,vf7,vf1
    ee_cop2::vadd_vector(cpu, 5U, 7U, 2U, kXyz, false);                          // vadd.xyz vf5,vf7,vf2
    qmfc2(kA1, 1U);                                                              // qmfc2 a1,vf1
    // d1 = |vf6 - vf5|; the quadword held in v1 goes back into vf1 mid-sequence.
    ee_cop2::vadd_vector(cpu, 1U, 6U, 5U, kXyz, true);                           // vsub.xyz vf1,vf6,vf5
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 8U, kY, 0U, false);                     // vaddx.y vf1,vf0,vf8x
    ee_cop2::vmul(cpu, 2U, 1U, 1U, kXyz);                                        // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                               // vadday.x acc,vf2,vf2y
    qmtc2(kV1, 1U);                                                              // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);      // vmaddz.x vf1,vf1,vf2z
    qmfc2(kV0, 1U);                                                              // qmfc2 v0,vf1
    qmtc2(kV0, 1U);                                                              // qmtc2 v0,vf1
    finish_distance(3U);
    retired += kStageC;
    if (fpu::lt(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(1U))) {                 // c.olt.s f3,f1; bc1t
        return record(3U, retired);
    }

    // d2 = |vf6 - vf4| with vf4 = P's first end point (a1).
    qmtc2(kA1, 4U);                                                              // qmtc2 a1,vf4
    set32(kV0, cpu.read_fpr_bits(2U));                                           // mfc1 v0,f2
    ee_cop2::vadd_vector(cpu, 1U, 6U, 4U, kXyz, true);                           // vsub.xyz vf1,vf6,vf4
    full_distance(3U);
    retired += kStageD2;
    if (fpu::lt(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(1U))) {
        return record(3U, retired);
    }

    // d3 = |vf3 - vf5| with vf3 = A's first end point (t0).
    qmtc2(kT0, 3U);                                                              // qmtc2 t0,vf3
    set32(kV0, cpu.read_fpr_bits(2U));                                           // mfc1 v0,f2
    ee_cop2::vadd_vector(cpu, 1U, 3U, 5U, kXyz, true);                           // vsub.xyz vf1,vf3,vf5
    full_distance(3U);
    retired += kStageD3;
    if (fpu::lt(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(1U))) {
        return record(3U, retired);
    }

    // d4 = |vf3 - vf4|.
    set32(kV0, cpu.read_fpr_bits(2U));                                           // mfc1 v0,f2
    ee_cop2::vadd_vector(cpu, 1U, 3U, 4U, kXyz, true);                           // vsub.xyz vf1,vf3,vf4
    full_distance(2U);
    retired += kStageD4;
    if (!fpu::lt(cpu.read_fpr_bits(2U), cpu.read_fpr_bits(1U))) {                // c.olt.s f2,f1; bc1f
        return fail(retired + kFailD4);
    }
    return record(2U, retired);
}

} // namespace

extern constinit const EngineFunction n3_00158260{0x00158260U, 1132U, 0xaa02439b090398a3ULL,
                                                  "VehicleProximityMinDistance", 214U, n3::live<&proximity<NativeFunctionContext>>,
                                                  EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00158260, proximity)
