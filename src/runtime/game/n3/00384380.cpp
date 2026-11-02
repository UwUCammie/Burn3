// 0x00384380 AssignSlotOffsets (a0 = table T, a1 = start used when T has none): lays the records of T out one after
// another. T+8 is the running start offset (when it is zero the incoming a1 is stored there first), T+12 points at
// the records (12 bytes each: word 0 receives the offset, word 1 is the length, byte 8 is cleared) and T+24 is the
// signed record count. Each record gets the offset where the previous one ended. Returns 1 in v0.
//
// The record pointer is read again for every store and the count at every test, because the record stores can land
// on T itself.
//
// Retired count: 3 for the entry test (+2 when the start is stored), 3 for the cursor load, 4 per count test (one
// more test than records), 14 per record, 4 for the exit, so 14 + 18 per record (16 + 18 when the start was zero).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStartOffset = 8U;           // running start offset
constexpr GuestAddress kRecordsOffset = 12U;        // pointer to the records
constexpr GuestAddress kCountOffset = 24U;          // signed record count
constexpr std::uint32_t kRecordSize = 12U;
constexpr GuestAddress kLengthOffset = 4U;          // word 1 of a record
constexpr GuestAddress kFlagOffset = 8U;            // byte cleared in each record

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;            // lw, beq, nop
constexpr std::uint32_t kStoreStartCost = 2U;       // beq and the delay slot sw
constexpr std::uint32_t kCursorCost = 3U;           // lw, beq, daddu
constexpr std::uint32_t kTestCost = 4U;             // lw, slt, bne, nop
constexpr std::uint32_t kRecordCost = 14U;
constexpr std::uint32_t kExitCost = 4U;             // beq, addiu, jr, nop
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run assign_slot_offsets(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress table = call.arg(0);
    std::uint32_t retired = kEntryCost + kCursorCost;
    if (m.read_u32_le(table + kStartOffset) == 0U) {
        m.write_u32_le(table + kStartOffset, call.arg(1));
        retired += kStoreStartCost;
    }
    std::uint32_t cursor = m.read_u32_le(table + kStartOffset);
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(table + kCountOffset))) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t record_offset = kRecordSize * index;
        m.write_u8(m.read_u32_le(table + kRecordsOffset) + record_offset + kFlagOffset, 0U);
        m.write_u32_le(m.read_u32_le(table + kRecordsOffset) + record_offset, cursor);
        cursor += m.read_u32_le(m.read_u32_le(table + kRecordsOffset) + record_offset + kLengthOffset);
        retired += kRecordCost;
        ++index;
    }
    call.return_value(1U);
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00384380, 120U, 0x508e09dfcf1f0ee0ULL, "AssignSlotOffsets", kCeiling, assign_slot_offsets,
         EngineReturn::Integer)
