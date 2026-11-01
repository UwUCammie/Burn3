// 0x0029f820 (LookupWordTable): v0 = the word at 0x0049eba0 + (a0 << 2). The shift and the
// add are 32-bit and wrap; the shifted index is also left in v1 (the sll's destination). The lw
// sits in the delay slot of the jr $ra, so all six instructions retire on every call. No stores,
// no calls, no loops, no other globals.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x4a, addiu -5216: the first word of the table.
constexpr GuestAddress kTable = 0x0049eba0U;

template <class Context>
n3::Run lookup_word_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t scaled = call.arg(0U) << 2U;
    call.cpu().write_gpr_sign_extended_32(3U, scaled);
    call.return_value(m.read_u32_le(kTable + scaled));
    return call.finish(6U);
}

} // namespace

extern constinit const EngineFunction n3_0029f820{0x0029f820U, 24U, 0x931d276e5ee51787ULL, "LookupWordTable", 6U,
                                                  n3::live<&lookup_word_table<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0029f820, lookup_word_table)
