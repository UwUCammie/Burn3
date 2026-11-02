// 0x0014a290 (a1 = strip object S, a2 = slot T of the target strip, a3 = first quad index, t0 = quad count):
// FollowRouteQuads. For each of the count quads from index a3 of S, both link chains of the quad must stay open
// ahead and arrive at T. A quad record holds two links: link 1 is the strip id byte at +4 and the quad halfword at
// +6, link 2 the strip id byte at +5 and the quad halfword at +8. A link passes when RouteOpenAheadScan(strip, quad, 1)
// is nonzero. The strip's slot is 0x0052268c + 8 * strip; when it is T the chain has arrived, otherwise the chain goes
// on through that strip's quad record at the same quad, reading the same link there. Link 1 is walked first and link
// 2 only when link 1 misses. After each quad the index moves on: past the last quad of S an open strip (closed flag
// 0) ends the check with 0, and a closed strip wraps (the count is subtracted from the index). Returns 1 when every
// quad passes, 0 otherwise.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00148ca0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// A strip object (a1) and a slot (a2) hold their strip record pointer at +4. The record holds the quad record array
// pointer (+8, 10-byte quad records), the quad count (halfword +12) and the closed flag (byte +14).
constexpr GuestAddress kRecordPointer = 4U;
constexpr GuestAddress kRecordQuads = 8U;
constexpr GuestAddress kRecordQuadCount = 12U;
constexpr GuestAddress kRecordClosed = 14U;
constexpr std::uint32_t kQuadRecordBytes = 10U;

// The slot of strip id b is 0x00522660 + 8 * b + 44.
constexpr GuestAddress kSlotFirst = 0x0052268cU;
constexpr GuestAddress kSlotStride = 8U;

// The two links of a quad record: offsets of the strip id byte and of the quad halfword.
struct RouteLink {
    GuestAddress strip_offset;
    GuestAddress quad_offset;
};
constexpr RouteLink kFirstLink{4U, 6U};
constexpr RouteLink kSecondLink{5U, 8U};

// Where a link leads: a strip id (0..255) and a quad index (0..65535).
struct LinkTarget {
    std::uint32_t strip;
    std::uint32_t quad;
};

constexpr std::uint32_t kFrameBytes = 144U;  // the entry's addiu $sp, $sp, -144
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included (0014a290-0014a470).
constexpr std::uint32_t kPrologueCost = 15U;    // 0014a290-0014a2c8: frame, saved registers, jump to the loop test
constexpr std::uint32_t kLoopTestCost = 3U;     // 0014a39c-0014a3a4: count test and its delay slot
constexpr std::uint32_t kDoneCost = 2U;         // 0014a3a8-0014a3ac: v0 = 1 and the jump to the epilogue
constexpr std::uint32_t kEpilogueCost = 11U;    // 0014a448-0014a470: restore registers and return
constexpr std::uint32_t kRecordCost = 11U;      // 0014a2cc-0014a2f4: strip record, quad, link 1, jump into walk 1
constexpr std::uint32_t kCallBlockCost = 6U;    // 0014a33c-0014a350 (walk 1), 0014a414-0014a428 (walk 2): jal, bne
constexpr std::uint32_t kSlotTestCost = 8U;     // 0014a2f8-0014a314 (walk 1), 0014a3d0-0014a3ec (walk 2)
constexpr std::uint32_t kChainStepCost = 9U;    // 0014a318-0014a338 (walk 1), 0014a3f0-0014a410 (walk 2)
constexpr std::uint32_t kFirstFoundCost = 6U;   // link 1 arrives: 0014a3b0-0014a3b4, 0014a354-0014a358, 0014a35c-0014a360
constexpr std::uint32_t kFirstMissCost = 2U;    // link 1 fails: 0014a354-0014a358
constexpr std::uint32_t kSecondSetupCost = 6U;  // 0014a3b8-0014a3cc: link 2 of the quad, jump into walk 2
constexpr std::uint32_t kSecondFoundCost = 4U;  // link 2 arrives: 0014a434-0014a438, 0014a35c-0014a360
constexpr std::uint32_t kSecondMissCost = 6U;   // link 2 fails: 0014a42c-0014a430, 0014a35c-0014a360, 0014a43c-0014a440
constexpr std::uint32_t kAdvanceCost = 7U;      // 0014a364-0014a37c: next index, strip record, past-the-end test
constexpr std::uint32_t kEndTestCost = 3U;      // 0014a380-0014a388: the closed flag past the last quad
constexpr std::uint32_t kOpenEndCost = 1U;      // 0014a444: an open strip past its last quad leaves v0 = 0
constexpr std::uint32_t kWrapCost = 2U;         // 0014a38c-0014a390: index minus the quad count
constexpr std::uint32_t kCountStepCost = 2U;    // 0014a394-0014a398: one quad less to do

// What a link walk ended with: Reached when the chain arrived at T, Missed when RouteOpenAheadScan returned 0.
enum class ChainOutcome : std::uint8_t { Reached, Missed, Declined, Overrun };

