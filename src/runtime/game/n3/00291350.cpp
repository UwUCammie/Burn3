// 0x00291350 (a0 = object, a1 = flag): SetFlagBit0x10. Sets bit 0x10 of the flags word at +84 of the object when a1
// is nonzero and clears it otherwise, storing the word back. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagsOffset = 84U;
constexpr std::uint32_t kFlagBit = 0x10U;
// Both paths retire 8 instructions, delay slots included: 2 for the test on a1, 4 for the body, 2 for jr.
constexpr std::uint32_t kRetiredCost = 8U;

template <class Context>
n3::Run set_flag_bit_0x10(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    // beq compares the whole 64-bit $a1, so the test does too.
    const bool set_bit = call.arg64(1) != 0U;
    const std::uint32_t flags = m.read_u32_le(object + kFlagsOffset);
    m.write_u32_le(object + kFlagsOffset, set_bit ? (flags | kFlagBit) : (flags & ~kFlagBit));
    return call.finish(kRetiredCost);
}

} // namespace

extern constinit const EngineFunction n3_00291350{0x00291350U, 48U, 0x95cdb9b56fe422dbULL, "SetFlagBit0x10", 8U,
                                                  n3::live<&set_flag_bit_0x10<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00291350, set_flag_bit_0x10)
