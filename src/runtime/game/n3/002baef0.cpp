// 0x002baef0 RelocateFiveWords (a0 = P): turns five self-relative words at P+0..P+16 into absolute addresses by
// adding P to each (32-bit wrap). Leaf; writes nothing else, returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kWordCount = 5U;
constexpr std::uint32_t kRetired = 16U;  // 0x002baef0..0x002baf2c: five load/add/store triples, jr and its delay slot

template <class Context>
n3::Run relocate_five_words(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress base = call.arg(0);
    for (std::uint32_t k = 0U; k < kWordCount; ++k) {
        const GuestAddress slot = base + 4U * k;
        m.write_u32_le(slot, m.read_u32_le(slot) + base);
    }
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002baef0, 64U, 0x6e5281f85a08a4c5ULL, "RelocateFiveWords", 16U, relocate_five_words, EngineReturn::Nothing)
