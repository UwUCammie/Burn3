// 0x003be8d0 (a0 record): RelocateRecordTable. The record's words +32 and +36 are self-relative offsets:
// each one that is nonzero becomes an absolute address in place (record + offset, 32-bit wrapping). The u16
// at +40 counts the 16-byte entries of the table at +36. When the word at +44 is nonzero every entry is
// relocated by RelocateSelfRelativePair (0x002bf7b0, native). When it is zero the entries go through
// 0x002bf7d0, which has no native yet, so that path declines unless the table is empty. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002bf7b0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;          // addiu $sp, $sp, -48
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kEntryShift = 4U;           // entries are 16 bytes

constexpr GuestAddress kHeadWord = 32U;             // self-relative, relocated when nonzero
constexpr GuestAddress kTableWord = 36U;            // self-relative table, relocated when nonzero
constexpr GuestAddress kCountHalf = 40U;            // u16 number of table entries
constexpr GuestAddress kModeWord = 44U;             // nonzero: entries relocated natively

// Retired instruction counts of the original's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 7U;         // 003be8d0-003be8e8: frame, saves, first test
constexpr std::uint32_t kHeadFixCost = 2U;          // 003be8ec-003be8f0: head word nonzero
constexpr std::uint32_t kTableTestCost = 3U;        // 003be8f4-003be8fc
constexpr std::uint32_t kTableFixCost = 3U;         // 003be900-003be908: table word nonzero
constexpr std::uint32_t kModeTestCost = 3U;         // 003be98c-003be994
constexpr std::uint32_t kLoopEntryCost = 4U;        // 003be998-003be99c and 003be90c-003be910
constexpr std::uint32_t kLoopTestCost = 5U;         // 003be930-003be940 (and 003be970-003be980)
constexpr std::uint32_t kPassCost = 7U;             // 003be914-003be92c: one pass around its jal
constexpr std::uint32_t kExitCost = 8U;             // 003be944-003be948, 003be984-003be988, 003be9a0-003be9ac
constexpr std::uint32_t kSecondEntryCost = 2U + kLoopTestCost;  // 003be94c-003be950, then the first test
constexpr std::uint32_t kSecondExitCost = 6U;       // 003be984-003be988, 003be9a0-003be9ac

template <class Context>
n3::Run relocate_record_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    std::uint32_t retired = kPrologueCost;
    const std::uint32_t head = m.read_u32_le(record + kHeadWord);
    if (head != 0U) {
        m.write_u32_le(record + kHeadWord, record + head);
        retired += kHeadFixCost;
    }
    retired += kTableTestCost;
    const std::uint32_t table = m.read_u32_le(record + kTableWord);
    if (table != 0U) {
        m.write_u32_le(record + kTableWord, record + table);
        retired += kTableFixCost;
    }
    retired += kModeTestCost;

    if (m.read_u32_le(record + kModeWord) == 0U) {
        // The second walk calls 0x002bf7d0 for each entry, which has no native yet: only an empty table finishes here.
        retired += kSecondEntryCost;
        if (m.read_u16_le(record + kCountHalf) != 0U) {
            return call.decline();
        }
        return call.finish(retired + kSecondExitCost);
    }

    retired += kLoopEntryCost;
    std::uint32_t entry_index = 0U;
    for (;;) {
        retired += kLoopTestCost;
        const std::uint32_t count = m.read_u16_le(record + kCountHalf);
        if (entry_index >= count) {
            break;
        }
        // The table word is reloaded every pass: the callee may have rewritten it.
        const GuestAddress entry = m.read_u32_le(record + kTableWord) + (entry_index << kEntryShift);
        call.set_arg(0, entry);
        const n3::Run callee =
            call.invoke([](auto& callee_memory) { return n3::fn_002bf7b0(callee_memory); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += kPassCost + callee.retired;
        entry_index = (entry_index + 1U) & 0xffffU;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003be8d0, 224U, 0xd8fa2d3f897bade2ULL, "RelocateRecordTable", kCeiling, relocate_record_table,
         EngineReturn::Nothing)
