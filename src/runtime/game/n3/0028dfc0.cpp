// 0x0028dfc0 (a0 = byte address): the float weight of an effect kind. Reads the byte k
// at a0 and returns f0 = 0.5 for k in {19, 11, 10, 7}, 1.0 for k in {17, 13, 12, 3, 2,
// 1, 6, 9, 8, 20} and 0.0 for any other k. Writes no memory.
// The original tests k against those 14 values in that order, 3 instructions each, so
// a hit on the n-th value retires 7 + 3n instructions and a miss retires 48.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Bit patterns of the two weights (the original loads them with lui 0x3f00 and lui 0x3f80).
constexpr std::uint32_t kHalfBits = 0x3f000000U;
constexpr std::uint32_t kOneBits = 0x3f800000U;

// The values the original tests, in its order. The first kHalfKinds weigh 0.5, the rest 1.0.
constexpr std::array<std::uint8_t, 14U> kKinds{19U, 11U, 10U, 7U, 17U, 13U, 12U, 3U, 2U, 1U, 6U, 9U, 8U, 20U};
constexpr std::size_t kHalfKinds = 4U;

template <class Context>
n3::Run effect_kind_weight(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint8_t kind = call.memory().read_u8(call.arg(0));
    // The lbu that loads k.
    std::uint32_t retired = 1U;
    for (std::size_t n = 0U; n < kKinds.size(); ++n) {
        // addiu, beq and its delay slot nop.
        retired += 3U;
        if (kind == kKinds[n]) {
            // lui, mtc1, beq and its nop to the shared return, then jr and its delay slot.
            call.cpu().write_fpr_bits(0U, n < kHalfKinds ? kHalfBits : kOneBits);
            return call.finish(retired + 6U);
        }
    }
    // beq and its nop to the zero exit, mtc1 $zero, then jr and its delay slot.
    call.cpu().write_fpr_bits(0U, 0U);
    return call.finish(retired + 5U);
}

} // namespace

extern constinit const EngineFunction n3_0028dfc0{0x0028dfc0U, 224U, 0x7dde6a4daf457229ULL, "EffectKindWeight", 49U,
                                                   n3::live<&effect_kind_weight<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(0028dfc0, effect_kind_weight)
