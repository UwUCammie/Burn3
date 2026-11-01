// 0x0035d250 (a0 = pool P, a1 = node X): ReleaseNodeChain. X sits on an intrusive list through its link pair
// (+20 next, +24 pointer to the previous link) and has a parent at +16. Every node also has a second link pair
// (+28 / +32) used by the pool's "touched" list at P+16, a use count at +36, and the pool keeps three lists with
// their heads at P+8 (released nodes, link pair +20/+24 of the node), P+16 (second pair) and P+24 (link pair
// +20/+24 again, count at P+60).
//
// Releasing X unlinks it from its list and, if it has a parent, drops one reference of that parent; a parent whose
// count reaches 0 is itself unlinked and its parent loses a reference, and so on, up to seven ancestors. Where the
// climb stops the stopping node is put on the P+16 list when it is not on it yet (references remain), or
// (parent missing) it is just unlinked. Then every node that reached 0 is, deepest first, taken off the P+16 list if
// it is on it and pushed onto the P+24 list (P+60 counts those). Finally X is pushed onto the P+8 list.
// An eighth ancestor needs the cold helpers 0x0035d1c0 / 0x0035ca00 / 0x0035d210 (no n3 file): the call is declined.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kParent = 16U;
constexpr GuestAddress kNext = 20U;
constexpr GuestAddress kPrevLink = 24U;
constexpr GuestAddress kNext2 = 28U;
constexpr GuestAddress kPrevLink2 = 32U;
constexpr GuestAddress kRefs = 36U;

constexpr GuestAddress kReleasedHead = 8U;    // P + 8
constexpr GuestAddress kTouchedHead = 16U;    // P + 16
constexpr GuestAddress kZeroedHead = 24U;     // P + 24
constexpr GuestAddress kZeroedCount = 60U;    // P + 60

constexpr std::uint32_t kMaxAncestors = 7U;   // ancestors handled natively (s4, s3, s2, s1, s0, s6, s7)

// Retired instruction counts per block from the listing.
constexpr std::uint32_t kEntry = 15U;
constexpr std::uint32_t kFirstStep = 17U;     // unlink X, drop the first ancestor's reference, test
constexpr std::uint32_t kStep = 13U;          // unlink a node, drop its parent's reference, test
constexpr std::uint32_t kParentTest = 3U;     // load the parent, branch if missing
constexpr std::uint32_t kUnlinkOnly = 7U;     // unlink a node whose parent is missing
constexpr std::uint32_t kTouchedTest = 3U;    // is the node on the P+16 list already
constexpr std::uint32_t kTouchedPush = 8U;
constexpr std::uint32_t kZeroedTest = 3U;
constexpr std::uint32_t kZeroedUnlink = 5U;
constexpr std::uint32_t kZeroedPush = 12U;
constexpr std::uint32_t kReleasePush = 10U;
constexpr std::uint32_t kReturn = 12U;
constexpr std::uint32_t kLongestPath = 318U;

// Takes node off the list its +20/+24 link pair is on.
template <class Memory>
void unlink(Memory& m, const GuestAddress node) {
    const std::uint32_t next = m.read_u32_le(node + kNext);
    m.write_u32_le(m.read_u32_le(node + kPrevLink), next);
    const std::uint32_t prev = m.read_u32_le(node + kPrevLink);
    m.write_u32_le(m.read_u32_le(node + kNext) + 4U, prev);
}

// Pushes the link pair at node + offset onto the list headed at head (head word = first link, links are
// {next, previous-link-pointer}), the way the pool does everywhere in this function.
template <class Memory>
void push_front(Memory& m, const GuestAddress head, const GuestAddress link, const GuestAddress prev_slot) {
    const std::uint32_t first = m.read_u32_le(head);
    m.write_u32_le(link, first);
    m.write_u32_le(prev_slot, head);
    m.write_u32_le(m.read_u32_le(head) + 4U, link);
    m.write_u32_le(head, link);
}

template <class Context>
n3::Run release_node_chain(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pool = call.arg(0);
    const GuestAddress released = call.arg(1);

    std::uint32_t retired = kEntry;
    GuestAddress chain[kMaxAncestors + 2U];
    chain[0] = released;
    std::uint32_t top = 0U;            // deepest chain index that was reached
    bool stopped_nonzero = false;      // true: chain[top] kept references; false: chain[top] had no parent

    const GuestAddress first_parent = m.read_u32_le(released + kParent);
    if (first_parent != 0U) {
        chain[1] = first_parent;
        for (std::uint32_t level = 1U;; ++level) {
            const GuestAddress node = chain[level];
            unlink(m, chain[level - 1U]);
            m.write_u32_le(node + kRefs, m.read_u32_le(node + kRefs) - 1U);
            m.write_u32_le(chain[level - 1U] + kParent, 0U);
            retired += level == 1U ? kFirstStep : kStep;
            top = level;
            if (level == kMaxAncestors + 1U) {
                return call.decline();   // eighth ancestor: the original calls the cold helpers
            }
            if (m.read_u32_le(node + kRefs) != 0U) {
                stopped_nonzero = true;
                break;
            }
            const GuestAddress parent = m.read_u32_le(node + kParent);
            retired += kParentTest;
            if (parent == 0U) {
                unlink(m, node);
                retired += kUnlinkOnly;
                break;
            }
            chain[level + 1U] = parent;
        }

        std::uint32_t zeroed_from = top;
        if (stopped_nonzero) {
            const GuestAddress node = chain[top];
            retired += kTouchedTest;
            if (m.read_u32_le(node + kNext2) == 0U) {
                push_front(m, pool + kTouchedHead, node + kNext2, node + kPrevLink2);
                retired += kTouchedPush;
            }
            zeroed_from = top - 1U;
        }
        for (std::uint32_t level = zeroed_from; level >= 1U; --level) {
            const GuestAddress node = chain[level];
            retired += kZeroedTest;
            const std::uint32_t second = m.read_u32_le(node + kNext2);
            if (second != 0U) {
                m.write_u32_le(m.read_u32_le(node + kPrevLink2), second);
                const std::uint32_t prev = m.read_u32_le(node + kPrevLink2);
                m.write_u32_le(m.read_u32_le(node + kNext2) + 4U, prev);
                retired += kZeroedUnlink;
            }
            push_front(m, pool + kZeroedHead, node + kNext, node + kPrevLink);
            m.write_u32_le(pool + kZeroedCount, m.read_u32_le(pool + kZeroedCount) + 1U);
            retired += kZeroedPush;
        }
    }

    push_front(m, pool + kReleasedHead, released + kNext, released + kPrevLink);
    retired += kReleasePush + kReturn;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0035d250, 1808U, 0x482e32c9787014c5ULL, "ReleaseNodeChain", 318U, release_node_chain, EngineReturn::Nothing)
