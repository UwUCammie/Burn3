// 0x0021cff0 (a0 = record R, a1 = signed slot index): IsSlotEntryFlagged. A leaf that writes no memory.
// Answers 1 when the enable byte at R+68 is clear. Otherwise a1 = -1 answers 0. Otherwise the id at
// R+380+4*a1 is sought among the 17 ids at R+196+4*i (i = 0..16). The first match answers whether the
// u16 at R+88+2*i has either of its two low bits set, and no match answers 1.
// The search is bounded by the original's `slti a2,17`, so it is written as straight code: every path's
// retired count is exact, and the declared cost is the longest path (no match, or a match on entry 16).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kEnableByte = 68U;     // byte: zero answers 1
constexpr GuestAddress kFlagTable = 88U;      // u16 flags, stride 2
constexpr GuestAddress kIdTable = 196U;       // u32 ids, stride 4
constexpr GuestAddress kSlotIds = 380U;       // u32 ids indexed by the slot, stride 4
constexpr std::uint32_t kEntries = 17U;
constexpr std::uint64_t kAllOnes = 0xffffffffffffffffULL;

// Retired instruction counts from the listing, delay slots included.
// Enable byte clear: lbu, beq and its nop, beq and its addiu, jr and its nop.
constexpr std::uint32_t kDisabledCost = 7U;
// Slot -1: lbu, beq and its nop, addiu, beq and its nop, beq and its daddu, jr and its nop.
constexpr std::uint32_t kNoSlotCost = 10U;
// Prologue through the slot address: 9. Each entry tested without a match: 11 (slti, beq and its nop,
// sll, lw, addu, lw, bne and its nop, beq and its addiu). A match on entry i: 26 + 11 * i, that is the
// prologue, i passed entries, the matching entry's 9 instructions, the flag block (6) and the exit (2).
constexpr std::uint32_t kMatchBaseCost = 26U;
constexpr std::uint32_t kEntryCost = 11U;
// No match: the prologue, 17 entries, then slti, beq and its nop, addiu, jr and its nop.
constexpr std::uint32_t kFallOutCost = 202U;

template <class Context>
n3::Run is_slot_entry_flagged(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    if (m.read_u8(record + kEnableByte) == 0U) {
        call.return_value(1U);
        return call.finish(kDisabledCost);
    }
    // addiu sign-extends -1 to all ones, and the original's beq compares the whole 64-bit register.
    if (call.arg64(1) == kAllOnes) {
        call.return_value(0U);
        return call.finish(kNoSlotCost);
    }
    // sll and addu are 32-bit, so the slot address wraps in 32 bits. Nothing in this function writes
    // memory, so the id the original reloads on every entry is the same word and is read once.
    const std::uint32_t id = m.read_u32_le(record + (call.arg(1) << 2U) + kSlotIds);
    for (std::uint32_t i = 0U; i < kEntries; ++i) {
        if (m.read_u32_le(record + kIdTable + 4U * i) == id) {
            const std::uint32_t flags = m.read_u16_le(record + kFlagTable + 2U * i);
            call.return_value((flags & 3U) != 0U ? 1U : 0U);
            return call.finish(kMatchBaseCost + kEntryCost * i);
        }
    }
    call.return_value(1U);
    return call.finish(kFallOutCost);
}

} // namespace

extern constinit const EngineFunction n3_0021cff0{0x0021cff0U, 132U, 0xe4a627112e48ced4ULL, "IsSlotEntryFlagged",
                                                   202U, n3::live<&is_slot_entry_flagged<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021cff0, is_slot_entry_flagged)
