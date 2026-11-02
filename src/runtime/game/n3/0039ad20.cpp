// 0x0039ad20 UpdateTrailSegmentsStep: a forwarding wrapper around UpdateTrailSegments (0x00399c30).
//
//   0039ad20  addiu $sp, $sp, -32        frame of 32 bytes
//   0039ad24  sd $ra, 0($sp)             saves the return address (its own frame, not reproduced)
//   0039ad28  jal 0x00399c30
//   0039ad2c  nop                        (delay slot)
//   0039ad30  ld $ra, 0($sp)             restores it, so ra is the same on return
//   0039ad34  jr $ra
//   0039ad38  addiu $sp, $sp, 32         (delay slot)
//
// Nothing is set, cleared or read around the call. a0 (the effect object) and f12 (dt) pass straight through, and the
// callee reads the high word of $v1 as it stands at entry, so the live CPU state must reach it unchanged. Only $sp is
// lowered for the callee (Call::invoke), as the original's addiu does, and it is restored after the call.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00399c30)

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kFrameBytes = 32U;   // addiu $sp, $sp, -32
constexpr std::uint32_t kOwnCost = 7U;       // 0x0039ad20..0x0039ad38: the wrapper's seven instructions, delay slots included
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run update_trail_segments_step(Context& context) {
    auto call = n3::guest_call(context);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00399c30(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kOwnCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0039ad20, 28U, 0xdc426035673357b6ULL, "UpdateTrailSegmentsStep", kCeiling, update_trail_segments_step,
         EngineReturn::Nothing)
