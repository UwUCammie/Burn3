// 0x002b72f0 (8 bytes): EmptyStub. The whole routine is `jr $ra` with a `nop` in its delay
// slot. It reads no argument, writes no memory and no register, so the native only returns
// to the caller. Both instructions retire (2 in all), and the result is nothing: v0 and f0
// are left alone, as the original leaves them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_stub(Context& context) {
    auto call = n3::guest_call(context);
    // The jr and its delay-slot nop, the only path.
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_002b72f0{0x002b72f0U, 8U, 0x589026cfbec22f44ULL, "EmptyStub", 2U,
                                                  n3::live<&empty_stub<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b72f0, empty_stub)