// RouteOpenAheadScan(strip, quad, 1): nonzero when the strip stays open ahead from that quad.
template <class Memory>
n3::Run call_route_open_ahead(n3::Call<Memory>& call, const LinkTarget at) {
    call.set_arg(0, at.strip);
    call.set_arg(1, at.quad);
    call.set_arg(2, 1U);
    return call.invoke([](auto& m) { return n3::fn_00148ca0(m); }, kFrameBytes);
}

// The address of quad record `index` of a strip object or slot `object` (record pointer, then its quad array).
template <class Memory>
GuestAddress quad_record_address(const Memory& m, const GuestAddress object, const std::uint32_t index) {
    return m.read_u32_le(m.read_u32_le(object + kRecordPointer) + kRecordQuads) + kQuadRecordBytes * index;
}

// The strip id and quad index that one link of a quad record points at.
template <class Memory>
LinkTarget read_link(const Memory& m, const GuestAddress record, const RouteLink link) {
    return LinkTarget{m.read_u8(record + link.strip_offset), m.read_u16_le(record + link.quad_offset)};
}

// One link chain from `at`: RouteOpenAheadScan must pass, then the strip's slot is compared with T. Off T, the chain
// goes on through that strip's quad record at the same quad, reading the same link there.
template <class Memory>
ChainOutcome walk_link_chain(n3::Call<Memory>& call, const std::uint64_t target_slot, const RouteLink link, LinkTarget at,
                             std::uint32_t& retired) {
    for (;;) {
        if (retired > kCeiling) {
            return ChainOutcome::Overrun;
        }
        retired += kCallBlockCost;
        const n3::Run scan = call_route_open_ahead(call, at);
        if (scan.declined) {
            return ChainOutcome::Declined;
        }
        retired += scan.retired;
        if (call.result() == 0U) {
            return ChainOutcome::Missed;
        }
        retired += kSlotTestCost;
        // The original compares the whole 64-bit a2 (beq on $s7) with the slot address.
        const GuestAddress slot = kSlotFirst + kSlotStride * at.strip;
        if (static_cast<std::uint64_t>(slot) == target_slot) {
            return ChainOutcome::Reached;
        }
        retired += kChainStepCost;
        at = read_link(call.memory(), quad_record_address(call.memory(), slot, at.quad), link);
    }
}

// The walks of the quad at `index`: link 1 first, then link 2 (read from the quad record again, as the original does)
// when link 1 misses. Adds each walk's count and its tail.
template <class Memory>
ChainOutcome follow_quad(n3::Call<Memory>& call, const GuestAddress strip, const std::uint32_t index,
                         const std::uint64_t target_slot, std::uint32_t& retired) {
    Memory& m = call.memory();
    ChainOutcome outcome = walk_link_chain(call, target_slot, kFirstLink,
                                           read_link(m, quad_record_address(m, strip, index), kFirstLink), retired);
    if (outcome == ChainOutcome::Reached) {
        retired += kFirstFoundCost;
        return outcome;
    }
    if (outcome != ChainOutcome::Missed) {
        return outcome;
    }
    retired += kFirstMissCost + kSecondSetupCost;
    outcome = walk_link_chain(call, target_slot, kSecondLink,
                              read_link(m, quad_record_address(m, strip, index), kSecondLink), retired);
    if (outcome == ChainOutcome::Reached) {
        retired += kSecondFoundCost;
    } else if (outcome == ChainOutcome::Missed) {
        retired += kSecondMissCost;
    }
    return outcome;
}

template <class Context>
n3::Run follow_route_quads(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress strip = call.arg(1);
    const std::uint64_t target_slot = call.arg64(2);
    std::uint32_t index = call.arg(3) & 0xffffU;
    std::uint32_t remaining = call.arg(4) & 0xffffU;
    std::uint32_t retired = kPrologueCost;
    for (;;) {
        retired += kLoopTestCost;
        if (remaining == 0U) {
            call.return_value(1U);
            return call.finish(retired + kDoneCost + kEpilogueCost);
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kRecordCost;
        const ChainOutcome outcome = follow_quad(call, strip, index, target_slot, retired);
        if (outcome == ChainOutcome::Declined) {
            return call.decline();
        }
        if (outcome == ChainOutcome::Overrun) {
            return call.finish(retired);
        }
        if (outcome == ChainOutcome::Missed) {
            call.return_value(0U);
            return call.finish(retired + kEpilogueCost);
        }
        retired += kAdvanceCost;
        index = (index + 1U) & 0xffffU;
        const GuestAddress record = m.read_u32_le(strip + kRecordPointer);
        const std::uint32_t quad_count = m.read_u16_le(record + kRecordQuadCount);
        if (index >= quad_count) {
            retired += kEndTestCost;
            if (m.read_u8(record + kRecordClosed) == 0U) {
                retired += kOpenEndCost;
                call.return_value(0U);
                return call.finish(retired + kEpilogueCost);
            }
            retired += kWrapCost;
            index = (index - quad_count) & 0xffffU;
        }
        retired += kCountStepCost;
        remaining = (remaining - 1U) & 0xffffU;
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014a290, 484U, 0xb14a0dbcc3468559ULL, "FollowRouteQuads", kCeiling, follow_route_quads,
         EngineReturn::Integer)
