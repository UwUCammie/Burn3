// 0x00140a30 (a0 = lane profile P): ChooseLaneTarget. Chooses the steering target point of an AI driver from the
// profile's per-bin obstacle data and stores it, with the direction and time to reach it, the speed to hold and the
// clearances it found. Returns nothing.
//
// The profile (P): u8 tags (signed) at +64, u8 distances at +320, s16 levels at +576, the car's width / high bin /
// low bin bytes at +1088..+1090, the target quad (+1120), its direction quad (+1136), time (+1156), speed (+1160),
// the danger float (+1164), the clearances and blocked sums (+1168 high, +1172 low, +1176 high blocked, +1180 low
// blocked), the racer (+1184), the driver (+1188), and the bytes side (+1196), low target bin (+1197), high target
// bin (+1198). A distance byte times the unit U (8/255) is a distance, a level times LU a height.
//
// The car's bins low..high give the nearest distance and lowest level. When the nearest is under 4.0 the car is
// close to something: the profile is scanned up from the car's low bin and down from its high bin for the nearest
// escape (a bin where the distance opens past a margin that tightens as the scan goes), each escape is centred on
// its stretch of open bins, the clearance (mean of 8 - distance over a car width) and the blocked sum (8 - distance
// of every bin under 2.0) of each side are stored, and a squeeze value is taken from the tagged bins beside the
// car. The side is chosen from the clearances, the driver's side request (2 high, 4 low) and whether an escape bin
// sits at the car's own edge; the target is the car's position plus the forward direction scaled by the lead (5.0
// or more, from the lowest level) plus the side direction scaled by the bin offset. The speed to hold comes from
// the lowest level across the car's middle half (drops of under 10, 20, 30 get their own speeds), else the cruise
// speed, or the squeezed speeds. Otherwise (open road) BestLaneGap (0x0013e790) picks a bin, the target is the car's
// position plus the forward direction scaled by (4 times) the speed plus the side offset of the gap, with a swerve
// straight ahead (10 times the speed) when something is within 1.0 under the car and the gap lies below it. Both
// paths end by storing the direction from the car to the target, the time (distance over speed, or the distance
// when the speed is 1.0 or less) and the normalised direction.
//
// The original's loops (min scan, escape scans, clearance, blocked and squeeze sums, middle level) are counted per
// pass from the listing. The sums the original re-stores in P every pass are kept in locals and stored once: no
// load in the function reads them back from another address. The high loop of the open-road path (0x141660) can
// never run (its counter starts at the car's high bin and needs to be 4 past it), the width tests `bltz` (0x140e20
// and others) never see a negative byte, so those paths are left out. VU0 operations are replayed in the original's
// order so VF1..VF3, ACC, Q and the flags end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <cstdint>

N3_CALLEE(0013e790)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// Profile layout.
constexpr GuestAddress kTags = 64U;
constexpr GuestAddress kDistances = 320U;
constexpr GuestAddress kLevels = 576U;
constexpr GuestAddress kCarWidth = 1088U;
constexpr GuestAddress kCarHigh = 1089U;
constexpr GuestAddress kCarLow = 1090U;
constexpr GuestAddress kTarget = 1120U;
constexpr GuestAddress kDirection = 1136U;
constexpr GuestAddress kTime = 1156U;
constexpr GuestAddress kSpeed = 1160U;
constexpr GuestAddress kDanger = 1164U;
constexpr GuestAddress kHighClearance = 1168U;
constexpr GuestAddress kLowClearance = 1172U;
constexpr GuestAddress kHighBlocked = 1176U;
constexpr GuestAddress kLowBlocked = 1180U;
constexpr GuestAddress kRacer = 1184U;
constexpr GuestAddress kDriver = 1188U;
constexpr GuestAddress kSide = 1196U;
constexpr GuestAddress kLowBin = 1197U;
constexpr GuestAddress kHighBin = 1198U;

// Driver, car, matrix and racer fields.
constexpr GuestAddress kForward = 6848U;
constexpr GuestAddress kSideDirection = 6864U;
constexpr GuestAddress kSideRequest = 7140U;
constexpr GuestAddress kSideRequestState = 7144U;
constexpr GuestAddress kSideRequestHeld = 7148U;
constexpr GuestAddress kCar = 9264U;
constexpr GuestAddress kCarMatrix = 516U;
constexpr GuestAddress kCarVelocity = 176U;
constexpr GuestAddress kMatrixAt = 32U;
constexpr GuestAddress kMatrixPosition = 48U;
constexpr GuestAddress kRacerCruise = 464U;
constexpr GuestAddress kRacerHolding = 504U;
constexpr GuestAddress kRacerOverride = 508U;

// Globals: gp-relative floats and the absolute speed table.
constexpr GuestAddress kBinWidth = static_cast<GuestAddress>(-30832);
constexpr GuestAddress kDistanceUnit = static_cast<GuestAddress>(-30680);
constexpr GuestAddress kLevelUnit = static_cast<GuestAddress>(-30676);
constexpr GuestAddress kSqueezeFree = static_cast<GuestAddress>(-30668);
constexpr GuestAddress kSqueezeLight = static_cast<GuestAddress>(-31992);
constexpr GuestAddress kHoldBelow10 = 0x01e7553cU;
constexpr GuestAddress kHoldBelow20 = 0x01e75540U;
constexpr GuestAddress kHoldBelow30 = 0x01e75544U;
constexpr GuestAddress kSqueezedSpeed = 0x01e75548U;
constexpr GuestAddress kLightSqueezeSpeed = 0x01e7554cU;
constexpr GuestAddress kLeadLevelScale = 0x01e75550U;

