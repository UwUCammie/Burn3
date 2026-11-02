// 0x0011a5c8 (f12 = x): logf, the libm natural logarithm, returning f0. Straight-line code with no calls or loops.
//
//   |x| <= 0x7fffff in bits (zero or denormal)  -> the float constant at 0x004b2b6c (the library's "log(0)" value)
//   x < 0                                       -> (x - x) / 0.0, which the PS2 divide turns into a clamped +-max
//   otherwise x = 2^e * (1 + f) is split by the usual fdlibm trick: the mantissa m is rebased around sqrt(2) so that
//   f = (1 + m/2^23) * 2^-k - 1 lies in [-0.29, 0.41] and e is adjusted by k.
//     f tiny (m within 15 of 0x7fffff... of the mantissa window): f == 0 gives e * ln2, otherwise the short series
//       f - f*f*(0.5 - f/3) (plus the e * ln2 terms)
//     otherwise s = f / (2 + f), z = s*s, w = z*z, two Horner chains in w, and one of two corrections chosen by the
//       sign of ((m + 0xffcf5c30) | (0x35c288 - m)), each with or without the e * ln2 terms.
// Every float operation is the PS2 FPU's (n3::fpu) in the printed order: none of them is associative there.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kNegativeInfinityValue = 0x004b2b6cU;  // lui 0x4b, lwc1 11116
constexpr std::uint32_t kMantissaMask = 0x007fffffU;
constexpr std::uint32_t kRebase = 0x004afb20U;                // adds m past the sqrt(2) boundary into bit 23
constexpr std::uint32_t kBoundaryBit = 0x00800000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kLn2Hi = 0x3f317180U;
constexpr std::uint32_t kLn2Lo = 0x3717f7d1U;
constexpr std::uint32_t kThird = 0x3eaaaaaaU;
constexpr std::uint32_t kLg1 = 0x3f2aaaaaU;
constexpr std::uint32_t kLg2 = 0x3eccccccU;
constexpr std::uint32_t kLg3 = 0x3e924925U;
constexpr std::uint32_t kLg4 = 0x3e638e29U;
constexpr std::uint32_t kLg5 = 0x3e3a3324U;
constexpr std::uint32_t kLg6 = 0x3e1cd04eU;
constexpr std::uint32_t kLg7 = 0x3e178896U;
constexpr std::uint32_t kCorrectionHigh = 0xffcf5c30U;
constexpr std::uint32_t kCorrectionLow = 0x0035c288U;

// Retired instruction counts per path (delay slots included, skipped likely-branch slots not counted).
constexpr std::uint32_t kPrologue = 10U;        // 0x0011a5c8-0x0011a5ec
constexpr std::uint32_t kTinyExit = 2U;         // jr + lwc1 delay slot
constexpr std::uint32_t kSignTest = 2U;         // 0x0011a5f8-0x0011a5fc
constexpr std::uint32_t kNegativeTail = 7U;     // 0x0011a600-0x0011a618
constexpr std::uint32_t kSplit = 20U;           // 0x0011a620-0x0011a66c
constexpr std::uint32_t kPositive = kPrologue + kSignTest + kSplit;   // 32
constexpr std::uint32_t kNearEqualTest = 4U;    // bc1fl taken: delay slot runs
constexpr std::uint32_t kNearEqualFallThrough = 3U;  // bc1fl not taken: delay slot skipped
constexpr std::uint32_t kExactZero = 2U;        // 0x0011a680-0x0011a684
constexpr std::uint32_t kExactMulTail = 12U;    // 0x0011a688-0x0011a6b4
constexpr std::uint32_t kSeries = 9U;           // 0x0011a6b8-0x0011a6d8
constexpr std::uint32_t kSeriesNoExponent = 2U; // 0x0011a6dc-0x0011a6e0
constexpr std::uint32_t kSeriesExponent = 14U;  // 0x0011a6e8-0x0011a71c
constexpr std::uint32_t kReturnBlock = 2U;      // 0x0011a8a0-0x0011a8a4
constexpr std::uint32_t kPolynomial = 52U;      // 0x0011a720-0x0011a7ec
constexpr std::uint32_t kCorrectionB = 5U;      // 0x0011a7f0-0x0011a800
constexpr std::uint32_t kCorrectionBNoExponent = 5U;
constexpr std::uint32_t kCorrectionBExponent = 15U;
constexpr std::uint32_t kCorrectionATest = 2U;  // 0x0011a858-0x0011a85c
constexpr std::uint32_t kCorrectionANoExponent = 3U;
constexpr std::uint32_t kCorrectionAExponent = 12U;

