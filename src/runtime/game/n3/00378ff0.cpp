// 0x00378ff0 (a0 = record): ZeroWordPair. Clears the first two words of the record: sw $zero at +0 (the
// first instruction), then sw $zero at +4 (the jr $ra delay slot, which runs before the return). Both stores
// are required. Nothing is read, and no result is produced (v0 and f0 are not written).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run zero_word_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    m.write_u32_le(record + 0U, 0U);
    m.write_u32_le(record + 4U, 0U);
    return call.finish(3U);
}

} // namespace

extern constinit const EngineFunction n3_00378ff0{0x00378ff0U, 12U, 0x0cc97abc0440a340ULL, "ZeroWordPair", 3U,
                                                  n3::live<&zero_word_pair<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00378ff0, zero_word_pair)