// Float constants as raw bits.
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kFour = 0x40800000U;
constexpr std::uint32_t kFive = 0x40a00000U;
constexpr std::uint32_t kEight = 0x41000000U;
constexpr std::uint32_t kTen = 0x41200000U;
constexpr std::uint32_t kTwenty = 0x41a00000U;
constexpr std::uint32_t kThirty = 0x41f00000U;
constexpr std::uint32_t kFifty = 0x42480000U;
constexpr std::uint32_t kLevelCeiling = 0x43fa0000U;  // 500.0: the start of the middle-level minimum
constexpr std::uint32_t kShortLengthBits = 0x2f800000U;  // 2^-32

constexpr std::uint8_t kMaskX = 8U;
constexpr std::int32_t kBinCount = 256;
constexpr std::int32_t kSkippedTag = 6;
constexpr std::uint32_t kCeiling = 30000U;
constexpr std::uint32_t kFrameBytes = 640U;  // addiu sp,sp,-640

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeldCheck = 11U;       // 0x140a30..0x140a58
constexpr std::uint32_t kStateCheck = 5U;       // 0x140a5c..0x140a6c
constexpr std::uint32_t kRequestRead = 1U;      // 0x140a70
constexpr std::uint32_t kMinHead = 24U;         // 0x140a74..0x140ad0
constexpr std::uint32_t kMinPass = 16U;         // 0x140ad4..0x140b10
constexpr std::uint32_t kMinTest = 3U;          // 0x140b14..0x140b1c
constexpr std::uint32_t kRouteTest = 6U;        // 0x140b20..0x140b34
constexpr std::uint32_t kMarginSetup = 7U + 3U; // 0x140b38..0x140b50, 0x140b58..0x140b60
constexpr std::uint32_t kMarginClamp = 1U;      // 0x140b54, 0x140ca0
constexpr std::uint32_t kScanTest = 3U;         // 0x140c08, 0x140c4c, 0x140d90, 0x140d54 (2)
constexpr std::uint32_t kLowerTest = 2U;        // 0x140d54..0x140d58
constexpr std::uint32_t kScanHead = 5U;         // 0x140b64..0x140b74 and 0x140cb0..0x140cc0
constexpr std::uint32_t kScanSample = 9U;       // the distance sample and its compare with the margin
constexpr std::uint32_t kScanPastStop = 2U;     // 0x140b9c, 0x140ce8
constexpr std::uint32_t kScanNearStop = 3U;     // 0x140bc8, 0x140d14: compare with the nearest
constexpr std::uint32_t kScanNearStep = 2U;     // 0x140bd4, 0x140d20
constexpr std::uint32_t kScanFar = 8U;          // 0x140bdc, 0x140d28
constexpr std::uint32_t kScanFarUpdate = 1U;    // 0x140bfc, 0x140d48
constexpr std::uint32_t kScanFarStep = 2U;      // 0x140c00, 0x140d4c
constexpr std::uint32_t kUpperDone = 4U;        // 0x140c14..0x140c20
constexpr std::uint32_t kLowerStart = 3U;       // 0x140d5c..0x140d64
constexpr std::uint32_t kRefineSample = 9U;     // 0x140c24, 0x140d68
constexpr std::uint32_t kRefineHit = 2U;        // 0x14141c, 0x141424
constexpr std::uint32_t kRefineMiss = 1U;       // 0x140c48, 0x140d8c
constexpr std::uint32_t kHalve = 3U;            // 0x140c58..0x140c60, 0x140d9c..0x140da4
constexpr std::uint32_t kHalveNegative = 2U;    // 0x140c64, 0x140da8
constexpr std::uint32_t kLowerBegin = 13U + 3U; // 0x140c6c..0x140c9c, 0x140ca4..0x140cac
constexpr std::uint32_t kHighClearStart = 13U;  // 0x140db0..0x140de0
constexpr std::uint32_t kClearTest = 4U;        // 0x140e10, 0x140f00
constexpr std::uint32_t kHighClearBody = 11U;   // 0x140de4..0x140e0c
constexpr std::uint32_t kLowClearBody = 10U;    // 0x140ed8..0x140efc
constexpr std::uint32_t kWidthConvert = 2U + 3U; // 0x140e20..0x140e30, 0x140f10..0x140f20
constexpr std::uint32_t kClearFinish = 12U;     // 0x140e50..0x140e7c, 0x140f40..0x140f6c
constexpr std::uint32_t kLowClearStart = 9U;    // 0x140eb4..0x140ed4
constexpr std::uint32_t kBlockedTest = 3U;      // 0x140ea8, 0x140f98
constexpr std::uint32_t kBlockedBody = 9U;      // 0x140e80..0x140ea0, 0x140f70..0x140f90
constexpr std::uint32_t kBlockedNext = 1U;      // 0x140ea4, 0x140f94
constexpr std::uint32_t kBlockedHit = 5U;       // 0x14142c.., 0x141440..
constexpr std::uint32_t kSqueezeStart = 14U;    // 0x140fa4..0x140fd8
constexpr std::uint32_t kSqueezeTest = 3U;      // 0x141020..0x141028
constexpr std::uint32_t kSqueezeBody = 7U;      // 0x140fdc..0x140ff4
constexpr std::uint32_t kSqueezeNext = 1U;      // 0x14101c
constexpr std::uint32_t kSqueezeBound = 3U;     // 0x140ff8..0x141000
constexpr std::uint32_t kSqueezeTerm = 6U;      // 0x141004..0x141018
constexpr std::uint32_t kSqueezeFinish = 4U + 3U;  // 0x14102c..0x141038, 0x14103c..0x141044
constexpr std::uint32_t kSqueezeDivide = 9U;    // 0x141064..0x141084
constexpr std::uint32_t kPickHigh = 16U;        // 0x1410ac..0x1410e8
constexpr std::uint32_t kPickLow = 15U;         // 0x1410ec..0x141124
constexpr std::uint32_t kLeadTarget = 27U;      // 0x141128..0x141190
constexpr std::uint32_t kLeadNegative = 2U;     // 0x141194
constexpr std::uint32_t kMiddleStart = 9U;      // 0x14119c..0x1411bc
constexpr std::uint32_t kMiddleTest = 3U;       // 0x141208..0x141210
constexpr std::uint32_t kMiddleDanger = 4U;     // 0x1411c0..0x1411cc
constexpr std::uint32_t kMiddleTag = 4U;        // 0x1411d0..0x1411dc
constexpr std::uint32_t kMiddleSample = 9U;     // 0x1411e0..0x141200
constexpr std::uint32_t kMiddleNext = 1U;       // 0x141204
constexpr std::uint32_t kAimHead = 21U;         // 0x141298..0x1412e8
constexpr std::uint32_t kAimDegenerate = 6U;    // 0x141844..0x141858
constexpr std::uint32_t kAimSpeed = 11U;        // 0x1412ec..0x141314
constexpr std::uint32_t kAimDivide = 29U;       // 0x141318..0x141388
constexpr std::uint32_t kAimDirect = 17U;       // 0x14138c..0x1413cc
constexpr std::uint32_t kAimDirection = 19U;    // 0x1413d0..0x141418
constexpr std::uint32_t kReturn = 7U;           // 0x14185c..0x141874
constexpr std::uint32_t kOpenHead = 19U;        // 0x1414fc..0x141544, the callee not included
constexpr std::uint32_t kOpenHoldCheck = 3U;    // 0x141548..0x141550
constexpr std::uint32_t kOpenHolding = 13U;     // 0x141554..0x141584
constexpr std::uint32_t kOpenFree = 16U;        // 0x1417e4..0x141820
constexpr std::uint32_t kOpenJoin = 14U;        // 0x141588..0x1415bc
constexpr std::uint32_t kOpenLowTest = 5U;      // 0x141640..0x141650
constexpr std::uint32_t kOpenLowSign = 2U;      // 0x1415c0..0x1415c4
constexpr std::uint32_t kOpenLowSample = 9U;    // 0x1415c8..0x1415e8
constexpr std::uint32_t kOpenLowHit = 5U;       // 0x1415ec..0x1415fc
constexpr std::uint32_t kOpenSwerve = 15U;      // 0x141600..0x141638
constexpr std::uint32_t kOpenLowNext = 1U;      // 0x14163c
constexpr std::uint32_t kOpenHighSkipped = 3U + 5U;  // 0x141654..0x14165c and 0x1416e4..0x1416f4: the dead loop
constexpr std::uint32_t kOpenMiddleStart = 5U + 8U;  // 0x1416f8..0x141708, 0x141714..0x141730
constexpr std::uint32_t kOpenLadderTen = 3U + 4U;
constexpr std::uint32_t kOpenLadderTwenty = 3U + 6U + 4U;
constexpr std::uint32_t kOpenLadderThirty = 3U + 6U + 6U + 4U;
constexpr std::uint32_t kLadderTen = 6U + 4U;
constexpr std::uint32_t kLadderTwenty = 6U + 6U + 4U;
constexpr std::uint32_t kLadderThirty = 6U + 6U + 6U + 4U;
constexpr std::uint32_t kLadderFree = 6U + 6U + 6U + 4U + 4U;
constexpr std::uint32_t kLadderLight = 6U + 6U + 6U + 4U + 4U + 3U;
constexpr std::uint32_t kLadderSqueezed = 6U + 6U + 6U + 4U + 4U + 4U;

