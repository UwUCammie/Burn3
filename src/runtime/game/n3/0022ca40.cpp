// 0x0022ca40 (a0 = object, a1 = slot index word): FillSlotPairAndTestRecords. Calls FillSlotPointerPair
// (0x0022c610) with a 32-byte frame and its out[] at frame + 24, then reads out[0] and out[1] in that order:
// the first one that is non-null and has a nonzero halfword at +164 makes the result 1, otherwise it is 0.
//
// The original's counter (a0, an int8 that starts at 0 and steps by 1) runs the test for 0 and 1 only, so the
// two slots are a fixed loop. Before the call a2 takes a1's full 64-bit value and a1 becomes out[] (the jal's
// delay slot); the callee reads the low byte of a2 as its slot index.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kOutOffset = 24U;        // out[] = frame + 24: two words, one record pointer each
constexpr std::uint32_t kSlotCount = 2U;
constexpr GuestAddress kFlagOffset = 164U;      // the halfword at record + 164 decides the slot
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included. The callee's count is added on top.
constexpr std::uint32_t kEntryCost = 7U;        // 0022ca40-0022ca58: frame, jal, the jump to the test
constexpr std::uint32_t kCounterTestCost = 5U;  // 0022ca84-0022ca94: counter < 2
constexpr std::uint32_t kSlotLoadCost = 5U;     // 0022ca5c-0022ca6c: out[counter] and its null test
constexpr std::uint32_t kHalfwordTestCost = 3U; // 0022ca70-0022ca78: the halfword and its zero test
constexpr std::uint32_t kStepCost = 4U;         // 0022caa0-0022caac: counter + 1, back to the test
constexpr std::uint32_t kExitCost = 5U;         // the exit branch (2) and 0022cab0-0022cab8 (3)

template <class Context>
n3::Run fill_slot_pair_and_test_records(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg64(2, call.arg64(1));
    call.set_arg(1, out);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_0022c610(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost + callee.retired;
    for (std::uint32_t slot = 0U; slot < kSlotCount; ++slot) {
        retired += kCounterTestCost + kSlotLoadCost;
        const GuestAddress record = memory.read_u32_le(out + 4U * slot);
        if (record == 0U) {
            retired += kStepCost;
            continue;
        }
        if (static_cast<std::int16_t>(memory.read_u16_le(record + kFlagOffset)) == 0) {
            retired += kHalfwordTestCost + kStepCost;
            continue;
        }
        call.return_value(1U);
        return call.finish(retired + kHalfwordTestCost + kExitCost);
    }
    call.return_value(0U);
    return call.finish(retired + kCounterTestCost + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022ca40, 124U, 0xc572c9787c3e0a78ULL, "FillSlotPairAndTestRecords", kCeiling, fill_slot_pair_and_test_records,
         EngineReturn::Integer)
