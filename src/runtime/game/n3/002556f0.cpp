// 0x002556f0 (a0 = state record O, a1 = range record A, f12 = level, f13 = rate): UpdateBlockCoverage. O holds the
// enable word (+0; zero leaves the record alone), the element count N (+20), a pointer T to one float threshold per
// block of 60 elements (+28), the intensity OUT (+32, written), the smoothed value S (+36, read and written) and the
// covered-block mask (+40, written). A holds the range floats low (+320) and high (+324).
//
// A block is covered unless its threshold is <= level - S. Bit i of the mask is block i (blocks from 32 on have no
// bit), and fraction = covered elements / N, where a covered block counts min(60, elements left). Below 0.25 the
// intensity is 1.0 and the target is high. Otherwise, with ramp = k * (fraction - 0.25) and k the float at
// $gp - 29428, the intensity is 1.0 - 0.75 * ramp and the target is low + ramp * (high - low). S then eases by
// 0.5 * rate * (high - low) toward the target and stops there. Returns nothing (v0 and f0 are scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The state record O (a0).
constexpr GuestAddress kEnable = 0U;
constexpr GuestAddress kCount = 20U;
constexpr GuestAddress kThresholds = 28U;
constexpr GuestAddress kOut = 32U;
constexpr GuestAddress kSmoothed = 36U;
constexpr GuestAddress kMask = 40U;

// The range record A (a1).
constexpr GuestAddress kRangeLow = 320U;
constexpr GuestAddress kRangeHigh = 324U;

constexpr GuestAddress kSlopeBelowGp = 29428U;      // the float k at $gp - 29428
constexpr std::uint32_t kBlockElements = 60U;
constexpr GuestAddress kThresholdBytes = 4U;

constexpr std::uint32_t kOneBits = 0x3f800000U;          // 1.0f
constexpr std::uint32_t kHalfBits = 0x3f000000U;         // 0.5f
constexpr std::uint32_t kQuarterBits = 0x3e800000U;      // 0.25f
constexpr std::uint32_t kThreeQuarterBits = 0x3f400000U; // 0.75f