// MIN.S / MAX.S on raw bits, ordered as the R5900 orders them.
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = static_cast<std::int32_t>(left);
    const auto signed_right = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                         : std::min(signed_left, signed_right));
}
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = static_cast<std::int32_t>(left);
    const auto signed_right = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                         : std::max(signed_left, signed_right));
}

// A byte or halfword of the profile at a bin index (which may lie just outside 0..255: the sampling reads the
// neighbouring arrays then, as the original does).
template <class Memory>
struct Lane {
    Memory& m;
    GuestAddress p;
    std::uint32_t unit;        // distance unit U
    std::uint32_t level_unit;  // level unit LU
    std::uint32_t bin_width;   // BW
    std::int32_t width;
    std::int32_t high;
    std::int32_t low;

    [[nodiscard]] std::int32_t raw_distance(const std::int32_t bin) const {
        return static_cast<std::int32_t>(m.read_u8(p + kDistances + static_cast<GuestAddress>(bin)));
    }
    [[nodiscard]] std::uint32_t distance(const std::int32_t bin) const {
        return fpu::mul(unit, fpu::from_int(raw_distance(bin)));
    }
    [[nodiscard]] std::uint32_t level(const std::int32_t bin) const {
        const auto raw = static_cast<std::int16_t>(m.read_u16_le(p + kLevels + 2U * static_cast<GuestAddress>(bin)));
        return fpu::mul(level_unit, fpu::from_int(raw));
    }
    [[nodiscard]] std::int32_t tag(const std::int32_t bin) const {
        return static_cast<std::int8_t>(m.read_u8(p + kTags + static_cast<GuestAddress>(bin)));
    }
};

