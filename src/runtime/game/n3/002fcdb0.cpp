// 0x002fcdb0 (a0 = object record, a1 = point P1, a2 = point P2; each point is a 16-byte vec4 and only xyz is used):
// UpdateRangeFadeFromDistance. The record's distance (+6384) is first copied to +6388. The distance between P1 and P2
// is then taken on VU0 (the square root of the summed squared xyz differences) and stored to +6384. Against +6392 and
// +6396: a distance above +6392 gives a fade factor (+6364) of 0; a distance at or below +6392 and below +6396 gives
// 1.0; otherwise the fade is 1.0 minus the magnitude of ((+6392 - +6396) * (d / +6392)) / +6392. The fixed-point value
// (+6216) is 65536 times the fade, truncated to an integer. Returns nothing: the callers read the stored fields.
// v1, a1 and f0..f3 end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kFade = 6364U;              // f32 fade factor
constexpr GuestAddress kFixed = 6216U;             // s32: 65536 * fade, truncated
constexpr GuestAddress kDistance = 6384U;          // f32 distance (its old value moves to kPreviousDistance)
constexpr GuestAddress kPreviousDistance = 6388U;  // f32 previous distance
constexpr GuestAddress kNear = 6392U;              // f32 threshold
constexpr GuestAddress kFar = 6396U;               // f32 threshold

constexpr std::uint32_t kOneBits = 0x3f800000U;    // 1.0f
constexpr std::uint32_t kScaleBits = 0x47800000U;  // 65536.0f

// Retired instruction counts per path, delay slots included. Each path also runs the 28 instructions of the
// distance part up to its first branch.
constexpr std::uint32_t kAboveNearCost = 40U;
constexpr std::uint32_t kBelowFarCost = 44U;
constexpr std::uint32_t kRampCost = 60U;
constexpr std::uint32_t kMirroredRampCost = 63U;  // the longest path, declared as the cost

// The registers the original leaves behind on every path: v1 is the last qmfc2 with its low 64 bits replaced by the
// sign-extended fixed-point value (mfc1), a1 is the last qmfc2 of vf1, and the FPRs hold each path's last values.
void leave_registers(guest::R5900State& cpu, const guest::Gpr128& v1, const guest::Gpr128& a1,
                     const std::uint32_t fixed, const std::uint32_t f1, const std::uint32_t f2,
                     const std::uint32_t f3) {
    cpu.write_gpr(3U, v1);
    cpu.write_gpr_sign_extended_32(3U, fixed);
    cpu.write_gpr(5U, a1);
    cpu.write_fpr_bits(0U, fixed);
    cpu.write_fpr_bits(1U, f1);
    cpu.write_fpr_bits(2U, f2);
    cpu.write_fpr_bits(3U, f3);
}

