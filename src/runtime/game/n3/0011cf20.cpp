// 0x0011cf20 SineKernel: fdlibm __kernel_sinf(x = f12, y = f13, iy = a0), the sine of a reduced argument with
// |x| <= pi/4, as a polynomial. Returns f0. Everything is raw IEEE bits through n3::fpu (the EE's FPU rounds toward
// zero and flushes denormals). y is read only when iy is nonzero.
//
// |x| < 2^-27 (x's bits with the sign cleared at most 0x31ffffff) and int(x) == 0 gives x itself. Otherwise, with
//   z = x*x, v = z*x, r = S2 + z*(S3 + z*(S4 + z*(S5 + z*S6))):
//   iy == 0:  x + v*(S1 + z*r)
//   iy != 0:  x - ((z*(0.5*y - v*r) - y) - v*S1)
//
// The retired counts are the listing's blocks on the path taken: 17 for the tiny return, 49 or 54 for |x| >= 2^-27
// (iy == 0 or not). An |x| < 2^-27 with int(x) != 0 cannot happen (such an x truncates to 0), so its counts (51 and
// 56) only keep the listing's paths exact; 56, the longest path, is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;

constexpr std::uint32_t kTinyLimit = 0x31ffffffU;  // |x| < 2^-27 (the bits of x with the sign cleared)
constexpr std::uint32_t kHalf = 0x3f000000U;       // 0.5
// fdlibm's S1..S6 as this build holds them (raw single-precision bits).
constexpr std::uint32_t kS1 = 0xbe2aaaaaU;
constexpr std::uint32_t kS2 = 0x3c088889U;
constexpr std::uint32_t kS3 = 0xb9500d01U;
constexpr std::uint32_t kS4 = 0x3638ef1aU;
constexpr std::uint32_t kS5 = 0xb2d72f34U;
constexpr std::uint32_t kS6 = 0x2f2ec9d2U;

// Retired original instructions per block of the listing, delay slots included.
constexpr std::uint32_t kTinyReturn = 17U;  // |x| < 2^-27 with int(x) == 0: the tiny test and the return
constexpr std::uint32_t kLargeHead = 14U;   // |x| >= 2^-27: up to z, which sits in the taken bnel's delay slot
constexpr std::uint32_t kTinyHead = 16U;    // |x| < 2^-27 with int(x) != 0 (unreachable): through z
constexpr std::uint32_t kPolynomial = 25U;  // the Horner chain for r, and the iy test with its delay slot
constexpr std::uint32_t kTailNoY = 10U;     // iy == 0 tail and the return
constexpr std::uint32_t kTailWithY = 15U;   // iy != 0 tail and the return

template <class Context>
n3::Run sine_kernel(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);
    const std::uint32_t y = call.cpu().read_fpr_bits(13U);
    const bool use_y = call.arg64(0U) != 0U;  // the listing's bne compares the whole register

    const bool tiny = (x & 0x7fffffffU) <= kTinyLimit;
    if (tiny && fpu::to_int(x) == 0) {
        call.cpu().write_fpr_bits(0U, x);
        return call.finish(kTinyReturn);
    }
    const std::uint32_t head = tiny ? kTinyHead : kLargeHead;

    const std::uint32_t z = fpu::mul(x, x);
    const std::uint32_t v = fpu::mul(z, x);
    // Horner in z, each operation with the listing's operand order.
    std::uint32_t t = fpu::add(fpu::mul(z, kS6), kS5);
    t = fpu::add(fpu::mul(z, t), kS4);
    t = fpu::add(fpu::mul(z, t), kS3);
    const std::uint32_t r = fpu::add(fpu::mul(z, t), kS2);

    if (!use_y) {
        const std::uint32_t inner = fpu::add(fpu::mul(z, r), kS1);
        call.cpu().write_fpr_bits(0U, fpu::add(x, fpu::mul(v, inner)));
        return call.finish(head + kPolynomial + kTailNoY);
    }
    const std::uint32_t vr = fpu::mul(v, r);
    const std::uint32_t vs1 = fpu::mul(v, kS1);
    std::uint32_t h = fpu::sub(fpu::mul(y, kHalf), vr);
    h = fpu::sub(fpu::mul(z, h), y);
    h = fpu::sub(h, vs1);
    call.cpu().write_fpr_bits(0U, fpu::sub(x, h));
    return call.finish(head + kPolynomial + kTailWithY);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0011cf20, 260U, 0x35fc8ae07ae05635ULL, "SineKernel", 56U, sine_kernel, EngineReturn::Float)
