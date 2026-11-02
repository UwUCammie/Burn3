// 0x0011b438 SquareRoot (libm sqrtf): the square root of f12 by an integer digit-by-digit recurrence on the
// mantissa, result in f0. No FPU arithmetic is used on the normal path, so the answer is a truncated root
// rounded to even on the last bit, exactly as the original computes it.
//
//   |x| <= 0x7fffff (zero or denormal bits): returns x unchanged (sign included).
//   x negative (sign bit set, not zero/denormal): (x - x) / (x - x) through the EE FPU (the game's "NaN").
//   otherwise: e = exponent - 127, m = (mantissa | hidden bit) << (e & 1), e >>= 1 (arithmetic). The recurrence
//   runs a fixed 25 passes: the probe bit starts at 1 << 24 and is halved once per pass, so it is zero after
//   exactly 25 passes whatever the input. Each pass either skips (m < acc + bit) or subtracts and grows acc.
//   The root is then rounded up by its low bit when a remainder is left, and the result bits are
//   0x3f000000 + (root >> 1) + (e << 23).
//
// Retired counts (delay slots and the likely-branch rule included): 12 for the zero/denormal return, 18 for the
// negative one; the positive path is 12 + 12 setup + 6 per skipped pass + 9 per subtracting pass + 2 + 1 (when a
// remainder is left) + 8, at most 260, which is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;

constexpr std::uint32_t kMagnitudeMask = 0x7fffffffU;
constexpr std::uint32_t kMantissaMask = 0x007fffffU;
constexpr std::uint32_t kHiddenBit = 0x00800000U;
constexpr std::uint32_t kFirstProbe = 0x01000000U;
constexpr std::uint32_t kExponentBias = 127U;
constexpr std::uint32_t kHalfBits = 0x3f000000U;

constexpr std::uint32_t kSmallReturn = 12U;     // zero/denormal: 10 to the test's delay slot, jr and its nop
constexpr std::uint32_t kNegativeReturn = 18U;  // 10 + bgez with delay slot, sub/nop/nop/div, jr and nop
constexpr std::uint32_t kPositiveHead = 24U;    // 10 + 2 to the sign test, then the 12-instruction setup
constexpr std::uint32_t kSkipPass = 6U;         // addu, slt, bnel with its delay slot, then bne with its delay slot
constexpr std::uint32_t kSubtractPass = 9U;     // the same without the delay slot, plus 4 for subtract and update
constexpr std::uint32_t kTail = 10U;            // beq with its delay slot, the final 8
constexpr std::uint32_t kRoundUp = 1U;          // addu when a remainder is left

template <class Context>
n3::Run square_root(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);

    if ((x & kMagnitudeMask) <= kMantissaMask) {
        call.cpu().write_fpr_bits(0U, x);
        return call.finish(kSmallReturn);
    }
    if ((x >> 31U) != 0U) {
        const std::uint32_t zero = fpu::sub(x, x);
        call.cpu().write_fpr_bits(0U, fpu::div(zero, zero));
        return call.finish(kNegativeReturn);
    }

    std::uint32_t exponent = (x >> 23U) - kExponentBias;
    std::uint32_t remainder = ((x & kMantissaMask) | kHiddenBit) << (exponent & 1U) << 1U;
    exponent = static_cast<std::uint32_t>(static_cast<std::int32_t>(exponent) >> 1);

    std::uint32_t retired = kPositiveHead;
    std::uint32_t accumulated = 0U;
    std::uint32_t root = 0U;
    // The probe bit goes 1 << 24, 1 << 23 ... 1, so there are exactly 25 passes.
    for (std::uint32_t bit = kFirstProbe; bit != 0U; bit >>= 1U) {
        const std::uint32_t trial = accumulated + bit;
        if (static_cast<std::int32_t>(remainder) < static_cast<std::int32_t>(trial)) {
            retired += kSkipPass;
        } else {
            remainder -= trial;
            accumulated = trial + bit;
            root += bit;
            retired += kSubtractPass;
        }
        remainder <<= 1U;
    }
    retired += kTail;
    if (remainder != 0U) {
        root += root & 1U;
        retired += kRoundUp;
    }
    const std::uint32_t result = kHalfBits + static_cast<std::uint32_t>(static_cast<std::int32_t>(root) >> 1) + (exponent << 23U);
    call.cpu().write_fpr_bits(0U, result);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0011b438, 204U, 0xe6f25877087756c2ULL, "SquareRoot", 260U, square_root, EngineReturn::Float)
