// 0x0022cd60 (a0 = object, a1 = slot index, of which only the low byte matters, read signed): SlotPairHasActiveRecord.
// It calls FillSlotPointerPair (0x0022c610) with out = $sp + 24 in a 32-byte frame; that call writes out[0] and out[1].
// It then returns 1 when out[0] or out[1] is a record pointer whose halfword at +32 is non-zero (out[0] is tested
// first), else 0. Only the zero test of that halfword matters, so the sign extension of lh does not change the result.
//
// The original's index loop cannot make more than two passes: its counter starts at 0 in $a0 (set in the delay slot
// of the branch into the test) and the test `signed byte < 2` ends it after the second pass, whatever the data. So the
// loop below is a bounded loop of two passes, and the retired count depends only on the path taken.
//
// Retired counts, delay slots included: 7 for the frame, the call and the jump into the test, plus the callee's own
// count. Each pass of the test is 5. Then a zero word costs 9, a record with a zero halfword costs 12, and a set
// halfword returns 1 at a cost of 13. Falling out of both passes costs 10 (the last test and the return with 0).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kOutOffset = 24U;           // out[k] = $sp + 24 + 4 * k, with k = 0, 1
constexpr std::uint32_t kOutEntries = 2U;
constexpr GuestAddress kFieldOffset = 32U;         // halfword at record + 32

// Retired instruction counts of the listing, delay slots included.
constexpr std::uint32_t kCallCost = 7U;            // 0x0022cd60-0x0022cd78: the frame, the call and the jump into the test
constexpr std::uint32_t kTestCost = 5U;            // 0x0022cda4-0x0022cdb4: one pass of the index test
constexpr std::uint32_t kEmptyCost = 9U;           // zero word: 0x0022cd7c-0x0022cd8c, then the step 0x0022cdc0-0x0022cdcc
constexpr std::uint32_t kClearedCost = 12U;        // zero halfword: 0x0022cd7c-0x0022cd98, then the step
constexpr std::uint32_t kHitCost = 13U;            // set halfword: 0x0022cd7c-0x0022cd98, then 0x0022cd9c-0x0022cda0 and the return
constexpr std::uint32_t kMissCost = 5U;            // after both passes: 0x0022cdb8-0x0022cdbc, then the return
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run slot_pair_has_active_record(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg64(2, call.arg64(1));  // daddu $a2, $a1, $zero
    call.set_arg(1, out);              // addiu $a1, $sp, 24 (the call's delay slot)
    const n3::Run fill = call.invoke([](auto& machine) { return n3::fn_0022c610(machine); }, kFrameBytes);
    if (fill.declined) {
        return call.decline();
    }
    std::uint32_t retired = kCallCost + fill.retired;
    for (std::uint32_t entry = 0U; entry < kOutEntries; ++entry) {
        retired += kTestCost;
        const GuestAddress record = call.memory().read_u32_le(out + 4U * entry);
        if (record == 0U) {
            retired += kEmptyCost;
            continue;
        }
        if (call.memory().read_u16_le(record + kFieldOffset) != 0U) {
            call.return_value(1U);
            return call.finish(retired + kHitCost);
        }
        retired += kClearedCost;
    }
    retired += kTestCost + kMissCost;
    call.return_value(0U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022cd60, 124U, 0x297eee224978e814ULL, "SlotPairHasActiveRecord", kCeiling, slot_pair_has_active_record,
         EngineReturn::Integer)
