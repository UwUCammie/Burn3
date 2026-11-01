// 0x00386300 (a0 = object, a1 = slot index or -1, f12 = value): SetFloatSlotOrAll.
// The object holds eight float words at +420, stride 4. When a1 is -1 (compared as the whole
// 64-bit register, as the original's bne does) f12's bits go to all eight slots, otherwise to
// the single slot a1 names, with no bounds check and the 32-bit address wrap. Returns nothing.
//
// The loop at 0x00386314 is a fixed eight-iteration loop: the delay slot of 0x0038630c clears a1,
// the test is sltiu a1 < 8, and a1 only ever goes up by one, so the all-slots path is written as
// straight code. Retired counts from the listing, delay slots included: all slots
// 3 + 2 + 9 * 3 + 8 * 4 + 2 + 2 = 68 (the declared cost), one slot 3 + 4 + 2 + 2 = 11.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kSlotOffset = 420U;                 // swc1 offset from the object pointer
constexpr std::uint32_t kSlotCount = 8U;                    // loop bound of the all-slots path
constexpr std::uint64_t kAllSlots = 0xffffffffffffffffULL;  // addiu $v1, $zero, -1 (whole register)
constexpr std::uint32_t kAllSlotsCost = 68U;
constexpr std::uint32_t kOneSlotCost = 11U;

template <class Context>
n3::Run set_float_slot_or_all(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t bits = call.cpu().read_fpr_bits(12U);
    if (call.arg64(1) == kAllSlots) {
        for (std::uint32_t i = 0U; i < kSlotCount; ++i) {
            m.write_u32_le(object + kSlotOffset + 4U * i, bits);
        }
        return call.finish(kAllSlotsCost);
    }
    // sll and addu are 32-bit operations, so only the low word of a1 reaches the address.
    const GuestAddress slot_address = object + kSlotOffset + (call.arg(1) << 2U);
    m.write_u32_le(slot_address, bits);
    return call.finish(kOneSlotCost);
}

} // namespace

extern constinit const EngineFunction n3_00386300{0x00386300U, 80U, 0xae7b02e0304fea6eULL, "SetFloatSlotOrAll", 68U,
                                                  n3::live<&set_float_slot_or_all<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00386300, set_float_slot_or_all)
