// 0x00119c20 ArcCosine (libm acosf, fdlibm's e_acosf): the arc cosine of f12, result in f0. Every float operation goes
// through n3::fpu (the EE FPU rounds toward zero, flushes denormals and has no NaN); the order of the operations is
// the listing's.
//
//   |x| == 1.0:                      +0.0 for +1.0, pi for -1.0
//   |x| > 1.0 (inf and NaN bits too): (x - x) / (x - x)
//   |x| <= 0x23000000 (tiny):        pi/2
//   |x| <  0.5:                      pi/2 - (x - (pio2_lo - x * R(x*x)))
//   x <= -0.5:                       z = (1 + x) / 2, s = sqrt(z):  pi - 2 * (s + (R(z) * s - pio2_lo))
//   x >= 0.5:                        z = (1 - x) / 2, s = sqrt(z), c = s with the low 12 bits cleared,
//                                    w = (z - c * c) / (s + c):     2 * (c + (R(z) * s + w))
// where R(t) = t * P(t) / Q(t) is the rational approximation, P and Q evaluated by Horner's rule in t.
//
// The square root is the native at 0x0011b438. The two big paths call it, so their count is the listing's plus its.
// Counts per path (without the callee): 20 / 24 for x = +1.0 / -1.0, 26 above 1.0, 30 tiny, 96 for |x| < 0.5, 100 for
// x <= -0.5 and 104 for x >= 0.5; with the callee's 260 at most the declared cost 364.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0011b438)

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;

constexpr std::uint32_t kMagnitudeMask = 0x7fffffffU;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kBelowHalf = 0x3effffffU;  // the largest magnitude below 0.5
constexpr std::uint32_t kTinyLimit = 0x23000000U;
constexpr std::uint32_t kPi = 0x40490fdaU;
constexpr std::uint32_t kHalfPi = 0x3fc90fdaU;
constexpr std::uint32_t kHalfPiLow = 0x33a22168U;
constexpr std::uint32_t kLowBitsMask = 0xfffff000U;

// R(t) = t*P(t) / Q(t), fdlibm's pS0..pS5 and qS1..qS4.
constexpr std::uint32_t kP0 = 0x3811ef08U;
constexpr std::uint32_t kP1 = 0x3a4f7f04U;
constexpr std::uint32_t kP2 = 0xbd241146U;
constexpr std::uint32_t kP3 = 0x3e4e0aa8U;
constexpr std::uint32_t kP4 = 0xbea6b08fU;
constexpr std::uint32_t kP5 = 0x3e2aaaaaU;
constexpr std::uint32_t kQ0 = 0x3d9dc62dU;
constexpr std::uint32_t kQ1 = 0xbf303360U;
constexpr std::uint32_t kQ2 = 0x4001572cU;
constexpr std::uint32_t kQ3 = 0xc019d138U;

// Retired original instructions per block, delay slots included.
constexpr std::uint32_t kEntry = 12U;           // through the |x| == 1.0 test with its delay slot
constexpr std::uint32_t kPlusOne = 3U + 1U + 4U;
constexpr std::uint32_t kMinusOne = 3U + 5U + 4U;
constexpr std::uint32_t kAboveOne = 3U + 6U + 1U + 4U;
constexpr std::uint32_t kRangeTest = 3U + 4U;   // the > 1.0 test, then the >= 0.5 test
constexpr std::uint32_t kTiny = 6U + 1U + 4U;
constexpr std::uint32_t kSmall = 6U + 66U + 1U + 4U;
constexpr std::uint32_t kNegativeHalf = 2U + 73U + 1U + 1U + 4U;
constexpr std::uint32_t kPositiveHalf = 2U + 77U + 1U + 1U + 4U;

struct Rational {
    std::uint32_t numerator;    // t * P(t)
    std::uint32_t denominator;  // Q(t)
};

// P and Q by Horner's rule in t, interleaved in the listing but independent, so evaluated one after the other.
Rational rational_parts(const std::uint32_t t) {
    std::uint32_t p = fpu::add(fpu::mul(t, kP0), kP1);
    p = fpu::add(fpu::mul(t, p), kP2);
    p = fpu::add(fpu::mul(t, p), kP3);
    p = fpu::add(fpu::mul(t, p), kP4);
    p = fpu::add(fpu::mul(t, p), kP5);
    std::uint32_t q = fpu::add(fpu::mul(t, kQ0), kQ1);
    q = fpu::add(fpu::mul(t, q), kQ2);
    q = fpu::add(fpu::mul(t, q), kQ3);
    q = fpu::add(fpu::mul(t, q), kOne);
    return {fpu::mul(t, p), q};
}

template <class Context>
n3::Run arc_cosine(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const std::uint32_t x = cpu.read_fpr_bits(12U);
    const std::uint32_t magnitude = x & kMagnitudeMask;
    const bool negative = (x >> 31U) != 0U;

    if (magnitude == kOne) {
        cpu.write_fpr_bits(0U, negative ? kPi : 0U);
        return call.finish(kEntry + (negative ? kMinusOne : kPlusOne));
    }
    if (magnitude > kOne) {
        const std::uint32_t zero = fpu::sub(x, x);
        cpu.write_fpr_bits(0U, fpu::div(zero, zero));
        return call.finish(kEntry + kAboveOne);
    }
    if (magnitude <= kBelowHalf) {
        if (magnitude <= kTinyLimit) {
            cpu.write_fpr_bits(0U, kHalfPi);
            return call.finish(kEntry + kRangeTest + kTiny);
        }
        const std::uint32_t t = fpu::mul(x, x);
        const Rational parts = rational_parts(t);
        const std::uint32_t r = fpu::div(parts.numerator, parts.denominator);
        std::uint32_t result = fpu::sub(kHalfPiLow, fpu::mul(x, r));
        result = fpu::sub(x, result);
        cpu.write_fpr_bits(0U, fpu::sub(kHalfPi, result));
        return call.finish(kEntry + kRangeTest + kSmall);
    }

    // |x| in [0.5, 1): z = (1 -+ |x|) / 2 and its square root.
    const std::uint32_t z = fpu::mul(negative ? fpu::add(x, kOne) : fpu::sub(kOne, x), kHalf);
    const Rational parts = rational_parts(z);
    call.set_float_arg(0U, z);
    const n3::Run root = call.invoke([](auto& m) { return n3::fn_0011b438(m); }, 48U);
    if (root.declined) {
        return call.decline();
    }
    const std::uint32_t s = call.float_result();
    const std::uint32_t r = fpu::div(parts.numerator, parts.denominator);
    std::uint32_t result = 0U;
    if (negative) {
        result = fpu::sub(fpu::mul(r, s), kHalfPiLow);
        result = fpu::add(s, result);
        result = fpu::add(result, result);
        result = fpu::sub(kPi, result);
    } else {
        const std::uint32_t c = s & kLowBitsMask;
        const std::uint32_t w = fpu::div(fpu::sub(z, fpu::mul(c, c)), fpu::add(s, c));
        result = fpu::add(fpu::mul(r, s), w);
        result = fpu::add(c, result);
        result = fpu::add(result, result);
    }
    cpu.write_fpr_bits(0U, result);
    return call.finish(kEntry + kRangeTest + (negative ? kNegativeHalf : kPositiveHalf) + root.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00119c20, 1060U, 0x3fe2a9f2f39d0aadULL, "ArcCosine", 364U, arc_cosine, EngineReturn::Float)
