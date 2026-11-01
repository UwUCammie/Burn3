// 0x00269ef0 (a0 = pointer W to two adjacent words, a1 = 32-bit amount): AddOffsetToWordPair. Adds a1 to the word at
// W+0 and then to the word at W+4, each as lw / addu / sw, in that order: read W+0, write W+0, read W+4, write W+4. The
// add wraps. There are no branches and no loops: the original retires 7 instructions on every path (the last sw sits in
// the delay slot of jr $ra). v0 is never written and the function returns nothing; v1 is scratch, so the native leaves it
// alone.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Retired instruction count of the only exit, delay slot included (brief.py: 0x00269ef0-0x00269f08).
constexpr std::uint32_t kRetired = 7U;

template <class Context>
n3::Run add_offset_to_word_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pair = call.arg(0);
    const std::uint32_t amount = call.arg(1);
    const std::uint32_t first = m.read_u32_le(pair) + amount;
    m.write_u32_le(pair, first);
    const std::uint32_t second = m.read_u32_le(pair + 4U) + amount;
    m.write_u32_le(pair + 4U, second);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00269ef0{0x00269ef0U, 28U, 0xe7c4737813501e68ULL, "AddOffsetToWordPair", 7U,
                                                  n3::live<&add_offset_to_word_pair<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00269ef0, add_offset_to_word_pair)
