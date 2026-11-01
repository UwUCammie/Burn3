// 0x00279550 (a0 = object record): GetField272. The whole routine is `lw $v0, 272($a0)` in the delay slot of
// `jr $ra`, two retired instructions with no loop, store or call. Like lw, it writes only the low doubleword of v0
// (the 32-bit word, sign-extended); GuestCall::return_value leaves bits 64..127 of v0 as they were, so a caller that
// stores the full quadword sees the same bits as with the original.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run get_field_272(Context& context) {
    auto call = n3::guest_call(context);
    // The address is a0 + 272 with 32-bit wrap-around, as the interpreter's effective address does it.
    const auto field = call.memory().read_u32_le(call.arg(0) + 272U);
    call.return_value(field);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00279550{0x00279550U, 8U, 0x5f2a0a55258dae49ULL, "GetField272", 2U,
                                                  n3::live<&get_field_272<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00279550, get_field_272)
