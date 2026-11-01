// 0x00148d60 FindTypeFiveRecord: walks a ring of 10-byte records looking for one whose type, the low three bits of
// its byte +3, is 5. a0 is the ring header: the word at +4 points at the ring, whose word at +8 is the record base,
// whose u16 at +12 is the record count and whose byte at +14 is the wrap flag. The walk starts at record a1 (u16)
// and tests at most a2 (u16) records, one per pass. Past the last record it goes on from record 0 when the wrap
// flag is set, and otherwise stops. Returns v0 = 0 for a type 5 record, 1 when none was found. Leaf, no stores.
//
// The pass count is data dependent (a2), so this is an N3_ENTRY: the dry run gives the exact count. The per-block
// counts are the listing's (brief.py), delay slots included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRingOffset = 4U;        // header +4: the ring
constexpr GuestAddress kRecordsOffset = 8U;     // ring +8: record base
constexpr GuestAddress kCountOffset = 12U;      // ring +12 (u16): record count
constexpr GuestAddress kWrapOffset = 14U;       // ring +14 (u8): wrap flag
constexpr std::uint32_t kRecordStride = 10U;
constexpr GuestAddress kTypeOffset = 3U;        // record +3, low three bits
constexpr std::uint8_t kTypeMask = 7U;
constexpr std::uint8_t kSoughtType = 5U;

// Retired counts per block, delay slots included.
constexpr std::uint32_t kEntryCost = 5U;        // 0x00148d60-0x00148d70 (also sets a0 = 5, the type sought)
constexpr std::uint32_t kTestCost = 4U;         // 0x00148dc4-0x00148dd0: once per pass, and once more to leave
constexpr std::uint32_t kBodyCost = 10U;        // 0x00148d74-0x00148d98: the record's type test
constexpr std::uint32_t kFoundCost = 6U;        // 0x00148de0-0x00148dec: v0 = 0 and return
constexpr std::uint32_t kAdvanceCost = 6U;      // 0x00148d9c-0x00148db0: next index, compare with the count
constexpr std::uint32_t kWrapTestCost = 3U;     // 0x00148db4-0x00148dbc: the wrap flag
constexpr std::uint32_t kWrapCost = 1U;         // 0x00148dc0: index = 0
constexpr std::uint32_t kNotFoundCost = 5U;     // 0x00148dd4-0x00148dec: v0 = 1 and return

// Calls retiring more than this are left to the original.
constexpr std::uint32_t kCeiling = 20000U;

struct RecordScan {
    std::uint32_t retired;
    bool found;
};

// The walk on guest memory (the implementation) or on a DryMemory (the call_cost run). It stops as soon as the count
// passes kCeiling, so a large budget cannot make the dry run long.
template <class Memory>
RecordScan scan_for_type_five(Memory& m, const GuestAddress header, std::uint32_t index, std::uint32_t budget) {
    const GuestAddress ring = m.read_u32_le(header + kRingOffset);
    std::uint32_t retired = kEntryCost;
    for (;;) {
        retired += kTestCost;
        if (budget == 0U) {
            return {retired + kNotFoundCost, false};
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        --budget;
        retired += kBodyCost;
        const GuestAddress record = m.read_u32_le(ring + kRecordsOffset) + kRecordStride * index;
        if ((m.read_u8(record + kTypeOffset) & kTypeMask) == kSoughtType) {
            return {retired + kFoundCost, true};
        }
        retired += kAdvanceCost;
        index = (index + 1U) & 0xffffU;
        if (index < static_cast<std::uint32_t>(m.read_u16_le(ring + kCountOffset))) {
            continue;
        }
        retired += kWrapTestCost;
        if (m.read_u8(ring + kWrapOffset) == 0U) {
            return {retired + kNotFoundCost, false};
        }
        retired += kWrapCost;
        index = 0U;
    }
}

template <class Context>
n3::Run find_type_five_record(Context& context) {
    auto call = n3::guest_call(context);
    const RecordScan scan =
        scan_for_type_five(call.memory(), call.arg(0), call.arg(1) & 0xffffU, call.arg(2) & 0xffffU);
    if (scan.retired > kCeiling) {
        return call.decline();
    }
    call.return_value(scan.found ? 0U : 1U);
    return call.finish(scan.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00148d60, 144U, 0xf6a6b9b487d050c6ULL, "FindTypeFiveRecord", kCeiling, find_type_five_record,
         EngineReturn::Integer)
