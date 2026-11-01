// 0x0027a8f0 (a0 = object, a1 = value): SetByteField93E. The whole function is `jr $ra` with
// `sb $a1, 2366($a0)` in its delay slot. The delay slot always executes, so the function stores the low byte of a1
// at a0 + 2366 (0x93e) and returns. It writes no register result. Its only path retires 2 instructions: the jr and
// its delay slot. GuestCall::finish sets the PC from $ra.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kFieldOffset = 2366U;  // 0x93e, the immediate of the sb

template <class Context>
n3::Run set_byte_field_93e(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const n3::GuestAddress object = call.arg(0);
    m.write_u8(object + kFieldOffset, static_cast<std::uint8_t>(call.arg(1)));
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_0027a8f0{0x0027a8f0U, 8U, 0xdc6f7591cd221400ULL, "SetByteField93E", 2U,
                                                  n3::live<&set_byte_field_93e<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0027a8f0, set_byte_field_93e)
