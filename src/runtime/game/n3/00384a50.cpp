// 0x00384a50 (a0 object): LoadWordAtArg0. A getter for the first word of the object a0 points to.
// The return is `jr $ra` with `lw $v0, 0($a0)` in its delay slot, so v0 = the word at a0 + 0.
// The delay slot always executes and retires, so a call retires 2 instructions. Returns v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run load_word_at_arg0(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    const std::uint32_t word = call.memory().read_u32_le(object);
    call.return_value(word);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00384a50{0x00384a50U, 8U, 0x56e37ecfbd559bfaULL, "LoadWordAtArg0", 2U,
                                                  n3::live<&load_word_at_arg0<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00384a50, load_word_at_arg0)
