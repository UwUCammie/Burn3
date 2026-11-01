// 0x0013c880 (8 bytes, leaf): NoOpReturn. The whole function is `jr $ra; nop`. It reads no argument and no
// guest memory, writes no register and no memory, and returns nothing: v0 and v1 are left as they were. Its only path
// retires 2 instructions, the jr and its delay slot. GuestCall::finish sets the PC from $ra, which is the jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run no_op_return_0013c880(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_0013c880{0x0013c880U, 8U, 0x589026cfbec22f44ULL, "NoOpReturn", 2U,
                                                  n3::live<&no_op_return_0013c880<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0013c880, no_op_return_0013c880)