template <class Context>
n3::Run update_range_fade(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress point_1 = call.arg(1);
    const GuestAddress point_2 = call.arg(2);

    // vaddw.x vf1,vf0,vf0w sets x to 1.0 and keeps the other lanes. The old distance is read, then copied to
    // +6388 before the point loads, as in the listing.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);
    const std::uint32_t previous = m.read_u32_le(object + kDistance);
    guest::Gpr128 v1 = cpu.read_vu0_vector(1U);                                 // qmfc2 v1,vf1
    m.write_u32_le(object + kPreviousDistance, previous);                       // swc1 f0,6388(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, point_2));                        // lqc2 vf1,0(a2)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, point_1));                        // lqc2 vf2,0(a1)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);                  // vsub.xyz vf1,vf2,vf1
    ee_cop2::vmul(cpu, 1U, 1U, 1U, n3::kMaskXyz);                               // vmul.xyz vf1,vf1,vf1
    guest::Gpr128 a1 = cpu.read_vu0_vector(1U);                                 // qmfc2 a1,vf1
    cpu.write_vu0_vector(2U, a1);                                               // qmtc2 a1,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, v1);                                               // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    v1 = cpu.read_vu0_vector(1U);                                               // qmfc2 v1,vf1
    cpu.write_vu0_vector(1U, v1);                                               // qmtc2 v1,vf1
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    v1 = cpu.read_vu0_vector(1U);                                               // qmfc2 v1,vf1
    cpu.write_vu0_vector(1U, v1);                                               // vwaitq (no wait); qmtc2 v1,vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                 // vmulq.x vf1,vf1,q
    v1 = cpu.read_vu0_vector(1U);                                               // qmfc2 v1,vf1
    const std::uint32_t distance = v1.words[0];
    m.write_u32_le(object + kDistance, distance);                               // sw v1,6384(a0)
    const std::uint32_t near_value = m.read_u32_le(object + kNear);             // mtc1 v1,f3; lwc1 f2,6392(a0)

    // c.ole.s f3,f2 with bc1f: the distance is not at or below near. NaN and infinite operands compare as the
    // largest finite value of their sign (normalize_cop1_operand), as on the EE.
    if (!fpu::le(distance, near_value)) {
        m.write_u32_le(object + kFade, 0U);                                     // sw zero,6364(a0)
        const std::uint32_t scaled = fpu::mul(kScaleBits, m.read_u32_le(object + kFade));  // lwc1 f0; mul.s
        const auto fixed = static_cast<std::uint32_t>(fpu::to_int(scaled));     // cvt.w.s
        m.write_u32_le(object + kFixed, fixed);                                 // sw v1,6216(a0)
        leave_registers(cpu, v1, a1, fixed, kScaleBits, near_value, distance);
        return call.finish(kAboveNearCost);
    }
    const std::uint32_t far_value = m.read_u32_le(object + kFar);               // lwc1 f0,6396(a0)
    // c.olt.s f3,f0 with bc1t: below far, so the fade is 1.0. The lui a1,0x3f80 leaves 1.0 in the low word of a1.
    if (fpu::lt(distance, far_value)) {
        m.write_u32_le(object + kFade, kOneBits);                               // sw a1,6364(a0)
        const std::uint32_t scaled = fpu::mul(kScaleBits, m.read_u32_le(object + kFade));  // lwc1 f0; mul.s
        const auto fixed = static_cast<std::uint32_t>(fpu::to_int(scaled));     // cvt.w.s
        m.write_u32_le(object + kFixed, fixed);                                 // sw v1,6216(a0)
        guest::Gpr128 a1_one = a1;
        a1_one.words[0] = kOneBits;
        a1_one.words[1] = 0U;                                                   // lui sign-extends its constant
        leave_registers(cpu, v1, a1_one, fixed, kScaleBits, near_value, distance);
        return call.finish(kBelowFarCost);
    }
    // Ramp: the distance is at or below near and at or beyond far. Each store to +6364 is read back by the next step
    // where the listing reads it, and the delay-slot store of the quotient always happens.
    const std::uint32_t span = fpu::sub(near_value, far_value);                 // sub.s f1,f2,f0
    const std::uint32_t ratio = fpu::div(distance, near_value);                 // div.s f0,f3,f2
    const std::uint32_t scaled_span = fpu::mul(span, ratio);                    // mul.s f2,f1,f0
    m.write_u32_le(object + kFade, scaled_span);                                // swc1 f2,6364(a0)
    const std::uint32_t near_again = m.read_u32_le(object + kNear);             // lwc1 f1,6392(a0)
    const std::uint32_t quotient = fpu::div(scaled_span, near_again);           // div.s f1,f2,f1
    m.write_u32_le(object + kFade, quotient);                                   // swc1 f1,6364(a0)
    const bool mirrored = fpu::lt(quotient, 0U);                                // c.olt.s f1,f0 with f0 = +0.0
    if (mirrored) {
        m.write_u32_le(object + kFade, fpu::neg(quotient));                     // neg.s f0,f1; swc1 f0,6364(a0)
    }
    const std::uint32_t fade = m.read_u32_le(object + kFade);                   // lwc1 f2,6364(a0)
    const std::uint32_t remaining = fpu::sub(kOneBits, fade);                   // sub.s f1,f1,f2
    const std::uint32_t scaled = fpu::mul(kScaleBits, remaining);               // mul.s f0,f0,f1
    const auto fixed = static_cast<std::uint32_t>(fpu::to_int(scaled));         // cvt.w.s f0,f0
    m.write_u32_le(object + kFade, remaining);                                  // swc1 f1,6364(a0)
    m.write_u32_le(object + kFixed, fixed);                                     // sw v1,6216(a0)
    leave_registers(cpu, v1, a1, fixed, remaining, fade, distance);
    return call.finish(mirrored ? kMirroredRampCost : kRampCost);
}

} // namespace

extern constinit const EngineFunction n3_002fcdb0{0x002fcdb0U, 332U, 0xa4a700123f80a44dULL,
                                                  "UpdateRangeFadeFromDistance", kMirroredRampCost,
                                                  n3::live<&update_range_fade<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002fcdb0, update_range_fade)
