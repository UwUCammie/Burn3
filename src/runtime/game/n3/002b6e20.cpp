// 0x002b6e20 (a0 = pool record P): rebuilds the free list of a fixed-block pool in address order.
// P+0 is the pool base (the first block), P+4 the block stride and P+16 the head of the free list. Each block's
// word +0 is its next pointer and word +4 a flag. Pass 1 walks the free list from its head, sets the flag of every
// node to 1 and counts the nodes. Pass 2 scans the pool from its base in address order and takes the first `count`
// blocks whose flag is 1: the first one becomes the head with its flag cleared, each later one is linked after the
// previous one with its flag holding the previous block's address, and the last one's next pointer is cleared.
// Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kBaseOffset = 0U;      // pool record: pool base
constexpr GuestAddress kStrideOffset = 4U;    // pool record: block stride
constexpr GuestAddress kHeadOffset = 16U;     // pool record: head of the free list
constexpr GuestAddress kNextOffset = 0U;      // block: next pointer
constexpr GuestAddress kFlagOffset = 4U;      // block: flag word
constexpr std::uint32_t kFlagMarked = 1U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeaderCost = 3U;     // lw, beq, nop: pool base is zero
constexpr std::uint32_t kHeadTestCost = 3U;   // lw, beq, nop: free list is empty
constexpr std::uint32_t kReturnCost = 2U;     // jr $ra, nop
constexpr std::uint32_t kWalkSetupCost = 2U;  // daddu, addiu
constexpr std::uint32_t kWalkCost = 7U;       // one node of pass 1
constexpr std::uint32_t kScanSetupCost = 3U;  // lw, daddu, addiu
constexpr std::uint32_t kProbeCost = 3U;      // lw flag, beq, nop
constexpr std::uint32_t kStepCost = 3U;       // lw stride, beq, addu
constexpr std::uint32_t kFoundCost = 2U;      // beq previous, nop
constexpr std::uint32_t kHeadLinkCost = 3U;   // sw head, beq, sw flag
constexpr std::uint32_t kChainLinkCost = 2U;  // sw next, sw flag
constexpr std::uint32_t kCountCost = 3U;      // addiu count, bne, daddu previous
constexpr std::uint32_t kEndCost = 2U;        // beq, sw next (last free block)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t rebuild_free_list(Memory& m, const GuestAddress pool) {
    std::uint32_t retired = kHeaderCost;
    if (m.read_u32_le(pool + kBaseOffset) == 0U) {
        return retired + kReturnCost;
    }
    retired += kHeadTestCost;
    GuestAddress node = m.read_u32_le(pool + kHeadOffset);
    if (node == 0U) {
        return retired + kReturnCost;
    }
    retired += kWalkSetupCost;

    // Pass 1: the store to the flag comes before the next pointer is read.
    std::uint32_t count = 0U;
    do {
        if (retired > kCeiling) {
            return retired;
        }
        m.write_u32_le(node + kFlagOffset, kFlagMarked);
        ++count;
        node = m.read_u32_le(node + kNextOffset);
        retired += kWalkCost;
    } while (node != 0U);

    // Pass 2: scan the pool from its base. A block whose flag is 1 is free; the scan stops after `count` of them.
    // Each step reads the stride from the pool record again, as the original does (the stores can reach it).
    retired += kScanSetupCost;
    GuestAddress block = m.read_u32_le(pool + kBaseOffset);
    GuestAddress previous = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return retired;
        }
        const std::uint32_t flag = m.read_u32_le(block + kFlagOffset);
        retired += kProbeCost;
        if (flag == kFlagMarked) {
            retired += kFoundCost;
            if (previous == 0U) {
                m.write_u32_le(pool + kHeadOffset, block);
                m.write_u32_le(block + kFlagOffset, 0U);
                retired += kHeadLinkCost;
            } else {
                m.write_u32_le(previous + kNextOffset, block);
                m.write_u32_le(block + kFlagOffset, previous);
                retired += kChainLinkCost;
            }
            retired += kCountCost;
            --count;
            previous = block;
            if (count == 0U) {
                m.write_u32_le(block + kNextOffset, 0U);
                return retired + kEndCost + kReturnCost;
            }
        }
        const std::uint32_t stride = m.read_u32_le(pool + kStrideOffset);
        retired += kStepCost;
        block += stride;
    }
}

template <class Context>
std::uint32_t rebuild_pool_free_list_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = rebuild_free_list(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run rebuild_pool_free_list(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(rebuild_free_list(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002b6e20{0x002b6e20U, 152U, 0x7d945c05a5486087ULL,
                                                  "RebuildPoolFreeListInAddressOrder", kCeiling,
                                                  n3::live<&rebuild_pool_free_list<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &rebuild_pool_free_list_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b6e20, rebuild_pool_free_list, rebuild_pool_free_list_cost)
