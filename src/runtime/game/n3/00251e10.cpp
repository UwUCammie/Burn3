// 0x00251e10 (FindHandleInTable; a0 = table, a1 = 64-bit key): a linear search of the table's keys. The signed
// 32-bit count is at table+4, and entry i's 64-bit key is the doubleword at table+1032+8*i. The search tests
// i = 0, 1, ... while i < count (signed) and stops at the first key equal to the full 64-bit a1, or with i = -1
// when none matches. v0 = 1 when the final i is >= 0, else 0 (the original tests the sign of i, not the match
// itself). Nothing is written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 4U;         // lw at table+4, signed
constexpr GuestAddress kKeysOffset = 1032U;       // doubleword i at table+1032+8*i
constexpr std::uint32_t kStride = 8U;             // bytes per key (sll by 3 in the listing)
constexpr std::uint32_t kNotFoundIndex = 0xffffffffU;  // addiu $a2,$zero,-1

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;          // lw, beq and its delay slot daddu
constexpr std::uint32_t kTestCost = 3U;           // slt, bne and its delay slot nop (every loop test)
constexpr std::uint32_t kMissCost = 1U;           // addiu $a2,$zero,-1, when the loop test fails
constexpr std::uint32_t kBodyCost = 5U;           // sll, addu, ld, beq and its delay slot nop
constexpr std::uint32_t kStepCost = 1U;           // addiu $a2,$a2,1 after a key that did not match
constexpr std::uint32_t kTailCost = 4U;           // slt, xori, jr and its delay slot nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Search {
    std::uint32_t retired{};
    std::uint32_t result{};
};

// The search on guest memory (the implementation) or on a DryMemory (call_cost). It only reads. It
// returns as soon as the count passes kCeiling; the implementation only runs for calls call_cost
// accepted, so that early return is never taken there.
template <class Memory>
Search find_handle(const Memory& m, const GuestAddress table, const std::uint64_t key) {
    std::uint32_t retired = kEntryCost;
    // The index is kept as the 32-bit value the original's 32-bit addiu leaves (sign-extended in the
    // register), so a wrap past 0x7fffffff is tested as negative, as in the original.
    const auto count = static_cast<std::int32_t>(m.read_u32_le(table + kCountOffset));
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (!(static_cast<std::int32_t>(index) < count)) {
            retired += kMissCost;
            index = kNotFoundIndex;
            break;
        }
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kBodyCost;
        const GuestAddress slot = table + index * kStride;
        // beq compares the whole 64-bit registers.
        if (m.read_u64_le(slot + kKeysOffset) == key) {
            break;
        }
        retired += kStepCost;
        ++index;
    }
    retired += kTailCost;
    return {retired, static_cast<std::int32_t>(index) < 0 ? 0U : 1U};
}

// The dry run of the search on the call's own registers: the exact retired count, or 0 (the original
// runs the call) when it passes kCeiling.
template <class Context>
std::uint32_t find_handle_in_table_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Search found = find_handle(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return found.retired <= kCeiling ? found.retired : 0U;
}

template <class Context>
n3::Run find_handle_in_table(Context& context) {
    auto call = n3::guest_call(context);
    const Search found = find_handle(call.memory(), call.arg(0), call.arg64(1U));
    call.return_value(found.result);
    return call.finish(found.retired);
}

} // namespace

extern constinit const EngineFunction n3_00251e10{0x00251e10U, 68U, 0x34e74b4c9e259343ULL, "FindHandleInTable",
                                                  kCeiling, n3::live<&find_handle_in_table<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &find_handle_in_table_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00251e10, find_handle_in_table, find_handle_in_table_cost)
