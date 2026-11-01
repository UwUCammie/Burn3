// 0x0028adc0 (a0 = object): GetObjectWord29196, a getter. Returns the word at object + 29196, read by the lw in the
// jr $ra delay slot. A leaf with no branches, calls or stores, so every call retires the same two instructions. The
// offset is positive and the sum wraps as 32-bit guest address arithmetic. v0 is written sign-extended, as the lw
// writes it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kWordOffset = 29196U;  // lw $v0, 29196($a0)

// jr and its delay slot (the lw) always retire: 2 on every path.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run get_object_word_29196(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t word = call.memory().read_u32_le(call.arg(0) + kWordOffset);
    call.return_value(word);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_0028adc0{0x0028adc0U, 8U, 0x9d09f4f2a87086c8ULL, "GetObjectWord29196",
                                                  kCost, n3::live<&get_object_word_29196<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0028adc0, get_object_word_29196)
