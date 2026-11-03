// 0x0030d560 (a0 = object O, a1 = signed int N, a2 = byte B): SetModeSlotAndNotify.
// Stores N at O+488 (sw), B at O+6655 (sb), the word at $gp-32192 at O+476 (lwc1/swc1) and 3 at O+484.
// It reloads N from O+488 and, when N < 3 (signed), stores the sign-extended byte at 0x004f6060 to O+492
// (lb/sw). On both branches it then calls 0x00285340 with a0 = 0x01e7a800 and returns that call's v0.
//
// Cold decline: every path reaches the jal at 0x0030d594, and the callee at 0x00285340 is not native. The
// replayed captures run it on every call (1452 to 4141 original instructions per call, against 15 to 23 for
// this function's own path), so call_cost returns 0 for every input, the original runs the whole function,
// and this body is never executed natively (the same rule as n3/003096e0.cpp). It keeps the stores and the
// branch exact and finishes at the jal with the count retired up to that jal's delay slot: 15 for N >= 3,
// 19 for N < 3. The full paths are 19 and 23 retired instructions of this function alone (the callee is not
// ours), so the declared cost is the longer one, 23.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kGpFloatOffset = 32192U;         // lwc1 f0, -32192(gp)
constexpr GuestAddress kSignedByteSource = 0x004f6060U;  // lui 0x004f, lb 24672
constexpr std::uint32_t kNumberOffset = 488U;
constexpr std::uint32_t kByteOffset = 6655U;
constexpr std::uint32_t kFloatOffset = 476U;
constexpr std::uint32_t kThreeOffset = 484U;
constexpr std::uint32_t kSignedByteOffset = 492U;

// Retired counts from the listing, delay slots included: the 12 instructions from the prologue through the
// nop after the bne at 0x0030d588, then the N < 3 branch (lui, lb, beq, and the sw in its delay slot) when
// it is taken, then the call setup (lui, jal, and the addiu in the jal's delay slot).
constexpr std::uint32_t kHeadRetired = 12U;
constexpr std::uint32_t kSmallBranchRetired = 4U;
constexpr std::uint32_t kCallSetupRetired = 3U;

template <class Context>
n3::Run set_mode_slot_and_notify(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t number = call.arg(1);
    const auto byte_value = static_cast<std::uint8_t>(call.arg(2));
    // lwc1 reads the word before the stores, as the original does.
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t float_bits = m.read_u32_le(gp - kGpFloatOffset);
    m.write_u32_le(object + kNumberOffset, number);
    m.write_u8(object + kByteOffset, byte_value);
    m.write_u32_le(object + kFloatOffset, float_bits);
    m.write_u32_le(object + kThreeOffset, 3U);
    std::uint32_t retired = kHeadRetired;
    // lw $v0, 488($a0) ; slti $v0, $v0, 3 ; bne taken when N < 3
    if (static_cast<std::int32_t>(m.read_u32_le(object + kNumberOffset)) < 3) {
        // lb $v0, 24672($v0) ; sw $v0, 492($a0) in the delay slot of the beq back to the call setup
        const auto signed_byte = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(kSignedByteSource)));
        m.write_u32_le(object + kSignedByteOffset, static_cast<std::uint32_t>(signed_byte));
        retired += kSmallBranchRetired;
    }
    // lui $a0, 0x01e8 ; jal 0x00285340 ; addiu $a0, $a0, -22528 (delay slot).
    // The call is never made natively: call_cost declines every input, so the original runs it.
    retired += kCallSetupRetired;
    return call.finish(retired);
}

// Every path reaches the jal, and the callee is not native: decline every call.
std::uint32_t set_mode_slot_and_notify_cost(const auto&) {
    return 0U;
}

} // namespace

extern constinit const EngineFunction n3_0030d560{0x0030d560U, 92U, 0x226227c74f7d97d4ULL, "SetModeSlotAndNotify",
                                                  23U, n3::live<&set_mode_slot_and_notify<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6,
                                                  {}, set_mode_slot_and_notify_cost};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030d560, set_mode_slot_and_notify)
