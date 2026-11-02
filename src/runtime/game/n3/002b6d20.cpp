// 0x002b6d20 MoveNodeToListFront (a0 = destination list descriptor, a1 = node, a2 = source list descriptor).
// Nodes are doubly linked: next at +0, previous at +4. A descriptor keeps its first node at +4. The node is
// unlinked from its list (the next node's previous becomes the node's previous; the previous node's next becomes the
// node's next, or, with no previous node, the source descriptor's first node becomes the node's next), then pushed
// on the front of the destination list. Every load stays where the listing has it, since the stores can alias.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kNext = 0U;
constexpr GuestAddress kPrev = 4U;
constexpr GuestAddress kFirst = 4U;  // descriptor field holding the first node

// Retired counts of the listing's parts (delay slots included).
constexpr std::uint32_t kNextAbsent = 3U;      // 0x2b6d20..0x2b6d28 taken
constexpr std::uint32_t kNextPresent = 5U;     // plus lw, sw
constexpr std::uint32_t kPrevPresent = 5U;     // 0x2b6d34..0x2b6d44
constexpr std::uint32_t kPrevAbsent = 8U;      // 0x2b6d34..0x2b6d3c, 0x2b6d6c..0x2b6d84
constexpr std::uint32_t kPushEmpty = 10U;      // 0x2b6d48..0x2b6d50, 0x2b6d88..0x2b6d94
constexpr std::uint32_t kPushOnto = 11U;       // 0x2b6d48..0x2b6d68, 0x2b6d90..0x2b6d94

template <class Context>
n3::Run move_node_to_list_front(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress dest = call.arg(0U);
    const GuestAddress node = call.arg(1U);
    const GuestAddress source = call.arg(2U);
    std::uint32_t retired;

    // The next node now points back at the node's previous one.
    const GuestAddress after = m.read_u32_le(node + kNext);
    if (after != 0U) {
        m.write_u32_le(after + kPrev, m.read_u32_le(node + kPrev));
        retired = kNextPresent;
    } else {
        retired = kNextAbsent;
    }

    // The previous node (or, for the first node, the source descriptor) now points at the node's next one.
    const GuestAddress before = m.read_u32_le(node + kPrev);
    if (before != 0U) {
        m.write_u32_le(before + kNext, m.read_u32_le(node + kNext));
        retired += kPrevPresent;
    } else {
        m.write_u32_le(source + kFirst, m.read_u32_le(node + kNext));
        retired += kPrevAbsent;
    }

    // Push on the front of the destination.
    const GuestAddress head = m.read_u32_le(dest + kFirst);
    if (head != 0U) {
        m.write_u32_le(node + kNext, head);
        m.write_u32_le(m.read_u32_le(node + kNext) + kPrev, node);
        retired += kPushOnto;
    } else {
        m.write_u32_le(node + kNext, 0U);
        retired += kPushEmpty;
    }
    m.write_u32_le(node + kPrev, 0U);
    m.write_u32_le(dest + kFirst, node);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002b6d20, 120U, 0xae43b24d8eb86855ULL, "MoveNodeToListFront", 24U, move_node_to_list_front,
               EngineReturn::Nothing)
