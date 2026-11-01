// 0x002b6ca0 (a0 = list descriptor D, a1 = node N): UnlinkNodeAndPushToFreeList. D[0] points at an owner
// object whose word at +16 is the head of its free list, and D[4] is the head of the list N is on. N[0] is
// the next node and N[4] the previous one. N leaves its list (next.prev takes N's prev; when N had no prev,
// D[4] takes N's next) and is pushed on the front of the free list. Every word is read where the listing
// reads it, since the stores can alias. Returns nothing (v0 is never written). The temporaries end with the
// values the listing leaves in them: v1 = owner and a2 = prev.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run unlink_node_and_push_to_free_list(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress descriptor = call.arg(0);
    const GuestAddress node = call.arg(1);

    // Next test: lw, beq, nop. With a next node, next.prev = node.prev: lw, sw.
    std::uint32_t retired = 3U;
    const GuestAddress next = m.read_u32_le(node);
    if (next != 0U) {
        const std::uint32_t prev_of_node = m.read_u32_le(node + 4U);
        m.write_u32_le(next + 4U, prev_of_node);
        retired += 2U;
    }

    // Prev test: lw, beq, nop.
    retired += 3U;
    const GuestAddress prev = m.read_u32_le(node + 4U);
    if (prev == 0U) {
        // Lw, beq, nop (3), then the store to D[4] and the jump to the tail (2). The original stores zero when
        // the next node is zero and the next node otherwise, which is one value either way.
        retired += 3U;
        const std::uint32_t following = m.read_u32_le(node);
        m.write_u32_le(descriptor + 4U, following);
        retired += 2U;
    } else {
        // prev.next = node.next, then node.prev = 0: lw, sw, sw.
        retired += 3U;
        const std::uint32_t following = m.read_u32_le(node);
        m.write_u32_le(prev, following);
        m.write_u32_le(node + 4U, 0U);
    }

    // Common tail (0x002b6ccc): N goes to the front of the free list. Lw, lw, sw, lw, beq, nop (6): the owner's
    // free head becomes N.next, and the old head, when there is one, gets N as its prev (1 more). Then
    // owner+16 = N (lw, beq, sw: 3) and the return (jr, nop: 2).
    retired += 6U;
    const GuestAddress owner = m.read_u32_le(descriptor);
    const std::uint32_t free_head = m.read_u32_le(owner + 16U);
    m.write_u32_le(node, free_head);
    const GuestAddress head = m.read_u32_le(node);
    if (head != 0U) {
        m.write_u32_le(head + 4U, node);
        retired += 1U;
    }
    retired += 3U;
    const GuestAddress owner_again = m.read_u32_le(descriptor);
    m.write_u32_le(owner_again + 16U, node);
    retired += 2U;

    // The temporaries end with the values the listing leaves: v1 = owner, a2 = prev.
    call.cpu().write_gpr_sign_extended_32(3U, owner_again);
    call.cpu().write_gpr_sign_extended_32(6U, prev);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002b6ca0{0x002b6ca0U, 120U, 0xe50e9f148b11fd7dULL,
                                                  "UnlinkNodeAndPushToFreeList", 25U,
                                                  n3::live<&unlink_node_and_push_to_free_list<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b6ca0, unlink_node_and_push_to_free_list)
