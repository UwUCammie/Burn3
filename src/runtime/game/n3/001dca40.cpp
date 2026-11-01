// 0x001dca40 (SpliceListOntoFreeList, leaf, void). Circular doubly-linked
// lists: next at +0, prev at +4, and an empty list's sentinel points to itself.
// The list record H = (word at $gp-24320) + 0x00670bd0 holds the source
// sentinel A at H+32 and the destination sentinel D at H+36. The source's
// nodes are spliced onto the end of D (between D.prev and D), then A is made
// empty again, and H is rewritten: H+36 = A, H+32 = D, H+8 = 0. The nodes may
// alias, so every read and store below keeps the listing's order.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Distance below $gp of the word that holds the list record base.
constexpr std::uint32_t kBelowGpBase = 24320U;
// lui 0x67 + addiu 3024 form the record offset from the base.
constexpr std::uint32_t kRecordOffset = 0x00670bd0U;

template <class Context>
n3::Run splice_list_onto_free_list(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress base_word = call.cpu().read_gpr_u32(n3::kGlobalPointer) - kBelowGpBase;
    const GuestAddress record = m.read_u32_le(base_word) + kRecordOffset;
    const GuestAddress source = m.read_u32_le(record + 32U);
    const GuestAddress dest = m.read_u32_le(record + 36U);
    const GuestAddress first = m.read_u32_le(source);
    // Retired original instructions on the path taken (no loops; delay slots
    // and likely-branch skips counted as the listing executes them).
    std::uint32_t retired = 10U;
    if (first != source) {
        const GuestAddress dest_first = m.read_u32_le(dest);
        if (dest_first != dest) {
            // Destination nonempty: the bnel is taken, so its delay slot (the
            // load of D.prev) runs on this path.
            const GuestAddress dest_last = m.read_u32_le(dest + 4U);
            m.write_u32_le(dest_last, first);
            m.write_u32_le(first + 4U, dest_last);
            const GuestAddress source_last = m.read_u32_le(source + 4U);
            m.write_u32_le(source_last, dest);
            m.write_u32_le(dest + 4U, source_last);
            retired += 3U + 5U;
        } else {
            // Destination empty: the bnel falls through and skips its delay slot.
            m.write_u32_le(dest, first);
            m.write_u32_le(first + 4U, dest);
            const GuestAddress source_last = m.read_u32_le(source + 4U);
            m.write_u32_le(dest + 4U, source_last);
            // The unconditional beq that follows runs its delay-slot store here.
            m.write_u32_le(source_last, dest);
            retired += 2U + 6U;
        }
        // The source becomes empty again.
        m.write_u32_le(source, source);
        m.write_u32_le(source + 4U, source);
        retired += 3U;
    }
    // Record update. The base word is loaded again from $gp-24320 after the
    // stores above, as the listing does.
    const GuestAddress final_record = m.read_u32_le(base_word) + kRecordOffset;
    m.write_u32_le(final_record + 36U, source);
    m.write_u32_le(final_record + 32U, dest);
    m.write_u32_le(final_record + 8U, 0U);
    retired += 6U;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001dca40{0x001dca40U, 132U, 0xf55d085274d499fbULL, "SpliceListOntoFreeList",
                                                  27U, n3::live<&splice_list_onto_free_list<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001dca40, splice_list_onto_free_list)
