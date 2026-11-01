// 0x0013e530 (a0 = lane profile P, a1 = strip object S, a2 = quad index (low 16 bits), a3 = tag, f12 = distance,
// f13 = level): MarkStripQuadInLaneProfile. S's road (u32 at +4) holds the pair table (u32 at +0; quad index q is
// the four halfwords at 4q +0, +2, +4, +6 = vertices 0, 1, 2, 3). The blend is the float at +6332 of the pointer at
// P+1108, clamped to [0, 1]. VU0 blends vertices 0 and 2 (the high edge) and 1 and 3 (the low edge), takes each
// result less the origin (VF4) and multiplies by the direction (VF3); the x lane of the dot is the projection.
// Bins are 5 * (projection - centre) truncated, biased by 128 to index the profile's 256 entries. The high index is
// clamped to 255 and the low index to 0; nothing is marked when the high index is negative or the low one is 256
// and up. Each index from low to high takes the distance byte 255 * f12 / 8 and tag a3 when f12 is below its old
// distance byte times 8/255 (the unit at $gp-30680), and the level halfword 32767 * clamp(f13, -500, 500) / 500 when
// f13 is below its old level times 500/32767 (at $gp-30676). Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// LaneProfileLayout (track.cpp).
constexpr GuestAddress kOrigin = 16U;             // VF4
constexpr GuestAddress kDirection = 48U;          // VF3
constexpr GuestAddress kTags = 64U;               // u8[256]
constexpr GuestAddress kDistances = 320U;         // u8[256]
constexpr GuestAddress kLevels = 576U;            // s16[256]
constexpr GuestAddress kCentre = 1100U;
constexpr GuestAddress kBlendSource = 1108U;      // pointer; its +6332 is the blend
constexpr GuestAddress kBlendOffset = 6332U;
constexpr GuestAddress kStripVertices = 0x0064d354U;  // lui 0x65, -11436: pointer to the vertex array
constexpr GuestAddress kDistanceUnitOffset = static_cast<GuestAddress>(-30680);  // gp-relative, 8/255
constexpr GuestAddress kLevelUnitOffset = static_cast<GuestAddress>(-30676);    // gp-relative, 500/32767

// The COP1 constants the listing builds with lui and mtc1.
constexpr std::uint32_t kOneBits = 0x3f800000U;                // 1.0
constexpr std::uint32_t kFiveBits = 0x40a00000U;               // 5.0
constexpr std::uint32_t kDistanceScaleBits = 0x437f0000U;      // 255.0
constexpr std::uint32_t kEightBits = 0x41000000U;              // 8.0
constexpr std::uint32_t kLevelRangeBits = 0x43fa0000U;         // 500.0
constexpr std::uint32_t kNegativeLevelRangeBits = 0xc3fa0000U; // -500.0
constexpr std::int32_t kFullScaleLevel = 32767;
constexpr std::int32_t kBinCount = 256;
constexpr std::uint32_t kBinBias = 128U;

constexpr unsigned kRegisterT1 = 9U;
constexpr unsigned kRegisterA2 = 6U;
constexpr std::uint8_t kMaskX = 8U;               // the .x field
constexpr std::uint8_t kMaskXyz = 14U;            // the .xyz fields

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPhaseOneCost = 83U;      // 0x13e530..0x13e678: blend, VU0 projections, bin indices
constexpr std::uint32_t kReturnCost = 2U;         // 0x13e788..0x13e78c: jr and its delay slot
constexpr std::uint32_t kGuardCost = 3U;          // 0x13e67c..0x13e684: low index against 256
constexpr std::uint32_t kPairCost = 2U;           // 0x13e688..0x13e68c: low index below zero?
constexpr std::uint32_t kLowClampCost = 2U;       // 0x13e754..0x13e758: low index clamped to zero
constexpr std::uint32_t kHighGuardCost = 3U;      // 0x13e690..0x13e698: high index against 256
constexpr std::uint32_t kHighClampCost = 1U;      // 0x13e69c: high index clamped to 255
constexpr std::uint32_t kConstantsCost = 19U;     // 0x13e6a0..0x13e6e8: units, distance byte, scale
constexpr std::uint32_t kTestCost = 3U;           // 0x13e740..0x13e748: the loop test
constexpr std::uint32_t kDistanceTestCost = 9U;   // 0x13e6ec..0x13e70c: the distance test
constexpr std::uint32_t kDistanceWriteCost = 3U;  // 0x13e75c..0x13e764: the distance and tag stores
constexpr std::uint32_t kLevelTestCost = 11U;     // 0x13e710..0x13e738: the level test
constexpr std::uint32_t kLevelWriteCost = 8U;     // 0x13e768..0x13e784: the level store
constexpr std::uint32_t kStepCost = 1U;           // 0x13e73c: the index step
constexpr std::uint32_t kExitCost = 2U;           // 0x13e74c..0x13e750: the loop exit jump

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// MIN.S and MAX.S on raw words: the sign-magnitude ordering of ee_cop1_arithmetic.hpp (min_s / max_s).
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// C.LT.S leaves its result in the FCR31 condition bit, which the next bc1t reads (the original's last compare
// is the level test of its last index, or none).
void set_condition(guest::R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                             : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
}