// The margin an escape scan must beat and the distance of the previous bin, both tightening as it goes.
struct Margin {
    std::uint32_t margin;
    std::uint32_t previous;
};

// The scan up from the car's low bin to the nearest escape, refined by a scan back down from there, centred and
// clamped to 1..255. The high target bin.
template <class Memory>
std::int32_t upper_target_bin(const Lane<Memory>& lane, Margin& scan, const std::uint32_t nearest,
                              std::uint32_t& retired) {
    std::int32_t edge = lane.low;
    for (;;) {
        retired += kScanTest;
        if (edge >= kBinCount) {
            break;
        }
        const bool past_car = lane.high < edge - lane.width;
        const std::uint32_t here = lane.distance(edge);
        retired += kScanHead + kScanSample;
        if (fpu::lt(here, scan.margin)) {
            if (past_car) {
                retired += kScanPastStop;
                --edge;
                break;
            }
            retired += kScanNearStop;
            if (fpu::lt(here, nearest)) {
                break;
            }
            retired += kScanNearStep;
            ++edge;
        } else {
            retired += kScanFar + kScanFarStep;
            if (!fpu::lt(here, scan.previous)) {
                scan.margin = fpu::sub(here, lane.bin_width);
                retired += kScanFarUpdate;
            }
            scan.previous = here;
            ++edge;
        }
        if (retired > kCeiling) {
            return edge;
        }
    }
    retired += kUpperDone;
    std::int32_t inner = edge;
    for (;;) {
        retired += kScanTest;
        if (!(lane.low < inner)) {
            break;
        }
        retired += kRefineSample;
        if (fpu::lt(lane.distance(inner), scan.margin)) {
            retired += kRefineHit;
            ++inner;
            break;
        }
        retired += kRefineMiss;
        --inner;
        if (retired > kCeiling) {
            return inner;
        }
    }
    retired += kHalve;
    std::int32_t sum = edge + inner;
    if (sum < 0) {
        retired += kHalveNegative;
        ++sum;
    }
    return std::clamp(sum >> 1, 1, 255);
}

// The scan down from the car's high bin, refined by a scan up from there, centred and clamped to 0..254. The low
// target bin.
template <class Memory>
std::int32_t lower_target_bin(const Lane<Memory>& lane, Margin& scan, const std::uint32_t nearest,
                              std::uint32_t& retired) {
    std::int32_t edge = lane.high;
    for (;;) {
        retired += kLowerTest;
        if (edge < 0) {
            break;
        }
        const bool past_car = edge + lane.width < lane.low;
        const std::uint32_t here = lane.distance(edge);
        retired += kScanHead + kScanSample;
        if (fpu::lt(here, scan.margin)) {
            if (past_car) {
                retired += kScanPastStop;
                ++edge;
                break;
            }
            retired += kScanNearStop;
            if (fpu::lt(here, nearest)) {
                break;
            }
            retired += kScanNearStep;
            --edge;
        } else {
            retired += kScanFar + kScanFarStep;
            if (!fpu::lt(here, scan.previous)) {
                scan.margin = fpu::sub(here, lane.bin_width);
                retired += kScanFarUpdate;
            }
            scan.previous = here;
            --edge;
        }
        if (retired > kCeiling) {
            return edge;
        }
    }
    retired += kLowerStart;
    std::int32_t inner = edge;
    for (;;) {
        retired += kScanTest;
        if (!(inner < lane.high)) {
            break;
        }
        retired += kRefineSample;
        if (fpu::lt(lane.distance(inner), scan.margin)) {
            retired += kRefineHit;
            --inner;
            break;
        }
        retired += kRefineMiss;
        ++inner;
        if (retired > kCeiling) {
            return inner;
        }
    }
    retired += kHalve;
    std::int32_t sum = edge + inner;
    if (sum < 0) {
        retired += kHalveNegative;
        ++sum;
    }
    return std::clamp(sum >> 1, 0, 254);
}

// The mean of (8.0 - distance) over a car width of bins starting at `first` and stepping by `step`. The original
// keeps the running sum in the profile and adds each term to ACC = 8.0 with MSUB.S.
template <class Memory>
std::uint32_t clearance(const Lane<Memory>& lane, const std::int32_t first, const std::int32_t step,
                        const std::uint32_t body, std::uint32_t& retired) {
    const std::uint32_t accumulator = fpu::add(0U, kEight);
    std::uint32_t sum = 0U;
    retired += kClearTest;
    for (std::int32_t k = 0; k < lane.width; ++k) {
        const std::int32_t bin = first + step * k;
        const std::uint32_t term = fpu::msub(accumulator, lane.unit, fpu::from_int(lane.raw_distance(bin)));
        sum = fpu::add(sum, term);
        retired += body + kClearTest;
        if (retired > kCeiling) {
            return sum;
        }
    }
    retired += kWidthConvert;
    return fpu::div(sum, fpu::from_int(lane.width));
}

