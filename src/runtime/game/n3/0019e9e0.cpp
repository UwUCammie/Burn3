// 0x0019e9e0 (SubobjectBySlot): a0 is an object's base address and a1 a slot selector. The selector is compared
// as a whole 64-bit register, against 2, then 1, then 0 (beq). Slot 2 gives a0 + 304, slot 1 gives a0 + 144,
// slot 0 gives a0 + 784, and any other value gives 0. Pure address arithmetic: no memory reads or writes. The
// result is written by the delay slot of the jump that leaves the tests (addiu, or daddu for the zero result).
//
// Retired counts follow the listing, delay slots included: 6 for slot 2, 10 for slot 1, and 12 for slot 0 or any
// other value. The declared cost is the longest path, 12.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kSlotTwoOffset = 304U;
constexpr std::uint32_t kSlotOneOffset = 144U;
constexpr std::uint32_t kSlotZeroOffset = 784U;

template <class Context>
n3::Run subobject_by_slot(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    // BEQ compares the whole register, so the selector is read as 64 bits.
    const std::uint64_t slot = call.arg64(1);
    if (slot == 2U) {
        call.return_value(object + kSlotTwoOffset);
        return call.finish(6U);
    }
    if (slot == 1U) {
        call.return_value(object + kSlotOneOffset);
        return call.finish(10U);
    }
    if (slot == 0U) {
        call.return_value(object + kSlotZeroOffset);
        return call.finish(12U);
    }
    call.return_value(0U);
    return call.finish(12U);
}

} // namespace

extern constinit const EngineFunction n3_0019e9e0{0x0019e9e0U, 68U, 0xfc9ed818535e63deULL, "SubobjectBySlot", 12U,
                                                   n3::live<&subobject_by_slot<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0019e9e0, subobject_by_slot)
