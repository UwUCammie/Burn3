// 0x00141fc0 (a0 = car C): UpdateCarStripHeight. The car's height above the road strip under it. The route object P
// (u32 at C+6324, 0 = none) holds a table record Q (u32 at P+4) whose first word T0 (u32 at Q) gives four u16 road
// point indices per segment: the segment (u16 at C+6328, 0xffff = none) selects the entry T0 + 4*segment, and its u16s
// at +0, +2, +4 and +6 are the points s2, s1, s4, s0. The point table (u32 at 0x0064d354) holds 16-byte points, and each
// point's y lane is the surface height there. The surface blends the y lanes by the car's blend t (float at C+7120,
// clamped to 0..1) and by the fraction CarStripEdgeFraction (0x0014a940) returns for C+6640, clamped the same way.
// The car's y (quadword at C+48) less the surface is stored at C+9276 by a VU0 subtract. That height less the limit at
// K+2160 (K = u32 at C+9264) picks the code (u16 at C+9290) and the counter (u32 at C+9296): above -1.0 the code takes
// the low half of the u32 at C+6336 and the counter clears; from -5.0 to -1.0 the counter clears; below -5.0 the counter
// counts up, and when it reaches 61 the original's call to 0x00141d60 (not native) runs, so the call declines. Without
// a route, a segment or a second index (C+7112 = 0xffff) the height and code reset and the counter clears; nothing is
// called. Returns nothing.

#include <algorithm>

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0014a940)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kRouteOffset = 6324U;              // C+6324: the route object P
constexpr GuestAddress kSegmentOffset = 6328U;            // C+6328: u16 segment index (0xffff = none)
constexpr GuestAddress kResetCodeOffset = 6336U;          // C+6336: u32 whose low half is the code on a reset
constexpr GuestAddress kStripRecordOffset = 6640U;        // C+6640: the record CarStripEdgeFraction reads (its a0)
constexpr GuestAddress kSecondOffset = 7112U;             // C+7112: u16 second index (0xffff = none)
constexpr GuestAddress kBlendOffset = 7120U;              // C+7120: float blend t
constexpr GuestAddress kPositionOffset = 48U;             // C+48: the car's position quadword (its y is used)
constexpr GuestAddress kStripObjectOffset = 9264U;        // C+9264: u32 K, the strip object
constexpr GuestAddress kHeightOffset = 9276U;             // C+9276: float height above the surface
constexpr GuestAddress kCodeOffset = 9290U;               // C+9290: u16 code
constexpr GuestAddress kCounterOffset = 9296U;            // C+9296: u32 count of frames below -5.0
constexpr GuestAddress kRouteTableOffset = 4U;            // P+4: the segment table record Q
constexpr GuestAddress kLimitOffset = 2160U;              // K+2160: float limit
constexpr GuestAddress kPointTableGlobal = 0x0064d354U;   // u32 pointer to the road point table (lui 0x65, lw -11436)
constexpr std::uint32_t kNoIndex = 0xffffU;
constexpr std::uint32_t kFrameBytes = 304U;               // addiu $sp, $sp, -304 of the original (the callee's frame)
constexpr std::uint32_t kOneBits = 0x3f800000U;           // 1.0f
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;      // -1.0f
constexpr std::uint32_t kMinusFiveBits = 0xc0a00000U;     // -5.0f
constexpr std::int32_t kCountLimit = 61;
// The call to CarStripEdgeFraction adds its own count, so the declared cost is the ceiling (a larger bound is safe).
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kRouteTestCost = 12U;         // 0x141fc0..0x141fec: prologue, the route test and its delay slot
constexpr std::uint32_t kSegmentTestCost = 4U;        // 0x141ff0..0x141ffc: the segment test (0xffff exit)
constexpr std::uint32_t kSecondTestCost = 3U;         // 0x142000..0x142008: the second index test (0xffff exit)
constexpr std::uint32_t kCheckedCost = kRouteTestCost + kSegmentTestCost + kSecondTestCost;   // 19
constexpr std::uint32_t kSurfaceCost = 80U;           // 0x14200c..0x142148: lookups, the call, the blend, VU0, first branch
constexpr std::uint32_t kResetCost = 5U;              // 0x1421a0..0x1421b0: the reset stores and the branch past the tail
constexpr std::uint32_t kResetTailCost = 10U;         // 0x1421cc..0x1421f0: ld ra and the epilogue
constexpr std::uint32_t kAboveTailCost = 15U;         // d > -1.0: 0x1421b4..0x1421c0, 0x142198 and the epilogue
constexpr std::uint32_t kBandTailCost = 19U;          // -5.0 <= d <= -1.0: 0x14214c..0x142160, 0x1421c4, 0x142198, epilogue
constexpr std::uint32_t kCountTailCost = 23U;         // d < -5.0, count below 61: 0x14214c..0x142160, 0x142164..0x14217c, 0x1421cc, epilogue

// MAX.S and MIN.S on raw words, as ee_cop1 max_s and min_s: a signed compare of the encodings, reversed when both
// operands are negative.
std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

std::uint32_t min_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

// Clamped to 0..1: max.s against +0.0, then min.s against 1.0.
std::uint32_t clamp_unit(const std::uint32_t bits) { return min_word(max_word(bits, 0U), kOneBits); }

// The y lane of the road point at table + 16*index. lq drops the low four address bits, so the lane is read from the
// aligned quadword.
template <class Memory>
std::uint32_t point_y(const Memory& m, const GuestAddress table, const std::uint32_t index) {
    return m.read_u32_le(((table + (index << 4U)) & ~15U) + 4U);
}

