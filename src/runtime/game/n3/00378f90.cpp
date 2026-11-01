// 0x00378f90 (a0 = set record S, a1 = item I): AddUniqueItemToSet. S holds a signed count at +0, a
// total at +4 and the item pointers from +8 (item i at S + 8 + 4*i). When I is not among the first
// count items it is stored after them, the count goes up by one, and the weight word of I (at I + 100)
// is added to the total. Nothing is returned. A count <= 0 means there is nothing to search.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kTotalOffset = 4U;      // int32 total at S + 4
constexpr std::uint32_t kItemsOffset = 8U;      // item i at S + 8 + 4*i
constexpr std::uint32_t kWeightOffset = 100U;   // weight word at I + 100

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 3U;         // lw count, beq to the loop test, daddu index = 0
constexpr std::uint32_t kTestCost = 3U;         // slt, bne, nop: index < count
constexpr std::uint32_t kCompareCost = 5U;      // sll, addu, lw item, beq, nop
constexpr std::uint32_t kStepCost = 1U;         // addiu index
constexpr std::uint32_t kFoundCost = 2U;        // jr ra, nop: I is already present, nothing stored
constexpr std::uint32_t kAppendCost = 10U;      // lw, addiu, sll, sw, addu, sw, lw, lw, addu, sw
constexpr std::uint32_t kReturnCost = 2U;       // jr ra, nop after the append

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t add_unique_item(Memory& m, const GuestAddress set, const std::uint64_t item) {
    std::uint32_t retired = kHeadCost;
    const std::uint32_t count = m.read_u32_le(set);
    std::uint32_t index = 0U;
    retired += kTestCost;
    while (static_cast<std::int32_t>(index) < static_cast<std::int32_t>(count)) {
        if (retired > kCeiling) {
            return retired;
        }
        const std::uint32_t entry = m.read_u32_le(set + kItemsOffset + 4U * index);
        retired += kCompareCost;
        // beq compares whole registers, and lw sign-extends the loaded word to 64 bits.
        if (static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(entry))) == item) {
            return retired + kFoundCost;
        }
        retired += kStepCost;
        ++index;
        retired += kTestCost;
    }
    // Append, in the original's order: the count store, then the item store, and the weight and
    // total are loaded after both, so overlapping addresses behave the same.
    const auto item_word = static_cast<std::uint32_t>(item);
    const std::uint32_t count_now = m.read_u32_le(set);
    const std::uint32_t grown = count_now + 1U;
    const std::uint32_t slot_offset = count_now << 2U;
    m.write_u32_le(set, grown);
    m.write_u32_le(set + slot_offset + kItemsOffset, item_word);
    const std::uint32_t weight = m.read_u32_le(item_word + kWeightOffset);
    const std::uint32_t total = m.read_u32_le(set + kTotalOffset);
    m.write_u32_le(set + kTotalOffset, total + weight);
    return retired + kAppendCost + kReturnCost;
}

template <class Context>
std::uint32_t add_unique_item_to_set_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = add_unique_item(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run add_unique_item_to_set(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(add_unique_item(call.memory(), call.arg(0), call.arg64(1)));
}

} // namespace

extern constinit const EngineFunction n3_00378f90{0x00378f90U, 96U, 0x9553597532b31f1bULL, "AddUniqueItemToSet",
                                                  kCeiling, n3::live<&add_unique_item_to_set<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &add_unique_item_to_set_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00378f90, add_unique_item_to_set, add_unique_item_to_set_cost)