// The accesses the interpreter faults on: lh and sh need 2-byte alignment, lw and lwc1 4-byte, lqc2 16-byte; lq
// masks the low four address bits; every access must lie in guest memory. The original faults there, so such a
// call is left to it (call.decline()).
template <class Memory>
bool halfword_ok(const Memory& m, const GuestAddress address) {
    return (address & 1U) == 0U && m.contains(address, 2U);
}

template <class Memory>
bool word_ok(const Memory& m, const GuestAddress address) {
    return (address & 3U) == 0U && m.contains(address, 4U);
}

template <class Memory>
bool byte_ok(const Memory& m, const GuestAddress address) {
    return m.contains(address, 1U);
}

template <class Memory>
bool quad_ok(const Memory& m, const GuestAddress address) {
    return m.contains(address & ~15U, 16U);
}

template <class Memory>
bool quad_aligned_ok(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost).
template <class Context>
n3::Run mark_strip_quad_in_lane_profile(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress profile = call.arg(0);
    const GuestAddress strip = call.arg(1);
    const std::uint32_t quad = call.arg(2) & 0xffffU;
    const auto tag = static_cast<std::uint8_t>(call.arg(3));
    const std::uint32_t distance = cpu.read_fpr_bits(12U);
    const std::uint32_t level = cpu.read_fpr_bits(13U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 0x13e530..0x13e570: the pair table of the strip's road, and the blend clamped to [0, 1].
    if (!word_ok(m, strip + 4U)) return call.decline();
    const GuestAddress road = m.read_u32_le(strip + 4U);
    if (!word_ok(m, road)) return call.decline();
    const GuestAddress pairs = m.read_u32_le(road);
    if (!word_ok(m, profile + kBlendSource)) return call.decline();
    const GuestAddress blend_source = m.read_u32_le(profile + kBlendSource);
    if (!word_ok(m, blend_source + kBlendOffset)) return call.decline();
    const std::uint32_t blend = max_bits(min_bits(m.read_u32_le(blend_source + kBlendOffset), kOneBits), 0U);

    // 0x13e560..0x13e5d8: VF1 = lerp(vertex 0, vertex 2) less the origin, its x lane (the high projection).
    const GuestAddress pair = pairs + (quad << 2U);
    if (!halfword_ok(m, pair)) return call.decline();
    const auto index0 = static_cast<std::uint32_t>(m.read_u16_le(pair));
    if (!word_ok(m, kStripVertices)) return call.decline();
    const GuestAddress vertices = m.read_u32_le(kStripVertices);
    if (!halfword_ok(m, pair + 4U)) return call.decline();
    const auto index2 = static_cast<std::uint32_t>(m.read_u16_le(pair + 4U));
    const GuestAddress vertex0 = vertices + (index0 << 4U);
    if (!quad_ok(m, vertex0)) return call.decline();
    const guest::Gpr128 quad0 = n3::load_quad_any(m, vertex0);
    const GuestAddress vertex2 = vertices + (index2 << 4U);
    if (!quad_ok(m, vertex2)) return call.decline();
    const guest::Gpr128 quad2 = n3::load_quad_any(m, vertex2);
    if (!quad_aligned_ok(m, profile + kOrigin) || !quad_aligned_ok(m, profile + kDirection)) {
        return call.decline();
    }
    const guest::Gpr128 origin = n3::load_quad_any(m, profile + kOrigin);
    const guest::Gpr128 direction = n3::load_quad_any(m, profile + kDirection);

    cpu.write_vu0_vector(1U, quad0);                                                // qmtc2 a1, vf1
    ee_cop2::vadda_broadcast(cpu, 1U, 0U, kMaskXyz, 0U);                            // vaddax.xyz acc, vf1, vf0x
    cpu.write_gpr_sign_extended_32(kRegisterT1, blend);                             // mfc1 t1, f0
    cpu.write_vu0_vector(2U, cpu.read_gpr(kRegisterT1));                            // qmtc2 t1, vf2
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kMaskXyz, 0U, false, true, true);    // vmsubax.xyz acc, vf1, vf2x
    cpu.write_vu0_vector(1U, quad2);                                                // qmtc2 v1, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskXyz, 0U, false, false, false);   // vmaddx.xyz vf1, vf1, vf2x
    cpu.write_vu0_vector(4U, origin);                                               // lqc2 vf4, 16(a0)
    cpu.write_vu0_vector(3U, direction);                                            // lqc2 vf3, 48(a0)
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, kMaskXyz, true);                          // vsub.xyz vf1, vf1, vf4
    ee_cop2::vmul(cpu, 2U, 1U, 3U, kMaskXyz);                                       // vmul.xyz vf2, vf1, vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                    // vaddw.x vf1, vf0, vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                              // vadday.x acc, vf2, vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf2z
    const std::uint32_t high = cpu.read_vu0_vector(1U).words[0];                    // qmfc2 v1, vf1; mtc1 v1, f1

    // 0x13e5dc..0x13e640: the same for vertex 1 and vertex 3 (the low projection).
    if (!halfword_ok(m, pair + 2U)) return call.decline();
    const auto index1 = static_cast<std::uint32_t>(m.read_u16_le(pair + 2U));
    if (!halfword_ok(m, pair + 6U)) return call.decline();
    const auto index3 = static_cast<std::uint32_t>(m.read_u16_le(pair + 6U));
    const GuestAddress vertex1 = vertices + (index1 << 4U);
    if (!quad_aligned_ok(m, vertex1)) return call.decline();
    const guest::Gpr128 quad1 = n3::load_quad_any(m, vertex1);
    cpu.write_gpr_sign_extended_32(kRegisterA2, blend);                             // mfc1 a2, f0
    const GuestAddress vertex3 = vertices + (index3 << 4U);
    if (!quad_ok(m, vertex3)) return call.decline();
    const guest::Gpr128 quad3 = n3::load_quad_any(m, vertex3);
    if (!word_ok(m, profile + kCentre)) return call.decline();
    const std::uint32_t centre = m.read_u32_le(profile + kCentre);                  // lwc1 f0, 1100(a0)

    cpu.write_vu0_vector(1U, quad1);                                                // lqc2 vf1, 0(a1)
    ee_cop2::vadda_broadcast(cpu, 1U, 0U, kMaskXyz, 0U);                            // vaddax.xyz acc, vf1, vf0x
    cpu.write_vu0_vector(2U, cpu.read_gpr(kRegisterA2));                            // qmtc2 a2, vf2
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kMaskXyz, 0U, false, true, true);    // vmsubax.xyz acc, vf1, vf2x
    cpu.write_vu0_vector(1U, quad3);                                                // qmtc2 v1, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskXyz, 0U, false, false, false);   // vmaddx.xyz vf1, vf1, vf2x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, kMaskXyz, true);                          // vsub.xyz vf1, vf1, vf4
    ee_cop2::vmul(cpu, 2U, 1U, 3U, kMaskXyz);                                       // vmul.xyz vf2, vf1, vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                    // vaddw.x vf1, vf0, vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                              // vadday.x acc, vf2, vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf2z
    const std::uint32_t low = cpu.read_vu0_vector(1U).words[0];                     // qmfc2 v1, vf1; mtc1 v1, f3

    // 0x13e644..0x13e678: the two indices, 5 * (projection - centre) truncated and biased by 128.
    const std::uint32_t high_scaled = fpu::mul(kFiveBits, fpu::sub(high, centre));
    const std::uint32_t low_scaled = fpu::mul(kFiveBits, fpu::sub(low, centre));
    auto high_index = static_cast<std::int32_t>(static_cast<std::uint32_t>(fpu::to_int(high_scaled)) + kBinBias);
    auto low_index = static_cast<std::int32_t>(static_cast<std::uint32_t>(fpu::to_int(low_scaled)) + kBinBias);

    std::uint32_t retired = kPhaseOneCost;
    if (high_index < 0) {
        return call.finish(retired + kReturnCost);
    }
    retired += kGuardCost;
    if (low_index >= kBinCount) {
        return call.finish(retired + kReturnCost);
    }
    retired += kPairCost;
    if (low_index < 0) {
        retired += kLowClampCost;
        low_index = 0;
    }
    retired += kHighGuardCost;
    if (high_index >= kBinCount) {
        retired += kHighClampCost;
        high_index = kBinCount - 1;
    }
    retired += kConstantsCost;

    // 0x13e6a0..0x13e6e8: the distance and level units from $gp, and the distance byte (255 * f12 / 8, truncated).
    if (!word_ok(m, gp + kDistanceUnitOffset) || !word_ok(m, gp + kLevelUnitOffset)) return call.decline();
    const std::uint32_t distance_unit = m.read_u32_le(gp + kDistanceUnitOffset);
    const std::uint32_t level_unit = m.read_u32_le(gp + kLevelUnitOffset);
    const auto distance_value = static_cast<std::uint32_t>(
        fpu::to_int(fpu::div(fpu::mul(kDistanceScaleBits, distance), kEightBits)));
    const std::uint32_t full_scale = fpu::from_int(kFullScaleLevel);

    // 0x13e6ec..0x13e748 and 0x13e6ec..0x13e784: each index from low to high, the distance and the level tests.
    retired += kTestCost;
    while (low_index <= high_index) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const auto bin = static_cast<GuestAddress>(low_index);
        const GuestAddress distance_address = profile + kDistances + bin;
        const GuestAddress tag_address = profile + kTags + bin;
        const GuestAddress level_address = profile + kLevels + (bin << 1U);

        retired += kDistanceTestCost;
        if (!byte_ok(m, distance_address)) return call.decline();
        const auto old_distance = static_cast<std::uint32_t>(m.read_u8(distance_address));
        const std::uint32_t scaled_distance =
            fpu::mul(distance_unit, fpu::from_int(static_cast<std::int32_t>(old_distance)));
        const bool distance_less = fpu::lt(distance, scaled_distance);  // c.olt.s f12, f0
        set_condition(cpu, distance_less);
        if (distance_less) {
            if (!byte_ok(m, tag_address)) return call.decline();
            m.write_u8(distance_address, static_cast<std::uint8_t>(distance_value));
            m.write_u8(tag_address, tag);
            retired += kDistanceWriteCost;
        }

        retired += kLevelTestCost;
        if (!halfword_ok(m, level_address)) return call.decline();
        const auto old_level = static_cast<std::int16_t>(m.read_u16_le(level_address));
        const std::uint32_t scaled_level = fpu::mul(level_unit, fpu::from_int(static_cast<std::int32_t>(old_level)));
        const bool level_less = fpu::lt(level, scaled_level);  // c.olt.s f13, f0
        set_condition(cpu, level_less);
        if (level_less) {
            const std::uint32_t clamped = min_bits(max_bits(level, kNegativeLevelRangeBits), kLevelRangeBits);
            const std::uint32_t scaled = fpu::div(fpu::mul(full_scale, clamped), kLevelRangeBits);
            m.write_u16_le(level_address, static_cast<std::uint16_t>(fpu::to_int(scaled)));
            retired += kLevelWriteCost;
        }

        retired += kStepCost;
        ++low_index;
        retired += kTestCost;
    }
    retired += kExitCost + kReturnCost;
    return call.finish(retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(0013e530, 608U, 0xce3fc2efd6a88772ULL, "MarkStripQuadInLaneProfile", kCeiling, mark_strip_quad_in_lane_profile,
         EngineReturn::Nothing)
