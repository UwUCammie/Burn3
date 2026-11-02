// 0x0035df60 FlushSpatialHierarchy (a0 = hierarchy H). H is the record that InsertIntoSpatialHierarchy (0x0035dbb0)
// and GroupCentroidExtent (0x0035d960) work on (their headers describe it). It holds two lists whose head links sit
// inside H: the pending points (next at H+8, prev at H+12, which is the tail) and the active clusters (next at H+16,
// prev at H+20). A node is the address of its link pair (next at +0, prev at +4); a list is empty when its tail is
// its own head's address.
//
// Phase 1 takes pending points from the tail and inserts each one at level 1 (0x0035dbb0), for as long as H's free
// cluster count (H+60) is not below its last level (H+52); both are compared signed. Phase 2 takes active clusters
// from the tail: each one first gets 0x0035d960, then it is unlinked and its link pair is cleared. Returns nothing.
//
// Either callee may decline. The decline is passed up: the dry run that sets the count runs the callees too, so the
// original then runs the whole call on memory that nothing has been changed in.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0035dbb0)
N3_CALLEE(0035d960)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Record H.
constexpr GuestAddress kPendingHead = 8U;
constexpr GuestAddress kPendingTail = 12U;
constexpr GuestAddress kActiveHead = 16U;
constexpr GuestAddress kActiveTail = 20U;
constexpr GuestAddress kLastLevel = 52U;     // signed
constexpr GuestAddress kFreeCount = 60U;     // signed
// A node's link pair, and the record a link belongs to (the link minus the offset).
constexpr GuestAddress kNext = 0U;
constexpr GuestAddress kPrev = 4U;
constexpr GuestAddress kPointLink = 20U;     // pending point P: 0035dbb0's a3
constexpr GuestAddress kClusterLink = 28U;   // active cluster C: 0035d960's a1
constexpr std::uint32_t kFrameBytes = 48U;   // the prologue's addiu sp,sp,-48, which both callees run under
constexpr std::uint32_t kLevel = 1U;         // t0 for 0035dbb0
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;        // prologue and the jump to the pending test
constexpr std::uint32_t kPendingTestCost = 3U;  // pending list empty?
constexpr std::uint32_t kLevelTestCost = 5U;    // free clusters below the last level?
constexpr std::uint32_t kInsertCost = 12U;      // unlink, the jal to 0035dbb0 with its delay slot, tail reload
constexpr std::uint32_t kActiveEntryCost = 2U;  // jump from phase 1 into the active test, with its tail load
constexpr std::uint32_t kActiveTestCost = 3U;   // active list empty?
constexpr std::uint32_t kExtentCost = 12U;      // jal to 0035d960 with its delay slot, unlink, clears, tail reload
constexpr std::uint32_t kReturnCost = 5U;       // epilogue and jr ra

// Takes `node` out of its list. The loads and stores are the original's, in its order: the second pair of loads
// comes after the first store and sees it.
template <class Memory>
void unlink_node(Memory& m, const GuestAddress node) {
    const GuestAddress next = m.read_u32_le(node + kNext);
    const GuestAddress prev = m.read_u32_le(node + kPrev);
    m.write_u32_le(prev + kNext, next);
    const GuestAddress prev_again = m.read_u32_le(node + kPrev);
    const GuestAddress next_again = m.read_u32_le(node + kNext);
    m.write_u32_le(next_again + kPrev, prev_again);
}

template <class Context>
n3::Run flush_spatial_hierarchy(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress hierarchy = call.arg(0U);
    const GuestAddress pending_head = hierarchy + kPendingHead;
    const GuestAddress active_head = hierarchy + kActiveHead;
    std::uint32_t retired = kEntryCost;

    // Phase 1: pending points, from the tail, while the free cluster count is not below the last level.
    GuestAddress tail = m.read_u32_le(hierarchy + kPendingTail);
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kPendingTestCost;
        if (tail == pending_head) {
            break;
        }
        retired += kLevelTestCost;
        if (static_cast<std::int32_t>(m.read_u32_le(hierarchy + kFreeCount)) <
            static_cast<std::int32_t>(m.read_u32_le(hierarchy + kLastLevel))) {
            break;
        }
        retired += kInsertCost;
        unlink_node(m, tail);
        call.set_arg(0U, hierarchy);
        call.set_arg(1U, 0U);
        call.set_arg(2U, hierarchy);
        call.set_arg(3U, tail - kPointLink);
        call.set_arg(4U, kLevel);
        const n3::Run inserted = call.invoke([](auto& machine) { return n3::fn_0035dbb0(machine); }, kFrameBytes);
        if (inserted.declined) {
            return call.decline();
        }
        retired += inserted.retired;
        tail = m.read_u32_le(hierarchy + kPendingTail);
    }

    // Phase 2: active clusters, from the tail. The callee runs before the unlink, as in the original.
    retired += kActiveEntryCost;
    GuestAddress active = m.read_u32_le(hierarchy + kActiveTail);
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kActiveTestCost;
        if (active == active_head) {
            break;
        }
        retired += kExtentCost;
        call.set_arg(0U, hierarchy);
        call.set_arg(1U, active - kClusterLink);
        const n3::Run extent = call.invoke([](auto& machine) { return n3::fn_0035d960(machine); }, kFrameBytes);
        if (extent.declined) {
            return call.decline();
        }
        retired += extent.retired;
        unlink_node(m, active);
        m.write_u32_le(active + kPrev, 0U);
        m.write_u32_le(active + kNext, 0U);
        active = m.read_u32_le(hierarchy + kActiveTail);
    }
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0035df60, 196U, 0x663e9c24484b9cd5ULL, "FlushSpatialHierarchy", kCeiling, flush_spatial_hierarchy,
         EngineReturn::Nothing)