// The sum of (8.0 - distance) over the bins from `first` to `last` (stepping by `step`) whose distance is under 2.0.
template <class Memory>
std::uint32_t blocked_sum(const Lane<Memory>& lane, const std::int32_t first, const std::int32_t last,
                          const std::int32_t step, std::uint32_t& retired) {
    std::uint32_t sum = 0U;
    for (std::int32_t bin = first; step > 0 ? bin <= last : bin >= last; bin += step) {
        const std::uint32_t here = lane.distance(bin);
        retired += kBlockedTest + kBlockedBody + kBlockedNext;
        if (fpu::lt(here, kTwo)) {
            sum = fpu::add(sum, fpu::sub(kEight, here));
            retired += kBlockedHit;
        }
        if (retired > kCeiling) {
            return sum;
        }
    }
    retired += kBlockedTest;
    return sum;
}

// The lowest level across the middle half of the car's bins (a quarter trimmed off each end), starting from 500.0;
// bins tagged 6 are left out unless the danger is under 50.0.
template <class Memory>
std::uint32_t middle_level(const Lane<Memory>& lane, const std::int32_t quarter, std::uint32_t& retired) {
    std::uint32_t result = kLevelCeiling;
    const std::int32_t end = lane.high - quarter;
    for (std::int32_t bin = lane.low + quarter;; ++bin) {
        retired += kMiddleTest;
        if (!(bin < end)) {
            break;
        }
        retired += kMiddleDanger;
        if (!fpu::lt(lane.m.read_u32_le(lane.p + kDanger), kFifty)) {
            retired += kMiddleTag;
            if (lane.tag(bin) == kSkippedTag) {
                retired += kMiddleNext;
                continue;
            }
        }
        result = min_bits(result, lane.level(bin));
        retired += kMiddleSample + kMiddleNext;
        if (retired > kCeiling) {
            return result;
        }
    }
    return result;
}

// A quarter of a span, rounded toward zero (the listing's bgez / sra pair).
std::int32_t quarter_of(const std::int32_t span) { return (span < 0 ? span + 3 : span) >> 2; }

void load_vf(guest::R5900State& cpu, const unsigned vf, const Gpr128& quad) { cpu.write_vu0_vector(vf, quad); }

// vf2 = {bits}: the scalar the multiplies broadcast (qmtc2 of a register holding a float).
Gpr128 scalar_quad(const std::uint32_t bits) { return Gpr128{{bits, 0U, 0U, 0U}}; }

// vf1 = the quad at `vector` scaled by `scale` in x, y and z (w kept): lqc2 vf1; qmtc2 vf2; vmulx.xyz vf1,vf1,vf2x.
template <class Memory>
void scaled_vector(guest::R5900State& cpu, Memory& m, const GuestAddress vector, const std::uint32_t scale) {
    load_vf(cpu, 1U, n3::load_quad_any(m, vector));
    load_vf(cpu, 2U, scalar_quad(scale));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
}

// The squared length of the quad at P+1136, left in vf1.x (vaddw.x, vmul.xyz, vadday.x, vmaddz.x).
template <class Memory>
std::uint32_t direction_length_squared(guest::R5900State& cpu, Memory& m, const GuestAddress p) {
    load_vf(cpu, 2U, n3::load_quad_any(m, p + kDirection));                          // lqc2 vf2,1136(s0)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);                                   // vmul.xyz vf2,vf2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                              // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U).words[0];
}

// The common tail (0x141298): the direction from the car to the target, the time to reach it and the normalised
// direction, then the two target bin bytes.
template <class Memory>
void aim_at_target(guest::R5900State& cpu, Memory& m, const GuestAddress p, const std::uint8_t high_bin,
                   const std::uint8_t low_bin, std::uint32_t& retired) {
    const GuestAddress driver = m.read_u32_le(p + kDriver);
    const GuestAddress car = m.read_u32_le(driver + kCar);
    const GuestAddress matrix = m.read_u32_le(car + kCarMatrix);
    retired += kAimHead;
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    const Gpr128 ones = cpu.read_vu0_vector(1U);                                    // qmfc2 a1,vf1
    load_vf(cpu, 2U, n3::load_quad_any(m, p + kTarget));                             // lqc2 vf2,1120(s0)
    load_vf(cpu, 1U, n3::load_quad_any(m, matrix + kMatrixPosition));                // lqc2 vf1,48(matrix)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);                      // vsub.xyz vf1,vf2,vf1
    n3::store_quad_any(m, p + kDirection, cpu.read_vu0_vector(1U));                  // sqc2 vf1,1136(s0)
    ee_cop2::vmul(cpu, 2U, 1U, 1U, n3::kMaskXyz);                                   // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                              // vadday.x acc,vf2,vf2y
    load_vf(cpu, 1U, ones);                                                         // qmtc2 a1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    if (fpu::lt(cpu.read_vu0_vector(1U).words[0], kShortLengthBits)) {
        retired += kAimDegenerate;  // a target on the car: aim along the car's at vector
        n3::store_quad_any(m, p + kDirection, n3::load_quad_any(m, matrix + kMatrixAt));
    }
    retired += kAimSpeed;
    const std::uint32_t speed = m.read_u32_le(car + kCarVelocity + 12U);
    // time = sqrt(length squared), divided by the speed when that is over 1.0.
    const bool divide = !fpu::le(speed, kOne);
    retired += divide ? kAimDivide : kAimDirect;
    direction_length_squared(cpu, m, p);
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                    // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);                 // vmulq.x vf1,vf1,q
    const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];
    m.write_u32_le(p + kTime, divide ? fpu::div(distance, speed) : distance);
    retired += kAimDirection;
    direction_length_squared(cpu, m, p);
    const Gpr128 vector = n3::load_quad_any(m, p + kDirection);                      // lq v1,1136(s0)
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                       // vrsqrt q,vf0w,vf1x
    load_vf(cpu, 1U, vector);                                                       // qmtc2 v1,vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);           // vmulq.xyz vf1,vf1,q
    n3::store_quad_any(m, p + kDirection, cpu.read_vu0_vector(1U));                  // sq v1,1136(s0)
    m.write_u8(p + kHighBin, high_bin);
    m.write_u8(p + kLowBin, low_bin);
    retired += kReturn;
}

