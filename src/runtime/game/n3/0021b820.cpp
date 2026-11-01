// 0x0021b820 (8 bytes, leaf): EmptyStub. The whole function is `jr $ra; nop`. It reads no argument and no guest
// memory, writes nothing and returns nothing: v0 and v1 are left as they were. Its only path retires 2 instructions,
// the jr and its delay slot. GuestCall::finish sets the PC from $ra, which is the jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run empty_stub(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_0021b820{0x0021b820U, 8U, 0x589026cfbec22f44ULL, "EmptyStub", 2U,
                                                  n3::live<&empty_stub<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021b820, empty_stub)
