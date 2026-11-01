// 0x00299590 (a0, a1 = two unsigned 32-bit numbers; a2 = high word out, a3 = low word out): UnsignedMulWide.
// A leaf (no calls, no memory reads) that multiplies a0 and a1 by shift and add, one bit of a0 per pass. t2 takes
// the bit that leaves a1, t4 is the low word of the running sum with its carry in bit 31, t3 the high word. It
// stores u32[a2] = t3 >> 1, then u32[a3] = t4 | (t3 << 31), in that order, and returns nothing.
// The R5900 sign-extends 32-bit results into the upper halves of registers, and some tests read those halves: the
// loop head reads all 64 bits of a0 (only on the first pass, after that a0 is a zero-extended word), and the bit-31
// tests of a1 are against lui 0x8000, which is 0xFFFFFFFF80000000, so they read all 64 bits of a1. Those are modelled
// on the full registers; the other values use their low words. The loop runs at most 31 passes, so the count is
// bounded (at most 717 retired) and call_cost never declines.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// lui $v1, 0x8000 is sign-extended to 0xFFFFFFFF80000000 on the R5900.
constexpr std::uint64_t kSignMask64 = 0xFFFFFFFF80000000ULL;
constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kLowSevenBits = 0x7fffffffU;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 6U;         // 0x00299590..0x002995a4: clears t2-t4, sets t1 and t0
constexpr std::uint32_t kHeadCost = 3U;          // 0x002995e0..0x002995e8: a0 & ~1 test, a1 & t1 in its delay slot
constexpr std::uint32_t kHighTestCost = 2U;      // 0x002995a8..0x002995ac: a1 & t1 branch, t2 <<= 1 in its delay slot
constexpr std::uint32_t kHighBitCost = 4U;       // 0x002995b0..0x002995bc: a1 keeps its low 31 bits, t2 |= 1
constexpr std::uint32_t kLowBitCost = 3U;        // 0x0029964c..0x00299654: a0 & 1 test and its nop
constexpr std::uint32_t kLowBitAddCost = 2U;     // 0x00299658..0x0029965c: jump to the carry test, t4 += a1 in its slot
constexpr std::uint32_t kCarryTestCost = 3U;     // 0x002995c0..0x002995c8: t4 bit 31 test, t3 += t2 in its delay slot
constexpr std::uint32_t kCarryCost = 3U;         // 0x002995cc and 0x00299628 blocks: t4 &= 0x7fffffff, t3 += 1
constexpr std::uint32_t kStepCost = 2U;          // 0x002995d8..0x002995dc: a0 >>= 1, a1 <<= 1
constexpr std::uint32_t kTailHighTestCost = 4U;  // 0x002995ec..0x002995f8: a1 bit 31 test, t2 <<= 1 in its slot
constexpr std::uint32_t kTailHighBitCost = 3U;   // 0x002995fc..0x00299604: a1 keeps its low 31 bits, t2 |= 1
constexpr std::uint32_t kTailLowBitCost = 3U;    // 0x00299608..0x00299610: a0 & 1 test, v1 = t3 >> 1 in its delay slot
constexpr std::uint32_t kTailAddCost = 5U;       // 0x00299614..0x00299624: t4 += a1, bit 31 test, t3 += t2 in its slot
constexpr std::uint32_t kTailShiftCost = 1U;     // 0x00299634: v1 = t3 >> 1
constexpr std::uint32_t kStoreCost = 5U;         // 0x00299638..0x00299648: a0 = t3 << 31, the two stores
constexpr std::uint32_t kReturnCost = 2U;        // 0x00299660..0x00299664: jr ra and its nop

struct WideProduct {
    std::uint32_t retired;  // instructions the original retires for these operands
    std::uint32_t high;     // stored at a2
    std::uint32_t low;      // stored at a3
};

