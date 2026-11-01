// 0x002a6650 (a0 = state object, a1 = busy word, f12 = value): TrySetPendingFlagWithValue. Returns 1 in v0 when the
// request was set, 0 when bit 0 of the word at a1 (the busy flag) was already set. The busy word is read first; on
// that early-out path nothing is written. On the set path the flags word at a0+0 gets bit 0x20 (read after the busy
// word, then stored) and the raw bits of f12 are stored at a0+16. A leaf with no calls; v1 is only a temporary and
// callers read v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagsOffset = 0U;           // lw / sw 0($a0): the flags word
constexpr GuestAddress kValueOffset = 16U;          // swc1 $f12, 16($a0): the float
constexpr std::uint32_t kBusyBit = 0x00000001U;     // andi $v0, $v0, 0x0001 on the word at a1
constexpr std::uint32_t kPendingBit = 0x00000020U;  // ori $v1, $v1, 0x0020 on the flags word
constexpr unsigned kFloatArgument = 12U;            // f12

// Retired counts from the listing, delay slots included. The busy exit (8) is the early-out; the set path (11) is the
// longest path and the declared cost. There are no loops and no calls, so no call_cost is needed.
constexpr std::uint32_t kBusyCost = 8U;
constexpr std::uint32_t kSetCost = 11U;

template <class Context>
n3::Run try_set_pending_flag_with_value(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress state = call.arg(0);
    const GuestAddress busy_word = call.arg(1);
    // The busy word is read before anything is written; the early-out path touches no other memory.
    if ((m.read_u32_le(busy_word) & kBusyBit) != 0U) {
        call.return_value(0U);
        return call.finish(kBusyCost);
    }
    const std::uint32_t flags = m.read_u32_le(state + kFlagsOffset);
    m.write_u32_le(state + kFlagsOffset, flags | kPendingBit);
    m.write_u32_le(state + kValueOffset, call.cpu().read_fpr_bits(kFloatArgument));
    call.return_value(1U);
    return call.finish(kSetCost);
}

} // namespace

extern constinit const EngineFunction n3_002a6650{0x002a6650U, 52U, 0xdc9470da734b1071ULL,
                                                  "TrySetPendingFlagWithValue", kSetCost,
                                                  n3::live<&try_set_pending_flag_with_value<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002a6650, try_set_pending_flag_with_value)
