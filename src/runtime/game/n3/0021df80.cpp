// 0x0021df80 (f12 = phase, f13 = a float; no GPR arguments): LoadViewportBasisGlobals. A leaf with no branches
// and no calls: 91 instructions on every path. The u32 at 0x00667050 is an index i, and R = 0x00665ec0 + 560 * i
// (32-bit wrap) is a per-viewport record. The rows of R at +1360, +1376 and +1392 go through VU0 into the globals
// at 0x0067cfd0..0x0067cfff, the scalar words of R and three gp-relative floats go out as quadwords at 0x0067d010..
// 0x0067d03f, and the phase fraction t - (1 + trunc t), with t = 2 * phase, goes to 0x0067d1a0. VF2 = (0, -f13, 0, 0)
// is stored first. The memory accesses and VU0 steps keep the original's order, so VF1..VF5 and the MAC/STATUS
// flags finish as the original leaves them. Returns nothing: both callers read no result.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kIndex = 0x00667050U;            // lw 28752(v1), v1 = 0x00660000
constexpr GuestAddress kRecords = 0x00665ec0U;          // addiu 24256 on 0x00660000: record 0
constexpr GuestAddress kRowFive = 0x0067cfd0U;          // lui 0x68, -12336
constexpr GuestAddress kRowFour = 0x0067cfe0U;          // lui 0x68, -12320
constexpr GuestAddress kRowThree = 0x0067cff0U;         // lui 0x68, -12304
constexpr GuestAddress kRowTwo = 0x0067d000U;           // lui 0x68, -12288
constexpr GuestAddress kQuadLow = 0x0067d010U;          // lui 0x68, -12272
constexpr GuestAddress kQuadMid = 0x0067d020U;          // lui 0x68, -12256
constexpr GuestAddress kQuadHigh = 0x0067d030U;         // lui 0x68, -12240
constexpr GuestAddress kFraction = 0x0067d1a0U;         // lui 0x68, -11872
constexpr GuestAddress kGp30564 = static_cast<GuestAddress>(-30564);  // float words read off $gp
constexpr GuestAddress kGp30560 = static_cast<GuestAddress>(-30560);
constexpr GuestAddress kGp30696 = static_cast<GuestAddress>(-30696);
constexpr std::uint32_t kTwo = 0x40000000U;             // 2.0f, mtc1 into f1
constexpr std::uint32_t kOne = 0x3f800000U;             // 1.0f, mtc1 into f0
constexpr std::uint32_t kFive = 0x40a00000U;            // 5.0f, lui 0x40a0 then sw into the frame
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint32_t kRetired = 91U;                 // every path runs the whole listing

