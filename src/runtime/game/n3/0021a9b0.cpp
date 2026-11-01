// 0x0021a9b0 "StepThresholdTimer": a0 = the timer record, f12 = the input, f13 = the frame's delta
// time, a1 = enable (nonzero runs the countdown). Record: s16 fired (+0), s16 armed (+2), f32 previous
// input (+4), f32 countdown (+8). Every exit stores the input to +4 last. The results live in the record,
// so callers ignore v0/f0 (EngineReturn::Nothing). Leaf with no loops: the declared cost is the longest
// path (52); the shortest is 13.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Float words in the $gp data area, as negative offsets from $gp (the address wraps to 32 bits).
constexpr std::uint32_t kRiseThresholdOffset = 31576U;   // T0: an idle timer arms when the input rises through it
constexpr std::uint32_t kInitialTimerOffset = 32160U;    // the countdown when the timer arms
constexpr std::uint32_t kDisarmThresholdOffset = 32084U; // an armed input at or below this disarms
constexpr std::uint32_t kRampLowOffset = 32192U;         // T3: the low edge of the ramp
constexpr std::uint32_t kRampHighOffset = 32708U;        // T4: the high edge of the ramp
constexpr std::uint32_t kRampWidthOffset = 31512U;       // the ramp width, the divisor of the ramp fraction
constexpr std::uint32_t kPeriodOffset = 31580U;          // added to the countdown when it expires

// 1.0, 2.0 and 3.0 as the original builds them (lui).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kThree = 0x40400000U;

// The join at 0x0021aa6c that the three countdown branches share. `countdown` is the value the branch
// just stored to +8 (the original reloads it there) and `retired` counts the instructions up to the join.
// A countdown below zero expires the timer: fired = 1, the period is added, and a result still below
// zero is clamped to +0.0. Every path ends by storing the input to +4.
std::uint32_t settle_countdown(auto& m, const GuestAddress record, const GuestAddress gp,
                               const std::uint32_t countdown, const std::uint32_t input,
                               const std::uint32_t retired) {
    // lwc1, mtc1, nop, c.olt.s, bc1f (taken unless below zero), nop: 6, then 0x0021a9f4 and the return: 2 + 2.
    if (!fpu::lt(countdown, 0U)) {
        m.write_u32_le(record + 4U, input);
        return retired + 6U + 2U + 2U;
    }
    // 0x0021aa84-0x0021aaa0: sh 1 to +0, load the period, add it to the reloaded countdown, and store the
    // sum to +8 in the delay slot of the test, so it is stored whichever way the test goes.
    m.write_u16_le(record, 1U);
    const std::uint32_t expired = fpu::add(countdown, m.read_u32_le(gp - kPeriodOffset));
    m.write_u32_le(record + 8U, expired);
    if (!fpu::lt(expired, 0U)) {
        m.write_u32_le(record + 4U, input);
        return retired + 6U + 8U + 2U + 2U;
    }
    // 0x0021aaa4: beq to 0x0021a9f4, with the store of +0.0 to +8 in its delay slot.
    m.write_u32_le(record + 8U, 0U);
    m.write_u32_le(record + 4U, input);
    return retired + 6U + 8U + 2U + 2U + 2U;
}

template <class Context>
n3::Run step_threshold_timer(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t input = call.cpu().read_fpr_bits(12U);
    const std::uint32_t dt = call.cpu().read_fpr_bits(13U);
    // beq $a1,$zero compares the whole 64-bit register.
    const bool timer_runs = call.arg64(1U) != 0U;

    // sh $zero,0($a0); lh $v1,2($a0); bne $v1,$zero (its delay-slot nop is retired either way).
    m.write_u16_le(record, 0U);
    const auto armed = static_cast<std::int16_t>(m.read_u16_le(record + 2U));
    if (armed == 0) {
        // Idle: the timer arms when the previous input is below T0 and the input is at or above it.
        const std::uint32_t previous = m.read_u32_le(record + 4U);
        const std::uint32_t rise = m.read_u32_le(gp - kRiseThresholdOffset);
        if (!fpu::lt(previous, rise)) {
            // bc1f taken: 4 + 5 retired, then 0x0021a9f4 and the return.
            m.write_u32_le(record + 4U, input);
            return call.finish(13U);
        }
        if (fpu::lt(input, rise)) {
            // bc1t taken: 4 + 5 + 3 retired, then 0x0021a9f4 and the return.
            m.write_u32_le(record + 4U, input);
            return call.finish(16U);
        }
        // 0x0021a9e0-0x0021a9f0: arm, with the countdown at its initial value.
        const std::uint32_t initial = m.read_u32_le(gp - kInitialTimerOffset);
        m.write_u16_le(record, 1U);
        m.write_u16_le(record + 2U, 1U);
        m.write_u32_le(record + 8U, initial);
        m.write_u32_le(record + 4U, input);
        return call.finish(21U);
    }

    // Armed. An input at or below the disarm threshold disarms (+2 = 0).
    const std::uint32_t disarm = m.read_u32_le(gp - kDisarmThresholdOffset);
    if (fpu::le(input, disarm)) {
        m.write_u16_le(record + 2U, 0U);
        m.write_u32_le(record + 4U, input);
        return call.finish(14U);
    }
    // A disabled timer keeps its countdown.
    if (!timer_runs) {
        m.write_u32_le(record + 4U, input);
        return call.finish(14U);
    }
    const std::uint32_t ramp_low = m.read_u32_le(gp - kRampLowOffset);
    if (fpu::le(input, ramp_low)) {
        // 0x0021aaac: at or below T3 the countdown drops by dt.
        const std::uint32_t countdown = fpu::sub(m.read_u32_le(record + 8U), dt);
        m.write_u32_le(record + 8U, countdown);
        return call.finish(settle_countdown(m, record, gp, countdown, input, 18U));
    }
    const std::uint32_t ramp_high = m.read_u32_le(gp - kRampHighOffset);
    if (!fpu::lt(input, ramp_high)) {
        // 0x0021aabc: at or above T4 the countdown drops by 3.0 * dt.
        const std::uint32_t countdown = fpu::msub(fpu::add(0U, m.read_u32_le(record + 8U)), kThree, dt);
        m.write_u32_le(record + 8U, countdown);
        return call.finish(settle_countdown(m, record, gp, countdown, input, 26U));
    }
    // 0x0021aa34: on the ramp the countdown drops by dt * (1.0 + 2.0 * fraction), with the fraction
    // (input - T3) / width. The accumulator sequence is ACC = 0 + 1.0, then ACC + 2.0 * fraction, then a
    // fresh ACC = 0 + countdown, then ACC - dt * (that sum).
    const std::uint32_t width = m.read_u32_le(gp - kRampWidthOffset);
    const std::uint32_t fraction = fpu::div(fpu::sub(input, ramp_low), width);
    const std::uint32_t ramp_scale = fpu::madd(fpu::add(0U, kOne), kTwo, fraction);
    const std::uint32_t countdown = fpu::msub(fpu::add(0U, m.read_u32_le(record + 8U)), dt, ramp_scale);
    m.write_u32_le(record + 8U, countdown);
    return call.finish(settle_countdown(m, record, gp, countdown, input, 32U));
}

} // namespace

extern constinit const EngineFunction n3_0021a9b0{0x0021a9b0U, 316U, 0xa99fd09c0abdd37aULL, "StepThresholdTimer", 52U,
                                                   n3::live<&step_threshold_timer<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021a9b0, step_threshold_timer)
