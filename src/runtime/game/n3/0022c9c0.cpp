// 0x0022c9c0 (a0 = object, a1 = slot index, of which only the low byte matters, read signed): returns 1 when one of
// the two record pointers that FillSlotPointerPair (0x0022c610) gives for that slot is non-null and has a non-zero
// halfword at +176, else 0. The callee writes the pointers into this function's frame array out[] = $sp + 24.
//
//   0022c9c0  addiu $sp, $sp, -32       frame of 32 bytes
//   0022c9c4  daddu $a2, $a1, $zero     the slot index, copied before a1 is reused
//   0022c9c8  sd $ra, 0($sp)
//   0022c9cc  jal 0x0022c610            a0 = object, a1 = out (delay slot)
//   0022c9d4  beq ... 0x0022ca04        loop over k = 0, 1 (the int8 counter goes 0, 1, 2 and stops at 2):
//                                       out[k] == 0 or halfword(out[k] + 176) == 0 -> next k, else return 1
//   0022ca18                            both passes missed: return 0
//   0022ca30  ld $ra; jr $ra; addiu $sp, $sp, 32
//
// Own retired instructions per path (the callee's count is added to each): 25 for a hit on k = 0; 39 or 42 for a
// hit on k = 1; 45, 48 or 51 for no hit. The callee retires 45, 46 or 72 to 82.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kOutOffset = 24U;              // out[] = $sp + 24 (two words)
constexpr GuestAddress kRecordHalfwordOffset = 176U;  // halfword of a record that is active when non-zero
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the blocks of the listing, delay slots included.
constexpr std::uint32_t kCallerPrefix = 5U;   // addiu $sp; daddu $a2; sd $ra; jal; addiu $a1 (delay slot)
constexpr std::uint32_t kCallerJoin = 2U;     // beq $zero, $zero (to the loop test); daddu $a0 (delay slot)
constexpr std::uint32_t kLoopTest = 5U;       // dsll32; dsra32; slti; bne; nop (delay slot)
constexpr std::uint32_t kPointerTest = 5U;    // sll; addu; lw; beq; nop (delay slot)
constexpr std::uint32_t kHalfwordTest = 3U;   // lh; beq; nop (delay slot)
constexpr std::uint32_t kNextPass = 4U;       // addiu; dsll32; beq; dsra32 (delay slot)
constexpr std::uint32_t kHitExit = 2U;        // beq $zero, $zero; addiu $v0, $zero, 1 (delay slot)
constexpr std::uint32_t kMissExit = 2U;       // beq $zero, $zero; daddu $v0, $zero, $zero (delay slot)
constexpr std::uint32_t kReturn = 3U;         // ld $ra; jr $ra; addiu $sp, $sp, 32 (delay slot)

struct RecordPass {
    std::uint32_t retired;  // the pass's own instructions, loop test included
    bool hit;               // the original returns 1 in this pass
};

// One pass of the original loop over out[k]: a null pointer or a zero halfword moves on to the next pass.
template <class Memory>
RecordPass test_record_pass(Memory& memory, const GuestAddress entry) {
    const GuestAddress record = memory.read_u32_le(entry);
    if (record == 0U) {
        return {kLoopTest + kPointerTest + kNextPass, false};
    }
    if (memory.read_u16_le(record + kRecordHalfwordOffset) == 0U) {
        return {kLoopTest + kPointerTest + kHalfwordTest + kNextPass, false};
    }
    return {kLoopTest + kPointerTest + kHalfwordTest + kHitExit, true};
}

template <class Context>
n3::Run slot_pair_has_active_record(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg64(2, call.arg64(1));
    call.set_arg(1, out);
    const n3::Run filled = call.invoke([](auto& m) { return n3::fn_0022c610(m); }, kFrameBytes);
    if (filled.declined) {
        return call.decline();
    }
    auto& memory = call.memory();
    const std::uint32_t before = kCallerPrefix + filled.retired + kCallerJoin;
    const RecordPass first = test_record_pass(memory, out);
    if (first.hit) {
        call.return_value(1U);
        return call.finish(before + first.retired + kReturn);
    }
    const RecordPass second = test_record_pass(memory, out + 4U);
    if (second.hit) {
        call.return_value(1U);
        return call.finish(before + first.retired + second.retired + kReturn);
    }
    call.return_value(0U);
    return call.finish(before + first.retired + second.retired + kLoopTest + kMissExit + kReturn);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022c9c0, 124U, 0x71f909d30b7f6204ULL, "SlotPairHasActiveRecord", kCeiling, slot_pair_has_active_record,
         EngineReturn::Integer)
