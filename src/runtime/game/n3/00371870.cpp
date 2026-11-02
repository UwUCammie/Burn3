// 0x00371870 ResolveNamedEntries (a0 = P): for each of the 144 name pointers in the table at 0x004c8a80, scans the
// global item list (header at 0x0051a868: count at +8, 16-byte entries from +12, item pointer at entry +8) and stores
// at P + 1024 + 4 * i the first item whose name (item +168) equals the name under strcasecmp, or 0 when no item does.
// The list header is read again for every name. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00126df0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kNameTable = 0x004c8a80U;   // lui 0x004d, addiu -30080: 144 name pointers
constexpr GuestAddress kListHeader = 0x0051a868U;  // lui 0x0052, lw -22424: pointer to the list header
constexpr std::uint32_t kNameCount = 144U;
constexpr std::uint32_t kResultOffset = 1024U;     // P + 1024: one item pointer per name
constexpr std::uint32_t kListCountOffset = 8U;     // signed item count
constexpr std::uint32_t kListEntriesOffset = 12U;  // entries, kEntryStride bytes each
constexpr std::uint32_t kEntryStride = 16U;
constexpr std::uint32_t kEntryItemOffset = 8U;
constexpr std::uint32_t kItemNameOffset = 168U;    // item + 144 + 24: the name strcasecmp compares
constexpr std::uint32_t kFrameBytes = 128U;        // the caller's addiu $sp, $sp, -128

// Retired instruction counts from the listing, delay slots included. Each comparison adds strcasecmp's own count.
constexpr std::uint32_t kEntryCost = 11U;          // 0x00371870..0x00371898: the prologue
constexpr std::uint32_t kNameCost = 15U;           // per name: its setup (9) and its store (6)
constexpr std::uint32_t kProbeCost = 14U;          // per item tested: the count test (4) and the call block (10)
constexpr std::uint32_t kStepCost = 1U;            // per item that did not match: the index step
constexpr std::uint32_t kMissCost = 5U;            // per name not found: the failing count test (4) and the clear (1)
constexpr std::uint32_t kReturnCost = 10U;         // 0x00371918..0x0037193c: the epilogue

// Calls retiring more than this are left to the original (call_cost returns 0), so a count reported is exact. The
// captured calls retire 916421 instructions each, so this must stay above that.
constexpr std::uint32_t kCeiling = 1000000U;
// strcasecmp's own ceiling: a comparison retiring more may have been cut short, so it is left to the original here.
constexpr std::uint32_t kCompareCeiling = 20000U;

template <class Context>
n3::Run resolve_named_entries(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress results = call.arg(0) + kResultOffset;
    std::uint32_t retired = kEntryCost;
    for (std::uint32_t name_index = 0U; name_index < kNameCount; ++name_index) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const std::uint32_t name = m.read_u32_le(kNameTable + 4U * name_index);
        const GuestAddress list = m.read_u32_le(kListHeader);
        const auto count = static_cast<std::int32_t>(m.read_u32_le(list + kListCountOffset));
        const GuestAddress entries = m.read_u32_le(list + kListEntriesOffset);
        retired += kNameCost;
        bool matched = false;
        std::uint32_t item_found = 0U;
        for (std::int32_t index = 0; index < count; ++index) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const std::uint32_t item =
                m.read_u32_le(entries + kEntryStride * static_cast<std::uint32_t>(index) + kEntryItemOffset);
            call.set_arg(0, item + kItemNameOffset);
            call.set_arg(1, name);
            const n3::Run compare = call.invoke([](auto& machine) { return n3::fn_00126df0(machine); }, kFrameBytes);
            if (compare.declined || compare.retired > kCompareCeiling) {
                return call.decline();
            }
            retired += kProbeCost + compare.retired;
            if (call.result() == 0U) {
                item_found = item;
                matched = true;
                break;
            }
            retired += kStepCost;
        }
        if (!matched) {
            retired += kMissCost;
        }
        m.write_u32_le(results + 4U * name_index, item_found);
    }
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00371870, 208U, 0x290004c738656bf2ULL, "ResolveNamedEntries", kCeiling, resolve_named_entries,
         EngineReturn::Nothing)
