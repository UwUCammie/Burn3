// 0x001beee0 (a0 = object O, a1 = pointer to the quad D = (dx, dy, dz, dw)): AddMotionDeltaAndNormalise. A leaf that
// returns nothing (EngineReturn::Nothing; v1 and f0 are scratch). It runs on the real VU0 state.
//
// The VU0 sequence: VF1.x = 1.0 (vaddw.x), then O+176 is rewritten with (dx, dy, dz, P.w) from P = the quad there
// and L = dx*dx + dy*dy + dz*dz is left in VF1.x (ACC holds dx*dx + dy*dy, vmaddz.x adds the z term with VF1.x = 1.0).
// SHORT (L < 2^-32, ordered compare, 22 + 11 retired): O+192 = the quad at u32[O+516] + 32, and O+176 becomes
// (dx, dy, dz, w) with w = (1 - 1) + VF2.x, where VF2 = (0, 0, the caller's VF1.z, VF1.w).
// LONG (28 retired): O+192 = D; root = sqrt(L) through f0 (the mtc1/mfc1 round trip keeps the upper doubleword of
// the GPR); Q = 1 / root from vrsqrt; O+192 = D * Q (xyz); O+176 = (dx, dy, dz, w) with w = (1 - 1) + root.
// JOIN (6 retired): f1 = the w lane of the quad now at O+176. For each of the n = (signed byte at O+4457) rows,
// row = O + 192*i: row+2172 = f1 / (the float at row+2160). Retired counts: 45 + 12n (short), 62 + 12n (long).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kW = 1U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

constexpr GuestAddress kPosition = 176U;       // P, then (dx, dy, dz, w)
constexpr GuestAddress kDirection = 192U;      // D, then D * Q
constexpr GuestAddress kCountByte = 4457U;     // signed byte: number of rows
constexpr GuestAddress kRowStride = 192U;      // row = O + 192 * i
constexpr GuestAddress kRowDivisor = 2160U;    // float read from each row
constexpr GuestAddress kRowResult = 2172U;     // float written to each row
constexpr std::uint32_t kShortLimit = 0x2f800000U;  // 2^-32, the bits of the threshold in f0

// Retired counts from the listing, delay slots included: 22 up to the length test, then the short (11) or long (28)
// block; the join (6); then the loop test (4) runs once more than its body (8) per row, and the return adds 2.
constexpr std::uint32_t kPrefixCost = 22U;
constexpr std::uint32_t kShortCost = 11U;
constexpr std::uint32_t kLongCost = 28U;
constexpr std::uint32_t kJoinCost = 6U;
constexpr std::uint32_t kExitCost = 6U;        // one loop test (4) and the return (2)
constexpr std::uint32_t kRowCost = 12U;        // one row's body (8) and the test after it (4)