// sll (like addu, addiu and lui) leaves its 32-bit result sign-extended in the 64-bit register.
std::uint64_t sign_extended(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

WideProduct unsigned_mul_wide_product(std::uint64_t a0, std::uint64_t a1) {
    std::uint32_t t2 = 0U;
    std::uint32_t t3 = 0U;
    std::uint32_t t4 = 0U;
    std::uint32_t retired = kEntryCost;
    // The loop head (0x002995e0): while a0 & ~1 != 0. Its delay slot always computes a1 & 0xFFFFFFFF80000000.
    for (;;) {
        retired += kHeadCost;
        const std::uint64_t a1_top = a1 & kSignMask64;
        if ((a0 & ~std::uint64_t{1}) == 0U) {
            break;
        }
        // 0x002995a8: the delay slot shifts t2 whether or not the branch is taken.
        retired += kHighTestCost;
        t2 <<= 1U;
        if (a1_top != 0U) {
            retired += kHighBitCost;
            a1 = (a1 << 33U) >> 33U;
            t2 |= 1U;
        }
        // 0x0029964c: a0 & 1 selects the add.
        retired += kLowBitCost;
        if ((a0 & 1U) != 0U) {
            retired += kLowBitAddCost;
            t4 += static_cast<std::uint32_t>(a1);
            // 0x002995c0: t3 += t2 is the delay slot of the carry test, so it runs before the carry.
            retired += kCarryTestCost;
            const bool carry = (t4 & kSignBit) != 0U;
            t3 += t2;
            if (carry) {
                retired += kCarryCost;
                t4 &= kLowSevenBits;
                t3 += 1U;
            }
        }
        // 0x002995d8: a0 >>= 1 and a1 <<= 1, both 32-bit operations.
        retired += kStepCost;
        a0 = static_cast<std::uint32_t>(a0) >> 1U;
        a1 = sign_extended(static_cast<std::uint32_t>(a1) << 1U);
    }
    // The tail (0x002995ec): a0 is 0 or 1 here, and the a1 test reads all 64 bits.
    retired += kTailHighTestCost;
    t2 <<= 1U;
    if ((a1 & kSignMask64) != 0U) {
        retired += kTailHighBitCost;
        a1 = (a1 << 33U) >> 33U;
        t2 |= 1U;
    }
    // 0x00299608: the last bit of a0.
    retired += kTailLowBitCost;
    if ((a0 & 1U) != 0U) {
        retired += kTailAddCost;
        t4 += static_cast<std::uint32_t>(a1);
        const bool carry = (t4 & kSignBit) != 0U;
        t3 += t2;
        if (carry) {
            retired += kCarryCost;
            t4 &= kLowSevenBits;
            t3 += 1U;
        }
        retired += kTailShiftCost;
    }
    // 0x00299638: the two stores are made by unsigned_mul_wide; this is their count and the return.
    retired += kStoreCost + kReturnCost;
    return WideProduct{retired, t3 >> 1U, t4 | (t3 << 31U)};
}

template <class Context>
std::uint32_t unsigned_mul_wide_cost(const Context& context) {
    return unsigned_mul_wide_product(context.cpu.read_gpr_u64(4U), context.cpu.read_gpr_u64(5U)).retired;
}

template <class Context>
n3::Run unsigned_mul_wide(Context& context) {
    auto call = n3::guest_call(context);
    const WideProduct product = unsigned_mul_wide_product(call.arg64(0U), call.arg64(1U));
    call.memory().write_u32_le(call.arg(2U), product.high);
    call.memory().write_u32_le(call.arg(3U), product.low);
    return call.finish(product.retired);
}

} // namespace

extern constinit const EngineFunction n3_00299590{0x00299590U, 216U, 0x17545995ca57f406ULL, "UnsignedMulWide", 800U,
                                                  n3::live<&unsigned_mul_wide<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &unsigned_mul_wide_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00299590, unsigned_mul_wide, unsigned_mul_wide_cost)
