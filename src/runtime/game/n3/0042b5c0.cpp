// 0x0042b5c0 (8 bytes): NoOpStub, an empty function: `jr $ra` with a `nop` in its delay slot. It reads no
// argument, writes no register and touches no memory, so the native does nothing but return. It reports the two
// instructions the original retires (the jr and its delay slot). Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run no_op_stub(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);  // jr $ra and its delay slot (nop)
}

} // namespace

extern constinit const EngineFunction n3_0042b5c0{0x0042b5c0U, 8U, 0x589026cfbec22f44ULL, "NoOpStub", 2U,
                                                  n3::live<&no_op_stub<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0042b5c0, no_op_stub)
