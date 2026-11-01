// 0x001bebd0 (a0 = body B, a1 = quad P, a2 = quad D): ApplyContactImpulseToBody. The record pointer at
// B+516 gives the origin O (quad at record+48). R = P - O (xyz) and C = R x D come from the VU0
// cross-product pair. The mode (u32 at B+5412) and the force F (word 1 of the quad at B+208) then choose
// a path. Mode 1 or 2, or |F| > EPS (the float at gp-28580) with F and C.y having the same sign, is path A:
// the accumulator quad A1 at B+240 takes D (xyz) and is stored back. Otherwise path B does the same, then
// re-reads the record pointer, forms C' = (P - O') x D from the new origin and adds it to the accumulator
// A2 at B+256. Returns nothing: v1, t0 and f0-f2 are temporaries here.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kRecordPointer = 516U;      // u32 at body+516: the record holding the origin
constexpr std::uint32_t kOriginOffset = 48U;        // origin quad at record+48
constexpr std::uint32_t kForceQuad = 208U;          // quad at body+208: word 1 is the force F
constexpr std::uint32_t kModeOffset = 5412U;        // u32 at body+5412: the mode
constexpr std::uint32_t kFirstAccumulator = 240U;   // quad at body+240: A1
constexpr std::uint32_t kSecondAccumulator = 256U;  // quad at body+256: A2
constexpr std::uint32_t kEpsilonBelowGp = 28580U;   // float EPS at gp-28580
constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kPlusOne = 0x3f800000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 25U;            // 0x1bebd0..0x1bec30: the cross product and the mode test
constexpr std::uint32_t kEpsilonCost = 5U;          // 0x1bec34..0x1bec44: |F| against EPS and its exit branch
constexpr std::uint32_t kSignCost = 20U;            // 0x1bec48..0x1bec94: the sign compare and its branch
constexpr std::uint32_t kSetFlagCost = 1U;          // 0x1bec98: daddiu t1, zero, 1
constexpr std::uint32_t kSelectCost = 2U;           // 0x1bec9c..0x1beca0: beq on t1 and its nop
constexpr std::uint32_t kPathACost = 5U;            // 0x1beca4..0x1becb4: A1 + D stored, then the jump
constexpr std::uint32_t kPathBCost = 15U;           // 0x1becb8..0x1becf0: A1 + D, then A2 + C' stored
constexpr std::uint32_t kReturnCost = 2U;           // 0x1becf4..0x1becf8: jr ra and its stack restore
// The block table's longest path is 70. Its two t1 branches are not correlated there, so the largest
// path the code can take is 69; the declared cost keeps the table's bound.
constexpr std::uint32_t kDeclaredCost = 70U;

template <class Context>
n3::Run apply_contact_impulse(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress body = call.arg(0);
    const GuestAddress position = call.arg(1);
    const GuestAddress delta = call.arg(2);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // The listing's loads: the record pointer, D, P, and the quad whose word 1 is F (lq aligns to 16).
    const GuestAddress record = m.read_u32_le(body + kRecordPointer);
    const Gpr128 d = n3::load_quad_any(m, delta);
    const Gpr128 p = n3::load_quad_any(m, position);
    const std::uint32_t force = n3::load_quad_any(m, body + kForceQuad).words[1];

    // lqc2 vf1,48(v1); qmtc2 t0,vf2; vsub.xyz vf1,vf2,vf1: R = P - O, with O's w kept in vf1.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + kOriginOffset));
    cpu.write_vu0_vector(2U, p);
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
    // qmfc2 v1,vf1 and qmtc2 v1,vf2 copy R into vf2; qmtc2 a3,vf1 puts D into vf1.
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, d);
    // vopmula.xyz acc,vf2,vf1 then vopmsub.xyz vf1,vf1,vf2: vf1.xyz = R x D.
    ee_cop2::vopmula(cpu, 2U, 1U);
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);
    // qmfc2 t0,vf1: word 1 of the cross product is C.y.
    const std::uint32_t cross_y = cpu.read_vu0_vector(1U).words[1];

    // lw v1,5412(a0): mode 1 or 2 takes path A at once (t1 = 1).
    const std::uint32_t mode = m.read_u32_le(body + kModeOffset);
    std::uint32_t retired = kHeadCost;
    bool path_a = false;
    if (mode - 1U < 2U) {
        path_a = true;
        retired += kSetFlagCost;
    } else {
        // lwc1 EPS; abs.s; c.ole.s; bc1t: |F| <= EPS leaves t1 = 0 (path B).
        const std::uint32_t epsilon = m.read_u32_le(gp - kEpsilonBelowGp);
        retired += kEpsilonCost;
        if (!fpu::le(fpu::abs(force), epsilon)) {
            // The sign bits of F and C.y, each as +-1.0; c.eq.s on the two sets t1.
            const std::uint32_t force_sign = (force & kSignBit) | kPlusOne;
            const std::uint32_t cross_sign = (cross_y & kSignBit) | kPlusOne;
            retired += kSignCost;
            path_a = fpu::eq(force_sign, cross_sign);
            if (path_a) {
                retired += kSetFlagCost;
            }
        }
    }
    retired += kSelectCost;

    if (path_a) {
        // lqc2 vf1,272(sp) reads D back from the frame; lqc2 vf2,240(a0); vadd.xyz vf2,vf2,vf1; sqc2 vf2,240(a0).
        cpu.write_vu0_vector(1U, d);
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, body + kFirstAccumulator));
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);
        n3::store_quad_any(m, body + kFirstAccumulator, cpu.read_vu0_vector(2U));
        retired += kPathACost;
    } else {
        // lqc2 vf3,272(sp) reads D; lqc2 vf1,240(a0); lqc2 vf2,288(sp) reads P; vadd.xyz vf1,vf1,vf3;
        // sqc2 vf1,240(a0).
        cpu.write_vu0_vector(3U, d);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, body + kFirstAccumulator));
        cpu.write_vu0_vector(2U, p);
        ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, false);
        n3::store_quad_any(m, body + kFirstAccumulator, cpu.read_vu0_vector(1U));
        // lw v1,516(a0) is read again, after the store: the origin O' and R' = P - O'.
        const GuestAddress record_again = m.read_u32_le(body + kRecordPointer);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, record_again + kOriginOffset));
        ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
        // vopmula.xyz acc,vf1,vf3 then vopmsub.xyz vf1,vf3,vf1: vf1.xyz = R' x D.
        ee_cop2::vopmula(cpu, 1U, 3U);
        ee_cop2::vopmsub(cpu, 1U, 3U, 1U);
        // qmfc2 v1,vf1 and qmtc2 v1,vf1 leave vf1 as it is.
        // lqc2 vf2,256(a0); vadd.xyz vf2,vf2,vf1; sqc2 vf2,256(a0).
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, body + kSecondAccumulator));
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);
        n3::store_quad_any(m, body + kSecondAccumulator, cpu.read_vu0_vector(2U));
        retired += kPathBCost;
    }
    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001bebd0{0x001bebd0U, 300U, 0x74e912b0d8014f51ULL,
                                                  "ApplyContactImpulseToBody", kDeclaredCost,
                                                  n3::live<&apply_contact_impulse<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001bebd0, apply_contact_impulse)
