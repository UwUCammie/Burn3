// 0x0022cde0 (a0 = object, a1 = slot index): SlotPairHasFlaggedRecord. Fills a two-entry table of record pointers at
// the bottom of its 32-byte frame with FillSlotPointerPair (0x0022c610), then returns 1 when out[0], or else out[1],
// is a non-null record whose halfword at +20 is nonzero, and 0 otherwise. A null entry is skipped.
//
//   0022cde0  addiu $sp, $sp, -32       frame: ra at +0, out[0] at +24, out[1] at +28
//   0022cde4  daddu $a2, $a1, $zero     a2 = the slot index, whole register
//   0022cde8  sd $ra, 0($sp)            (frame store, not reproduced: see below)
//   0022cdec  jal 0x0022c610            FillSlotPointerPair(object, out = sp + 24, index)
//   0022cdf0  addiu $a1, $sp, 24        (delay slot)
//   0022cdf4  beq $zero, $zero, 0x0022ce24   loop head with k = 0 (delay slot: a0 = 0)
//   0022ce24..0022ce58                  k = 0, 1: out[k] is tested, then the halfword at out[k] + 20; then return
//
// The original reloads ra from the frame after the call. Only a slot byte of -5 or -6 lets the callee's out[] store
// reach that word, so the native declines those states (decided before the callee runs) and the original runs them.
// Otherwise the native never changes ra or sp and returns through ra as the caller left it.
// Retired counts, plus the callee's: 25 with out[0] flagged, 39 or 42 with out[1] flagged, otherwise 45 plus 3 for
// each non-null entry.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kOutOffset = 24U;          // out[k] at the frame's +24 + 4k
constexpr GuestAddress kSlotOffset = 4001U;       // int8 slot byte of entry k: object + 4001 + k
constexpr GuestAddress kRecordFlagOffset = 20U;   // halfword of a record that decides the result
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the original's own instructions on each step, delay slots included.
constexpr std::uint32_t kPrologueCost = 5U;       // 0022cde0-0022cdf0: frame, a2 copy, ra store, jal, its delay slot
constexpr std::uint32_t kLoopEntryCost = 2U;      // 0022cdf4-0022cdf8: branch to the loop head and its delay slot
constexpr std::uint32_t kHeadCost = 5U;           // 0022ce24-0022ce34: k < 2 test, branch, delay slot
constexpr std::uint32_t kLoadCost = 5U;           // 0022cdfc-0022ce0c: out[k] load, out[k] == 0 branch, delay slot
constexpr std::uint32_t kHalfwordCost = 3U;       // 0022ce10-0022ce18: halfword load, zero branch, delay slot
constexpr std::uint32_t kHitCost = 2U;            // 0022ce1c-0022ce20: v0 = 1 and the branch to the return
constexpr std::uint32_t kStepCost = 4U;           // 0022ce40-0022ce4c: k + 1 and the branch back to the head
constexpr std::uint32_t kExitCost = 2U;           // 0022ce38-0022ce3c: v0 = 0 and the branch to the return
constexpr std::uint32_t kReturnCost = 3U;         // 0022ce50-0022ce58: ra reload, jr, frame pop (delay slot)

// The callee's out[] store takes its slot from object + index + 4001 when that entry is flagged, and from
// object + 4001 + k for both entries otherwise. Slots -6 and -5 aim the store at the saved ra (frame + 0 and + 4).
template <class Memory>
bool slot_hits_saved_ra(const Memory& memory, const GuestAddress address) {
    const std::int32_t slot = static_cast<std::int8_t>(memory.read_u8(address));
    return slot == -5 || slot == -6;
}

template <class Memory>
bool may_store_saved_ra(const Memory& memory, const GuestAddress object, const std::int32_t index) {
    if (index < 0) {
        return false;  // the negative index stores out[0] and out[1] only
    }
    return slot_hits_saved_ra(memory, object + static_cast<GuestAddress>(index) + kSlotOffset) ||
           slot_hits_saved_ra(memory, object + kSlotOffset) || slot_hits_saved_ra(memory, object + kSlotOffset + 1U);
}

template <class Context>
n3::Run slot_pair_has_flagged_record(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    const std::int32_t index = static_cast<std::int8_t>(call.arg(1) & 0xFFU);
    if (may_store_saved_ra(call.memory(), object, index)) {
        return call.decline();
    }
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg64(2, call.arg64(1));  // daddu $a2, $a1, $zero
    call.set_arg(1, out);              // addiu $a1, $sp, 24
    const n3::Run filled = call.invoke([](auto& callee) { return n3::fn_0022c610(callee); }, kFrameBytes);
    if (filled.declined) {
        return call.decline();
    }
    // The original's loop runs at most two passes (k = 0, 1), so it is written out as a bounded loop.
    std::uint32_t retired = kPrologueCost + kLoopEntryCost + filled.retired;
    for (std::uint32_t k = 0U; k < 2U; ++k) {
        retired += kHeadCost + kLoadCost;
        const GuestAddress record = call.memory().read_u32_le(out + 4U * k);
        if (record != 0U) {
            retired += kHalfwordCost;
            if (call.memory().read_u16_le(record + kRecordFlagOffset) != 0U) {
                call.return_value(1U);
                return call.finish(retired + kHitCost + kReturnCost);
            }
        }
        retired += kStepCost;
    }
    call.return_value(0U);
    return call.finish(retired + kHeadCost + kExitCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022cde0, 124U, 0x748e7870dfb372c8ULL, "SlotPairHasFlaggedRecord", kCeiling, slot_pair_has_flagged_record,
         EngineReturn::Integer)
