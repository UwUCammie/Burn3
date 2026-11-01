// 0x00160520 (a0 = container, a1 = signed entry index, a2 = byte offset inside an entry): IsEntryUsedInTable.
// The container's word at +0 points to a header: +16 is the signed entry count, +8 the array of 18-byte entries.
// For 0 <= a1 < count the result is 1 unless the byte at entry(a1) + a2 + 12 is 0xff, the unused marker; otherwise
// the result is 0 and the array and the byte are never read. A leaf with no stores. The original's v1 temporary
// is caller-saved and not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kHeaderEntries = 8U;      // lw 8(header): the entry array
constexpr GuestAddress kHeaderCount = 16U;       // lw 16(header): the signed entry count
constexpr GuestAddress kEntryStride = 18U;       // a1 * 18, built as (a1 << 3) + a1, then << 1
constexpr GuestAddress kSlotOffset = 12U;        // lbu 12(entry + a2): the slot id
constexpr std::uint8_t kUnusedSlot = 0xffU;

// Retired instruction counts of the three paths, delay slots included.
constexpr std::uint32_t kAboveCountCost = 8U;    // a1 >= count (signed): the first beq is taken
constexpr std::uint32_t kNegativeCost = 10U;     // a1 < 0 (and below the count): the bltz is taken
constexpr std::uint32_t kInRangeCost = 19U;      // 0 <= a1 < count: the longest path

template <class Context>
n3::Run is_entry_used_in_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress container = call.arg(0);
    const auto entry_index = static_cast<std::int32_t>(call.arg(1));
    const GuestAddress byte_offset = call.arg(2);
    const GuestAddress header = m.read_u32_le(container);
    const auto entry_count = static_cast<std::int32_t>(m.read_u32_le(header + kHeaderCount));
    if (entry_index >= entry_count) {
        call.return_value(0U);
        return call.finish(kAboveCountCost);
    }
    if (entry_index < 0) {
        call.return_value(0U);
        return call.finish(kNegativeCost);
    }
    const GuestAddress entries = m.read_u32_le(header + kHeaderEntries);
    const GuestAddress slot =
        entries + static_cast<GuestAddress>(entry_index) * kEntryStride + byte_offset + kSlotOffset;
    call.return_value(m.read_u8(slot) != kUnusedSlot ? 1U : 0U);
    return call.finish(kInRangeCost);
}

} // namespace

extern constinit const EngineFunction n3_00160520{0x00160520U, 80U, 0x6890b8eea9a0592bULL, "IsEntryUsedInTable", 19U,
                                                  n3::live<&is_entry_used_in_table<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00160520, is_entry_used_in_table)
