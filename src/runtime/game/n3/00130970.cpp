// 0x00130970 (8 bytes, leaf): EmptyStub130970. The whole function is `jr $ra; nop`. It reads no argument and no
// guest memory, writes no register and no memory, and returns nothing: v0 and v1 are left as they were. Its only path
// retires 2 instructions, the jr and its delay slot. GuestCall::finish sets the PC from $ra, which is the jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_stub_130970(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00130970{0x00130970U, 8U, 0x589026cfbec22f44ULL, "EmptyStub130970", 2U,
                                                  n3::live<&empty_stub_130970<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00130970, empty_stub_130970)
