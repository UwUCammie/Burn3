// 0x003fba10 (a0 record, a1 flag): bit 2 (0x04) of the byte at a0+232 is set
// when a1 is nonzero and cleared when a1 is zero. The byte is read and written
// back on both paths, even when the bit already has the requested value.
// No return value.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFlagOffset = 232U;   // lbu/sb $x, 232($a0)
constexpr std::uint8_t kBit2 = 0x04U;         // ori $v1, $v1, 0x0004
constexpr std::uint8_t kBit2Clear = 0xfbU;    // andi $v1, $v1, 0x00fb

template <class Context>
n3::Run set_flag_bit2(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress flag = call.arg(0) + kFlagOffset;
    // beq $a1,$zero compares the whole 64-bit register on the R5900.
    const bool set_bit = call.arg64(1) != 0U;
    const std::uint8_t byte = m.read_u8(flag);
    const auto value = set_bit ? static_cast<std::uint8_t>(byte | kBit2)
                               : static_cast<std::uint8_t>(byte & kBit2Clear);
    m.write_u8(flag, value);
    // Set path: beq (not taken), delay slot, lbu, ori, beq $zero,$zero, its sb
    // delay slot, jr, delay slot = 8. Clear path: beq (taken), delay slot, lbu,
    // andi, sb, jr, delay slot = 7.
    return call.finish(set_bit ? 8U : 7U);
}

} // namespace

extern constinit const EngineFunction n3_003fba10{0x003fba10U, 44U, 0xfeaae012f98190d2ULL, "SetFlagBit2", 8U,
                                                  n3::live<&set_flag_bit2<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003fba10, set_flag_bit2)
