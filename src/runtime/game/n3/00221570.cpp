// 0x00221570 (a0 = object, 8 bytes, leaf): ClearField1CC. The whole function is `jr $ra; sw $zero, 460($a0)`:
// it stores the word 0 at a0 + 460 (0x1cc) in the jr's delay slot, then returns. It writes no register and makes no
// call, so the caller reads no result (EngineReturn::Nothing). Its only path retires 2 instructions, the jr and its
// delay slot, so the declared cost is 2. GuestCall::finish sets the PC from $ra, which is the jr's target.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFieldOffset = 460U;  // sw $zero, 460($a0)

template <class Context>
n3::Run clear_field_1cc(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    m.write_u32_le(object + kFieldOffset, 0U);
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00221570{0x00221570U, 8U, 0xdb730431c35e1cb7ULL, "ClearField1CC", 2U,
                                                  n3::live<&clear_field_1cc<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00221570, clear_field_1cc)
