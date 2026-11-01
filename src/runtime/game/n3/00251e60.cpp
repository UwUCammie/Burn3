// 0x00251e60 (a0 = table object, a1 = 64-bit key): a linear search of the entries for the key. The entry count is
// the signed word at a0+4; entry i is the doubleword at a0+1032+8*i, compared with the full 64-bit a1. Returns v0 =
// the index of the first matching entry, or 0 when nothing matches (0 is ambiguous with index 0, as in the original).
// Nothing is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 4U;         // table + 4, signed
constexpr GuestAddress kEntriesOffset = 1032U;    // table + 1032 + 8 * index
constexpr std::uint32_t kEntryStride = 8U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;          // lw, beq and its delay slot (daddu v0 = 0), to the loop test
constexpr std::uint32_t kTestCost = 3U;           // slt, bne and its delay slot
constexpr std::uint32_t kCompareCost = 5U;        // sll, addu, ld, beq and its delay slot
constexpr std::uint32_t kStepCost = 1U;           // addiu
constexpr std::uint32_t kMatchExitCost = 2U;      // jr and its delay slot, after a match
constexpr std::uint32_t kMissExitCost = 3U;       // daddu v0 = 0, then jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so a reported count is always the
// original's exact count and the scan below cannot run away on a corrupt count.
constexpr std::uint32_t kCeiling = 20000U;

struct Lookup {
    std::uint32_t retired{};
    std::uint32_t index{};
};

// The scan on guest memory (the implementation) or on a DryMemory (call_cost). Each pass through the loop body
// retires at least 9 instructions, so the ceiling test there ends the scan of any huge count.
template <class Memory>
Lookup find_entry_index(const Memory& m, const GuestAddress table, const std::uint64_t key) {
    std::uint32_t retired = kEntryCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(table + kCountOffset));
    for (std::uint32_t index = 0U;; ++index) {
        retired += kTestCost;
        if (!(static_cast<std::int32_t>(index) < count)) {
            return {retired + kMissExitCost, 0U};
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        const GuestAddress entry = table + kEntriesOffset + kEntryStride * index;
        retired += kCompareCost;
        if (m.read_u64_le(entry) == key) {
            return {retired + kMatchExitCost, index};
        }
        retired += kStepCost;
    }
}

template <class Context>
n3::Run find_entry_index_native(Context& context) {
    auto call = n3::guest_call(context);
    const Lookup found = find_entry_index(call.memory(), call.arg(0), call.arg64(1U));
    call.return_value(found.index);
    return call.finish(found.retired);
}

template <class Context>
std::uint32_t find_entry_index_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Lookup found = find_entry_index(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return found.retired <= kCeiling ? found.retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_00251e60{0x00251e60U, 60U, 0x2853d6bc4dcfa217ULL, "FindEntryIndexByKey",
                                                  kCeiling, n3::live<&find_entry_index_native<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_entry_index_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00251e60, find_entry_index_native, find_entry_index_cost)
