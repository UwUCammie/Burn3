// 0x003580b0 (a0 = object, f12 = step): StepDampedSpringToTarget, a leaf that returns nothing. The object's position is
// a four-lane VU0 vector P: x is the word at +172 (the w lane of the quad at +160), y, z and w are the words at +192,
// +196 and +200. The target R is P, unless the flag byte at +481 is set: then R = (word +396, the angle, word +420,
// word +424), where the angle is the word at +192 plus the difference of the word at +416 from it, moved a full turn
// toward zero when that difference is at least 180. The spring constants come from the gp-relative floats G1, G2, G3:
// rate = 2 / G3, s = rate * step, e = 1 / (1 + s + G2 s^2 + G1 s^3). With the velocity quad V at +464 and O = P - R:
// moved = (V + O rate) step, V' = (V - moved rate) e, P' = R + (O + moved) e. V' is stored to +464, and to +160 the
// quad whose w lane is P'.x, then P'.y, P'.z, P'.w go to +192, +196 and +200.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kPositionQuad = 160U;   // quad at +160: its w word (+172) is the position's x
constexpr GuestAddress kPositionY = 192U;      // the position's y, and the angle the target is measured from
constexpr GuestAddress kPositionZ = 196U;
constexpr GuestAddress kPositionW = 200U;
constexpr GuestAddress kFlag = 481U;           // byte: nonzero selects the target built from +384
constexpr GuestAddress kTargetQuad = 384U;     // quad at +384: its w word (+396) is the target's x
constexpr GuestAddress kTargetAngle = 416U;    // the target angle: the difference is this word minus the one at +192
constexpr GuestAddress kTargetZ = 420U;
constexpr GuestAddress kTargetW = 424U;
constexpr GuestAddress kVelocityQuad = 464U;   // quad at +464: the velocity V
constexpr GuestAddress kGlobalG1 = 30584U;     // lwc1 at -30584($gp)
constexpr GuestAddress kGlobalG2 = 30580U;     // lwc1 at -30580($gp)
constexpr GuestAddress kGlobalG3 = 26700U;     // lwc1 at -26700($gp)

constexpr std::uint32_t kOne = 0x3f800000U;        // 1.0
constexpr std::uint32_t kTwo = 0x40000000U;        // 2.0
constexpr std::uint32_t kHalfTurn = 0x43340000U;   // 180.0
constexpr std::uint32_t kFullTurn = 0x43b40000U;   // 360.0

// VU0 destination masks (bit 3 is x, bit 0 is w) and the lane numbers of broadcast operands.
constexpr std::uint8_t kDestX = 8U;
constexpr std::uint8_t kDestY = 4U;
constexpr std::uint8_t kDestZ = 2U;
constexpr std::uint8_t kDestW = 1U;
constexpr std::uint8_t kDestXyzw = 15U;
constexpr std::size_t kLaneX = 0U;
constexpr std::size_t kLaneW = 3U;

// Retired instruction counts by path, delay slots included: the flag is clear; the flag is set and the angle
// difference is under 180; the flag is set and the difference is wrapped.
constexpr std::uint32_t kCostUntargeted = 82U;
constexpr std::uint32_t kCostNearTarget = 108U;
constexpr std::uint32_t kCostWrapped = 122U;

// A VU0 register holding a 32-bit value in lane x (qmtc2 of a GPR; the broadcast operations read only that lane).
guest::Gpr128 scalar(const std::uint32_t bits) {
    guest::Gpr128 value{};
    value.words[0] = bits;
    return value;
}

// Macro-mode FMAC instructions publish their MAC and STATUS flags along with the destination.
guest::Gpr128 published(guest::R5900State& cpu, const guest::Vu0MacroFmacResult& result) {
    guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
    return result.value;
}

