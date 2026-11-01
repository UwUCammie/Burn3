// 0x00295c80 (a0 = object O, a1 = selector): the record R = word(O + 76). Selector 1 reads R's count
// (halfword +12) and array (word +16); any other selector reads the count (halfword +14) and array
// (word +20). Each 24-byte element of the array gets +12 = the value 0x00291a50 returns, +16 = 0 and
// +20 = 0x00295c70, and 0x00293710 then clears the element's first three words. Callers read no result.
//
// Every recorded call has a zero count, so the loop never runs. A non-zero count reaches the cold
// 0x00293710 call: call_cost declines it and the original runs it. The implementation therefore only
// does the setup, the call to 0x00291a50 (always executed: a read-only leaf whose loop never iterates,
// because its bound a1 is zero) and the return. It stores nothing outside its own frame.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCalleeWord = 0x01e7ced8U;   // 0x00291a50's a0: lui 0x01e8, addiu -12584

// Retired instruction counts from the listing, delay slots included, without 0x00291a50's own.
constexpr std::uint32_t kFixedSelectedOne = 32U;    // selector 1: 15 + 5 + 4 + 2 + 6
constexpr std::uint32_t kFixedOtherSelector = 31U;  // otherwise: 15 + 5 + 4 + 1 + 6
// The longest zero-count path: selector 1 with 0x00291a50's longest path (18), so 32 + 18.
constexpr std::uint32_t kLongestCost = 50U;

// 0x00291a50 with a0 = kCalleeWord and a1 = 0 (set by the jal's delay slot). Its loop tests a3 < a1
// with a3 = 0 and a1 = 0, so it never iterates, and it stores nothing. Its return value only feeds the
// caller's loop body, which the native does not run, so only the retired count is needed: 7 when the
// word is negative, 9 when it is zero, 18 when it is positive.
template <class Memory>
std::uint32_t callee_retired(const Memory& memory) {
    const auto word = static_cast<std::int32_t>(memory.read_u32_le(kCalleeWord));
    if (word < 0) {
        return 7U;   // lw, slt, bne (taken), nop, daddu, jr, nop
    }
    if (word == 0) {
        return 9U;   // lw, slt, bne, nop, beq (taken), nop, daddu, jr, nop
    }
    return 18U;      // lw, slt, bne, nop, beq, nop, lw, daddu, addiu, beq, addiu, slt, bne, nop, beq, nop, jr, nop
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the
// retired count, or 0 when the count is non-zero: the loop would run, so call_cost declines the call.
template <class Memory>
std::uint32_t init_callback_slots(const Memory& memory, const GuestAddress object, const bool selector_is_one) {
    const GuestAddress record = memory.read_u32_le(object + 76U);
    const std::uint16_t count =
        selector_is_one ? memory.read_u16_le(record + 12U) : memory.read_u16_le(record + 14U);
    if (count != 0) {
        return 0U;
    }
    const std::uint32_t fixed = selector_is_one ? kFixedSelectedOne : kFixedOtherSelector;
    return fixed + callee_retired(memory);
}

// The dispatcher runs this only after call_cost accepted the call, so the count is zero here.
template <class Context>
n3::Run init_callback_slot_array(Context& context) {
    auto call = n3::guest_call(context);
    // The original's bne compares the whole register, so the selector is tested as 64 bits.
    return call.finish(init_callback_slots(call.memory(), call.arg(0), call.arg64(1U) == 1U));
}

template <class Context>
std::uint32_t init_callback_slot_array_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    return init_callback_slots(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u64(5U) == 1U);
}

} // namespace

extern constinit const EngineFunction n3_00295c80{0x00295c80U, 264U, 0xbf4df80290ae8424ULL, "InitCallbackSlotArray",
                                                  kLongestCost, n3::live<&init_callback_slot_array<NativeFunctionContext>>, EngineReturn::Nothing, {},
                                                  1e-4, 1e-6, {}, &init_callback_slot_array_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00295c80, init_callback_slot_array, init_callback_slot_array_cost)
