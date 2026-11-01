// 0x003767b0 (12 bytes, leaf): SetHeadWordAndFlagByte. Three instructions, no branches:
//   sw $a1, 0($a0)        the 32-bit value in a1 is stored at the object's +0
//   jr $ra                return
//   sb $a2, 52($a0)       in the delay slot, so it always runs: the low byte of a2 is stored at +52
// Only memory is written. No register is written, so v0, v1 and f0 are left as they were, and the
// return is EngineReturn::Nothing. The path has no branch or loop, so every call retires 3
// instructions: the declared cost is 3 and no call_cost resolver is needed.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run set_head_word_and_flag_byte_003767b0(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    m.write_u32_le(object, call.arg(1));
    m.write_u8(object + 52U, static_cast<std::uint8_t>(call.arg(2) & 0xFFU));
    return call.finish(3U);
}

} // namespace

extern constinit const EngineFunction n3_003767b0{0x003767b0U, 12U, 0xca7f4b344a7b1fbfULL, "SetHeadWordAndFlagByte", 3U,
                                                  n3::live<&set_head_word_and_flag_byte_003767b0<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003767b0, set_head_word_and_flag_byte_003767b0)