template <class Context>
n3::Run add_motion_delta_and_normalise(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    // lq a1,0(a1): the delta quad D (the load ignores the low four address bits).
    const Gpr128 delta = n3::load_quad_any(m, call.arg(1));

    // vaddw.x vf1,vf0,vf0w: VF1.x = 1.0; the other lanes are the caller's (qmfc2 v1,vf1 keeps them as "ones").
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);
    const Gpr128 ones = cpu.read_vu0_vector(1U);

    // lqc2 vf2,176(O); qmtc2 a1,vf1; vadd.xyz vf2,vf0,vf1; vmul.xyz vf1,vf1,vf1; sqc2 vf2,176(O)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, object + kPosition));
    cpu.write_vu0_vector(1U, delta);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);
    ee_cop2::vmul(cpu, 1U, 1U, 1U, kXyz);
    n3::store_quad_any(m, object + kPosition, cpu.read_vu0_vector(2U));

    // qmfc2 a2,vf1; qmtc2 a2,vf2; vadday.x acc,vf2,vf2y; qmtc2 v1,vf1; vmaddz.x vf1,vf1,vf2z
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
    cpu.write_vu0_vector(1U, ones);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);

    // qmfc2 v1,vf1; mtc1 v1,f1; c.olt.s f1,f0 against 2^-32 (f0)
    const Gpr128 squared = cpu.read_vu0_vector(1U);
    cpu.write_fpr_bits(1U, squared.words[0]);
    cpu.write_fpr_bits(0U, kShortLimit);
    const bool short_path = fpu::lt(squared.words[0], kShortLimit);

    std::uint32_t scalar = kShortLimit;  // f0 when no row is written

    if (short_path) {
        // lqc2 vf1,176(O): (dx, dy, dz, P.w). daddu v1,zero,zero clears the low doubleword of v1; the high one
        // is ones.z/ones.w (from the qmfc2 of VF1 that held them), and qmtc2 v1,vf2 copies that quad.
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kPosition));
        cpu.write_vu0_vector(2U, Gpr128{{0U, 0U, ones.words[2], ones.words[3]}});
        // vsubw.w vf1,vf0,vf0w; vaddx.w vf1,vf1,vf2x; sqc2 vf1,176(O)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kW, 3U, true);
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kW, 0U, false);
        n3::store_quad_any(m, object + kPosition, cpu.read_vu0_vector(1U));
        // lw v1,516(O); lq v1,32(v1); sq v1,192(O)
        n3::store_quad_any(m, object + kDirection, n3::load_quad_any(m, m.read_u32_le(object + 516U) + 32U));
    } else {
        // vaddw.x vf1,vf0,vf0w (VF1 = ones again); sq a1,192(O)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);
        const Gpr128 ones_again = cpu.read_vu0_vector(1U);
        n3::store_quad_any(m, object + kDirection, delta);
        // qmfc2 v1,vf1; lqc2 vf1,192(O) (the quad D); vmul.xyz vf2,vf1,vf1; vadday.x acc,vf2,vf2y
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kDirection));
        ee_cop2::vmul(cpu, 2U, 1U, 1U, kXyz);
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
        // qmtc2 v1,vf1; vmaddz.x vf1,vf1,vf2z: VF1.x = L again
        cpu.write_vu0_vector(1U, ones_again);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);
        // lq v1,192(O) (D again); vrsqrt q,vf0w,vf1x: Q = 1 / sqrt(L)
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
        // qmfc2 a1,vf1; mtc1 a1,f0; sqrt.s f0,f0; mfc1 a1,f0 (sign-extended into the low doubleword); qmtc2 a1,vf1
        Gpr128 length = cpu.read_vu0_vector(1U);
        const std::uint32_t root = fpu::sqrt(length.words[0]);
        cpu.write_fpr_bits(0U, root);
        length.words[0] = root;
        length.words[1] = (root & 0x80000000U) != 0U ? 0xffffffffU : 0U;
        cpu.write_vu0_vector(1U, length);
        scalar = root;
        // qmtc2 v1,vf1 (D); vmulq.xyz vf1,vf1,q; qmfc2 v1,vf1; sq v1,192(O)
        cpu.write_vu0_vector(1U, delta);
        ee_cop2::vmul_control(cpu, 1U, 1U, kXyz, ee_cop2::kControlQ);
        n3::store_quad_any(m, object + kDirection, cpu.read_vu0_vector(1U));
        // lqc2 vf1,176(O); qmtc2 a1,vf2; vsubw.w vf1,vf0,vf0w; vaddx.w vf1,vf1,vf2x; sqc2 vf1,176(O)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kPosition));
        cpu.write_vu0_vector(2U, length);
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kW, 3U, true);
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kW, 0U, false);
        n3::store_quad_any(m, object + kPosition, cpu.read_vu0_vector(1U));
    }

    // JOIN: lq a1,176(O) (the quad just stored); f1 = its w lane (the stack copy at sp+76 holds the same word).
    const std::uint32_t w = n3::load_quad_any(m, object + kPosition).words[3];
    cpu.write_fpr_bits(1U, w);

    // The row loop: each row's float at +2172 is f1 / the float at +2160 (div.s). Counted by a signed byte.
    std::uint32_t rows = 0U;
    while (static_cast<std::int32_t>(rows) < static_cast<std::int8_t>(m.read_u8(object + kCountByte))) {
        const GuestAddress row = object + kRowStride * rows;
        const std::uint32_t divisor = m.read_u32_le(row + kRowDivisor);
        scalar = fpu::div(w, divisor);
        m.write_u32_le(row + kRowResult, scalar);
        ++rows;
    }
    cpu.write_fpr_bits(0U, scalar);

    const std::uint32_t path_cost = short_path ? kShortCost : kLongCost;
    return call.finish(kPrefixCost + path_cost + kJoinCost + kExitCost + kRowCost * rows);
}

} // namespace

// Declared cost: the longest path, the long block with 127 rows (62 + 12 * 127).
extern constinit const EngineFunction n3_001beee0{0x001beee0U, 324U, 0x06dc380fc87e2cf3ULL,
                                                  "AddMotionDeltaAndNormalise", 1586U, n3::live<&add_motion_delta_and_normalise<NativeFunctionContext>>,
                                                  EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001beee0, add_motion_delta_and_normalise)
