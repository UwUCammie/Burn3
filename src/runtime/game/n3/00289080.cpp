// 0x00289080 PickBestEntriesPerColumn (a0 = object O, 664 bytes; returns nothing).
//
// The word at O+0 is a mask over 12 table rows: row k is listed when bit k is set, in bit order (N rows listed;
// with N = 0 the function returns at once). The table at 0x0049bb40 has 12-byte entries, entry(k, j) at
// 0x0049bb40 + 180*k + 12*j for the 15 columns j, with a float at +4 and a flag byte at +8. For each column a walk
// over the N listed rows keeps two picks, both starting at the first listed row:
//   best      moves to a flagged entry whose float is not <= the float of the current best (c.ole.s false);
//   fallback  moves to an unflagged entry whose float is < the float of the current fallback (c.olt.s true).
// When the walk saw a flagged entry the column takes entry(best, j), otherwise entry(fallback, j). Its two words
// (raw bits, copied in the original's order) go to O + 21056 + 8*j, then StoreSlotRecord (0x00388af0) runs with
// a0 = 0x01e7569c, a1 = j, a2 = that pair's address and f12 = the float at gp - 29464.
//
// The listed rows live in a local array here: the original keeps them in its own frame, which nothing reads back.
// The retired count is the listing's blocks, delay slots included, summed along the path the data takes, so the
// body runs unchanged on guest memory (live) and on a DryMemory (call_cost).

#include "takedown/runtime/game/n3_support.hpp"

#include <array>