template <class Context>
n3::Run logf_body(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const std::uint32_t x = cpu.read_fpr_bits(12U);

    if ((x & 0x7fffffffU) <= kMantissaMask) {
        cpu.write_fpr_bits(0U, call.memory().read_u32_le(kNegativeInfinityValue));
        return call.finish(kPrologue + kTinyExit);
    }
    if (static_cast<std::int32_t>(x) < 0) {
        cpu.write_fpr_bits(0U, fpu::div(fpu::sub(x, x), 0U));
        return call.finish(kPrologue + kSignTest + kNegativeTail);
    }

    // x = 2^exponent * (1 + m / 2^23); k is 1 when the mantissa lies above the sqrt(2) boundary.
    const std::uint32_t m = x & kMantissaMask;
    const std::uint32_t k = (kRebase + m) & kBoundaryBit;
    const std::int32_t exponent = static_cast<std::int32_t>(x >> 23U) - 127 + static_cast<std::int32_t>(k >> 23U);
    const std::uint32_t f = fpu::sub(m | (k ^ kOne), kOne);

    if (((m + 15U) & kMantissaMask) < 16U) {
        // f is tiny.
        if (fpu::eq(f, 0U)) {
            if (exponent == 0) {
                cpu.write_fpr_bits(0U, 0U);
                return call.finish(kPositive + kNearEqualFallThrough + kExactZero + kReturnBlock);
            }
            const std::uint32_t e = fpu::from_int(exponent);
            cpu.write_fpr_bits(0U, fpu::add(fpu::mul(e, kLn2Hi), fpu::mul(e, kLn2Lo)));
            return call.finish(kPositive + kNearEqualFallThrough + kExactZero + kExactMulTail);
        }
        const std::uint32_t ff = fpu::mul(f, f);
        const std::uint32_t r = fpu::mul(ff, fpu::sub(kHalf, fpu::mul(f, kThird)));
        if (exponent == 0) {
            cpu.write_fpr_bits(0U, fpu::sub(f, r));
            return call.finish(kPositive + kNearEqualTest + kSeries + kSeriesNoExponent);
        }
        const std::uint32_t e = fpu::from_int(exponent);
        const std::uint32_t lo = fpu::sub(r, fpu::mul(e, kLn2Lo));
        cpu.write_fpr_bits(0U, fpu::sub(fpu::mul(e, kLn2Hi), fpu::sub(lo, f)));
        return call.finish(kPositive + kNearEqualTest + kSeries + kSeriesExponent);
    }

    // General case: s = f / (2 + f), then log(1 + f) = f - s * (f - R) with R from two Horner chains in w = s^4.
    const std::uint32_t s = fpu::div(f, fpu::add(f, kTwo));
    const std::uint32_t z = fpu::mul(s, s);
    const std::uint32_t w = fpu::mul(z, z);
    std::uint32_t t1 = fpu::mul(w, kLg7);
    std::uint32_t t2 = fpu::mul(w, kLg6);
    t1 = fpu::add(t1, kLg5);
    t2 = fpu::add(t2, kLg4);
    t1 = fpu::mul(w, t1);
    t2 = fpu::mul(w, t2);
    t1 = fpu::add(t1, kLg3);
    t2 = fpu::add(t2, kLg2);
    t1 = fpu::mul(w, t1);
    t2 = fpu::mul(w, t2);
    t1 = fpu::add(t1, kLg1);
    const std::uint32_t r = fpu::add(fpu::mul(z, t1), t2);
    const std::uint32_t e = fpu::from_int(exponent);
    const std::int32_t selector = static_cast<std::int32_t>(((m + kCorrectionHigh) | (kCorrectionLow - m)));

    if (selector > 0) {
        const std::uint32_t hfsq = fpu::mul(fpu::mul(f, kHalf), f);
        if (exponent == 0) {
            const std::uint32_t t = fpu::sub(hfsq, fpu::mul(s, fpu::add(hfsq, r)));
            cpu.write_fpr_bits(0U, fpu::sub(f, t));
            return call.finish(kPositive + kPolynomial + kCorrectionB + kCorrectionBNoExponent);
        }
        const std::uint32_t hi = fpu::mul(e, kLn2Hi);
        const std::uint32_t lo = fpu::mul(e, kLn2Lo);
        const std::uint32_t t = fpu::sub(fpu::sub(hfsq, fpu::add(fpu::mul(s, fpu::add(hfsq, r)), lo)), f);
        cpu.write_fpr_bits(0U, fpu::sub(hi, t));
        return call.finish(kPositive + kPolynomial + kCorrectionB + kCorrectionBExponent);
    }
    if (exponent == 0) {
        cpu.write_fpr_bits(0U, fpu::sub(f, fpu::mul(s, fpu::sub(f, r))));
        return call.finish(kPositive + kPolynomial + kCorrectionATest + kCorrectionANoExponent);
    }
    const std::uint32_t hi = fpu::mul(e, kLn2Hi);
    const std::uint32_t lo = fpu::mul(e, kLn2Lo);
    const std::uint32_t t = fpu::sub(fpu::sub(fpu::mul(s, fpu::sub(f, r)), lo), f);
    cpu.write_fpr_bits(0U, fpu::sub(hi, t));
    return call.finish(kPositive + kPolynomial + kCorrectionATest + kCorrectionAExponent + kReturnBlock);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0011a5c8, 736U, 0x0b7a3f240004020bULL, "logf_body", 104U, logf_body, EngineReturn::Float)
