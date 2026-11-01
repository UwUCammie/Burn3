// 0x00293530 (8 bytes, leaf): ReturnConstant11940000. The whole function is `jr $ra ; lui $v0, 0x1194`. The lui sits
// in the delay slot of the jr, so it executes on every call: v0 = 0x11940000. That value is positive, so the 64-bit
// register is 0x0000000011940000 (return_value sign-extends from 32 bits, which is the same). It reads no argument and
// no guest memory, writes no memory and no other register. Both instructions retire on the only path: 2.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

template <class Context>
n3::Run return_constant_11940000(Context& context) {
    auto call = n3::guest_call(context);
    // lui $v0, 0x1194 in the delay slot of jr $ra.
    call.return_value(0x11940000U);
    // jr $ra and the lui in its delay slot: both retire on every path.
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00293530{0x00293530U, 8U, 0x53b1365e9a87956dULL, "ReturnConstant11940000", 2U,
                                                  n3::live<&return_constant_11940000<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00293530, return_constant_11940000)
