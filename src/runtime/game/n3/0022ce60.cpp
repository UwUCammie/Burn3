// 0x0022ce60 (a0 = object, a1 = slot index, of which only the low byte matters, read signed): calls
// FillSlotPointerPair (0x0022c610) with the slot index in a2 and a two-word pair at sp + 24 as its out[]. Then, for
// word 0 and then word 1, a non-null record pointer whose halfword at +56 is nonzero makes the result 1 at once.
// Otherwise the result is 0. The original's counter goes 0, 1 and stops at 2, so the pair loop is two fixed passes.
//
//   0022ce60  addiu $sp, $sp, -32      frame (the sd $ra at sp + 0 is frame traffic, not reproduced)
//   0022ce64  daddu $a2, $a1, $zero    the original a1, copied before a1 changes
//   0022ce6c  jal 0x0022c610           a1 = sp + 24 (delay slot), a0 unchanged
//   0022cea4  counter in v1 = sext8(a0), a0 = 0 after the call: taken while v1 < 2
//   0022ce7c  v0 = word at sp + 24 + 4 * counter; zero: step; else lh +56 nonzero: return 1, otherwise step
//   0022ced0  return (ld $ra, jr, addiu $sp)

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;      // addiu $sp, $sp, -32
constexpr GuestAddress kPairOffset = 24U;       // the callee's out[] is sp + 24 in that frame
constexpr GuestAddress kMarkOffset = 56U;       // halfword of a record pointer that marks it
constexpr std::uint32_t kPairWords = 2U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;        // addiu, daddu, sd, jal and its delay slot, beq and its delay slot
constexpr std::uint32_t kTestCost = 5U;         // dsll32, dsra32, slti, bne and its delay slot
constexpr std::uint32_t kLoadCost = 5U;         // sll, addu, lw, beq and its delay slot
constexpr std::uint32_t kMarkCost = 3U;         // lh, beq and its delay slot
constexpr std::uint32_t kStepCost = 4U;         // addiu, dsll32, beq and its delay slot: the counter steps
constexpr std::uint32_t kHitCost = 2U;          // beq and its delay slot: v0 = 1
constexpr std::uint32_t kMissCost = 2U;         // beq and its delay slot: v0 = 0 once the counter reaches 2
constexpr std::uint32_t kReturnCost = 3U;       // ld, jr and the addiu in its delay slot

template <class Context>
n3::Run slot_pair_has_marked_record(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t slot_word = call.arg64(1);
    call.set_arg64(2, slot_word);
    const GuestAddress pair = call.stack_pointer() - kFrameBytes + kPairOffset;
    call.set_arg(1, pair);
    const n3::Run filled = call.invoke([](auto& callee_machine) { return n3::fn_0022c610(callee_machine); }, kFrameBytes);
    if (filled.declined) {
        return call.decline();
    }
    auto& m = call.memory();
    std::uint32_t retired = kEntryCost + filled.retired;
    for (std::uint32_t word = 0U; word < kPairWords; ++word) {
        retired += kTestCost + kLoadCost;
        const GuestAddress record = m.read_u32_le(pair + 4U * word);
        if (record != 0U) {
            retired += kMarkCost;
            const auto mark = static_cast<std::int16_t>(m.read_u16_le(record + kMarkOffset));
            if (mark != 0) {
                call.return_value(1U);
                return call.finish(retired + kHitCost + kReturnCost);
            }
        }
        retired += kStepCost;
    }
    call.return_value(0U);
    return call.finish(retired + kTestCost + kMissCost + kReturnCost);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(0022ce60, 124U, 0x052ecd71d00c44bcULL, "SlotPairHasMarkedRecord", kCeiling, slot_pair_has_marked_record,
         EngineReturn::Integer)