N3_CALLEE(00388af0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTable = 0x0049bb40U;                             // lui 0x004a, -17600
constexpr GuestAddress kRecordObject = 0x01e7569cU;                      // lui 0x01e7, +22172 (a0 of the callee)
constexpr GuestAddress kGlobalFloat = static_cast<GuestAddress>(-29464); // gp-relative float passed in f12
constexpr GuestAddress kPairsOffset = 21056U;                            // column j's pair at O + 21056 + 8j
constexpr std::uint32_t kRows = 12U;
constexpr std::uint32_t kColumns = 15U;
constexpr std::uint32_t kColumnStride = 12U;
constexpr std::uint32_t kRowStride = 180U;
constexpr std::uint32_t kPairStride = 8U;
constexpr std::uint32_t kFrameBytes = 80U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions per listing block, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;          // 0x00289080-0x002890a4
constexpr std::uint32_t kRowTestCost = 6U;         // 0x002890a8-0x002890bc: is row k in the mask
constexpr std::uint32_t kRowStepCost = 6U;         // 0x002890c0-0x002890d4: k + 1 and the row loop test
constexpr std::uint32_t kRowListedCost = 8U;       // 0x002892e0-0x002892fc: list row k
constexpr std::uint32_t kNoRowsExitCost = 8U;      // 0x002890d8 (taken), 0x00289300, 0x00289304-0x00289314
constexpr std::uint32_t kColumnsEnterCost = 5U;    // 0x002890d8 (not taken), 0x002890e0-0x002890e8
constexpr std::uint32_t kColumnStartCost = 14U;    // 0x002890ec-0x00289120: column state, jump to the walk
constexpr std::uint32_t kWalkTestCost = 5U;        // 0x002891ec-0x002891fc: walk index < N
constexpr std::uint32_t kWalkEntryCost = 11U;      // 0x00289124-0x0028914c: row k's entry and its flag test
constexpr std::uint32_t kFlaggedCost = 17U;        // 0x00289150-0x00289188 and 0x00289194-0x00289198
constexpr std::uint32_t kUnflaggedCost = 15U;      // 0x0028919c-0x002891d4
constexpr std::uint32_t kPickMoveCost = 2U;        // 0x0028918c-0x00289190 or 0x002891d8-0x002891dc
constexpr std::uint32_t kWalkStepCost = 3U;        // 0x002891e0-0x002891e8
constexpr std::uint32_t kPickTestCost = 2U;        // 0x00289200-0x00289204: any flagged entry in this column?
constexpr std::uint32_t kBestCopyCost = 20U;       // 0x00289208-0x00289254
constexpr std::uint32_t kFallbackCopyCost = 19U;   // 0x00289258-0x002892a0
constexpr std::uint32_t kCallCost = 13U;           // 0x002892a4-0x002892d4: f12, arguments, jal, loop test
constexpr std::uint32_t kLastColumnExitCost = 7U;  // 0x002892d8-0x002892dc, 0x00289304-0x00289314

template <class Context>
n3::Run pick_best_entries_per_column(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    // Rows listed from the mask, in bit order.
    std::uint32_t retired = kEntryCost;
    const std::uint32_t mask = m.read_u32_le(object);
    std::array<std::uint32_t, kRows> rows{};
    std::uint32_t count = 0U;
    for (std::uint32_t row = 0U; row < kRows; ++row) {
        if ((mask & (1U << row)) != 0U) {
            rows[count] = row;
            ++count;
            retired += kRowListedCost;
        }
        retired += kRowTestCost + kRowStepCost;
    }
    if (count == 0U) {
        return call.finish(retired + kNoRowsExitCost);
    }
    retired += kColumnsEnterCost;

    for (std::uint32_t column = 0U; column < kColumns; ++column) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kColumnStartCost + kWalkTestCost;
        const GuestAddress column_base = kTable + kColumnStride * column;

        // The walk: best and fallback are indices into rows, both starting at the first listed row.
        std::uint32_t best = 0U;
        std::uint32_t fallback = 0U;
        bool flagged_seen = false;
        std::uint32_t walk = 0U;
        while (walk < count) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            retired += kWalkEntryCost;
            const GuestAddress entry = column_base + kRowStride * rows[walk];
            const std::uint32_t entry_float = m.read_u32_le(entry + 4U);
            const std::uint32_t flag = m.read_u8(entry + 8U);
            if (flag != 0U) {
                retired += kFlaggedCost;
                const std::uint32_t best_float = m.read_u32_le(column_base + kRowStride * rows[best] + 4U);
                if (!fpu::le(entry_float, best_float)) {
                    best = walk;
                    retired += kPickMoveCost;
                }
                flagged_seen = true;
            } else {
                retired += kUnflaggedCost;
                const std::uint32_t fallback_float = m.read_u32_le(column_base + kRowStride * rows[fallback] + 4U);
                if (fpu::lt(entry_float, fallback_float)) {
                    fallback = walk;
                    retired += kPickMoveCost;
                }
            }
            retired += kWalkStepCost;
            ++walk;
            retired += kWalkTestCost;
        }

        // The pick's two words are copied as raw bits, word 0 before word 1 is read, as the original does.
        retired += kPickTestCost;
        const std::uint32_t pick = flagged_seen ? best : fallback;
        retired += flagged_seen ? kBestCopyCost : kFallbackCopyCost;
        const GuestAddress source = column_base + kRowStride * rows[pick];
        const GuestAddress pair = object + kPairsOffset + kPairStride * column;
        m.write_u32_le(pair, m.read_u32_le(source));
        m.write_u32_le(pair + 4U, m.read_u32_le(source + 4U));

        // StoreSlotRecord(a0 = record object, a1 = column, a2 = the pair, f12 = the global float).
        retired += kCallCost;
        call.set_float_arg(0U, m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) + kGlobalFloat));
        call.set_arg(0U, kRecordObject);
        call.set_arg(1U, column);
        call.set_arg(2U, pair);
        const n3::Run record = call.invoke([](auto& machine) { return n3::fn_00388af0(machine); }, kFrameBytes);
        if (record.declined) {
            return call.decline();
        }
        retired += record.retired;
    }
    retired += kLastColumnExitCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00289080, 664U, 0xf9c7b6f6070bc137ULL, "PickBestEntriesPerColumn", kCeiling, pick_best_entries_per_column,
         EngineReturn::Nothing)
