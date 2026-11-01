// 0x002a0850 (a0 = heap header H, a1 = size, a2 = alignment): AllocAlignedFromBlockList. Returns the new
// block's payload address in v0, or 0. A first-fit walk over the address-ordered, doubly linked block list
// from H+8 (the first block) up to the end sentinel node at H+12. A block is a 12-byte header (+0 next,
// +4 previous, +8 size) with its payload at block + 12. In the gap after a block the new block's header goes
// at the aligned payload minus 12, which must not start before the block's end; the gap must hold the new
// block (header + size) before the next block. The first gap that fits takes the new block, linked in
// between by the original's stores in the original's order.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFirstBlockSlot = 8U;       // H+8: the first block
constexpr GuestAddress kSentinelSlot = 12U;        // H+12: the end sentinel node
constexpr GuestAddress kNextOffset = 0U;           // block+0: next block
constexpr GuestAddress kPreviousOffset = 4U;       // block+4: previous block
constexpr GuestAddress kSizeOffset = 8U;           // block+8: payload size
constexpr std::uint32_t kBlockHeader = 12U;        // the payload starts this far into a block

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 6U;           // 0x850-0x864: first block, masks, the need
constexpr std::uint32_t kWalkCost = 8U;            // 0x868-0x884: next, size and the gap test
constexpr std::uint32_t kFitCost = 10U;            // 0x888-0x8ac: the aligned candidate and the next test
constexpr std::uint32_t kSkipCost = 1U;            // 0x8c4: the gap is too small
constexpr std::uint32_t kNextCloseCost = 1U;       // 0x91c: the candidate runs past next
constexpr std::uint32_t kBelowEndTestCost = 3U;    // 0x8b0-0x8b8: the candidate test against the block's end
constexpr std::uint32_t kBelowEndCost = 2U;        // 0x8bc-0x8c0: the candidate starts before the end, dropped
constexpr std::uint32_t kJoinCost = 2U;            // 0x920-0x924: sentinel load on the way to the loop test
constexpr std::uint32_t kLoopTestCost = 2U;        // 0x8c8-0x8cc: next against the sentinel, delay slot steps
constexpr std::uint32_t kFoundTestCost = 2U;       // 0x8d0-0x8d4: is there a candidate
constexpr std::uint32_t kLinkCost = 7U;            // 0x8d8-0x8f0: the new block's next, its previous, a test
constexpr std::uint32_t kFollowingLinkCost = 1U;   // 0x8f4: the following block's previous
constexpr std::uint32_t kCurLinkTestCost = 4U;     // 0x8f8-0x904: reloads and the test for cur's next
constexpr std::uint32_t kCurLinkCost = 1U;         // 0x908: cur's next
constexpr std::uint32_t kSizeCost = 4U;            // 0x90c-0x918: the new block's size, the result
constexpr std::uint32_t kReturnCost = 2U;          // 0x928-0x92c: jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct AllocOutcome {
    std::uint32_t retired;  // instructions the original retires on the path taken
    std::uint32_t pointer;  // v0 on return
};

// The candidate block (0 when there is none) is linked in between cur and its following block (0x8d8-0x90c),
// and the payload address returned. Without a candidate the result is 0 (0x8d0).
template <class Memory>
AllocOutcome finish_candidate(Memory& m, const GuestAddress header, const GuestAddress cur,
                              const GuestAddress candidate, const std::uint32_t size, std::uint32_t retired) {
    retired += kFoundTestCost;
    if (candidate == 0U) {
        return {retired + kReturnCost, 0U};
    }
    retired += kLinkCost;
    const GuestAddress following = m.read_u32_le(cur + kNextOffset);
    m.write_u32_le(candidate + kNextOffset, following);
    m.write_u32_le(candidate + kPreviousOffset, cur);
    const GuestAddress following_now = m.read_u32_le(candidate + kNextOffset);
    const GuestAddress sentinel = m.read_u32_le(header + kSentinelSlot);
    if (following_now != sentinel) {
        m.write_u32_le(following_now + kPreviousOffset, candidate);
        retired += kFollowingLinkCost;
    }
    const GuestAddress sentinel_now = m.read_u32_le(header + kSentinelSlot);
    const GuestAddress cur_now = m.read_u32_le(candidate + kPreviousOffset);
    retired += kCurLinkTestCost;
    if (cur_now != sentinel_now) {
        m.write_u32_le(cur_now + kNextOffset, candidate);
        retired += kCurLinkCost;
    }
    m.write_u32_le(candidate + kSizeOffset, size);
    retired += kSizeCost;
    return {retired + kReturnCost, candidate + kBlockHeader};
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// count and the result, or a count above kCeiling as soon as the walk passes it.
template <class Memory>
AllocOutcome first_fit(Memory& m, const GuestAddress header, const std::uint32_t size, const std::uint32_t alignment) {
    const std::uint32_t align_low = alignment - 1U;        // t1
    const std::uint32_t align_mask = ~align_low;           // t0 (nor with $zero)
    const std::uint32_t need = size + kBlockHeader;        // t2
    std::uint32_t retired = kEntryCost;
    GuestAddress cur = m.read_u32_le(header + kFirstBlockSlot);
    GuestAddress candidate = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kWalkCost;
        const GuestAddress next = m.read_u32_le(cur + kNextOffset);
        const std::uint32_t cur_size = m.read_u32_le(cur + kSizeOffset);
        const std::uint32_t gap = (next - cur) - (cur_size + kBlockHeader);
        if (gap < need) {
            retired += kSkipCost;
        } else {
            const GuestAddress end = cur + cur_size + kBlockHeader;
            candidate = ((end + kBlockHeader + align_low) & align_mask) - kBlockHeader;
            retired += kFitCost;
            if (next < candidate + need) {
                candidate = 0U;
                retired += kNextCloseCost + kJoinCost;
            } else {
                retired += kBelowEndTestCost;
                if (candidate >= end) {
                    return finish_candidate(m, header, cur, candidate, size, retired);
                }
                candidate = 0U;
                retired += kBelowEndCost + kJoinCost;
            }
        }
        retired += kLoopTestCost;
        const GuestAddress sentinel = m.read_u32_le(header + kSentinelSlot);
        cur = next;
        if (next == sentinel) {
            return finish_candidate(m, header, cur, candidate, size, retired);
        }
    }
}

template <class Context>
std::uint32_t first_fit_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = first_fit(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                            context.cpu.read_gpr_u32(6U))
                                      .retired;
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run alloc_aligned_from_block_list(Context& context) {
    auto call = n3::guest_call(context);
    const AllocOutcome outcome = first_fit(call.memory(), call.arg(0), call.arg(1), call.arg(2));
    call.return_value(outcome.pointer);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_002a0850{0x002a0850U, 224U, 0xa14b335e88ed7652ULL, "AllocAlignedFromBlockList",
                                                  kCeiling, n3::live<&alloc_aligned_from_block_list<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &first_fit_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002a0850, alloc_aligned_from_block_list, first_fit_cost)
