// 0x0022cfe0 (a0 = object, a1 = slot index, of which only the low byte matters, read signed): FillSlotPairAndSelect.
// Calls FillSlotPointerPair (0x0022c610) with the object, a two-word out[] in this frame (out = caller $sp + 40, the
// frame being 48 bytes) and the slot index. Then for index 0 and 1 in order: a non-null out[index] whose halfword at
// +68 is nonzero stores the index as a byte at object + 4000 and returns 1. Otherwise returns 0.
//
//   0022cfe0  addiu $sp, $sp, -48      frame of 48 bytes, out = $sp + 40
//   0022cfe4  daddu $a2, $a1, $zero    slot index (64-bit copy, taken before a1 is replaced)
//   0022cfec  addiu $a1, $sp, 40       out
//   0022cff4  jal 0x0022c610           FillSlotPointerPair (a0 is not changed)
//   0022cffc  beq $zero, $zero, 0x0022d030, with a0 = 0 in its delay slot
//   0022d030..0022d068                 scan of index 0 and 1 and the return
//
// The scan runs at most twice (the index goes 0, 1 and then stops at 2 whatever the data), so it is written as two
// passes. Retired counts: 29 (index 0 hit), 43 or 46 (index 1 hit), 48, 51 or 54 (no hit), each plus the callee's.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
constexpr GuestAddress kOutOffset = 40U;           // out[] = the caller's $sp + 40 after the frame is lowered
constexpr GuestAddress kSelectionOffset = 4000U;   // byte that receives the index of the hit
constexpr GuestAddress kHitHalfwordOffset = 68U;   // halfword of a record that makes a hit
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the blocks on the paths (delay slots included).
constexpr std::uint32_t kCallBlockCost = 9U;       // up to the jal, its delay slot, the beq and its delay slot
constexpr std::uint32_t kIndexTestCost = 5U;       // index < 2 (taken)
constexpr std::uint32_t kRecordLoadCost = 5U;      // out[index] and its null test
constexpr std::uint32_t kHitTestCost = 3U;         // halfword test of a non-null record
constexpr std::uint32_t kHitCost = 3U;             // store the index and return value 1
constexpr std::uint32_t kNextIndexCost = 4U;       // index + 1 and back to the test
constexpr std::uint32_t kExitCost = 2U;            // index == 2: return value 0
constexpr std::uint32_t kReturnCost = 4U;          // restore $ra and $s0, return

template <class Context>
n3::Run fill_slot_pair_and_select(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint64_t slot_argument = call.arg64(1);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;

    call.set_arg64(2, slot_argument);
    call.set_arg(1, out);
    const n3::Run filled = call.invoke([](auto& m) { return n3::fn_0022c610(m); }, kFrameBytes);
    if (filled.declined) {
        return call.decline();
    }

    std::uint32_t retired = kCallBlockCost + filled.retired;
    for (std::uint32_t index = 0U; index < 2U; ++index) {
        retired += kIndexTestCost + kRecordLoadCost;
        const GuestAddress record = memory.read_u32_le(out + 4U * index);
        if (record != 0U) {
            retired += kHitTestCost;
            if (memory.read_u16_le(record + kHitHalfwordOffset) != 0U) {
                memory.write_u8(object + kSelectionOffset, static_cast<std::uint8_t>(index));
                call.return_value(1U);
                return call.finish(retired + kHitCost + kReturnCost);
            }
        }
        retired += kNextIndexCost;
    }
    retired += kIndexTestCost + kExitCost + kReturnCost;
    call.return_value(0U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022cfe0, 140U, 0x5fc380cf56a131a8ULL, "FillSlotPairAndSelect", kCeiling, fill_slot_pair_and_select,
         EngineReturn::Integer)
