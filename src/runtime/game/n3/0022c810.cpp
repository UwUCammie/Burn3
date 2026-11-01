// 0x0022c810 (a0 = object, a1 = selector X, a2 = slot index): calls FillSlotPointerPair (0x0022c610) with a0 and
// a2 unchanged and out[] at $sp + 40. Then it returns 1 when one of the non-zero words out[0], out[1] (a record
// pointer) has a non-zero halfword at pointer + 12 * X + 20 (+ 22 when X == 8), else 0.
// The selector is read as a full register (X == 8 is a 64-bit compare) but only its low word feeds the address.
// The original's loop over out[0], out[1] runs exactly twice (its int8 counter goes 0, 1 and stops at 2), so it is
// written as a fixed two-slot loop. The original's own frame stores (ra, s0) are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022c610)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;              // addiu $sp, $sp, -48
constexpr GuestAddress kOutOffset = 40U;                // the jal's delay slot: a1 = $sp + 40 (out[])
constexpr std::uint32_t kOutSlots = 2U;                 // out[0], out[1]
constexpr std::uint32_t kRecordStride = 12U;            // selector * 12 (sll, addu, sll)
constexpr std::uint64_t kWideSelector = 8U;             // X == 8 is compared as a full 64-bit register
constexpr GuestAddress kFieldOffset = 20U;              // halfword at record + 12 * X + 20
constexpr GuestAddress kWideFieldOffset = 22U;          // halfword at record + 12 * X + 22 when X == 8

// Retired instruction counts of the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 11U;               // 0x0022c810-0x0022c838: frame and saves, jal and its delay slot, setup
constexpr std::uint32_t kSlotTestCost = 5U;             // 0x0022c83c-0x0022c84c: slot counter < 2 (taken or not)
constexpr std::uint32_t kSlotLoadCost = 5U;             // 0x0022c850-0x0022c860: out[slot] and its zero test
constexpr std::uint32_t kNextSlotCost = 4U;             // 0x0022c898-0x0022c8a4: counter + 1, back to the test
constexpr std::uint32_t kRecordCost = 2U;               // 0x0022c864-0x0022c868: selector != 8 test
constexpr std::uint32_t kFieldCost = 3U;                // 0x0022c87c-0x0022c884: halfword at +20 (selector != 8)
constexpr std::uint32_t kWideFieldCost = 4U;            // 0x0022c86c-0x0022c878: halfword at +22 (selector == 8)
constexpr std::uint32_t kFieldTestCost = 2U;            // 0x0022c888-0x0022c88c: branch on the halfword
constexpr std::uint32_t kHitCost = 2U;                  // 0x0022c890-0x0022c894: v0 = 1
constexpr std::uint32_t kExitCost = 1U;                 // 0x0022c8a8: v0 = 0
constexpr std::uint32_t kEpilogueCost = 4U;             // 0x0022c8ac-0x0022c8b8: restore ra and s0, pop the frame
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run pair_record_halfword_set(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    // The selector is read before the call: the out[] pointer then replaces a1.
    const std::uint64_t selector = call.arg64(1);
    const GuestAddress out = call.stack_pointer() - kFrameBytes + kOutOffset;
    call.set_arg(1, out);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0022c610(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    std::uint32_t retired = kEntryCost + callee.retired;
    const std::uint32_t scale = kRecordStride * static_cast<std::uint32_t>(selector);
    const bool wide = selector == kWideSelector;
    for (std::uint32_t slot = 0U; slot < kOutSlots; ++slot) {
        retired += kSlotTestCost;
        const GuestAddress record = m.read_u32_le(out + 4U * slot);
        retired += kSlotLoadCost;
        if (record == 0U) {
            retired += kNextSlotCost;
            continue;
        }
        retired += kRecordCost;
        const GuestAddress field = scale + record + (wide ? kWideFieldOffset : kFieldOffset);
        retired += wide ? kWideFieldCost : kFieldCost;
        retired += kFieldTestCost;
        if (m.read_u16_le(field) != 0U) {
            retired += kHitCost + kEpilogueCost;
            call.return_value(1U);
            return call.finish(retired);
        }
        retired += kNextSlotCost;
    }
    // The counter reaches 2: the test that leaves the loop, v0 = 0, then the epilogue.
    retired += kSlotTestCost + kExitCost + kEpilogueCost;
    call.return_value(0U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0022c810, 172U, 0xce861ba99c216449ULL, "PairRecordHalfwordSet", kCeiling, pair_record_halfword_set,
         EngineReturn::Integer)
