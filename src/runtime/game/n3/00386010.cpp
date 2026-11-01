// 0x00386010 (IsObjectStateSix): a leaf predicate on the object at a0. It returns v0 = 1 when the 32-bit word at
// a0 + 464 equals 6, else 0. The original is `lw v0,464(a0); xori v0,v0,6; jr ra; sltiu v0,v0,1`: the lw
// sign-extends the word, the xori leaves zero only when the whole 64-bit value is 6, and the sltiu (a 64-bit unsigned
// compare with 1) turns that zero into 1. A word such as 0x80000006 gives 0, because its sign-extended upper half
// survives the xori. No stores, no calls and no loops: every call retires the same four instructions (the lw, the
// xori, the jr and the sltiu in the jr's delay slot).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lw 464(a0): the object's state word.
constexpr GuestAddress kStateOffset = 464U;
constexpr std::uint32_t kStateSix = 6U;

// lw, xori, jr and the sltiu in its delay slot always retire, so every call counts 4.
constexpr std::uint32_t kCost = 4U;

template <class Context>
n3::Run is_object_state_six(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object_address = call.arg(0U);
    const std::uint32_t state_word = call.memory().read_u32_le(object_address + kStateOffset);
    call.return_value(state_word == kStateSix ? 1U : 0U);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00386010{0x00386010U, 16U, 0x5942562336958e1eULL, "IsObjectStateSix", kCost,
                                                  n3::live<&is_object_state_six<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00386010, is_object_state_six)
