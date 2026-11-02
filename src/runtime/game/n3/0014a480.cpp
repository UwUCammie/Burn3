// 0x0014a480 QuadEntryForDriver (a0 = R, a driver's quad-entry rules record: the AI driver record + 6640; a1 = strip
// handle H; a2 = quad index, low 16 bits used): returns in v0 the entry bits of that quad: 4 closed, 1 discouraged,
// 0 open. A leaf with no stores and no calls.
//
// The strip is H[4]; its record list is at strip + 8, and each record is 10 bytes. A quad's kind is byte 3 of its
// record, low three bits. Kind 5 is closed to everyone. Otherwise the rules apply when race[6420] is nonzero (race =
// R[4]); without them only kind 4 is discouraged. With them:
//   - kind 4 while R[497] is clear: discouraged when R[657] is set, else closed;
//   - kind 1 or 3 while R[496] is clear and R[508] is set: discouraged when R[658] is set, else closed.
// The two cases need different kinds, so at most one of them sets a bit. Every test below is one listing block (its
// load or compare, its branch and its delay slot), so the retired count of a path is the sum of the blocks it runs.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Strip and record layout.
constexpr GuestAddress kStripOffset = 4U;           // H + 4: the strip
constexpr GuestAddress kRecordListOffset = 8U;      // strip + 8: the first record
constexpr std::uint32_t kRecordBytes = 10U;
constexpr GuestAddress kKindOffset = 3U;            // record + 3: the kind byte
constexpr std::uint32_t kKindMask = 7U;
constexpr std::uint32_t kQuadMask = 0xffffU;

// Rules record layout (R).
constexpr GuestAddress kRaceOffset = 4U;                      // R + 4: the race
constexpr GuestAddress kRaceRulesOffset = 6420U;              // race + 6420: nonzero applies the rules
constexpr GuestAddress kSideKindsAllowedOffset = 496U;        // u8
constexpr GuestAddress kRestrictedAllowedOffset = 497U;       // u8
constexpr GuestAddress kSideKindsActiveOffset = 508U;         // u32
constexpr GuestAddress kRestrictedDiscouragedOffset = 657U;   // u8
constexpr GuestAddress kSideKindsDiscouragedOffset = 658U;    // u8

// Quad kinds and entry bits.
constexpr std::uint32_t kKindClosed = 5U;
constexpr std::uint32_t kKindRestricted = 4U;   // closed or discouraged by the rules
constexpr std::uint32_t kKindSideA = 1U;        // kinds 1 and 3 share a rule
constexpr std::uint32_t kKindSideB = 3U;
constexpr std::uint32_t kEntryOpen = 0U;
constexpr std::uint32_t kEntryDiscouraged = 1U;
constexpr std::uint32_t kEntryClosed = 4U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kRecordCost = 12U;       // 0x0014a480-0x0014a4ac: strip, record, kind and the kind 5 branch
constexpr std::uint32_t kClosedEntryCost = 1U;   // 0x0014a554: v0 = 4 (the return is kReturnCost)
constexpr std::uint32_t kRulesCost = 4U;         // 0x0014a4b0-0x0014a4bc: race, its rules flag, the branch to the rules
constexpr std::uint32_t kTestCost = 3U;          // one test: its load or compare, its branch and the delay slot
constexpr std::uint32_t kSetCost = 1U;           // a bit set on the fall-through (0x0014a500, 0x0014a548)
constexpr std::uint32_t kSetJumpCost = 2U;       // a bit set, then a jump: beq zero with the ori in its delay slot
constexpr std::uint32_t kJoinCost = 2U;          // 0x0014a54c-0x0014a550: the jump to the return
constexpr std::uint32_t kReturnCost = 2U;        // jr $ra and its delay slot

struct EntryResult {
    std::uint32_t bits{};
    std::uint32_t retired{};
};

// The entry bits of quad `quad` of the strip behind handle `handle`, under the rules at `rules`, and the instructions the
// original retires for that quad. Each read is made only on the paths that make it, as the original's loads are.
template <class Memory>
EntryResult driver_quad_entry(const Memory& m, const GuestAddress rules, const GuestAddress handle,
                              const std::uint32_t quad) {
    const GuestAddress strip = m.read_u32_le(handle + kStripOffset);
    const GuestAddress record = m.read_u32_le(strip + kRecordListOffset) + quad * kRecordBytes;
    const std::uint32_t kind = m.read_u8(record + kKindOffset) & kKindMask;
    if (kind == kKindClosed) {
        return {kEntryClosed, kRecordCost + kClosedEntryCost + kReturnCost};
    }

    const GuestAddress race = m.read_u32_le(rules + kRaceOffset);
    if (m.read_u32_le(race + kRaceRulesOffset) == 0U) {
        // Without the rules only kind 4 is discouraged.
        if (kind == kKindRestricted) {
            return {kEntryDiscouraged, kRecordCost + kRulesCost + kTestCost + kSetJumpCost + kJoinCost + kReturnCost};
        }
        return {kEntryOpen, kRecordCost + kRulesCost + kTestCost + kReturnCost};
    }

    std::uint32_t bits = kEntryOpen;
    std::uint32_t retired = kRecordCost + kRulesCost;

    // Kind 4 while R[497] is clear. The kind 4 test and the R[657] test are skipped when their conditions fail.
    retired += kTestCost;
    if (m.read_u8(rules + kRestrictedAllowedOffset) == 0U) {
        retired += kTestCost;
        if (kind == kKindRestricted) {
            retired += kTestCost;
            if (m.read_u8(rules + kRestrictedDiscouragedOffset) != 0U) {
                bits |= kEntryDiscouraged;
                retired += kSetJumpCost;
            } else {
                bits |= kEntryClosed;
                retired += kSetCost;
            }
        }
    }

    // Kinds 1 and 3 while R[496] is clear and R[508] is set. Kind 1 skips the kind 3 test.
    retired += kTestCost;
    if (m.read_u8(rules + kSideKindsAllowedOffset) != 0U) {
        return {bits, retired + kReturnCost};
    }
    retired += kTestCost;
    if (kind != kKindSideA) {
        retired += kTestCost;
        if (kind != kKindSideB) {
            return {bits, retired + kReturnCost};
        }
    }
    retired += kTestCost;
    if (m.read_u32_le(rules + kSideKindsActiveOffset) == 0U) {
        return {bits, retired + kReturnCost};
    }
    retired += kTestCost;
    if (m.read_u8(rules + kSideKindsDiscouragedOffset) != 0U) {
        bits |= kEntryDiscouraged;
        retired += kSetJumpCost;
    } else {
        bits |= kEntryClosed;
        retired += kSetCost;
    }
    return {bits, retired + kJoinCost + kReturnCost};
}

template <class Context>
n3::Run quad_entry_for_driver(Context& context) {
    auto call = n3::guest_call(context);
    const EntryResult entry = driver_quad_entry(call.memory(), call.arg(0), call.arg(1), call.arg(2) & kQuadMask);
    call.return_value(entry.bits);
    return call.finish(entry.retired);
}

} // namespace
} // namespace takedown::runtime

// 43 is the most any input retires. The listing's longest path (48) needs kind 4 and kind 3 at once, so no input takes it.
N3_ENTRY_PLAIN(0014a480, 224U, 0x3d823e68a7cbc973ULL, "QuadEntryForDriver", 43U, quad_entry_for_driver,
               EngineReturn::Integer)
