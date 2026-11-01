// 0x001b20c0 (8 bytes, leaf): EmptyCallback. The whole function is `jr $ra; nop`. It reads no argument and no
// guest memory, writes no register and no memory, and returns nothing: v0 and v1 are left as they were. Its only path
// retires 2 instructions, the jr and its delay slot. GuestCall::finish sets the PC from $ra, which is the jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_callback_001b20c0(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_001b20c0{0x001b20c0U, 8U, 0x589026cfbec22f44ULL, "EmptyCallback", 2U,
                                                  n3::live<&empty_callback_001b20c0<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001b20c0, empty_callback_001b20c0)