// What a path decides: the two bins stored in the profile's bytes (the gap for both on open road).
struct Outcome {
    std::int32_t high_bin;
    std::int32_t low_bin;
    bool declined;
};

// What both paths start from.
struct Start {
    GuestAddress driver;
    GuestAddress racer;
    GuestAddress gp;
    std::uint32_t side_request;
    std::uint32_t nearest;
    std::uint32_t lowest;
};

// The speed to hold for a drop beside the car (levels under 10, 20, 30), or null when it is not that deep.
GuestAddress hold_speed_for(const std::uint32_t drop) {
    if (fpu::lt(drop, kTen)) {
        return kHoldBelow10;
    }
    if (fpu::lt(drop, kTwenty)) {
        return kHoldBelow20;
    }
    return fpu::lt(drop, kThirty) ? kHoldBelow30 : 0U;
}

// Something is close: escape scans up and down, clearances, the side, the lead target and the speed to hold.
template <class Memory>
Outcome close_path(guest::R5900State& cpu, Memory& m, const Lane<Memory>& lane, const Start& start,
                   std::uint32_t& retired) {
    const GuestAddress p = lane.p;
    Margin scan{fpu::sub(start.nearest, lane.bin_width), start.nearest};
    retired += kMarginSetup;
    if (fpu::lt(scan.margin, 0U)) {
        scan.margin = 0U;
        retired += kMarginClamp;
    }
    const std::int32_t high_bin = upper_target_bin(lane, scan, start.nearest, retired);
    if (retired > kCeiling) {
        return {};
    }
    retired += kLowerBegin;
    scan = Margin{fpu::sub(start.nearest, lane.bin_width), start.nearest};
    if (fpu::lt(scan.margin, 0U)) {
        scan.margin = 0U;
        retired += kMarginClamp;
    }
    const std::int32_t low_bin = lower_target_bin(lane, scan, start.nearest, retired);
    if (retired > kCeiling) {
        return {};
    }

    // Clearance and blocked sums of both sides, stored in the profile.
    retired += kHighClearStart;
    const std::uint32_t high_clear = clearance(lane, high_bin, -1, kHighClearBody, retired);
    m.write_u32_le(p + kHighClearance, high_clear);
    retired += kClearFinish;
    const std::uint32_t high_blocked = blocked_sum(lane, lane.low, high_bin, 1, retired);
    m.write_u32_le(p + kHighBlocked, high_blocked);
    retired += kLowClearStart;
    const std::uint32_t low_clear = clearance(lane, low_bin, 1, kLowClearBody, retired);
    m.write_u32_le(p + kLowClearance, low_clear);
    retired += kClearFinish;
    const std::uint32_t low_blocked = blocked_sum(lane, lane.high, low_bin, -1, retired);
    m.write_u32_le(p + kLowBlocked, low_blocked);
    if (retired > kCeiling) {
        return {};
    }

    // How hemmed in the car is by what is beside it (tags 2 to 4), as a fraction of 8.0 per bin of its width.
    retired += kSqueezeStart;
    const std::uint32_t accumulator = fpu::add(0U, kEight);
    std::uint32_t squeeze_sum = 0U;
    for (std::int32_t k = 0; k < lane.width; ++k) {
        const std::int32_t bin = lane.low + k;
        const std::int32_t tag = lane.tag(bin);
        retired += kSqueezeTest + kSqueezeBody + kSqueezeNext;
        if (tag >= 2) {
            retired += kSqueezeBound;
            if (tag < 5) {
                retired += kSqueezeTerm;
                squeeze_sum = fpu::add(squeeze_sum, fpu::msub(accumulator, lane.unit, fpu::from_int(lane.raw_distance(bin))));
            }
        }
        if (retired > kCeiling) {
            return {};
        }
    }
    retired += kSqueezeTest + kSqueezeFinish + kSqueezeDivide;
    const std::uint32_t squeeze = fpu::div(squeeze_sum, fpu::mul(kEight, fpu::from_int(lane.width)));

    // The side. Going low is the default; the high side needs an escape bin off the car's own edge and either the
    // driver asking for it, the low side being blocked, or a smaller blocked total.
    const bool low_open = fpu::eq(kEight, low_clear);
    const bool high_open = fpu::eq(kEight, high_clear);
    const auto decide_high = [&]() {
        if (!low_open) {
            retired += 3U;  // 0x141088
            if (start.side_request == 4U) {
                retired += 1U;  // 0x1410a0
                return false;
            }
            retired += 3U;  // 0x141094
            if (high_open) {
                retired += 1U;
                return false;
            }
        }
        retired += 3U;  // 0x141454
        if (lane.high == high_bin) {
            retired += 1U;
            return false;
        }
        retired += 6U;  // 0x141460
        if (!high_open) {
            retired += 3U;  // 0x141478
            if (start.side_request == 2U) {
                retired += 2U;  // 0x141490
                return true;
            }
            retired += 3U;  // 0x141484
            if (low_open) {
                retired += 2U;
                return true;
            }
        }
        retired += 3U;  // 0x141498
        if (lane.low == low_bin) {
            retired += 2U;
            return true;
        }
        retired += 3U;  // 0x1414a4
        const bool high_smaller = fpu::lt(fpu::add(high_clear, high_blocked), fpu::add(low_clear, low_blocked));
        retired += (high_smaller ? 0U : 1U) + 2U;  // 0x1414b0, 0x1414b4
        return high_smaller;
    };
    const bool go_high = decide_high();
    retired += 2U;  // 0x1410a4
    const std::int32_t offset_bins = go_high ? high_bin - lane.high : low_bin - lane.low;
    retired += go_high ? kPickHigh : kPickLow;
    m.write_u8(p + kSide, go_high ? 1U : 2U);
    scaled_vector(cpu, m, start.driver + kSideDirection, fpu::mul(lane.bin_width, fpu::from_int(offset_bins)));
    n3::store_quad_any(m, p + kTarget, cpu.read_vu0_vector(1U));

    // The lead target: forward scaled by the lead plus the car's position, added to the side offset.
    retired += kLeadTarget;
    if (lane.high < lane.low) {
        retired += kLeadNegative;
    }
    const std::uint32_t lead =
        max_bits(kFive, fpu::mul(kTwo, fpu::div(start.lowest, m.read_u32_le(kLeadLevelScale))));
    const GuestAddress car = m.read_u32_le(start.driver + kCar);
    const GuestAddress matrix = m.read_u32_le(car + kCarMatrix);
    load_vf(cpu, 3U, n3::load_quad_any(m, p + kTarget));                             // lqc2 vf3,1120(s0)
    load_vf(cpu, 2U, scalar_quad(lead));                                             // qmtc2 v1,vf2
    load_vf(cpu, 1U, n3::load_quad_any(m, start.driver + kForward));                 // lqc2 vf1,6848(a1)
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U);                      // vmulx.xyz vf2,vf1,vf2x
    load_vf(cpu, 1U, n3::load_quad_any(m, matrix + kMatrixPosition));                // lqc2 vf1,48(matrix)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);                      // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 3U, 3U, 1U, n3::kMaskXyz, false);                      // vadd.xyz vf3,vf3,vf1
    n3::store_quad_any(m, p + kTarget, cpu.read_vu0_vector(3U));                     // sqc2 vf3,1120(s0)

    // The speed to hold, from the lowest level across the car's middle half.
    retired += kMiddleStart;
    const std::uint32_t drop = middle_level(lane, quarter_of(lane.high - lane.low), retired);
    if (retired > kCeiling) {
        return {};
    }
    GuestAddress source = hold_speed_for(drop);
    if (source != 0U) {
        retired += fpu::lt(drop, kTen) ? kLadderTen : (fpu::lt(drop, kTwenty) ? kLadderTwenty : kLadderThirty);
    } else if (fpu::le(squeeze, m.read_u32_le(start.gp + kSqueezeFree))) {
        retired += kLadderFree;
        source = start.racer + kRacerCruise;
    } else if (fpu::le(squeeze, m.read_u32_le(start.gp + kSqueezeLight))) {
        retired += kLadderLight;
        source = kLightSqueezeSpeed;
    } else {
        retired += kLadderSqueezed;
        source = kSqueezedSpeed;
    }
    m.write_u32_le(p + kSpeed, m.read_u32_le(source));
    return {high_bin, low_bin, false};
}

