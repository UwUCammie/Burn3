// 0x00163e90 (a0 = pool manager M, a1 = entry E, a2 = notify flag): RetireQueueEntry. E joins the end of one of M's two
// singly linked queues: queue A when the byte at 0x01e65499 is nonzero, queue B otherwise. Queue A also writes 120 into
// a 32-slot byte ring (the index byte moves on and wraps to 0 at 32); queue B counts its byte down by one. E's flags
// then decide whether ReleaseRecordMoveLast (0x001b84a0) frees E's pool record: it does when a2 is nonzero, flag bit 1
// is clear, and either flag bit 2 is clear or E's signed kind byte is 8. Last, E's words at +268 and +272 clear, E's id
// bit leaves M's 256-bit bitmap, and E's flag bit 0 clears. Returns nothing (v0 is not written here).
//
// Retired counts run from 65 (queue B, nothing released) to 89 (queue A with a ring wrap, a kind-8 record with
// flag bit 2 set, and a release), plus the callee's count when it runs. The listing's bgez on the id byte is always
// taken (a zero-extended byte is never negative), so its fixup block is not reproduced. Stores keep the listing's
// order: the ring slot is the index byte itself at index 33, and an entry may alias a queue word.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001b84a0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// The listing adds lui 0x0003 (0x30000) to M, then each access adds its own displacement.
constexpr GuestAddress kHigh = 0x00030000U;
constexpr GuestAddress kQueueBHead = kHigh + 25444U;   // word: head of queue B
constexpr GuestAddress kQueueBTail = kHigh + 25448U;   // word: tail of queue B
constexpr GuestAddress kQueueAHead = kHigh + 25456U;   // word: head of queue A
constexpr GuestAddress kQueueATail = kHigh + 25460U;   // word: tail of queue A
constexpr GuestAddress kRing = kHigh + 25464U;         // bytes: slot kRing + index
constexpr GuestAddress kRingIndex = kHigh + 25497U;    // byte
constexpr GuestAddress kCounter = kHigh + 25516U;      // byte, counted down by queue B
constexpr GuestAddress kUsedBits = kHigh + 25312U;     // eight words: bit (id & 31) of word (id >> 5)
constexpr GuestAddress kQueueSelect = 0x01e65499U;     // byte: nonzero selects queue A

// Fields of entry E.
constexpr GuestAddress kNext = 264U;                   // word: next entry of its queue
constexpr GuestAddress kKind = 371U;                   // signed byte
constexpr GuestAddress kFlags = 372U;                  // byte
constexpr GuestAddress kClearedA = 268U;               // word, cleared on retire
constexpr GuestAddress kClearedB = 272U;               // word, cleared on retire
constexpr GuestAddress kId = 375U;                     // byte: its bit in the used bitmap

constexpr std::uint32_t kFlagBit0 = 0x01U;
constexpr std::uint32_t kFlagBit1 = 0x02U;
constexpr std::uint32_t kFlagBit2 = 0x04U;
constexpr std::int8_t kReleaseKind = 8;                // kind that is tested for release when flag bit 2 is set
constexpr std::uint8_t kRingMark = 120U;
constexpr std::uint32_t kRingSize = 32U;
constexpr GuestAddress kPool = 0x0055ef70U;            // the pool ReleaseRecordMoveLast frees from
constexpr std::uint32_t kFrameBytes = 48U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing, delay slots included, callee excluded.
constexpr std::uint32_t kPrologueCost = 9U;    // 0x00163e90..0x00163eb0: frame, queue select, branch and its slot
constexpr std::uint32_t kHeadTestCost = 5U;    // either queue's head test, branch and its slot
constexpr std::uint32_t kAppendCost = 8U;      // either queue's append (empty or not), jump and slot included
constexpr std::uint32_t kQueueCost = kPrologueCost + kHeadTestCost + kAppendCost;   // 22 on both queues
constexpr std::uint32_t kRingCost = 16U;       // 0x00163ee8..0x00163f24
constexpr std::uint32_t kRingWrapCost = 4U;    // 0x00163f28..0x00163f34, when the index reaches 32
constexpr std::uint32_t kCounterCost = 7U;     // 0x00163f6c..0x00163f84
constexpr std::uint32_t kFlagsCost = 4U;       // 0x00163f88..0x00163f94: flags load and bit 2 test
constexpr std::uint32_t kKindCost = 4U;        // 0x00164068..0x00164074: kind load and compare
constexpr std::uint32_t kKindMatchCost = 1U;   // 0x00164078: kind is 8, on to the bit 1 test
constexpr std::uint32_t kBit1Cost = 3U;        // bit 1 test: andi, bne and its slot
constexpr std::uint32_t kNotifyCost = 2U;      // 0x00163fa4..0x00163fa8: a2 test
constexpr std::uint32_t kCallCost = 4U;        // 0x00163fac..0x00163fb8: lui, move, jal and its slot
constexpr std::uint32_t kRetireCost = 29U;     // 0x00163fbc..0x00163fcc, 0x00163fdc..0x00164024, return (5 + 19 + 5)

