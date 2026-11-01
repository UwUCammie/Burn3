// 0x001a0370 (a0 = pointer to a float): IsFloatNotMinusOne. Loads the float at *a0 into f0, sets f1 = -1.0f
// (bits 0xbf800000) and compares them with c.eq.s. bc1f branches to the return when the compare is false, so
// v0 = 1 is the result for every value that is not exactly -1.0f, a NaN included (9 instructions retired). The
// delay slot (addiu $v0, $zero, 1) runs on both paths; equal values fall through to daddu $v0, $zero, $zero,
// which makes v0 = 0 (10 retired). Leaf: no memory stores, no calls. Result in v0 (EngineReturn::Integer).

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;

constexpr std::uint32_t kMinusOneBits = 0xbf800000U;  // lui $v0, 0xbf80 (-1.0f), then mtc1 into $f1
constexpr std::uint32_t kNotEqualCost = 9U;           // 0x1a0370-0x1a0388 (7, delay slot included) + jr + nop
constexpr std::uint32_t kEqualCost = 10U;             // the same 7, + daddu + jr + nop

template <class Context>
n3::Run is_float_not_minus_one(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const GuestAddress address = call.arg(0);
    cpu.write_fpr_bits(0U, call.memory().read_u32_le(address));  // lwc1 $f0, 0($a0)
    cpu.write_fpr_bits(1U, kMinusOneBits);                       // lui $v0, 0xbf80 ; mtc1 $v0, $f1
    ee_cop1::compare_s(cpu, ee_cop1::Compare::Equal, 1U, 0U);     // c.eq.s $f1, $f0
    if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) == 0U) {   // bc1f 0x001a0390: not equal
        call.return_value(1U);                                    // addiu $v0, $zero, 1 (delay slot)
        return call.finish(kNotEqualCost);
    }
    call.return_value(0U);  // the delay slot set v0 = 1; daddu $v0, $zero, $zero clears it
    return call.finish(kEqualCost);
}

} // namespace

extern constinit const EngineFunction n3_001a0370{0x001a0370U, 40U, 0xaa11d8ebef86eb1aULL, "IsFloatNotMinusOne",
                                                  10U, n3::live<&is_float_not_minus_one<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001a0370, is_float_not_minus_one)