// mfc1 writes the low word of a GPR and sign-extends it into the low doubleword; the rest is kept.
Gpr128 with_sign_extended_low(Gpr128 value, const std::uint32_t bits) {
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

template <class Context>
n3::Run load_viewport_basis_globals(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t phase = cpu.read_fpr_bits(12U);

    // Phase fraction: mtc1 2.0,f1; mul.s f2,f1,f12; cvt.w.s f1,f2; cvt.s.w f1,f1; add.s f0,f0,f1; sub.s f0,f2,f0.
    const std::uint32_t t = fpu::mul(kTwo, phase);
    const std::uint32_t fraction = fpu::sub(t, fpu::add(kOne, fpu::from_int(fpu::to_int(t))));

    cpu.write_vu0_vector(5U, n3::load_quad_any(m, kRowFive));                       // lqc2 vf5,-12336(a0): rewritten below
    const std::uint32_t index = m.read_u32_le(kIndex);                          // lw t5,28752(v1)
    const std::uint32_t times_seven = (index << 3U) - index;                    // sll a3,t5,3; subu a3,a3,t5
    const GuestAddress record = kRecords + ((((times_seven << 2U) + times_seven)) << 4U);  // 560 * index
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyzw, true);                 // vsub.xyzw vf2,vf0,vf0
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, kRowThree));                      // lqc2 vf3,-12304(a0): rewritten below
    const std::uint32_t negated = fpu::neg(cpu.read_fpr_bits(13U));             // neg.s f0,f13
    cpu.write_vu0_vector(1U, with_sign_extended_low(cpu.read_gpr(3U), negated));  // mfc1 v1,f0; qmtc2 v1,vf1
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, kRowFour));                       // lqc2 vf4,-12320(a1): rewritten below
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);                // vaddx.y vf2,vf0,vf1x
    n3::store_quad_any(m, kRowTwo, cpu.read_vu0_vector(2U));                        // sqc2 vf2,-12288(t0)

    const std::uint32_t float_30564 = m.read_u32_le(gp + kGp30564);             // lwc1 f1,-30564(gp)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + 1360U));                 // lqc2 vf2,1360(t4)
    const std::uint32_t r1520 = m.read_u32_le(record + 1520U);                  // lwc1 f4,1520(t4)
    const std::uint32_t r1524 = m.read_u32_le(record + 1524U);                  // lwc1 f3,1524(t4)
    const std::uint32_t r1536 = m.read_u32_le(record + 1536U);                  // lwc1 f8,1536(t4)
    const std::uint32_t r1540 = m.read_u32_le(record + 1540U);                  // lwc1 f7,1540(t4)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 1360U));                 // lqc2 vf1,1360(t4)
    ee_cop2::vadd_vector(cpu, 5U, 0U, 2U, n3::kMaskXyz, false);                 // vadd.xyz vf5,vf0,vf2
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 0U, n3::kMaskW, 3U, true);             // vsubw.w vf5,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 5U, 5U, 1U, n3::kMaskW, 2U, false);            // vaddz.w vf5,vf5,vf1z
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + 1376U));                 // lqc2 vf2,1376(t4)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 1376U));                 // lqc2 vf1,1376(t4)
    const std::uint32_t r1672 = m.read_u32_le(record + 1672U);                  // lwc1 f6,1672(t4)
    const std::uint32_t r1676 = m.read_u32_le(record + 1676U);                  // lwc1 f5,1676(t4)
    const std::uint32_t r1504 = m.read_u32_le(record + 1504U);                  // lwc1 f4,1504(t4)
    const std::uint32_t r1508 = m.read_u32_le(record + 1508U);                  // lwc1 f3,1508(t4)
    ee_cop2::vadd_vector(cpu, 4U, 0U, 2U, n3::kMaskXyz, false);                 // vadd.xyz vf4,vf0,vf2
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, n3::kMaskW, 3U, true);             // vsubw.w vf4,vf0,vf0w
    const std::uint32_t float_30560 = m.read_u32_le(gp + kGp30560);             // lwc1 f1,-30560(gp)
    m.write_u32_le(kFraction, fraction);                                        // swc1 f0,-11872(a0)
    n3::store_quad_any(m, kQuadMid, Gpr128{{r1520, r1524, float_30564, kFive}});    // sq v1, built on the frame
    const std::uint32_t float_30696 = m.read_u32_le(gp + kGp30696);             // lwc1 f0,-30696(gp)
    ee_cop2::vadd_broadcast(cpu, 4U, 4U, 1U, n3::kMaskW, 2U, false);            // vaddz.w vf4,vf4,vf1z
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + 1392U));                 // lqc2 vf2,1392(t4)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 1392U));                 // lqc2 vf1,1392(t4)
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, n3::kMaskXyz, false);                 // vadd.xyz vf3,vf0,vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, n3::kMaskW, 3U, true);            // vsubw.w vf3,vf0,vf0w
    n3::store_quad_any(m, kQuadHigh, Gpr128{{r1536, r1540, float_30560, float_30696}});  // sq a0, built on the frame
    n3::store_quad_any(m, kQuadLow, Gpr128{{r1672, r1676, r1504, r1508}});          // sq v1, built on the frame
    ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, n3::kMaskW, 2U, false);            // vaddz.w vf3,vf3,vf1z
    n3::store_quad_any(m, kRowFive, cpu.read_vu0_vector(5U));                       // sqc2 vf5,-12336(t3)
    n3::store_quad_any(m, kRowFour, cpu.read_vu0_vector(4U));                       // sqc2 vf4,-12320(t2)
    n3::store_quad_any(m, kRowThree, cpu.read_vu0_vector(3U));                      // sqc2 vf3,-12304(t1)
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0021df80{0x0021df80U, 364U, 0x45f93d2fe36a6b2dULL,
                                                  "LoadViewportBasisGlobals", kRetired,
                                                  n3::live<&load_viewport_basis_globals<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021df80, load_viewport_basis_globals)
