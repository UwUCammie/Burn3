// 0x0032ee00 (a0 = blink record): BlinkPatternStep, the per-frame step of a blinking indicator. The record holds
// its owner pointer at +68, timer A and timer B (floats) at +76 and +80, a bit pattern at +84 and an active byte at
// +88. The owner's byte at +6381 is its lit flag, cleared every frame. Both timers count down by the float at
// 0x0052267c each frame. When A runs out it restarts at 25.0 and the active byte clears if the record was active,
// otherwise A is set to zero. While A is still running and the record is active, B counts down too; when B runs
// out it is refilled by the float at $gp-30928 and the pattern rotates right by one bit (bit 0 wraps to bit 31).
// The owner is lit while bit 0 of the pattern is set. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFrameStep = 0x0052267cU;      // float: lui 0x52, +9852
constexpr GuestAddress kOwnerOffset = 68U;
constexpr GuestAddress kTimerAOffset = 76U;
constexpr GuestAddress kTimerBOffset = 80U;
constexpr GuestAddress kPatternOffset = 84U;
constexpr GuestAddress kActiveOffset = 88U;
constexpr GuestAddress kLitFlagOffset = 6381U;        // on the owner
constexpr std::uint32_t kRefillDistance = 30928U;     // float at $gp - 30928
constexpr std::uint32_t kRestartBits = 0x41c80000U;   // 25.0f
constexpr std::uint8_t kClear = 0U;
constexpr std::uint8_t kLit = 1U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 10U;              // owner, lit flag clear, timer A step and its test
constexpr std::uint32_t kActiveTestCost = 3U;         // active byte test (lbu, branch, delay slot)
constexpr std::uint32_t kRestartCost = 3U;            // timer A = 25.0, active cleared
constexpr std::uint32_t kClearTimerCost = 2U;         // timer A = +0.0 (branch, delay slot)
constexpr std::uint32_t kTimerBCost = 7U;             // timer B step and its test (branch, delay slot)
constexpr std::uint32_t kRefillCost = 12U;            // timer B refill and the pattern rotation
constexpr std::uint32_t kPatternTestCost = 5U;        // bit 0 of the pattern test
constexpr std::uint32_t kLightCost = 3U;              // owner lit flag set
constexpr std::uint32_t kJoinCost = 2U;               // branch to the exit (branch, delay slot)
constexpr std::uint32_t kReturnCost = 2U;             // jr $ra and its delay slot

template <class Context>
n3::Run blink_pattern_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    // The owner's lit flag is cleared first, every frame.
    const GuestAddress owner = m.read_u32_le(record + kOwnerOffset);
    m.write_u8(owner + kLitFlagOffset, kClear);
    // Timer A counts down; its store sits in the bc1f delay slot, so it always happens.
    const std::uint32_t frame_step = m.read_u32_le(kFrameStep);
    const std::uint32_t timer_a_old = m.read_u32_le(record + kTimerAOffset);
    const std::uint32_t timer_a = fpu::sub(timer_a_old, frame_step);
    m.write_u32_le(record + kTimerAOffset, timer_a);
    std::uint32_t retired = kHeadCost + kActiveTestCost;
    const bool active = m.read_u8(record + kActiveOffset) != 0U;

    if (fpu::le(timer_a, 0U)) {
        if (active) {
            m.write_u32_le(record + kTimerAOffset, kRestartBits);
            m.write_u8(record + kActiveOffset, kClear);
            retired += kRestartCost + kJoinCost + kReturnCost;
        } else {
            m.write_u32_le(record + kTimerAOffset, 0U);
            retired += kClearTimerCost + kJoinCost + kReturnCost;
        }
        return call.finish(retired);
    }
    if (!active) {
        retired += kReturnCost;
        return call.finish(retired);
    }

    // Timer B counts down; again the store always happens.
    const std::uint32_t timer_b_old = m.read_u32_le(record + kTimerBOffset);
    const std::uint32_t frame_step_b = m.read_u32_le(kFrameStep);
    const std::uint32_t timer_b = fpu::sub(timer_b_old, frame_step_b);
    m.write_u32_le(record + kTimerBOffset, timer_b);
    retired += kTimerBCost;
    if (fpu::le(timer_b, 0U)) {
        // Refill from the $gp float, then rotate the pattern right by one bit (store, reload, store).
        const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
        const std::uint32_t refill = m.read_u32_le(gp - kRefillDistance);
        m.write_u32_le(record + kTimerBOffset, fpu::add(timer_b, refill));
        const std::uint32_t pattern = m.read_u32_le(record + kPatternOffset);
        const std::uint32_t low_bit = pattern & 1U;
        m.write_u32_le(record + kPatternOffset, pattern >> 1U);
        const std::uint32_t shifted = m.read_u32_le(record + kPatternOffset);
        m.write_u32_le(record + kPatternOffset, shifted | (low_bit << 31U));
        retired += kRefillCost;
    }

    // Bit 0 of the pattern (as stored by the rotation above, if any) lights the owner.
    const std::uint32_t pattern_now = m.read_u32_le(record + kPatternOffset);
    retired += kPatternTestCost;
    if ((pattern_now & 1U) != 0U) {
        const GuestAddress owner_again = m.read_u32_le(record + kOwnerOffset);
        m.write_u8(owner_again + kLitFlagOffset, kLit);
        retired += kLightCost;
    }
    retired += kJoinCost + kReturnCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0032ee00{0x0032ee00U, 216U, 0x2ce151d6a9dc929fULL, "BlinkPatternStep", 44U,
                                                  n3::live<&blink_pattern_step<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0032ee00, blink_pattern_step)