// Appends `entry` to a queue. An empty queue takes it as head and tail; otherwise the old tail links to it. Either way
// its next link clears. The stores keep the listing's order.
template <class Memory>
void append_to_queue(Memory& m, const GuestAddress head_at, const GuestAddress tail_at, const GuestAddress entry) {
    if (m.read_u32_le(head_at) == 0U) {
        m.write_u32_le(head_at, entry);
        m.write_u32_le(tail_at, entry);
        m.write_u32_le(entry + kNext, 0U);
        return;
    }
    const GuestAddress tail = m.read_u32_le(tail_at);
    m.write_u32_le(tail + kNext, entry);
    m.write_u32_le(entry + kNext, 0U);
    m.write_u32_le(tail_at, entry);
}

// Queue A's ring: the index byte picks the slot that gets kRingMark, then the index moves on and wraps to 0 once it
// reaches the ring size. The index is read back after the slot store, so an index of 33 (the slot is the index byte
// itself) reads back as kRingMark. Returns whether the index wrapped.
template <class Memory>
bool advance_ring(Memory& m, const GuestAddress manager) {
    const std::uint32_t slot = m.read_u8(manager + kRingIndex);
    m.write_u8(manager + kRing + slot, kRingMark);
    m.write_u8(manager + kRingIndex, static_cast<std::uint8_t>(m.read_u8(manager + kRingIndex) + 1U));
    const std::uint32_t next = m.read_u8(manager + kRingIndex);
    if (next < kRingSize) {
        return false;
    }
    m.write_u8(manager + kRingIndex, 0U);
    return true;
}

// The retire block, in the listing's order: E's words at +268 and +272 clear, E's id bit leaves the bitmap, and E's
// flag bit 0 clears. The id and the flags are read here, after any release, as the listing reads them.
template <class Memory>
void retire_entry(Memory& m, const GuestAddress manager, const GuestAddress entry) {
    m.write_u32_le(entry + kClearedA, 0U);
    m.write_u32_le(entry + kClearedB, 0U);
    const std::uint32_t id = m.read_u8(entry + kId);
    const GuestAddress word = manager + kUsedBits + 4U * (id >> 5U);
    m.write_u32_le(word, m.read_u32_le(word) & ~(1U << (id & 31U)));
    m.write_u8(entry + kFlags, static_cast<std::uint8_t>(m.read_u8(entry + kFlags) & ~kFlagBit0));
}

template <class Context>
n3::Run retire_queue_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress manager = call.arg(0);
    const GuestAddress entry = call.arg(1);
    const std::uint32_t notify = call.arg(2);

    std::uint32_t retired = 0U;
    if (m.read_u8(kQueueSelect) != 0U) {
        append_to_queue(m, manager + kQueueAHead, manager + kQueueATail, entry);
        retired = kQueueCost + kRingCost;
        if (advance_ring(m, manager)) {
            retired += kRingWrapCost;
        }
    } else {
        append_to_queue(m, manager + kQueueBHead, manager + kQueueBTail, entry);
        m.write_u8(manager + kCounter, static_cast<std::uint8_t>(m.read_u8(manager + kCounter) - 1U));
        retired = kQueueCost + kCounterCost;
    }

    // The flags are read after the queue step, as the listing reads them.
    const std::uint32_t flags = m.read_u8(entry + kFlags);
    retired += kFlagsCost;
    if ((flags & kFlagBit2) != 0U) {
        retired += kKindCost;
        if (static_cast<std::int8_t>(m.read_u8(entry + kKind)) != kReleaseKind) {
            retire_entry(m, manager, entry);
            return call.finish(retired + kRetireCost);
        }
        retired += kKindMatchCost;
    }
    retired += kBit1Cost;
    if ((flags & kFlagBit1) == 0U) {
        retired += kNotifyCost;
        if (notify != 0U) {
            // a1 still holds E (the listing's daddu copies it; nothing above writes it); the callee reads a0 and a1.
            call.set_arg(0, kPool);
            const n3::Run released =
                call.invoke([](auto& memory) { return n3::fn_001b84a0(memory); }, kFrameBytes);
            if (released.declined) {
                return call.decline();
            }
            retired += kCallCost + released.retired;
        }
    }
    retire_entry(m, manager, entry);
    return call.finish(retired + kRetireCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00163e90, 516U, 0x6092320cc0228764ULL, "RetireQueueEntry", kCeiling, retire_queue_entry,
         EngineReturn::Nothing)
