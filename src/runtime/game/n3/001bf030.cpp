// 0x001bf030 (a0 = object O, a1 = mode selector M): AddScaledDirectionByMode. For M - 12 in 0..8 (unsigned)
// the word at 0x004b7d50 + 4 * (M - 12) is a jump-table target. Its case body computes
// s = (w / divisor) * (k * O.speed), where w is the word 3 of the quadword at O+176 (the float at O+188),
// divisor is the float at $gp-30780 and k is the float at $gp-30892, $gp-30776 or $gp-30772, or -1.5 for the
// body at 0x001bf120. Then O.pos.xyz += O.dir.xyz * s on VU0 (the w lanes are kept) and the position is stored
// back at O+240. Other selectors only store the quadword to the frame and return. Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kJumpTable = 0x004b7d50U;   // lui 0x4b, addiu 32080: one word per selector
constexpr std::uint32_t kSelectorBias = 12U;       // addiu $v1, $a1, -12
constexpr std::uint32_t kSelectorCount = 9U;       // sltiu $at, $v1, 9
constexpr std::uint32_t kFrameBytes = 32U;         // addiu $sp, $sp, -32
constexpr std::uint32_t kDivisorOffset = 30780U;   // lwc1 $f2, -30780($gp)
constexpr GuestAddress kWordOffset = 176U;         // lq $a2, 176($a0)
constexpr GuestAddress kDirectionOffset = 192U;    // lqc2 $vf2, 192($a0)
constexpr GuestAddress kPositionOffset = 240U;     // lqc2 $vf1, 240($a0)
constexpr GuestAddress kSpeedOffset = 496U;        // lwc1 $f0, 496($a0)
constexpr std::uint32_t kMinusOneAndAHalf = 0xbfc00000U;

// The four case bodies a jump-table target can name (the release image's words at kJumpTable).
constexpr GuestAddress kCaseGp30892 = 0x001bf06cU;          // selector 5: k = float at $gp-30892
constexpr GuestAddress kCaseGp30776 = 0x001bf0a8U;          // selectors 0 and 1: k = float at $gp-30776
constexpr GuestAddress kCaseGp30772 = 0x001bf0e4U;          // selectors 2 and 3: k = float at $gp-30772
constexpr GuestAddress kCaseMinusOneAndAHalf = 0x001bf120U; // selectors 4, 6, 7 and 8: k = -1.5

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kOutOfRangeCost = 10U;
constexpr std::uint32_t kInRangeCost = 32U;

bool is_case_body(const GuestAddress target) {
    return target == kCaseGp30892 || target == kCaseGp30776 || target == kCaseGp30772 ||
           target == kCaseMinusOneAndAHalf;
}

// The bits of k for the case body a target names.
std::uint32_t case_factor(const auto& memory, const std::uint32_t gp, const GuestAddress target) {
    switch (target) {
    case kCaseGp30892:
        return memory.read_u32_le(gp - 30892U);
    case kCaseGp30776:
        return memory.read_u32_le(gp - 30776U);
    case kCaseGp30772:
        return memory.read_u32_le(gp - 30772U);
    case kCaseMinusOneAndAHalf:
        return kMinusOneAndAHalf;
    default:
        break;
    }
    // call_cost declines every other target, so the original runs it and this is never reached.
    throw std::logic_error("AddScaledDirectionByMode: jump-table target has no case body");
}

// A target no case body has is declined (0), so the original code runs that call.
template <class Context>
std::uint32_t add_scaled_direction_by_mode_cost(const Context& context) {
    const std::uint32_t selector = context.cpu.read_gpr_u32(5U) - kSelectorBias;
    if (selector >= kSelectorCount) {
        return kOutOfRangeCost;
    }
    return is_case_body(context.memory.read_u32_le(kJumpTable + 4U * selector)) ? kInRangeCost : 0U;
}

template <class Context>
n3::Run add_scaled_direction_by_mode(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t selector = call.arg(1) - kSelectorBias;
    const bool in_range = selector < kSelectorCount;

    // lq $a2, 176($a0); sq $a2, 0($a1) with $a1 = $sp; lwc1 $f3, 12($sp) reads the stored word back.
    const guest::Gpr128 quad = n3::load_quad_any(m, object + kWordOffset);
    cpu.write_gpr(6U, quad);
    n3::store_quad_any(m, frame, quad);
    const std::uint32_t f3 = m.read_u32_le(frame + 12U);
    cpu.write_fpr_bits(3U, f3);
    cpu.write_gpr_sign_extended_32(3U, selector);  // addiu $v1, $a1, -12
    cpu.write_gpr_sign_extended_32(5U, frame);     // addiu $a1, $sp, 0
    cpu.write_gpr_u64(1U, in_range ? 1U : 0U);     // sltiu $at, $v1, 9
    if (!in_range) {
        return call.finish(kOutOfRangeCost);
    }

    cpu.write_gpr_sign_extended_32(5U, kJumpTable);                         // lui/addiu $a1
    const GuestAddress target = m.read_u32_le(kJumpTable + 4U * selector);  // lw $v1, 0($v1)
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t quotient = fpu::div(f3, m.read_u32_le(gp - kDivisorOffset));  // div.s $f2, $f3, $f2
    const std::uint32_t factor = case_factor(m, gp, target);
    const std::uint32_t scale = fpu::mul(factor, m.read_u32_le(object + kSpeedOffset));  // mul.s $f0, $f1, $f0
    const std::uint32_t s = fpu::mul(quotient, scale);                                    // mul.s $f0, $f2, $f0
    cpu.write_fpr_bits(1U, factor);
    cpu.write_fpr_bits(2U, quotient);
    cpu.write_fpr_bits(0U, s);

    cpu.write_vu0_vector(2U, n3::load_quad_any(m, object + kDirectionOffset));  // lqc2 $vf2, 192($a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kPositionOffset));   // lqc2 $vf1, 240($a0)
    // qmtc2 $v1, $vf3 copies the whole register: the low doubleword is s (mfc1 sign-extends it), the high
    // doubleword is the caller's, which no 32-bit instruction above writes.
    guest::Gpr128 v1 = cpu.read_gpr(3U);
    v1.words[0] = s;
    v1.words[1] = (s & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(3U, v1);
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, n3::kMaskXyz, 0U);                       // vmulx.xyz $vf2, $vf2, $vf3x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, false);                       // vadd.xyz $vf1, $vf1, $vf2
    n3::store_quad_any(m, object + kPositionOffset, cpu.read_vu0_vector(1U));            // sqc2 $vf1, 240($a0)
    cpu.write_gpr_sign_extended_32(3U, s);                                            // mfc1 $v1, $f0
    return call.finish(kInRangeCost);
}

} // namespace

extern constinit const EngineFunction n3_001bf030{0x001bf030U, 308U, 0xb1384a9c7b9da34cULL,
                                                  "AddScaledDirectionByMode", kInRangeCost,
                                                  n3::live<&add_scaled_direction_by_mode<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &add_scaled_direction_by_mode_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001bf030, add_scaled_direction_by_mode, add_scaled_direction_by_mode_cost)
