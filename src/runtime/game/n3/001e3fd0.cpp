// 0x001e3fd0 RwFrameUpdateObjects (a0 = frame F; returns F as the whole 64-bit a0).
// The root frame R = F[160] has a flag byte at +3 and a dirty-list node at R+8 ({next at +0, prev at +4}).
// While R's low two flag bits are clear, R is not on the dirty list yet: its node becomes the head of the
// list whose head word is at 0x00670c8c (the node's next is the old first entry, its prev is the head word,
// the old first entry's prev is pointed at the node, and the head word points at the node). Then R's flag
// byte gets its low two bits set, and F's flag byte gets bits 2-3 set.
//
// The retired counts come from the listing: 13 when R was already flagged (the bnel is taken and its delay
// slot runs), 22 when it was not (the bnel falls through and its delay slot is skipped).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRootOffset = 160U;        // F + 160: the root frame R
constexpr GuestAddress kFlagsOffset = 3U;         // flag byte at +3 of both F and R
constexpr GuestAddress kNodeOffset = 8U;          // R + 8: the dirty-list node inside R
constexpr GuestAddress kNodeNext = 0U;            // node + 0
constexpr GuestAddress kNodePrev = 4U;            // node + 4
constexpr GuestAddress kDirtyHead = 0x00670c8cU;  // head word of the dirty list (lui 0x67, +3212)
constexpr std::uint8_t kRootBits = 0x03U;         // set on R's flags once it is on the list
constexpr std::uint8_t kFrameBits = 0x0cU;        // set on F's flags

constexpr std::uint32_t kFlaggedCost = 13U;       // R was already flagged
constexpr std::uint32_t kLinkedCost = 22U;        // R was linked onto the list by this call

template <class Context>
n3::Run rw_frame_update_objects(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress frame = call.arg(0);
    const GuestAddress root = m.read_u32_le(frame + kRootOffset);
    const std::uint8_t root_flags = m.read_u8(root + kFlagsOffset);
    std::uint32_t retired = kFlaggedCost;
    if ((root_flags & kRootBits) == 0U) {
        // The head is read before the node's stores and again after them, as the original does: when the node
        // overlaps the head word, the second read sees the first store.
        const GuestAddress node = root + kNodeOffset;
        const GuestAddress first = m.read_u32_le(kDirtyHead + kNodeNext);
        m.write_u32_le(node + kNodePrev, kDirtyHead);
        m.write_u32_le(node + kNodeNext, first);
        m.write_u32_le(m.read_u32_le(kDirtyHead + kNodeNext) + kNodePrev, node);
        m.write_u32_le(kDirtyHead + kNodeNext, node);
        retired = kLinkedCost;
    }
    // Both paths: F[160] is read again (the list stores may have changed it), R gets the flags read on entry
    // with its low two bits set, then F's flag byte is read (after that store) and gets bits 2-3.
    const GuestAddress root_now = m.read_u32_le(frame + kRootOffset);
    m.write_u8(root_now + kFlagsOffset, static_cast<std::uint8_t>(root_flags | kRootBits));
    const std::uint8_t frame_flags = m.read_u8(frame + kFlagsOffset);
    m.write_u8(frame + kFlagsOffset, static_cast<std::uint8_t>(frame_flags | kFrameBits));
    call.return_value64(call.arg64(0));
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001e3fd0, 92U, 0xd35c79a0a975e657ULL, "RwFrameUpdateObjects", 22U, rw_frame_update_objects,
               EngineReturn::Integer)
