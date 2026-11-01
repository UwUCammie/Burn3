// 0x00269e80 (a0 record of four 32-bit words, a1 a delta): adds a1 to the words at
// +8, +12, +0 and +4, in that order (each a lw, addu and sw). Straight line: no
// branch, no call and no result. The original leaves its last sum in $v1, a
// temporary, so the native does not reproduce that register.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

void add_to_word(auto& memory, const GuestAddress address, const std::uint32_t delta) {
    memory.write_u32_le(address, memory.read_u32_le(address) + delta);
}

template <class Context>
n3::Run offset_four_words(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t delta = call.arg(1);
    add_to_word(m, record + 8U, delta);
    add_to_word(m, record + 12U, delta);
    add_to_word(m, record, delta);
    add_to_word(m, record + 4U, delta);
    return call.finish(13U);
}

} // namespace

extern constinit const EngineFunction n3_00269e80{0x00269e80U, 52U, 0x8aa3f76003234708ULL, "OffsetFourWords", 13U,
                                           n3::live<&offset_four_words<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00269e80, offset_four_words)
