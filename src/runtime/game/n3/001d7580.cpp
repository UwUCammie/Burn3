// 0x001d7580 (TransferSlotNodeAndStore): a1 picks a 16-byte slot of the table at record+44 (slot offset = a1 << 4).
// The slot's block at +4 goes to TransferListHeadToFront (0x002b6da0), which moves a node onto the front of the list
// headed at the slot's +8. The word a2 is stored at +64 of that node, and the function returns node + 8. When the
// callee finds its source list empty it returns 0, so the store goes to address 64 and the result is 8, as in the
// original.
//
//   001d7580  addiu $sp, $sp, -32       frame of 32 bytes
//   001d7584  sll $v1, $a1, 4           v1 = slot << 4
//   001d7588  sd $ra, 16($sp)           frame save and restore (not reproduced)
//   001d758c  sq $s0, 0($sp)
//   001d7590  lw $v0, 44($a0)           table = record->table
//   001d7594  daddu $s0, $a2, $zero     the value a2, kept for the store
//   001d7598  addu $v0, $v0, $v1        entry = table + slot << 4
//   001d759c  jal 0x002b6da0            callee(entry + 4)
//   001d75a0  addiu $a0, $v0, 4         (delay slot)
//   001d75a4  sw $s0, 64($v0)           node->word at +64 = value
//   001d75a8  ld $ra, 16($sp)
//   001d75ac  addiu $v0, $v0, 8         return node + 8
//   001d75b0  lq $s0, 0($sp)
//   001d75b4  jr $ra
//   001d75b8  addiu $sp, $sp, 32        (delay slot)
//
// Retired counts (delay slots included): 15 here, plus the callee's 9 (empty source list) or 19 to 21.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002b6da0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTableOffset = 44U;          // lw $v0, 44($a0)
constexpr std::uint32_t kSlotShift = 4U;            // sll $v1, $a1, 4: 16-byte slots
constexpr GuestAddress kEntryBlock = 4U;            // addiu $a0, $v0, 4
constexpr GuestAddress kNodeWord = 64U;             // sw $s0, 64($v0)
constexpr GuestAddress kNodePayload = 8U;           // addiu $v0, $v0, 8
constexpr std::uint32_t kFrameBytes = 32U;          // addiu $sp, $sp, -32
constexpr std::uint32_t kBeforeCall = 9U;           // 0x001d7580-0x001d75a0: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 6U;            // 0x001d75a4-0x001d75b8
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run transfer_slot_node_and_store(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const std::uint32_t slot = call.arg(1U);
    const std::uint32_t value = call.arg(2U);
    const std::uint32_t slot_offset = slot << kSlotShift;
    const GuestAddress table = m.read_u32_le(record + kTableOffset);
    const GuestAddress entry = table + slot_offset;
    call.cpu().write_gpr_sign_extended_32(3U, slot_offset);     // $v1 holds the slot offset on every path
    call.set_arg(0U, entry + kEntryBlock);
    const n3::Run callee = call.invoke([](auto& target) { return n3::fn_002b6da0(target); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    const GuestAddress node = call.result();
    m.write_u32_le(node + kNodeWord, value);
    call.return_value(node + kNodePayload);
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d7580, 60U, 0x9f88cb753e0ab90bULL, "TransferSlotNodeAndStore", kCeiling, transfer_slot_node_and_store,
         EngineReturn::Integer)
