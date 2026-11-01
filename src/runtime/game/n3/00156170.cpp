// 0x00156170 (a0 = object, f12 = delta): CheckRemainingRangeFits. A leaf with no stores. The sub-record at
// object+200 holds two floats: cur at +5512 and limit at +4300. Returns v0 = 0 when cur is negative. Otherwise
// returns 1 when limit <= cur + delta and 0 when it is not. The original also leaves temporaries in f0, f1
// and v1; they are caller-saved, so they are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kRecordOffset = 200U;      // lw 200(a0): the sub-record pointer
constexpr std::uint32_t kCurOffset = 5512U;        // lwc1 5512(rec): cur
constexpr std::uint32_t kLimitOffset = 4300U;      // lwc1 4300(rec): limit

// Retired instruction counts of the three exits, delay slots included.
constexpr std::uint32_t kNegativeCost = 9U;        // cur < 0: the bc1t at 0x00156180 is taken to 0x001561a8
constexpr std::uint32_t kBelowLimitCost = 15U;     // limit > cur + delta: the bc1f at 0x00156194 is taken
constexpr std::uint32_t kReachedCost = 16U;        // limit <= cur + delta: the bc1f falls through, xori leaves v0 = 1

template <class Context>
n3::Run check_remaining_range_fits(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = m.read_u32_le(call.arg(0) + kRecordOffset);
    const std::uint32_t cur = m.read_u32_le(record + kCurOffset);
    if (fpu::lt(cur, 0U)) {
        call.return_value(0U);
        return call.finish(kNegativeCost);
    }
    const std::uint32_t sum = fpu::add(cur, cpu.read_fpr_bits(12U));
    const std::uint32_t limit = m.read_u32_le(record + kLimitOffset);
    if (fpu::le(limit, sum)) {
        call.return_value(1U);
        return call.finish(kReachedCost);
    }
    call.return_value(0U);
    return call.finish(kBelowLimitCost);
}

} // namespace

extern constinit const EngineFunction n3_00156170{0x00156170U, 68U, 0x48d336ae63879c38ULL, "CheckRemainingRangeFits",
                                                  16U, n3::live<&check_remaining_range_fits<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00156170, check_remaining_range_fits)
