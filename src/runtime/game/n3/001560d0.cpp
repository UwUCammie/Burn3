// 0x001560d0 (a0 = object, f12 = x): ThresholdReachedTest. The object's record P is the pointer at +200;
// t is the float at P+5760 and lim the float at P+4300. Returns 0 when t is below zero (c.olt.s, t against
// +0.0). Otherwise it adds x to t (add.s) and returns 1 when lim <= t + x (c.ole.s with lim as the left
// operand), 0 when it is not. A leaf with no calls and no stores; f0 and v1 are temporaries, callers read v0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRecordPointer = 200U;     // lw $v1, 200($a0)
constexpr GuestAddress kValueOffset = 5760U;      // lwc1 $f1, 5760($v1): t
constexpr GuestAddress kThresholdOffset = 4300U;  // lwc1 $f1, 4300($v1): lim

// Retired instruction counts from the listing, delay slots included: t below zero takes the
// branch to the return (9); otherwise the threshold test passes and falls through (16), or it
// branches to the shared return (15).
constexpr std::uint32_t kBelowZeroCost = 9U;
constexpr std::uint32_t kReachedCost = 16U;
constexpr std::uint32_t kNotReachedCost = 15U;

template <class Context>
n3::Run threshold_reached_test(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = m.read_u32_le(call.arg(0) + kRecordPointer);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);
    const std::uint32_t value = m.read_u32_le(record + kValueOffset);
    if (fpu::lt(value, 0U)) {
        call.return_value(0U);
        return call.finish(kBelowZeroCost);
    }
    const std::uint32_t sum = fpu::add(value, x);
    const std::uint32_t threshold = m.read_u32_le(record + kThresholdOffset);
    if (fpu::le(threshold, sum)) {
        call.return_value(1U);
        return call.finish(kReachedCost);
    }
    call.return_value(0U);
    return call.finish(kNotReachedCost);
}

} // namespace

extern constinit const EngineFunction n3_001560d0{0x001560d0U, 68U, 0x7e383e97006e7c3dULL, "ThresholdReachedTest",
                                                  16U, n3::live<&threshold_reached_test<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001560d0, threshold_reached_test)
