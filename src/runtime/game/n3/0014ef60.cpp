// 0x0014ef60 (a0 = object A): InitConstraintSlots. Returns nothing (v0 is never written on the paths handled here). The
// word at A+1388 is a pointer P. When P is zero the function returns at once. Otherwise loop 1 runs the byte at A+1420
// times and calls 0x003a1010 on each pass, loop 2 sets the words at A+1064 and A+1068 to 4 (exactly two passes), and
// loop 3 runs the signed word at P+1020 times and calls 0x003a1d10 on each pass. Both calls are cold (the recorded game
// never reached them), so the native handles only the paths that make no call: P is zero, or the byte at +1420 is zero
// and the count at P+1020 is not positive. call_cost declines every other input, and the original runs those.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPointerOffset = 1388U;      // word: pointer P (zero: nothing to initialise)
constexpr GuestAddress kFirstCountOffset = 1420U;   // byte: loop 1 count (any non-zero count calls 0x003a1010)
constexpr GuestAddress kSlotOffset = 1064U;         // loop 2 sets the words at +1064 + 4*pass to kSlotValue
constexpr std::uint32_t kSlotCount = 2U;            // loop 2 runs exactly this many passes (counter from 0, slti 2)
constexpr std::uint32_t kSlotValue = 4U;
constexpr GuestAddress kCountOffset = 1020U;        // word at P+1020: loop 3 count (a positive count calls 0x003a1d10)

// Retired instruction counts of the listing's blocks, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 9U;            // 0x0014ef60-0x0014ef80: prologue, pointer test and its delay slot
constexpr std::uint32_t kPathStartCost = 9U;        // 0x0014ef84-0x0014efa4: global loads, frame stores, jump to loop 1's test
constexpr std::uint32_t kLoop1TestCost = 4U;        // 0x0014efbc-0x0014efc8: byte test and its delay slot
constexpr std::uint32_t kLoop2SetupCost = 2U;       // 0x0014efcc-0x0014efd0: counter and value setup
constexpr std::uint32_t kLoop2PassCost = 7U;        // 0x0014efd4-0x0014efec: one pass of loop 2
constexpr std::uint32_t kLoop2ExitCost = 2U;        // 0x0014eff0-0x0014eff4: jump to loop 3's test and its delay slot
constexpr std::uint32_t kLoop3TestCost = 5U;        // 0x0014f020-0x0014f030: count test and its delay slot
constexpr std::uint32_t kReturnCost = 7U;           // 0x0014f034-0x0014f04c: epilogue, jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The retired count of a call, and whether its path reaches a cold call (then the count is void).
struct InitResult {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Loops 1 and 3 each call a cold
// routine on their first pass, so a path that enters either one stops there and is declined. Loop 2 has a fixed count,
// so it needs no ceiling check.
template <class Memory>
InitResult init_slots(Memory& m, const GuestAddress object) {
    std::uint32_t retired = kEntryCost;
    if (m.read_u32_le(object + kPointerOffset) == 0U) {
        return {retired + kReturnCost, false};
    }
    retired += kPathStartCost;
    // Loop 1 enters when the zero-extended byte is positive (slt against the counter 0).
    retired += kLoop1TestCost;
    if (m.read_u8(object + kFirstCountOffset) != 0U) {
        return {retired, true};
    }
    retired += kLoop2SetupCost;
    for (std::uint32_t pass = 0U; pass < kSlotCount; ++pass) {
        m.write_u32_le(object + kSlotOffset + 4U * pass, kSlotValue);
        retired += kLoop2PassCost;
    }
    retired += kLoop2ExitCost;
    // Loop 3 re-reads the pointer after loop 2's stores, as the original does.
    const GuestAddress pointer = m.read_u32_le(object + kPointerOffset);
    retired += kLoop3TestCost;
    if (static_cast<std::int32_t>(m.read_u32_le(pointer + kCountOffset)) > 0) {
        return {retired, true};
    }
    return {retired + kReturnCost, false};
}

// call_cost: the same body on a DryMemory, with the call's own $a0. The count is the original's exact count; a call that
// reaches a cold routine or retires more than kCeiling returns 0, and the original runs it.
template <class Context>
std::uint32_t init_constraint_slots_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const InitResult result = init_slots(dry, context.cpu.read_gpr_u32(4U));
    return (result.declined || result.retired > kCeiling) ? 0U : result.retired;
}

template <class Context>
n3::Run init_constraint_slots(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(init_slots(call.memory(), call.arg(0)).retired);
}

} // namespace

extern constinit const EngineFunction n3_0014ef60{0x0014ef60U, 240U, 0xb664c71d807cc48eULL, "InitConstraintSlots",
                                                  kCeiling, n3::live<&init_constraint_slots<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &init_constraint_slots_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0014ef60, init_constraint_slots, init_constraint_slots_cost)
