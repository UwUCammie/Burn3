// 0x0013ad70 (a0 unused, a1 slot index, a2 selector): IsSlotEnabledByFlag. Returns in v0 whether the slot's byte is
// clear: 1 when the byte is zero, 0 when it is not. The selector picks the table: a2 != 0 reads table A at
// 0x0051bff7, a2 == 0 reads table B at 0x0051c040. An index of -1 returns 0 without any load.
//
// A leaf with no loop, store or call. The original's beq tests on $a1 and $a2 compare whole 64-bit registers, so
// the -1 and selector tests read them with arg64. The address is the 32-bit wrap of table + index (addu), with no
// scaling and no range check, so any index reads that address.
//
// Retired instruction counts from the listing, delay slots included: index -1 takes 6 (addiu, beq, nop, daddu,
// jr, nop); a2 == 0 takes 15; a2 != 0 takes 16 (the xori sits in a delay slot, so the table A block runs 7).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x52 with addiu -16393 (table A) and addiu -16320 (table B): one byte per slot.
constexpr GuestAddress kTableA = 0x0051bff7U;
constexpr GuestAddress kTableB = 0x0051c040U;
// addiu $v0, $zero, -1 as a whole register: the value beq compares $a1 against.
constexpr std::uint64_t kNoSlot = 0xffffffffffffffffULL;

constexpr std::uint32_t kCostNoSlot = 6U;
constexpr std::uint32_t kCostTableB = 15U;
constexpr std::uint32_t kCostTableA = 16U;

template <class Context>
n3::Run slot_enabled_by_flag(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    if (call.arg64(1U) == kNoSlot) {
        call.return_value(0U);
        return call.finish(kCostNoSlot);
    }
    const bool table_a = call.arg64(2U) != 0U;
    const GuestAddress table = table_a ? kTableA : kTableB;
    // addu: the low 32 bits of the index, added to the table base and wrapped to 32 bits.
    const std::uint8_t slot = m.read_u8(table + call.arg(1U));
    call.return_value(slot == 0U ? 1U : 0U);
    return call.finish(table_a ? kCostTableA : kCostTableB);
}

} // namespace

extern constinit const EngineFunction n3_0013ad70{0x0013ad70U, 92U, 0x9624adcba3963bfeULL, "IsSlotEnabledByFlag",
                                                  kCostTableA, n3::live<&slot_enabled_by_flag<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0013ad70, slot_enabled_by_flag)
