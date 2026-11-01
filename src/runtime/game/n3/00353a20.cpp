// 0x00353a20 (8 bytes): EmptyStub, the emptiest routine: `jr $ra` with a `nop` in its delay slot.
// It reads no argument, writes no register and no memory, and makes no call. Both instructions
// retire on the only path, so the count is 2. Callers ignore any result, so the entry is Nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_stub(Context& context) {
    auto call = n3::guest_call(context);
    // jr $ra and its delay-slot nop: both retire on every path.
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00353a20{0x00353a20U, 8U, 0x589026cfbec22f44ULL, "EmptyStub", 2U,
                                                  n3::live<&empty_stub<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00353a20, empty_stub)
