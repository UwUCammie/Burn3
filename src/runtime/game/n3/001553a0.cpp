// 0x001553a0 (TimedFieldGate): a timed gate on the record at a0. The float timer
// at +1520 is advanced by the float delta in f12. The bit pattern of -1.0 there
// means idle, and the call returns 0. Otherwise the advanced timer is compared
// (c.olt.s) with the float threshold at +12: below it returns 0, and otherwise
// returns the word at +1516 (lw, sign-extended into v0). Only reads memory: no
// stores, no calls, no loops. Retired instructions (delay slots included): 10
// when idle, 18 below the threshold, 17 otherwise. The declared cost is the longest.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kIdleTimerBits = 0xbf800000U;  // -1.0f
constexpr GuestAddress kTimerOffset = 1520U;
constexpr GuestAddress kThresholdOffset = 12U;
constexpr GuestAddress kValueOffset = 1516U;

template <class Context>
n3::Run timed_field_gate(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const std::uint32_t timer = m.read_u32_le(record + kTimerOffset);
    if (fpu::eq(kIdleTimerBits, timer)) {
        call.return_value64(0U);
        return call.finish(10U);
    }
    const std::uint32_t advanced = fpu::add(timer, call.cpu().read_fpr_bits(12U));
    const std::uint32_t threshold = m.read_u32_le(record + kThresholdOffset);
    if (fpu::lt(advanced, threshold)) {
        call.return_value64(0U);
        return call.finish(18U);
    }
    call.return_value(m.read_u32_le(record + kValueOffset));
    return call.finish(17U);
}

} // namespace

extern constinit const EngineFunction n3_001553a0{0x001553a0U, 80U, 0xf3b15f16ba0a42eaULL, "TimedFieldGate", 18U,
                                                  n3::live<&timed_field_gate<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001553a0, timed_field_gate)
