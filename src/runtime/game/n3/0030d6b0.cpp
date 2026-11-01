// 0x0030d6b0 (a0 = S, a1 = E, a2 = step): AppendEntryToTimeline, a leaf with no result. S is a sequencer record and
// E an entry being appended: S+468 is a running cursor (a word) and S+472 the last entry appended (a pointer).
// The original, in listing order: halfword E+8 = the low 16 bits of the cursor; S+468 = the cursor read again
// + step (32-bit wrap); E+4 = the old S+472; S+472 = E (the jr delay slot). The second read of the cursor is
// deliberate (E can alias S), so every access is made in the same order through the same accessors.
// Straight-line code with no call: nine retired instructions on its only path, delay slot included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCursor = 468U;      // word at S + 468
constexpr GuestAddress kLastEntry = 472U;   // word at S + 472
constexpr GuestAddress kEntryLink = 4U;     // word at E + 4
constexpr GuestAddress kEntryTag = 8U;      // halfword at E + 8

constexpr std::uint32_t kRetired = 9U;      // the nine instructions of the listing, jr delay slot included

template <class Context>
n3::Run append_entry_to_timeline(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress sequencer = call.arg(0);
    const GuestAddress entry = call.arg(1);
    const std::uint32_t step = call.arg(2);

    const std::uint32_t cursor = m.read_u32_le(sequencer + kCursor);
    m.write_u16_le(entry + kEntryTag, static_cast<std::uint16_t>(cursor));
    const std::uint32_t advanced = m.read_u32_le(sequencer + kCursor) + step;
    m.write_u32_le(sequencer + kCursor, advanced);
    const std::uint32_t previous = m.read_u32_le(sequencer + kLastEntry);
    m.write_u32_le(entry + kEntryLink, previous);
    m.write_u32_le(sequencer + kLastEntry, entry);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0030d6b0{0x0030d6b0U, 36U, 0xe4b400b5a5ced5fdULL, "AppendEntryToTimeline", 9U,
                                                   n3::live<&append_entry_to_timeline<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030d6b0, append_entry_to_timeline)
