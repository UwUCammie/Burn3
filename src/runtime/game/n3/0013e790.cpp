// 0x0013e790 (a0 = lane profile P): BestLaneGap. Finds the most open bin of the profile between its two ends and
// returns it in v0. Bin 128 is returned when no score is below the least score (the global at $gp-30664).
//
// The window: low = clamp(5 * -centre) and high = clamp(5 * (gap end - centre)), where centre is the float at P+1100
// and gap end the float at P+1092. cvt.w.s truncates, +128 biases the value into a bin, and the clamp sets a negative
// bin to 0 and a bin of 256 or more to 255. The loop scores bins low+3 .. high-4 in order (none runs when high - low is
// 6 or less). A bin scores
//   openness * (tent(bin / 128) * tent(2 * (bin - low) / (high - low)))
// where openness = binWeight * (sum of the distance bytes at bin-3, bin-1, bin, bin+1, bin+3) * distanceUnit. The
// distance bytes are the profile's u8[256] at P+320; binWeight and distanceUnit are the floats at $gp-30832 and
// $gp-30680. tent(x) is x while x <= 1.0 and 2.0 - x above it (NaN folds too, since the game's compare fails on NaN).
// A score that is not below the best so far takes over, so the last best bin wins. Nothing is stored.
//
// Count: the instructions the listing retires on the path taken. The prefix (45) always runs. A clamp that moves a
// bin adds its instructions. Each pass retires its loop test and body (40), each tent fold (1) and each new best (2)
// add theirs, and the test that leaves the loop with the exit (7) ends the count. P passes give 52 + 40 P + extras.

#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// LaneProfileLayout (track.cpp).
constexpr GuestAddress kGapEnd = 1092U;            // float: the far end of the gap, offset from the centre
constexpr GuestAddress kCentre = 1100U;            // float: the centre of the profile
constexpr GuestAddress kDistances = 320U;          // u8[256]: the distance of each bin
constexpr std::uint32_t kDistanceBytes = 256U;

// Globals, read off $gp.
constexpr GuestAddress kBinWeight = static_cast<GuestAddress>(-30832);
constexpr GuestAddress kDistanceUnit = static_cast<GuestAddress>(-30680);
constexpr GuestAddress kLeastScore = static_cast<GuestAddress>(-30664);

// COP1 constants the listing builds with lui and mtc1.
constexpr std::uint32_t kFiveBits = 0x40a00000U;       // 5.0: bins per unit of offset
constexpr std::uint32_t kOneBits = 0x3f800000U;        // 1.0: the top of a tent
constexpr std::uint32_t kTwoBits = 0x40000000U;        // 2.0: a tent folds about 1.0
constexpr std::uint32_t kBinScaleBits = 0x43000000U;   // 128.0: the centre bin

constexpr std::uint32_t kCentreBin = 128U;             // offset 0 is bin 128, and the bin returned when nothing scores
constexpr std::int32_t kBinLimit = 256;                // bins run 0..255
constexpr std::int32_t kReach = 3;                     // the taps reach 3 bins each way
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing (delay slots included).
constexpr std::uint32_t kLowBinCost = 11U;             // 0x13e790..0x13e7b8: 5 * -centre, truncated, biased, bltz
constexpr std::uint32_t kHighBinCost = 12U;            // 0x13e7cc..0x13e7f8: 5 * (gap end - centre), truncated, biased, bltz
constexpr std::uint32_t kClampTestCost = 3U;           // 0x13e7bc and 0x13e7fc: slti, bne, nop (once per end)
constexpr std::uint32_t kSetupCost = 16U;              // 0x13e80c..0x13e848: span, least score, constants, jump to the test
constexpr std::uint32_t kFloorCost = 2U;               // 0x13e904, 0x13e90c: a negative bin is set to 0
constexpr std::uint32_t kCapCost = 1U;                 // 0x13e7c8, 0x13e808: a bin of 256 or more is set to 255
constexpr std::uint32_t kTestCost = 3U;                // 0x13e8f0: slt, bne, nop
constexpr std::uint32_t kBodyCost = 37U;               // 0x13e84c..0x13e8ec, without the folds and the new best
constexpr std::uint32_t kFoldCost = 1U;                // 0x13e8bc, 0x13e8cc: the 2.0 - x of a tent
constexpr std::uint32_t kUpdateCost = 2U;              // 0x13e8e4: mov.s and the bin copy
constexpr std::uint32_t kLeaveCost = 4U;               // 0x13e8fc (beq, nop) and 0x13e914 (jr, nop)

