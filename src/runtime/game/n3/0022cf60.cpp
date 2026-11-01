// 0x0022cf60 (a0 = object, a1 = slot index, of which only the low byte matters, read signed): FillSlotPointerPair
// (0x0022c610) stores two record pointers in out[], which is this frame's +24 (the entry $sp - 8). The result is 1
// when out[0] or out[1] is a record pointer whose halfword at +92 is non-zero, and 0 otherwise.
//
//   0022cf60  addiu $sp, $sp, -32       frame of 32 bytes
//   0022cf64  daddu $a2, $a1, $zero     slot index, whole register
//   0022cf68  sd $ra, 0($sp)            return address in the frame (below the entry $sp, not reproduced)
//   0022cf6c  jal 0x0022c610            FillSlotPointerPair(a0, out, a1)
//   0022cf70  addiu $a1, $sp, 24        (delay slot) out = sp + 24
//   0022cf74  beq $zero, $zero, 0x0022cfa4
//   0022cf78  daddu $a0, $zero, $zero   (delay slot) loop index = 0
//   0022cfa4 .. 0022cfb4                index < 2 (signed, from the low byte) continues, else result 0
//   0022cf7c .. 0022cf8c                v0 = out[index]; a null pointer goes to the step
//   0022cf90 .. 0022cf98                v0 = halfword at record + 92; zero goes to the step
//   0022cf9c .. 0022cfa0                result 1
//   0022cfc0 .. 0022cfcc                index = index + 1 (low byte), back to the test
//   0022cfb8 .. 0022cfbc                result 0
//   0022cfd0 .. 0022cfd8                ld $ra, jr $ra, addiu $sp, $sp, 32
//
// The index starts at 0 and the test stops it at 2, so the loop is always two passes; the retired counts below are
// the listing's blocks along each path.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;             // addiu $sp, $sp, -32
constexpr GuestAddress kOutOffset = 24U;               // out[i] is at the frame's +24 + 4 * i
constexpr std::uint32_t kPassCount = 2U;               // the index runs 0, 1
constexpr GuestAddress kRecordHalfwordOffset = 92U;    // lh $v0, 92($v0)
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included. The callee's count is added where it runs.
constexpr std::uint32_t kEntryCost = 7U;         // 0022cf60-0022cf78: up to the jal and its delay slot, then the beq and its delay slot
constexpr std::uint32_t kIndexTestCost = 5U;     // 0022cfa4-0022cfb4: dsll32, dsra32, slti, bne and the bne's delay slot
constexpr std::uint32_t kPointerTestCost = 5U;   // 0022cf7c-0022cf8c: sll, addu, lw, beq and the beq's delay slot
constexpr std::uint32_t kHalfwordTestCost = 3U;  // 0022cf90-0022cf98: lh, beq and the beq's delay slot
constexpr std::uint32_t kStepCost = 4U;          // 0022cfc0-0022cfcc: addiu, dsll32, beq and the beq's delay slot
constexpr std::uint32_t kFoundCost = 2U;         // 0022cf9c-0022cfa0: beq and its delay slot (result 1)
constexpr std::uint32_t kMissCost = 2U;          // 0022cfb8-0022cfbc: beq and its delay slot (result 0)
constexpr std::uint32_t kReturnCost = 3U;        // 0022cfd0-0022cfd8: ld, jr and the jr's delay slot

template <class Context>
n3::Run slot_pair_has_nonzero_record_field(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg64(2, call.arg64(1));   // daddu $a2, $a1, $zero (before the delay slot changes a1)
    call.set_arg(1, out);               // addiu $a1, $sp, 24 (the jal's delay slot)
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0022c610(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    auto& m = call.memory();
    std::uint32_t retired = kEntryCost + callee.retired;
    for (std::uint32_t index = 0U; index < kPassCount; ++index) {
        retired += kIndexTestCost + kPointerTestCost;
        const GuestAddress record = m.read_u32_le(out + 4U * index);
        if (record != 0U) {
            retired += kHalfwordTestCost;
            if (m.read_u16_le(record + kRecordHalfwordOffset) != 0U) {
                call.return_value(1U);
                return call.finish(retired + kFoundCost + kReturnCost);
            }
        }
        retired += kStepCost;
    }
    call.return_value(0U);
    return call.finish(retired + kIndexTestCost + kMissCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022cf60, 124U, 0xc8d61b81fff547c0ULL, "SlotPairHasNonzeroRecordField", kCeiling,
         slot_pair_has_nonzero_record_field, EngineReturn::Integer)