// Open road: BestLaneGap's bin, the target ahead of the car offset to it, a swerve straight ahead when something
// is just under the car and the gap lies below it, and the speed to hold.
template <class Memory, class CallT>
Outcome open_path(CallT& call, guest::R5900State& cpu, Memory& m, const Lane<Memory>& lane, const Start& start,
                  std::uint32_t& retired) {
    const GuestAddress p = lane.p;
    call.set_arg(0U, p);
    const n3::Run gap_run = call.invoke([](auto& machine) { return n3::fn_0013e790(machine); }, kFrameBytes);
    if (gap_run.declined) {
        return {0, 0, true};
    }
    retired += kOpenHead + gap_run.retired;
    const auto gap = static_cast<std::int32_t>(call.result());
    scaled_vector(cpu, m, start.driver + kSideDirection, fpu::mul(lane.bin_width, fpu::from_int(gap - 128)));
    n3::store_quad_any(m, p + kTarget, cpu.read_vu0_vector(1U));

    const GuestAddress car = m.read_u32_le(start.driver + kCar);
    const GuestAddress matrix = m.read_u32_le(car + kCarMatrix);
    const std::uint32_t speed = m.read_u32_le(car + kCarVelocity + 12U);
    // A racer that is holding (override clear, holding word set) leads by the speed, otherwise by four times it.
    std::uint32_t lead = fpu::mul(kFour, speed);
    if (m.read_u32_le(start.racer + kRacerOverride) != 0U) {
        retired += kOpenFree;
    } else {
        retired += kOpenHoldCheck;
        if (m.read_u32_le(start.racer + kRacerHolding) != 0U) {
            retired += kOpenHolding;
            lead = speed;
        } else {
            retired += kOpenFree;
        }
    }
    load_vf(cpu, 1U, n3::load_quad_any(m, start.driver + kForward));                 // lqc2 vf1,6848(a1)
    load_vf(cpu, 3U, n3::load_quad_any(m, p + kTarget));                             // lqc2 vf3,1120(s0)
    load_vf(cpu, 2U, scalar_quad(lead));                                             // qmtc2 v1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                      // vmulx.xyz vf1,vf1,vf2x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, false);                      // vadd.xyz vf1,vf1,vf3
    retired += kOpenJoin;
    load_vf(cpu, 2U, n3::load_quad_any(m, matrix + kMatrixPosition));                // lqc2 vf2,48(matrix)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);                      // vadd.xyz vf1,vf2,vf1
    n3::store_quad_any(m, p + kTarget, cpu.read_vu0_vector(1U));                     // sqc2 vf1,1120(s0)

    // The five bins from the car's low bin down: one within 1.0 marks the low side blocked (clearance 1.0) and, when
    // the gap is below the car, swerves the target straight ahead.
    for (std::int32_t bin = lane.low;; --bin) {
        retired += kOpenLowTest;
        if (bin < lane.low - 4) {
            break;
        }
        retired += kOpenLowSign;
        if (bin >= 0) {
            retired += kOpenLowSample;
            if (fpu::lt(lane.distance(bin), kOne)) {
                retired += kOpenLowHit;
                m.write_u32_le(p + kLowClearance, kOne);
                if (gap < lane.low) {
                    retired += kOpenSwerve;
                    load_vf(cpu, 1U, n3::load_quad_any(m, start.driver + kForward));      // lqc2 vf1,6848(a0)
                    load_vf(cpu, 3U, n3::load_quad_any(m, matrix + kMatrixPosition));     // lqc2 vf3,48(matrix)
                    load_vf(cpu, 2U, scalar_quad(fpu::mul(kTen, speed)));                 // qmtc2 v1,vf2
                    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);           // vmulx.xyz vf1,vf1,vf2x
                    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, n3::kMaskXyz, false);           // vadd.xyz vf1,vf3,vf1
                    n3::store_quad_any(m, p + kTarget, cpu.read_vu0_vector(1U));          // sqc2 vf1,1120(s0)
                }
            }
        }
        retired += kOpenLowNext;
    }
    // The loop over the bins above the car starts at the car's high bin and runs while it is under high + 4 with the
    // body behind a test that needs it to be 4 past: it never runs.
    retired += kOpenHighSkipped;

    retired += kOpenMiddleStart;
    if (lane.high < lane.low) {
        retired += kLeadNegative;
    }
    const std::uint32_t drop = middle_level(lane, quarter_of(lane.high - lane.low), retired);
    GuestAddress source = hold_speed_for(drop);
    if (source != 0U) {
        retired += fpu::lt(drop, kTen) ? kOpenLadderTen : (fpu::lt(drop, kTwenty) ? kOpenLadderTwenty : kOpenLadderThirty);
    } else {
        retired += kOpenLadderThirty;
        source = start.racer + kRacerCruise;
    }
    m.write_u32_le(p + kSpeed, m.read_u32_le(source));
    return {gap, gap, false};
}