// The guard the listing's loads need: an aligned word of backed guest memory. Anything else declines the call, and the
// original runs it.
template <class Memory>
bool word_ok(const Memory& m, const GuestAddress address) {
    return (address & 3U) == 0U && m.contains(address, 4U);
}

// The bin of a scaled offset (5 * offset, a float): truncated, biased by 128, clamped to 0..255. The clamp path's
// instructions are added to `retired`.
std::int32_t clamp_bin(const std::uint32_t scaled, std::uint32_t& retired) {
    const auto bin = static_cast<std::int32_t>(static_cast<std::uint32_t>(fpu::to_int(scaled)) + kCentreBin);
    if (bin < 0) {
        retired += kFloorCost;
        return 0;
    }
    if (bin >= kBinLimit) {
        retired += kCapCost;
        return kBinLimit - 1;
    }
    return bin;
}

// A tent: x while x <= 1.0, else 2.0 - x. The game's c.ole.s fails on NaN, so NaN folds as well. A fold is the sub.s,
// one more instruction in the pass.
std::uint32_t tent(const std::uint32_t x, std::uint32_t& pass_cost) {
    if (fpu::le(x, kOneBits)) {
        return x;
    }
    pass_cost += kFoldCost;
    return fpu::sub(kTwoBits, x);
}

template <class Context>
n3::Run best_lane_gap_bin(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress profile = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    if (!word_ok(m, profile + kGapEnd) || !word_ok(m, profile + kCentre) ||
        !m.contains(profile + kDistances, kDistanceBytes) || !word_ok(m, gp + kBinWeight) ||
        !word_ok(m, gp + kDistanceUnit) || !word_ok(m, gp + kLeastScore)) {
        return call.decline();
    }
    const std::uint32_t centre = m.read_u32_le(profile + kCentre);
    const std::uint32_t gap_end = m.read_u32_le(profile + kGapEnd);
    const std::uint32_t bin_weight = m.read_u32_le(gp + kBinWeight);
    const std::uint32_t distance_unit = m.read_u32_le(gp + kDistanceUnit);
    std::uint32_t best = m.read_u32_le(gp + kLeastScore);

    std::uint32_t retired = kLowBinCost + kClampTestCost + kHighBinCost + kClampTestCost + kSetupCost;
    const std::int32_t low = clamp_bin(fpu::mul(kFiveBits, fpu::neg(centre)), retired);
    const std::int32_t high = clamp_bin(fpu::mul(kFiveBits, fpu::sub(gap_end, centre)), retired);
    const std::uint32_t span = fpu::from_int(high - low);
    std::int32_t best_bin = static_cast<std::int32_t>(kCentreBin);

    for (std::int32_t bin = low + kReach; bin < high - kReach; ++bin) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress cell = profile + kDistances + static_cast<GuestAddress>(bin);
        const std::uint32_t sum = std::uint32_t{m.read_u8(cell - 3U)} + std::uint32_t{m.read_u8(cell - 1U)} +
                                  std::uint32_t{m.read_u8(cell)} + std::uint32_t{m.read_u8(cell + 1U)} +
                                  std::uint32_t{m.read_u8(cell + 3U)};
        const std::uint32_t openness =
            fpu::mul(fpu::mul(bin_weight, fpu::from_int(static_cast<std::int32_t>(sum))), distance_unit);

        std::uint32_t pass = kTestCost + kBodyCost;
        const std::uint32_t centred = tent(fpu::div(fpu::from_int(bin), kBinScaleBits), pass);
        const std::uint32_t inside = tent(fpu::div(fpu::from_int(2 * (bin - low)), span), pass);
        const std::uint32_t score = fpu::mul(openness, fpu::mul(centred, inside));
        if (!fpu::lt(score, best)) {
            best = score;
            best_bin = bin;
            pass += kUpdateCost;
        }
        retired += pass;
    }
    retired += kTestCost + kLeaveCost;
    call.return_value(static_cast<std::uint32_t>(best_bin));
    return call.finish(retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(0013e790, 396U, 0xd143f438dce3df91ULL, "BestLaneGap", kCeiling, best_lane_gap_bin, EngineReturn::Integer)
