// 0x00103d20 (WordTableLookup10): a bounds-checked read of the 10-word table at
// 0x00483f70. The original compares the full 64-bit a0 (sltiu is a 64-bit
// unsigned compare), so an index with any high bit set takes the zero path.
// In range: v0 = table[a0], loaded with lw (sign-extended), and v1 holds the
// word's address. Out of range: v0 = 0 and v1 is left alone. Only the in-range
// path reads memory. No stores, no calls, no loops: 8 retired instructions in
// range, 5 out of range (delay slots included).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTable = 0x00483f70U;  // lui 0x48, addiu 16240
constexpr std::uint64_t kEntries = 10U;

template <class Context>
n3::Run word_table_lookup10(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t index = call.arg64(0U);
    if (index >= kEntries) {
        call.return_value64(0U);
        return call.finish(5U);
    }
    const auto slot = static_cast<std::uint32_t>(index);
    const GuestAddress word_address = kTable + (slot << 2U);
    call.cpu().write_gpr_sign_extended_32(3U, word_address);
    call.return_value(call.memory().read_u32_le(word_address));
    return call.finish(8U);
}

} // namespace

extern constinit const EngineFunction n3_00103d20{0x00103d20U, 40U, 0x95666ba8bad81bb2ULL, "WordTableLookup10", 8U,
                                                  n3::live<&word_table_lookup10<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00103d20, word_table_lookup10)
