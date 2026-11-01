// 0x0014e860 (a0 object): LinkedObjectValueOrZero, a leaf getter. When the word at +1376 is non-zero (a link is
// set) it follows the pointer at +1380 and returns in v0 the word at that pointer's +952, sign-extended as lw
// does. Otherwise it returns 0. v1 ends with the flag word on both paths, as the original leaves it. Every captured
// call passes a0 = 0x006481A0 (a fixed singleton); any object is read through a0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kV1 = 3U;
constexpr GuestAddress kLinkFlag = 1376U;
constexpr GuestAddress kLinkPointer = 1380U;
constexpr GuestAddress kLinkedWord = 952U;

// Retired instruction counts from the listing, delay slots included. Flag clear: lw, beq (taken), its daddu
// delay slot, jr, nop. Flag set: lw, beq (not taken), daddu, lw, lw, jr, nop. The daddu always executes; on the
// flag-set path the next lw overwrites v0 with the pointer.
constexpr std::uint32_t kClearCost = 5U;
constexpr std::uint32_t kSetCost = 7U;

template <class Context>
n3::Run linked_object_value_or_zero(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t flag = m.read_u32_le(object + kLinkFlag);
    call.cpu().write_gpr_sign_extended_32(kV1, flag);
    if (flag == 0U) {
        call.return_value(0U);
        return call.finish(kClearCost);
    }
    // No null check on the pointer: the original dereferences it unconditionally when the flag is set.
    const GuestAddress linked = m.read_u32_le(object + kLinkPointer);
    call.return_value(m.read_u32_le(linked + kLinkedWord));
    return call.finish(kSetCost);
}

} // namespace

extern constinit const EngineFunction n3_0014e860{0x0014e860U, 28U, 0x5246765bc7f6b0b8ULL,
                                                  "LinkedObjectValueOrZero", kSetCost,
                                                  n3::live<&linked_object_value_or_zero<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0014e860, linked_object_value_or_zero)
