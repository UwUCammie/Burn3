// 0x0022cee0 (a0 = object, a1 = slot index): fills a two-entry pointer pair by calling FillSlotPointerPair
// (0x0022c610) with a 32-byte frame, then returns 1 when out[0] or out[1] (tested in that order) is a record
// pointer whose halfword at +44 is nonzero, else 0. A null pointer or a zero halfword moves on to the next slot.
//
//   0022cee0  addiu $sp, $sp, -32          frame of 32 bytes
//   0022cee4  daddu $a2, $a1, $zero        the index, whole register, for the callee
//   0022cee8  sd $ra, 0($sp)
//   0022ceec  jal 0x0022c610               out = frame + 24 (delay slot 0022cef0)
//   0022cef4  beq $zero, $zero, 0x0022cf24 slot counter = 0 (delay slot 0022cef8)
//   0022cf24..0022cf34                     slot < 2 ? test that slot : leave the scan
//   0022cefc..0022cf0c                     word = out[slot]; null moves on
//   0022cf10..0022cf18                     halfword at word + 44; zero moves on, else return 1 (0022cf1c)
//   0022cf40..0022cf4c                     slot += 1, back to the test
//   0022cf38..0022cf3c                     not found: v0 = 0 (delay slot), to the epilogue
//   0022cf1c..0022cf20                     found: v0 = 1 (delay slot), to the epilogue
//   0022cf50..0022cf58                     epilogue: restore $ra and $sp, return
//
// The original saves $ra in its frame and reloads it at the end; the native keeps the entry $ra instead (frame words
// are not reproduced). A callee store reaches the saved word only through a slot byte of -6 (out - 24 is frame + 0),
// and that would redirect the original's return; this native does not reproduce that.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kPairOffset = 24U;        // out[0] at frame + 24, out[1] at frame + 28
constexpr std::uint32_t kPairCount = 2U;
constexpr std::uint32_t kEntryBytes = 4U;
constexpr GuestAddress kRecordHalfword = 44U;    // the record halfword that decides the slot

// Retired instruction counts of the listing, delay slots included. The callee's own count is added at the call.
constexpr std::uint32_t kCallSite = 7U;          // 0022cee0-0022cef8: prologue, jal and delay slot, beq and delay slot
constexpr std::uint32_t kSlotTest = 5U;          // 0022cf24-0022cf34: slot < 2, or the test that ends the scan
constexpr std::uint32_t kSlotLoad = 5U;          // 0022cefc-0022cf0c: out[slot] and its null test
constexpr std::uint32_t kRecordTest = 3U;        // 0022cf10-0022cf18: the halfword and its zero test
constexpr std::uint32_t kNextSlot = 4U;          // 0022cf40-0022cf4c: slot += 1, back to the test
constexpr std::uint32_t kSetResult = 2U;         // the branch to the epilogue and its delay slot (sets v0)
constexpr std::uint32_t kEpilogue = 3U;          // 0022cf50-0022cf58: ld $ra, jr $ra and the $sp restore
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run slot_pair_has_active_record(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kPairOffset;
    call.set_arg64(2, call.arg64(1));
    call.set_arg(1, out);
    const n3::Run filled = call.invoke([](auto& machine) { return n3::fn_0022c610(machine); }, kFrameBytes);
    if (filled.declined) {
        return call.decline();
    }
    auto& m = call.memory();
    std::uint32_t retired = kCallSite + filled.retired;
    // The original's slot counter starts at 0 and the test stops it at 2, so the scan tests at most two slots.
    for (std::uint32_t slot = 0U; slot < kPairCount; ++slot) {
        retired += kSlotTest + kSlotLoad;
        const GuestAddress record = m.read_u32_le(out + kEntryBytes * slot);
        if (record == 0U) {
            retired += kNextSlot;
            continue;
        }
        retired += kRecordTest;
        if (m.read_u16_le(record + kRecordHalfword) == 0U) {
            retired += kNextSlot;
            continue;
        }
        call.return_value(1U);
        return call.finish(retired + kSetResult + kEpilogue);
    }
    call.return_value(0U);
    return call.finish(retired + kSlotTest + kSetResult + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022cee0, 124U, 0xbba336f669fe13b0ULL, "SlotPairHasActiveRecord", kCeiling, slot_pair_has_active_record,
         EngineReturn::Integer)