// Retired counts of the listing's blocks, delay slots included (0x002556f0-0x00255894).
constexpr std::uint32_t kEnableTestCost = 3U;   // 0x002556f0-0x002556f8
constexpr std::uint32_t kSetupCost = 9U;        // 0x002556fc-0x0025571c: the mask reset, N, T and the limit
constexpr std::uint32_t kTestCost = 4U;         // 0x0025573c-0x00255748: before every block, and once more at the end
constexpr std::uint32_t kUncoveredCost = 7U;    // 0x00255720-0x00255738: the threshold is <= the limit
constexpr std::uint32_t kCoveredCost = 19U;     // covered with more than 60 elements left (0x00255850-0x00255878 path)
constexpr std::uint32_t kLastCoveredCost = 17U; // covered with 60 or fewer elements left (0x00255850-0x00255874 path)
constexpr std::uint32_t kFractionCost = 13U;    // 0x0025574c-0x0025577c: covered / N and the quarter test
constexpr std::uint32_t kLowCost = 4U;          // 0x00255880-0x0025588c: fraction below a quarter
constexpr std::uint32_t kRampCost = 17U;        // 0x00255780-0x002557c0: fraction of a quarter or more
constexpr std::uint32_t kRiseTestCost = 4U;     // 0x002557c4-0x002557d0: S <= target
constexpr std::uint32_t kEaseCost = 13U;        // 0x00255810-0x00255840 (rising), 0x002557d4-0x00255804 (falling)
constexpr std::uint32_t kRiseClampCost = 3U;    // 0x00255844-0x0025584c: the target stored, then the jump to the return
constexpr std::uint32_t kFallClampCost = 4U;    // 0x00255808-0x0025580c and 0x00255848-0x0025584c
constexpr std::uint32_t kReturnCost = 2U;       // 0x00255890-0x00255894: jr $ra and its delay slot
constexpr std::uint32_t kDisabledCost = kEnableTestCost + kReturnCost;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run update_block_coverage(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    if (m.read_u32_le(record + kEnable) == 0U) {
        return call.finish(kDisabledCost);
    }
    const GuestAddress range = call.arg(1U);
    const std::uint32_t level = cpu.read_fpr_bits(12U);
    const std::uint32_t rate = cpu.read_fpr_bits(13U);
    const std::uint32_t smoothed = m.read_u32_le(record + kSmoothed);
    const std::uint32_t limit = fpu::sub(level, smoothed);
    const std::uint32_t count = m.read_u32_le(record + kCount);
    const std::uint32_t low = m.read_u32_le(range + kRangeLow);
    const std::uint32_t high = m.read_u32_le(range + kRangeHigh);
    GuestAddress threshold = m.read_u32_le(record + kThresholds);

    // One pass per block. The covered-block mask is built in a local and stored after the loop: the original's
    // intermediate stores to +40 are read back by nothing (N and the thresholds are other words).
    std::uint32_t retired = kEnableTestCost + kSetupCost;
    std::uint32_t covered = 0U;
    std::uint32_t mask = 0U;
    std::uint32_t bit = 1U;
    for (std::uint32_t first = 0U;; first += kBlockElements) {
        retired += kTestCost;
        if (first >= count) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (fpu::le(m.read_u32_le(threshold), limit)) {
            retired += kUncoveredCost;
        } else {
            mask |= bit;
            const std::uint32_t left = count - first;
            if (left > kBlockElements) {
                covered += kBlockElements;
                retired += kCoveredCost;
            } else {
                covered += left;
                retired += kLastCoveredCost;
            }
        }
        threshold += kThresholdBytes;
        bit <<= 1U;
    }

    // The intensity and the target the smoothed value moves toward.
    retired += kFractionCost;
    const std::uint32_t fraction = fpu::div(fpu::from_int(static_cast<std::int32_t>(covered)),
                                            fpu::from_int(static_cast<std::int32_t>(count)));
    std::uint32_t out = kOneBits;
    std::uint32_t target = high;
    if (fpu::lt(fraction, kQuarterBits)) {
        retired += kLowCost;
    } else {
        retired += kRampCost;
        const std::uint32_t slope = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) - kSlopeBelowGp);
        const std::uint32_t ramp = fpu::mul(slope, fpu::sub(fraction, kQuarterBits));
        out = fpu::msub(fpu::add(0U, kOneBits), kThreeQuarterBits, ramp);
        target = fpu::madd(fpu::add(0U, low), ramp, fpu::sub(high, low));
    }
    m.write_u32_le(record + kMask, mask);
    m.write_u32_le(record + kOut, out);

    // S eases toward the target by 0.5 * rate * (high - low): up when S <= target (clamped to the target when it
    // overshoots), down otherwise (clamped to the target when it undershoots).
    retired += kRiseTestCost;
    const std::uint32_t half = fpu::mul(kHalfBits, rate);
    const std::uint32_t step = fpu::sub(high, low);
    if (fpu::le(smoothed, target)) {
        const std::uint32_t next = fpu::madd(fpu::add(0U, smoothed), half, step);
        if (fpu::le(next, target)) {
            retired += kEaseCost + kReturnCost;
            m.write_u32_le(record + kSmoothed, next);
        } else {
            retired += kEaseCost + kRiseClampCost + kReturnCost;
            m.write_u32_le(record + kSmoothed, target);
        }
    } else {
        const std::uint32_t next = fpu::msub(fpu::add(0U, smoothed), half, step);
        if (fpu::lt(next, target)) {
            retired += kEaseCost + kFallClampCost + kReturnCost;
            m.write_u32_le(record + kSmoothed, target);
        } else {
            retired += kEaseCost + kReturnCost;
            m.write_u32_le(record + kSmoothed, next);
        }
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002556f0, 424U, 0x9ece6cb347429ff8ULL, "UpdateBlockCoverage", kCeiling, update_block_coverage, EngineReturn::Nothing)