template <class Context>
n3::Run choose_lane_target(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    using Mem = std::remove_cvref_t<decltype(m)>;
    const GuestAddress p = call.arg(0);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress driver = m.read_u32_le(p + kDriver);

    // The side the driver asks for, while it is not held and its request state is 1 or 2.
    std::uint32_t retired = kHeldCheck;
    std::uint32_t side_request = 0U;
    if (m.read_u32_le(driver + kSideRequestHeld) == 0U) {
        retired += kStateCheck;
        if (m.read_u32_le(driver + kSideRequestState) - 1U < 2U) {
            retired += kRequestRead;
            side_request = m.read_u32_le(driver + kSideRequest);
        }
    }
    m.write_u32_le(p + kLowClearance, 0U);
    m.write_u32_le(p + kHighClearance, 0U);
    m.write_u8(p + kSide, 16U);

    const Lane<Mem> lane{m,
                         p,
                         m.read_u32_le(gp + kDistanceUnit),
                         m.read_u32_le(gp + kLevelUnit),
                         m.read_u32_le(gp + kBinWidth),
                         static_cast<std::int32_t>(m.read_u8(p + kCarWidth)),
                         static_cast<std::int32_t>(m.read_u8(p + kCarHigh)),
                         static_cast<std::int32_t>(m.read_u8(p + kCarLow))};

    // The nearest distance and lowest level across the car's bins.
    std::uint32_t nearest = lane.distance(lane.low);
    std::uint32_t lowest = lane.level(lane.low);
    for (std::int32_t bin = lane.low + 1; bin <= lane.high; ++bin) {
        nearest = min_bits(nearest, lane.distance(bin));
        lowest = min_bits(lowest, lane.level(bin));
        retired += kMinPass + kMinTest;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }
    retired += kMinHead + kMinTest + kRouteTest;

    const Start start{driver, m.read_u32_le(p + kRacer), gp, side_request, nearest, lowest};
    const Outcome outcome = fpu::lt(nearest, kFour) ? close_path(cpu, m, lane, start, retired)
                                                    : open_path(call, cpu, m, lane, start, retired);
    if (outcome.declined) {
        return call.decline();
    }
    if (retired > kCeiling) {
        return call.finish(retired);
    }
    aim_at_target(cpu, m, p, static_cast<std::uint8_t>(outcome.high_bin), static_cast<std::uint8_t>(outcome.low_bin),
                  retired);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00140a30, 3656U, 0x92eb58fdde23c811ULL, "ChooseLaneTarget", kCeiling, choose_lane_target,
         EngineReturn::Nothing)
