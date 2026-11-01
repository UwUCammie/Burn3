// 0x002a2c80 (a0 = object pointer): ReadField588. A leaf getter: v0 = the 32-bit word at object + 588.
// The lw sits in the delay slot of jr $ra, so it runs on the only path and counts as retired (2 in total).
// No stores, no calls, no loops. return_value sign-extends into v0, as the R5900 lw does.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kField = 588U;  // lw 588(a0)

template <class Context>
n3::Run read_field588(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    call.return_value(call.memory().read_u32_le(object + kField));
    // jr $ra and its delay slot lw: 2 retired instructions.
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_002a2c80{0x002a2c80U, 8U, 0xe6be8706ad601598ULL, "ReadField588", 2U,
                                                  n3::live<&read_field588<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002a2c80, read_field588)
