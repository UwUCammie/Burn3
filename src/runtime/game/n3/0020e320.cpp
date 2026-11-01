// 0x0020e320 (8 bytes, leaf): EmptyStub. The whole function is `jr $ra; nop`. It reads no argument and no
// guest memory, writes no register and no memory, and returns nothing: v0 and v1 are left as they were. Its only
// path retires 2 instructions, the jr and its delay slot. GuestCall::finish sets the PC from $ra, which is the
// jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_stub_0020e320(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_0020e320{0x0020e320U, 8U, 0x589026cfbec22f44ULL, "EmptyStub", 2U,
                                                  n3::live<&empty_stub_0020e320<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0020e320, empty_stub_0020e320)
