// 0x002f4ea0 (a0 = tuning record P, a1 = first output, a2 = second output, f12 = x, f13 = y, f14 = w, f15 = z):
// SampleResponseAndFalloff. It stores f15 into P + 8 (the word both callees read), then stores
// InterpolateResponseCurve(P, x, y) at a1 and PowerFalloffBlend(P, |x|, |w|) at a2, each store taking the callee's
// f0. It returns nothing itself: f0 is left holding the second callee's result, as the original leaves it.
//
// The 80-byte frame only saves ra, s0-s2, f20 and f21, and those saves are not reproduced (the check ignores words
// below the entry $sp). The routine's own retired count is 28: 14 up to the first jal and its delay slot, 5 from the
// store after it through the second jal and its delay slot, and 9 from the store to the return. Each callee adds
// its own retired count. Both callees run in the entry's dry run first, so a decline from either hands the whole call
// to the original before anything is written.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002f4ba0)
N3_CALLEE(002f4dc0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 80U;          // the caller's addiu $sp, $sp, -80
constexpr GuestAddress kScaleField = 8U;            // swc1 $f15, 8($a0): the word both callees read
constexpr std::uint32_t kBeforeFirstCall = 14U;     // 0x002f4ea0-0x002f4ed4: up to the first jal and its delay slot
constexpr std::uint32_t kBetweenCalls = 5U;         // 0x002f4ed8-0x002f4ee8: the store, the setup and the second jal
constexpr std::uint32_t kAfterSecondCall = 9U;      // 0x002f4eec-0x002f4f0c: the store, the restores and the return
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run sample_response_and_falloff(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress tuning = call.arg(0);
    const GuestAddress curve_out = call.arg(1);
    const GuestAddress falloff_out = call.arg(2);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);
    const std::uint32_t y = call.cpu().read_fpr_bits(13U);
    const std::uint32_t w = call.cpu().read_fpr_bits(14U);
    const std::uint32_t z = call.cpu().read_fpr_bits(15U);

    // The original stores f15 before its first jal, so both callees read the new word.
    memory.write_u32_le(tuning + kScaleField, z);

    // InterpolateResponseCurve(P, x, y): f12 and f13 are the arguments this routine received, untouched so far.
    call.set_arg(0, tuning);
    call.set_float_arg(0, x);
    call.set_float_arg(1, y);
    const n3::Run curve = call.invoke([](auto& machine) { return n3::fn_002f4ba0(machine); }, kFrameBytes);
    if (curve.declined) {
        return call.decline();
    }
    memory.write_u32_le(curve_out, call.float_result());

    // PowerFalloffBlend(P, |x|, |w|).
    call.set_arg(0, tuning);
    call.set_float_arg(0, fpu::abs(x));
    call.set_float_arg(1, fpu::abs(w));
    const n3::Run falloff = call.invoke([](auto& machine) { return n3::fn_002f4dc0(machine); }, kFrameBytes);
    if (falloff.declined) {
        return call.decline();
    }
    memory.write_u32_le(falloff_out, call.float_result());

    return call.finish(kBeforeFirstCall + curve.retired + kBetweenCalls + falloff.retired + kAfterSecondCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f4ea0, 112U, 0xffad61f236a81423ULL, "SampleResponseAndFalloff", kCeiling, sample_response_and_falloff,
         EngineReturn::Nothing)
