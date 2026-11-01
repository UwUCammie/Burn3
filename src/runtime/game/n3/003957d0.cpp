// 0x003957d0 (a0 = record R): writes each slot index into the first word of its slot, then clears the byte at R + 5.
// Slot k is the 16-byte entry at R + 40 + 16 * k and holds k as one word (sw); the byte store is `sb $zero, 5($a0)`.
//
//   003957d0  beq $zero, $zero, 0x003957e8   delay slot daddu: a1 = 0
//   003957d8..003957e4                       sll, addu, sw a1 at v1 + 40, addiu a1 += 1   (loop body)
//   003957e8..003957f0                       slti v1, a1, 2; bne back to the body; nop    (loop test)
//   003957f4  jr $ra; delay slot sb $zero, 5($a0)
//
// The loop bound is fixed: a1 runs 0, 1 and stops at 2 for every input, so each call retires the same 21
// instructions: entry 2, three loop tests of 3, two bodies of 4, return 2. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kSlotCount = 2U;
constexpr GuestAddress kSlotTable = 40U;       // slot k at record + 40 + 16 * k
constexpr GuestAddress kSlotStride = 16U;
constexpr GuestAddress kFlagByte = 5U;         // record + 5 is cleared
constexpr std::uint8_t kClearedFlag = 0U;
constexpr std::uint32_t kPathCost = 21U;       // the only path, see the listing above

template <class Context>
n3::Run init_slot_indices(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    for (std::uint32_t slot = 0U; slot < kSlotCount; ++slot) {
        m.write_u32_le(record + kSlotTable + kSlotStride * slot, slot);
    }
    m.write_u8(record + kFlagByte, kClearedFlag);
    return call.finish(kPathCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003957d0, 44U, 0xd729ed5c0efc078aULL, "InitSlotIndices", kPathCost, init_slot_indices,
               EngineReturn::Nothing)
