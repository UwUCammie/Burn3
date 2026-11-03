// 0x00432160 (no arguments, result in v0): PickRandomFlaggedSlot. Each of the 100 slots 0..99 is tested with
// IsSlotEnabledByFlag (0x0013ad70, native) as (handle, slot, 0), where the handle is the word at 0x0051bf9c. That
// test returns 0 for a flagged slot. With no flagged slot the original returns 0 and writes nothing, which is the
// only path this file runs. As soon as one slot is flagged, the original moves the two-word state at 0x00518680 and
// 0x00518684, takes the k-th flagged slot (k = new state word modulo the flagged count) and returns what 0x003e9520
// gives for it. 0x003e9520 has no native yet, so this file declines that path before anything is written.
//
// Retired instructions on the covered path (delay slots included, from the listing): the prologue is 11; each slot
// is 10 (the two argument moves, the jal and its delay slot, the bne and its nop, then the addiu, slti, bne and its
// nop) plus the callee's own count (15 for table B); the tail is 14 (blez and its nop, the two jumps to the return
// with their delay slots, and the 8 epilogue instructions). That is 11 + 100 * 25 + 14 = 2525. The slot loop has the
// constant bound 100 (slti $v0, $s3, 100), so it needs no ceiling test.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0013ad70)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kHandleWord = 0x0051bf9cU;   // lui 0x52 with lw -16484
constexpr std::uint32_t kSlotCount = 100U;
constexpr std::uint32_t kFrameBytes = 96U;          // the original's addiu $sp, $sp, -96
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint32_t kPrologueCost = 11U;        // 0x00432160-0x00432188
constexpr std::uint32_t kSlotCost = 10U;            // one pass, without the callee's own count
constexpr std::uint32_t kTailCost = 14U;            // 0x004321b8-0x00432278 on the path with no flagged slot

template <class Context>
n3::Run pick_random_flagged_slot(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t handle = call.memory().read_u32_le(kHandleWord);
    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t slot = 0U; slot < kSlotCount; ++slot) {
        call.set_arg(0, handle);
        call.set_arg(1, slot);
        call.set_arg(2, 0U);
        const n3::Run test = call.invoke([](auto& m) { return n3::fn_0013ad70(m); }, kFrameBytes);
        if (test.declined) {
            return call.decline();
        }
        if (call.result() == 0U) {
            // A flagged slot: the original takes the random pick branch, which is not native.
            return call.decline();
        }
        retired += kSlotCost + test.retired;
    }
    call.return_value(0U);
    return call.finish(retired + kTailCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00432160, 284U, 0xbf86fd01be455441ULL, "PickRandomFlaggedSlot", kCeiling, pick_random_flagged_slot,
         EngineReturn::Integer)
