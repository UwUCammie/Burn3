// 0x002bc450 (a0 = object, a1 = 32-bit value): SetWordAt304. The whole function is `jr $ra` with
// `sw $a1, 304($a0)` in its delay slot, so it stores a1 into the word at a0 + 304 and returns. No loops, no calls,
// no globals, no stack frame. It returns nothing: v0 is never written. Its only path retires 2 instructions, the jr
// and its delay slot (the store), so the declared cost is 2 and there is no call_cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFieldOffset = 304U;   // sw $a1, 304($a0)
constexpr std::uint32_t kRetired = 2U;        // jr $ra and its delay slot

template <class Context>
n3::Run set_word_at_304(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t value = call.arg(1);
    m.write_u32_le(object + kFieldOffset, value);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002bc450{0x002bc450U, 8U, 0x61db495fef1fa4beULL, "SetWordAt304", 2U,
                                                  n3::live<&set_word_at_304<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002bc450, set_word_at_304)
