// 0x00164510 (a0 = owner object, a1 = query value, a2 = byte offset): StepTrackSegmentIndex. Word 0 of the
// owner is a header H. The cursor byte at a0 + a2 + 0x363a7 selects an entry of the table at H+164 (24-byte
// entries, count at H+168, signed start word at +4 and end word at +8). The cursor moves one entry towards
// the query, or wraps from the last entry to the first (query 0) or from the first to the last (query equal
// to the last entry's end word). The function returns 1 when the cursor moved and 0 when it stayed. The
// original compares whole 64-bit registers (the query as passed, the table words sign-extended), so the
// comparisons do the same.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCursorOffset = 0x000363a7U;  // lui 0x0003 and ori 0x63a7, added to a0 + a2
constexpr GuestAddress kHeaderTable = 164U;          // H+164: the 24-byte entries
constexpr GuestAddress kHeaderCount = 168U;          // H+168: the entry count
constexpr GuestAddress kEntryStart = 4U;             // signed start word of an entry
constexpr GuestAddress kEntryEnd = 8U;               // signed end word of an entry
constexpr std::uint32_t kEntryBytes = 24U;

// Retired instruction counts of the listing's blocks on each path, delay slots included.
constexpr std::uint32_t kHeadCost = 14U;      // 0x164510: cursor, header, table, entry = table + index * 24
constexpr std::uint32_t kLastTestCost = 2U;   // 0x1645a0: query == 0, when the cursor is on the last entry
constexpr std::uint32_t kStoreZeroCost = 3U;  // 0x1645a8: cursor = 0, return 1
constexpr std::uint32_t kIndexTestCost = 2U;  // 0x164548: cursor == 0 ?
constexpr std::uint32_t kWrapTestCost = 7U;   // 0x164550: query == end word of the last entry ?
constexpr std::uint32_t kStoreLastCost = 3U;  // 0x16456c: cursor = count - 1, return 1
constexpr std::uint32_t kEndTestCost = 4U;    // 0x164578: end < query ?
constexpr std::uint32_t kStartTestCost = 4U;  // 0x164588: query < start ?
constexpr std::uint32_t kZeroCost = 2U;       // 0x164598: return 0
constexpr std::uint32_t kStepUpCost = 5U;     // 0x1645b4: cursor + 1, return 1
constexpr std::uint32_t kStepDownCost = 4U;   // 0x1645c8: cursor - 1, return 1
constexpr std::uint32_t kReturnCost = 2U;     // 0x1645d8: jr $ra and its delay slot

// lw fills the whole 64-bit register with the word's sign extension.
std::int64_t sign_extended(const std::uint32_t word) {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(word));
}

template <class Context>
n3::Run step_track_segment_index(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress owner = call.arg(0);
    const std::uint64_t query = call.arg64(1);
    const GuestAddress cursor = owner + call.arg(2) + kCursorOffset;
    const GuestAddress header = m.read_u32_le(owner);
    const std::uint32_t index = m.read_u8(cursor);
    const std::uint32_t count = m.read_u32_le(header + kHeaderCount);
    const GuestAddress table = m.read_u32_le(header + kHeaderTable);
    const std::uint32_t last = count - 1U;
    const GuestAddress entry = table + index * kEntryBytes;
    std::uint32_t retired = kHeadCost;

    if (index == last) {
        retired += kLastTestCost;
        if (query == 0U) {
            m.write_u8(cursor, 0U);
            call.return_value(1U);
            retired += kStoreZeroCost + kReturnCost;
            return call.finish(retired);
        }
    }
    retired += kIndexTestCost;
    if (index == 0U) {
        retired += kWrapTestCost;
        // table + count * 24 - 16, the end word of the last entry (the same address as last * 24 + 8).
        const GuestAddress last_end = m.read_u32_le(table + last * kEntryBytes + kEntryEnd);
        if (query == static_cast<std::uint64_t>(sign_extended(last_end))) {
            m.write_u8(cursor, static_cast<std::uint8_t>(last));
            call.return_value(1U);
            retired += kStoreLastCost + kReturnCost;
            return call.finish(retired);
        }
    }
    retired += kEndTestCost;
    const std::int64_t signed_query = static_cast<std::int64_t>(query);
    if (sign_extended(m.read_u32_le(entry + kEntryEnd)) < signed_query) {
        const std::uint8_t reread = m.read_u8(cursor);
        m.write_u8(cursor, static_cast<std::uint8_t>(reread + 1U));
        call.return_value(1U);
        retired += kStepUpCost + kReturnCost;
        return call.finish(retired);
    }
    retired += kStartTestCost;
    if (signed_query < sign_extended(m.read_u32_le(entry + kEntryStart))) {
        const std::uint8_t reread = m.read_u8(cursor);
        m.write_u8(cursor, static_cast<std::uint8_t>(reread - 1U));
        call.return_value(1U);
        retired += kStepDownCost + kReturnCost;
        return call.finish(retired);
    }
    call.return_value(0U);
    retired += kZeroCost + kReturnCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00164510{0x00164510U, 208U, 0x0316f230b27c7d5aULL, "StepTrackSegmentIndex",
                                                  39U, n3::live<&step_track_segment_index<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00164510, step_track_segment_index)
