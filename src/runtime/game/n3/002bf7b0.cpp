// 0x002bf7b0 (a0 record): RelocateSelfRelativePair. The record's first two words
// (+0 and +4) are offsets measured from the record itself; each becomes an absolute
// guest address in place: [a0+0] = a0 + [a0+0], then [a0+4] = a0 + [a0+4] (32-bit
// wrapping). The second store sits in the jr delay slot and retires with the jump, so
// the path is 7 instructions. No result: $v1 is a scratch temporary and is not written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run relocate_self_relative_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t first = record + m.read_u32_le(record);
    m.write_u32_le(record, first);
    const std::uint32_t second = record + m.read_u32_le(record + 4U);
    m.write_u32_le(record + 4U, second);
    return call.finish(7U);
}

} // namespace

extern constinit const EngineFunction n3_002bf7b0{0x002bf7b0U, 28U, 0x0ef119c0a2c42300ULL, "RelocateSelfRelativePair", 7U,
                                           n3::live<&relocate_self_relative_pair<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002bf7b0, relocate_self_relative_pair)
