// 0x001d7560 (a0 = object O, a1 = slot index I, a2 = node address + 8): the slot table base is the word at O+44,
// and its 16-byte slot I is E = base + 16 * I. The function returns E in v0 and tail-calls UnlinkNodeAndPushToFreeList
// (0x002b6ca0) with a0 = E + 4 (the list descriptor) and a1 = a2 - 8 (the node). The callee never writes v0, so
// v0 = E is what the original hands back.
//
//   001d7560  lw $v0, 44($a0)        v0 = O[+44]
//   001d7564  sll $v1, $a1, 4        v1 = 16 * I
//   001d7568  addiu $a1, $a2, -8     a1 = node
//   001d756c  addu $v0, $v0, $v1     v0 = E
//   001d7570  j 0x002b6ca0           tail call, frame 0
//   001d7574  addiu $a0, $v0, 4      (delay slot) a0 = E + 4

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002b6ca0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSlotArrayOffset = 44U;   // lw $v0, 44($a0)
constexpr std::uint32_t kSlotShift = 4U;         // sll $v1, $a1, 4: slots are 16 bytes
constexpr std::uint32_t kNodeOffset = 8U;        // addiu $a1, $a2, -8
constexpr GuestAddress kDescriptorOffset = 4U;   // addiu $a0, $v0, 4
// The six instructions above, the tail jump and its delay slot included; the callee's count is added to it.
constexpr std::uint32_t kPathCost = 6U;
// Calls retiring more than this are left to the original (the dry run returns 0), so the reported count is exact.
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run release_slot_node(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    const std::uint32_t index = call.arg(1);
    const std::uint32_t node = call.arg(2) - kNodeOffset;
    const GuestAddress slot = call.memory().read_u32_le(object + kSlotArrayOffset) + (index << kSlotShift);
    call.return_value(slot);
    call.set_arg(0, slot + kDescriptorOffset);
    call.set_arg(1, node);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_002b6ca0(machine); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kPathCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d7560, 24U, 0x326a4dc7e29b324cULL, "ReleaseSlotNode", kCeiling, release_slot_node, EngineReturn::Integer)