// VADD / VSUB: FD = FS +/- FT on the lanes in the mask.
guest::Gpr128 vadd(guest::R5900State& cpu, const guest::Gpr128& destination, const guest::Gpr128& left,
                   const guest::Gpr128& right, const std::uint8_t mask, const bool subtract) {
    return published(cpu, guest::execute_vu0_macro_vector_add_subtract(destination, left, right, mask, subtract));
}

// VADDx..w: FD = FS + FT.lane on the lanes in the mask.
guest::Gpr128 vadd_lane(guest::R5900State& cpu, const guest::Gpr128& destination, const guest::Gpr128& left,
                        const guest::Gpr128& right, const std::uint8_t mask, const std::size_t lane) {
    return published(cpu,
                     guest::execute_vu0_macro_broadcast_add_subtract(destination, left, right, mask, lane, false));
}

// VMULx..w: FD = FS * FT.lane on the lanes in the mask.
guest::Gpr128 vmul_lane(guest::R5900State& cpu, const guest::Gpr128& destination, const guest::Gpr128& left,
                        const guest::Gpr128& right, const std::uint8_t mask, const std::size_t lane) {
    return published(cpu, guest::execute_vu0_macro_broadcast_multiply(destination, left, right, mask, lane));
}

// The angle difference, moved toward zero by a full turn: diff - 360 * sign(diff), with the R5900 ADDA/MADD rounding.
std::uint32_t wrap_half_turn(const std::uint32_t difference) {
    const std::uint32_t sign_one = (difference & 0x80000000U) | kOne;   // +1.0 or -1.0, with the difference's sign
    const std::uint32_t accumulator = fpu::add(0U, difference);         // adda.s
    return fpu::madd(accumulator, kFullTurn, fpu::neg(sign_one));       // madd.s
}

