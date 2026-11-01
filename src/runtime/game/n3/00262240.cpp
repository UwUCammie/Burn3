// 0x00262240 (a0 = base): FlushThreeQuadEntryRanges. It calls FlushQuadEntryRange (0x00307440) three times, on the
// records at base + 52*i + 12 for i = 0, 1, 2, in that order. Returns nothing.
//
//   00262240  addiu $sp, $sp, -64        frame of 64 bytes (ra at +32, s1 at +16, s0 at +0)
//   00262244..0026224c                   the three saves into the frame (not reproduced: below the entry sp)
//   00262250  daddu $s1, $a0, $zero      s1 = base
//   00262254  daddu $s0, $zero, $zero    i = 0
//   00262258..0026226c                   v0 = s1 + 52*s0 (the sll/addu chain computes 52*s0)
//   00262270  jal 0x00307440             delay slot 00262274: a0 = v0 + 12
//   00262278  addiu $s0, $s0, 1
//   0026227c  slti $v1, $s0, 3           v1 = (i < 3)
//   00262280  bne $v1, $zero, 0x00262258 delay slot 00262284: nop
//   00262288..00262298                   restores of ra, s1 and s0, jr $ra, delay slot addiu $sp, $sp, 64
//
// The trip count is fixed at three (s0 counts 0, 1, 2 and nothing else writes it), so the function's own retired
// count is fixed too: 6 + 3 * 12 + 5 = 47. Each call adds the callee's own count from its run. The loop is therefore
// written with its constant bound, not as a data-dependent loop.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00307440)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kRecordCount = 3U;        // the loop's trip count (slti $s0, 3 after the increment)
constexpr GuestAddress kRecordStride = 52U;       // v0 = base + 52 * i
constexpr GuestAddress kRecordOffset = 12U;       // a0 = v0 + 12, from the jal's delay slot
constexpr std::uint32_t kFrameBytes = 64U;        // addiu $sp, $sp, -64
constexpr std::uint32_t kPrologueCost = 6U;       // 00262240..00262254, delay slots included
constexpr std::uint32_t kIterationCost = 12U;     // 00262258..00262284: the jal and its delay slot, the branch and its nop
constexpr std::uint32_t kEpilogueCost = 5U;       // 00262288..00262298, the jr's delay slot included
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on the live machine or on a DryMemory (the call_cost that N3_ENTRY makes).
template <class Context>
n3::Run flush_three_quad_entry_ranges(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress base = call.arg(0);
    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t index = 0U; index < kRecordCount; ++index) {
        const GuestAddress record = base + kRecordStride * index;
        call.return_value(record);                 // v0, as the original's addu leaves it
        call.set_arg(0, record + kRecordOffset);   // a0 of the jal, written by its delay slot
        const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00307440(m); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        // slti $v1, $s0, 3 after the increment: 1 while the loop goes on, 0 on the last pass.
        call.cpu().write_gpr_sign_extended_32(3U, index + 1U < kRecordCount ? 1U : 0U);
        retired += kIterationCost + callee.retired;
    }
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00262240, 92U, 0xf74c4699d619e082ULL, "FlushThreeQuadEntryRanges", kCeiling, flush_three_quad_entry_ranges,
         EngineReturn::Nothing)
