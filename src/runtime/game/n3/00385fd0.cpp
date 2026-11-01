// 0x00385fd0 (a0 object): IsFlagWordOne, a leaf predicate. Returns in v0 whether the 32-bit word at a0+464 is
// exactly 1 (1), or anything else (0). The original is `lw; xori 1; jr ra; sltiu 1` (the sltiu sits in the jr's
// delay slot): v0 ends as ((word ^ 1) == 0), which is word == 1. Plain 32-bit equality is exact. The lw sign-extends
// into the 64-bit register, but only a word equal to 1 can make the sltiu produce 1, so 0xFFFFFFFF gives 0.
// Only v0 is written and nothing is stored. The one path retires 4 instructions (lw, xori, jr, sltiu).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFlagWordOffset = 464U;

template <class Context>
n3::Run is_flag_word_one(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t word = m.read_u32_le(object + kFlagWordOffset);
    call.return_value(word == 1U ? 1U : 0U);
    return call.finish(4U);
}

} // namespace

extern constinit const EngineFunction n3_00385fd0{0x00385fd0U, 16U, 0x4206e77dc4e0e245ULL, "IsFlagWordOne", 4U,
                                           n3::live<&is_flag_word_one<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00385fd0, is_flag_word_one)