// The surface height from the four road points' y lanes: the upper pair (y2, y4) blended by t, the lower pair (y1, y0)
// blended by t, then the two blended by u. The accumulator order is the listing's: adda.s sets ACC = 0 + y, then madd.s
// adds the product to ACC.
std::uint32_t blend_surface(const std::uint32_t y4, const std::uint32_t y2, const std::uint32_t y1,
                            const std::uint32_t y0, const std::uint32_t t, const std::uint32_t u) {
    const std::uint32_t upper = fpu::madd(fpu::add(0U, y2), t, fpu::sub(y4, y2));
    const std::uint32_t lower = fpu::madd(fpu::add(0U, y1), t, fpu::sub(y0, y1));
    return fpu::madd(fpu::add(0U, lower), u, fpu::sub(upper, lower));
}

// The reset: the height is zero, the code takes the low half of C+6336 and the counter clears.
template <class Memory>
void reset_car(Memory& m, const GuestAddress car) {
    m.write_u32_le(car + kHeightOffset, 0U);
    m.write_u16_le(car + kCodeOffset, static_cast<std::uint16_t>(m.read_u32_le(car + kResetCodeOffset)));
    m.write_u32_le(car + kCounterOffset, 0U);
}

template <class Context>
n3::Run update_car_strip_height(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress car = call.arg(0);

    const GuestAddress route = m.read_u32_le(car + kRouteOffset);
    if (route == 0U) {
        reset_car(m, car);
        return call.finish(kRouteTestCost + kResetCost + kResetTailCost);
    }
    const std::uint32_t segment = m.read_u16_le(car + kSegmentOffset);
    if (segment == kNoIndex) {
        reset_car(m, car);
        return call.finish(kRouteTestCost + kSegmentTestCost + kResetCost + kResetTailCost);
    }
    if (m.read_u16_le(car + kSecondOffset) == kNoIndex) {
        reset_car(m, car);
        return call.finish(kCheckedCost + kResetCost + kResetTailCost);
    }

    // The segment's entry holds the four point indices. They are read before the call, as the listing reads them.
    const GuestAddress table = m.read_u32_le(m.read_u32_le(route + kRouteTableOffset));
    const GuestAddress entry = table + segment * 4U;
    const std::uint32_t s2 = m.read_u16_le(entry);
    const std::uint32_t s1 = m.read_u16_le(entry + 2U);
    const std::uint32_t s4 = m.read_u16_le(entry + 4U);
    const std::uint32_t s0 = m.read_u16_le(entry + 6U);
    const std::uint32_t t = clamp_unit(m.read_u32_le(car + kBlendOffset));

    // jal 0x0014a940 with a0 = C+6640 (the delay slot sets a0 before the callee runs).
    call.set_arg(0, car + kStripRecordOffset);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0014a940(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    const std::uint32_t u = clamp_unit(call.float_result());

    // The four road points are read after the call, as the listing loads them.
    const GuestAddress points = m.read_u32_le(kPointTableGlobal);
    const std::uint32_t surface = blend_surface(point_y(m, points, s4), point_y(m, points, s2), point_y(m, points, s1),
                                                point_y(m, points, s0), t, u);

    // lqc2 vf1,48(s3); lq v1 of road point s1, then mfc1 v1,f1 keeps that v1's upper doubleword while setting the
    // lower one to the surface bits (sign-extended); qmtc2 v1,vf2; vsubx.y vf1,vf1,vf2x. The y lane after the subtract
    // is what the listing's sqc2 and lwc1 pair hands back.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, car + kPositionOffset));
    const n3::Gpr128 point1 = n3::load_quad_any(m, points + (s1 << 4U));
    const std::uint32_t surface_high = (surface & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(2U, n3::Gpr128{{surface, surface_high, point1.words[2], point1.words[3]}});
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, 4U, 0U, true);
    const std::uint32_t height = cpu.read_vu0_vector(1U).words[1];

    m.write_u32_le(car + kHeightOffset, height);
    const GuestAddress strip_object = m.read_u32_le(car + kStripObjectOffset);
    const std::uint32_t d = fpu::sub(height, m.read_u32_le(strip_object + kLimitOffset));
    if (!fpu::le(d, kMinusOneBits)) {
        m.write_u16_le(car + kCodeOffset, static_cast<std::uint16_t>(m.read_u32_le(car + kResetCodeOffset)));
        m.write_u32_le(car + kCounterOffset, 0U);
        return call.finish(kCheckedCost + kSurfaceCost + kAboveTailCost + callee.retired);
    }
    if (!fpu::lt(d, kMinusFiveBits)) {
        m.write_u32_le(car + kCounterOffset, 0U);
        return call.finish(kCheckedCost + kSurfaceCost + kBandTailCost + callee.retired);
    }
    // Below -5.0: the counter is stored, then tested. A count of 61 or more is the original's call to 0x00141d60, which
    // has no native, so the call declines. The dry run that decides this discards the stores made before it.
    const std::uint32_t counter = m.read_u32_le(car + kCounterOffset) + 1U;
    m.write_u32_le(car + kCounterOffset, counter);
    if (static_cast<std::int32_t>(counter) >= kCountLimit) {
        return call.decline();
    }
    return call.finish(kCheckedCost + kSurfaceCost + kCountTailCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00141fc0, 564U, 0x1122ce1eba803c13ULL, "UpdateCarStripHeight", kCeiling, update_car_strip_height,
         EngineReturn::Nothing)
