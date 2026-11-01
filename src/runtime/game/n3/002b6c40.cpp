// 0x002b6c40 (a0 = list object L): ReleaseListToFreePool. L+0 points at the pool struct P, and L+4 is the head of a
// singly linked list of nodes (node+0 = next, node+4 = prev). The whole list is spliced onto the front of P's free list
// (P+16): the tail's next becomes the old free head f, a non-null f gets prev = tail, P+16 becomes the old list head,
// and L+4 is cleared. An empty list changes nothing. Returns nothing (v0 is never written).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kOwnerOffset = 0U;       // L+0: the pool struct P
constexpr GuestAddress kHeadOffset = 4U;        // L+4: head of the node list
constexpr GuestAddress kNextOffset = 0U;        // node+0: next node
constexpr GuestAddress kPrevOffset = 4U;        // node+4: previous node
constexpr GuestAddress kFreeHeadOffset = 16U;   // P+16: head of P's free list

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCheckCost = 3U;    // lw, beq, nop: the empty-list test
constexpr std::uint32_t kEnterWalkCost = 2U;    // beq (always taken), daddu: the walk starts at the head
constexpr std::uint32_t kWalkStepCost = 6U;     // lw, nop, nop, nop, bne, nop: one node's next pointer
constexpr std::uint32_t kWalkAdvanceCost = 1U;  // daddu: the walk moves on to the next node
constexpr std::uint32_t kSpliceCost = 6U;       // lw, lw, sw, lw, beq, nop: tail->next = f
constexpr std::uint32_t kPrevLinkCost = 1U;     // sw: f->prev = tail, only when f is not null
constexpr std::uint32_t kHeadStoreCost = 3U;    // lw, sw, sw: P+16 = old list head, L+4 = 0
constexpr std::uint32_t kReturnCost = 2U;       // jr $ra and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is always
// the original's exact count. The walk also stops here, so a cyclic list cannot keep the dry run going forever.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as the walk passes it.
template <class Memory>
std::uint32_t release_list(Memory& m, const GuestAddress list) {
    const std::uint32_t head = m.read_u32_le(list + kHeadOffset);
    if (head == 0U) {
        return kHeadCheckCost + kReturnCost;
    }
    std::uint32_t retired = kHeadCheckCost + kEnterWalkCost;
    // The walk ends at the tail, the node whose next pointer is 0.
    GuestAddress tail = head;
    for (;;) {
        const std::uint32_t next = m.read_u32_le(tail + kNextOffset);
        retired += kWalkStepCost;
        if (retired > kCeiling) {
            return retired;
        }
        if (next == 0U) {
            break;
        }
        retired += kWalkAdvanceCost;
        tail = next;
    }
    // The stores keep the listing's order and reads: the tail's next is read back after it is stored, and L+0 is read
    // again after the first stores, since they may have changed it.
    const GuestAddress owner = m.read_u32_le(list + kOwnerOffset);
    const std::uint32_t old_free_head = m.read_u32_le(owner + kFreeHeadOffset);
    m.write_u32_le(tail + kNextOffset, old_free_head);
    const std::uint32_t linked = m.read_u32_le(tail + kNextOffset);
    if (linked != 0U) {
        m.write_u32_le(linked + kPrevOffset, tail);
        retired += kPrevLinkCost;
    }
    const GuestAddress owner_again = m.read_u32_le(list + kOwnerOffset);
    m.write_u32_le(owner_again + kFreeHeadOffset, head);
    m.write_u32_le(list + kHeadOffset, 0U);
    retired += kSpliceCost + kHeadStoreCost + kReturnCost;
    return retired;
}

// A DryMemory that declines an access the original would fault on (lw and sw need a 4-byte aligned address inside
// guest memory) instead of performing it. The faulted flag makes call_cost return 0, so the original runs the call and
// faults.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        return accessible(address) ? dry_.read_u32_le(address) : 0U;
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (accessible(address)) {
            dry_.write_u32_le(address, value);
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool accessible(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return false;
        }
        return true;
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// call_cost: the exact count of a path that returns, and 0 (the original runs the call) when the walk passes the
// ceiling or an access would fault.
template <class Context>
std::uint32_t release_list_to_free_pool_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = release_list(dry, context.cpu.read_gpr_u32(4U));
    return (!dry.faulted() && retired <= kCeiling) ? retired : 0U;
}

template <class Context>
n3::Run release_list_to_free_pool(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(release_list(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002b6c40{0x002b6c40U, 96U, 0x2aa1cb77dc8127f5ULL, "ReleaseListToFreePool",
                                                  kCeiling, n3::live<&release_list_to_free_pool<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &release_list_to_free_pool_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002b6c40, release_list_to_free_pool, release_list_to_free_pool_cost)
