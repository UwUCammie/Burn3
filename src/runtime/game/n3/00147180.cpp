// 0x00147180 (AdvanceSequenceIndex): steps a sequence cursor. $a0 is the object (its word at +4 points to a
// sequence header P), $a1 points at the u16 cursor, and the low byte of $a2 is a signed step. The cursor plus the
// step is stored when it lies in 0 .. count-1 (the u16 count at P+12), and $a0 is returned. Otherwise the byte at
// P+14 decides. Set, the sum is taken modulo the count, stored, and $a0 is returned. Clear, the 10-byte entry of the
// cursor's ORIGINAL value (entries at P+8) names a link: byte +4 of the entry is a row of the table at 0x00522660
// (8-byte rows, the address 44 bytes into the row) unless it is 255, and then byte +5 is tried the same way. 255
// again gives 0. Nothing is stored on the link path.
//
// Retired counts follow the listing's blocks, delay slots included: 16 for an in-range step; 19 or 24 for a wrap
// (candidate below zero or at/after the count); 30 or 35 for a first link; 41 to 47 for a second link or none.
// The declared cost is the longest route through the listing. Its route through 0x001471f0 with the test at
// 0x00147208 not taken cannot run (the row address is never zero there), so it is an upper bound only.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kLinkTable = 0x00522660U;  // lui 0x52, addiu 9824
constexpr std::uint32_t kLinkRowStride = 8U;
constexpr std::uint32_t kLinkRowOffset = 44U;
constexpr std::uint32_t kEntryStride = 10U;
constexpr std::uint8_t kNoLink = 0xffU;

constexpr GuestAddress kHeaderEntries = 8U;  // u32: pointer to the 10-byte entries
constexpr GuestAddress kHeaderCount = 12U;   // u16: number of entries
constexpr GuestAddress kHeaderWrap = 14U;    // byte: nonzero when the cursor wraps

// The address of link row `link` in the table (8-byte rows, the address 44 bytes into the row).
GuestAddress link_row(const std::uint32_t link) {
    return kLinkTable + link * kLinkRowStride + kLinkRowOffset;
}

template <class Context>
n3::Run advance_sequence_index(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress cursor = call.arg(1);
    // dsll32/dsra32 sign-extend bits 0-7 of $a2, and addu adds the step to the cursor in 32 bits.
    const auto step = static_cast<std::int32_t>(static_cast<std::int8_t>(call.arg(2) & 0xffU));
    const std::uint32_t index = m.read_u16_le(cursor);
    const std::uint32_t candidate = index + static_cast<std::uint32_t>(step);
    const GuestAddress header = m.read_u32_le(object + 4U);

    // Entry block (6 retired); the range block (5) only when the candidate is not negative.
    std::uint32_t retired = 6U;
    if (static_cast<std::int32_t>(candidate) >= 0) {
        retired += 5U;
        const auto count = static_cast<std::int32_t>(m.read_u16_le(header + kHeaderCount));
        if (static_cast<std::int32_t>(candidate) < count) {
            m.write_u16_le(cursor, static_cast<std::uint16_t>(candidate));
            call.return_value64(call.arg64(0));
            return call.finish(retired + 3U + 2U);  // the store and jump, then the return
        }
    }

    // Wrap test block (4 retired). A set wrap flag stores the candidate modulo the count.
    retired += 4U;
    if (m.read_u8(header + kHeaderWrap) != 0U) {
        const std::uint32_t count = m.read_u16_le(header + kHeaderCount);
        // The original divides candidate + count by count and stores HI. A zero count leaves the dividend in
        // HI, as the interpreter's DIV does.
        const auto dividend = static_cast<std::int32_t>(candidate + count);
        const std::uint32_t remainder =
            count == 0U ? static_cast<std::uint32_t>(dividend)
                        : static_cast<std::uint32_t>(dividend % static_cast<std::int32_t>(count));
        m.write_u16_le(cursor, static_cast<std::uint16_t>(remainder));
        call.return_value64(call.arg64(0));
        return call.finish(retired + 7U + 2U);  // the remainder block, then the return
    }

    // Link path, from the entry of the ORIGINAL index (the candidate is not used here). Entries block (10).
    retired += 10U;
    const GuestAddress entries = m.read_u32_le(header + kHeaderEntries);
    const GuestAddress entry = entries + index * kEntryStride;
    const std::uint8_t first_link = m.read_u8(entry + 4U);
    if (first_link != kNoLink) {
        static_cast<void>(m.read_u16_le(entry + 6U));  // loaded by the original, never used
        retired += 6U + 2U + 2U;                        // row address, its test (taken), the return
        call.return_value(link_row(first_link));
        return call.finish(retired);
    }
    retired += 2U + 2U + 9U;  // jump to the second test, its test (not taken), and the second link load
    const std::uint8_t second_link = m.read_u8(entry + 5U);
    if (second_link != kNoLink) {
        retired += 5U + 2U + 2U;  // second row address, its test (taken), the return
        call.return_value(link_row(second_link));
    } else {
        retired += 2U + 2U + 2U + 2U;  // jump, test (not taken), jump, the return
        call.return_value(0U);
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00147180{0x00147180U, 268U, 0x8b49afd4c4e27355ULL, "AdvanceSequenceIndex",
                                                  53U, n3::live<&advance_sequence_index<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00147180, advance_sequence_index)