template <class Context>
n3::Run step_damped_spring_to_target(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t step = cpu.read_fpr_bits(12U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // The position P, from the quad at +160 and the words at +192, +196 and +200.
    const guest::Gpr128 quad = n3::load_quad_any(m, object + kPositionQuad);
    guest::Gpr128 position = vadd_lane(cpu, guest::Gpr128{}, n3::kVf0, quad, kDestX, kLaneW);
    position = vadd_lane(cpu, position, n3::kVf0, scalar(m.read_u32_le(object + kPositionY)), kDestY, kLaneX);
    position = vadd_lane(cpu, position, n3::kVf0, scalar(m.read_u32_le(object + kPositionZ)), kDestZ, kLaneX);
    position = vmul_lane(cpu, position, n3::kVf0, scalar(m.read_u32_le(object + kPositionW)), kDestW, kLaneX);

    // The target R: P itself, or (flag set) the quad at +384 whose y lane is the unwrapped angle.
    guest::Gpr128 target = position;
    std::uint32_t cost = kCostUntargeted;
    if (m.read_u8(object + kFlag) != 0U) {
        const std::uint32_t angle = m.read_u32_le(object + kPositionY);
        const std::uint32_t difference = fpu::sub(m.read_u32_le(object + kTargetAngle), angle);
        std::uint32_t wrapped = difference;
        cost = kCostNearTarget;
        if (!fpu::lt(fpu::abs(difference), kHalfTurn)) {
            wrapped = wrap_half_turn(difference);
            cost = kCostWrapped;
        }
        const guest::Gpr128 far_quad = n3::load_quad_any(m, object + kTargetQuad);
        target = vadd_lane(cpu, guest::Gpr128{}, n3::kVf0, far_quad, kDestX, kLaneW);
        target = vadd_lane(cpu, target, n3::kVf0, scalar(fpu::add(angle, wrapped)), kDestY, kLaneX);
        target = vadd_lane(cpu, target, n3::kVf0, scalar(m.read_u32_le(object + kTargetZ)), kDestZ, kLaneX);
        target = vmul_lane(cpu, target, n3::kVf0, scalar(m.read_u32_le(object + kTargetW)), kDestW, kLaneX);
    }

    // The spring constants (FPU): rate = 2 / G3, s = rate * step, accumulator = 1 + s + G2 s^2, and
    // inverse = 1 / (accumulator + G1 s^3). The accumulator is left in the FPU accumulator, as the original does.
    const std::uint32_t rate = fpu::div(kTwo, m.read_u32_le(gp - kGlobalG3));
    const std::uint32_t s = fpu::mul(rate, step);
    const std::uint32_t g2s = fpu::mul(m.read_u32_le(gp - kGlobalG2), s);
    const std::uint32_t g1s2 = fpu::mul(fpu::mul(m.read_u32_le(gp - kGlobalG1), s), s);
    std::uint32_t accumulator = fpu::add(0U, fpu::add(kOne, s));
    accumulator = fpu::madd(accumulator, g2s, s);
    const std::uint32_t inverse = fpu::div(kOne, fpu::madd(accumulator, s, g1s2));
    cpu.set_fpu_accumulator_bits(accumulator);

    // The VU0 step, one op at a time, in the original's order (each publishes its flags).
    const guest::Gpr128 velocity = n3::load_quad_any(m, object + kVelocityQuad);
    const guest::Gpr128 offset = vadd(cpu, guest::Gpr128{}, position, target, kDestXyzw, true);
    const guest::Gpr128 scaled = vmul_lane(cpu, guest::Gpr128{}, offset, scalar(rate), kDestXyzw, kLaneX);
    const guest::Gpr128 pull = vadd(cpu, guest::Gpr128{}, velocity, scaled, kDestXyzw, false);
    const guest::Gpr128 moved = vmul_lane(cpu, guest::Gpr128{}, pull, scalar(step), kDestXyzw, kLaneX);
    const guest::Gpr128 rebound = vmul_lane(cpu, guest::Gpr128{}, moved, scalar(rate), kDestXyzw, kLaneX);
    const guest::Gpr128 residual = vadd(cpu, guest::Gpr128{}, velocity, rebound, kDestXyzw, true);
    const guest::Gpr128 carry = vadd(cpu, guest::Gpr128{}, offset, moved, kDestXyzw, false);
    const guest::Gpr128 velocity_next =
        vmul_lane(cpu, guest::Gpr128{}, residual, scalar(inverse), kDestXyzw, kLaneX);
    const guest::Gpr128 flow = vmul_lane(cpu, guest::Gpr128{}, carry, scalar(inverse), kDestXyzw, kLaneX);
    const guest::Gpr128 next = vadd(cpu, guest::Gpr128{}, target, flow, kDestXyzw, false);
    const guest::Gpr128 next_quad = vmul_lane(cpu, quad, n3::kVf0, next, kDestW, kLaneX);

    // The stores, in the original's order.
    n3::store_quad_any(m, object + kVelocityQuad, velocity_next);
    n3::store_quad_any(m, object + kPositionQuad, next_quad);
    m.write_u32_le(object + kPositionY, next.words[1]);
    m.write_u32_le(object + kPositionZ, next.words[2]);
    m.write_u32_le(object + kPositionW, next.words[3]);

    // The registers the original leaves behind. VF3 is t1 after mfc1 of inverse: mfc1 writes the low doubleword of t1
    // and keeps its high words, so VF3's y word is the sign extension and its z and w words are t1's entry words.
    guest::Gpr128 vf3 = cpu.read_gpr(9U);
    vf3.words[0] = inverse;
    vf3.words[1] = (inverse & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(1U, next);
    cpu.write_vu0_vector(2U, next_quad);
    cpu.write_vu0_vector(3U, vf3);
    cpu.write_vu0_vector(4U, target);
    cpu.write_vu0_vector(5U, offset);
    cpu.write_fpr_bits(0U, next.words[3]);
    return call.finish(cost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003580b0, 496U, 0xb168f1dd02b7719fULL, "StepDampedSpringToTarget", 122U, step_damped_spring_to_target,
               EngineReturn::Nothing)
