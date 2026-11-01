// 0x00386cf0 (a0 = record R, a1 = base B, a3 = stride S): stores B at R+40 and S at R+32, then fills the table whose
// pointer is at R+36 with 8-byte entries. Entry i, while i is below the count at R+28 (signed): the word at entry+0
// becomes B + i*S and the byte at entry+4 is cleared. Returns 1 in v0.
//
// The table pointer is read again after each word store, and the count at every test, because those stores can land
// on them. Retired count: 5 for the entry block, 4 per count test (one more test than entries), 10 per entry, 2 for
// the return, so 11 + 14 per entry.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kBaseOffset = 40U;          // word written from a1
constexpr GuestAddress kStrideOffset = 32U;        // word written from a3
constexpr GuestAddress kCountOffset = 28U;         // signed entry count
constexpr GuestAddress kTableOffset = 36U;         // table pointer
constexpr std::uint32_t kEntrySize = 8U;           // sll by 3
constexpr GuestAddress kFlagOffset = 4U;           // byte cleared inside each entry
constexpr std::uint8_t kFlagClear = 0U;
constexpr std::uint32_t kReturnValue = 1U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 5U;           // the two stores, two moves and the branch to the first test
constexpr std::uint32_t kTestCost = 4U;            // lw, slt, bne and its delay slot
constexpr std::uint32_t kEntryBodyCost = 10U;      // one entry
constexpr std::uint32_t kReturnCost = 2U;          // jr $ra and its delay slot (addiu v0, 1)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is always
// the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t fill_entries(Memory& m, const GuestAddress record, const std::uint32_t base, const std::uint32_t stride) {
    m.write_u32_le(record + kBaseOffset, base);
    m.write_u32_le(record + kStrideOffset, stride);
    std::uint32_t retired = kEntryCost + kTestCost;
    std::uint32_t index = 0U;
    while (static_cast<std::int32_t>(index) < static_cast<std::int32_t>(m.read_u32_le(record + kCountOffset))) {
        if (retired > kCeiling) {
            return retired;
        }
        retired += kEntryBodyCost + kTestCost;
        const std::uint32_t offset = index * kEntrySize;
        const GuestAddress first_table = m.read_u32_le(record + kTableOffset);
        m.write_u32_le(first_table + offset, base + index * stride);
        const GuestAddress table = m.read_u32_le(record + kTableOffset);
        m.write_u8(table + offset + kFlagOffset, kFlagClear);
        ++index;
    }
    return retired + kReturnCost;
}

template <class Context>
n3::Run fill_strided_entries(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t retired = fill_entries(call.memory(), call.arg(0), call.arg(1), call.arg(3));
    call.return_value(kReturnValue);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00386cf0, 84U, 0x60f08ac148ac0824ULL, "FillStridedEntries", kCeiling, fill_strided_entries,
         EngineReturn::Integer)
