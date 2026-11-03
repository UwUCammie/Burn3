// 0x001cb780 (a0 = object, a1 = flag (the whole 64-bit register is tested), f12 = limit in degrees): is the
// object's aim direction within `limit` degrees of the fixed reference direction? Returns 0 or 1.
//
// The test (AngleBelow below, the same 62-instruction block twice in the listing) normalises the xyz of a direction
// quad and of the global reference quad at 0x004c9cf0 on VU0 (x*x + y*y first, then + z, rsqrt through Q), dots the
// two unit vectors in the same order, clamps the dot to [-1, 1], takes acosf (0x0011e328), scales it by the
// radians-to-degrees float at $gp-32724 and compares it to the limit.
//
// The object is tested first: when its enable byte (+530) and handle (+408) are non-zero, the current direction at
// +368 is tested. Failing that, and only when the flag is non-zero, each of the slots (signed count at +4457) is
// tested from the last to the first: a slot is skipped while its enable byte (obj + 192 * i + 2259) is zero or the
// type byte (u32 at +3268, + i + 1196, signed) is 3, otherwise the direction quad at obj + 192 * i + 2112 is
// tested. The first test that passes ends the search with 1. Nothing is stored.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0011e328)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kReferenceDirection = 0x004c9cf0U;  // lui 0x4d, -25360
constexpr std::int32_t kRadiansToDegrees = -32724;         // $gp-relative float
constexpr GuestAddress kEnabled = 530U;                    // object: enable byte
constexpr GuestAddress kHandle = 408U;                     // object: handle (u32)
constexpr GuestAddress kDirection = 368U;                  // object: direction quad
constexpr GuestAddress kSlotCount = 4457U;                 // object: signed slot count
constexpr GuestAddress kTypes = 3268U;                     // object: pointer to the slot type bytes
constexpr GuestAddress kTypeBias = 1196U;                  // slot type = signed byte at types + i + 1196
constexpr GuestAddress kSlotEnabled = 2259U;               // slot: enable byte
constexpr GuestAddress kSlotDirection = 2112U;             // slot: direction quad
constexpr std::uint32_t kSlotStride = 192U;
constexpr std::int32_t kSkippedType = 3;
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint32_t kFrameBytes = 96U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 10U;      // 0x001cb780..0x001cb7a4
constexpr std::uint32_t kHandleCost = 3U;      // 0x001cb7a8..0x001cb7b0 (the object is skipped when the handle is 0)
constexpr std::uint32_t kAngleCost = 62U;      // the VU0 block, acosf's jal and the compare (callee extra)
constexpr std::uint32_t kHitCost = 2U;         // set v0 = 1, jump to the epilogue
constexpr std::uint32_t kFlagCost = 2U;        // 0x001cb8b4: flag test
constexpr std::uint32_t kNoFlagCost = 1U;      // 0x001cba0c: v0 = 0
constexpr std::uint32_t kLoopEntryCost = 2U;   // 0x001cb8bc: jump to the loop test, load the count
constexpr std::uint32_t kLoopTestCost = 3U;    // 0x001cb9f8: test, delay-slot decrement
constexpr std::uint32_t kSlotHeadCost = 7U;    // 0x001cb8c4..0x001cb8dc: slot address, enable byte
constexpr std::uint32_t kSlotTypeCost = 6U;    // 0x001cb8e0..0x001cb8f4: type byte
constexpr std::uint32_t kLoopExitCost = 2U;    // 0x001cba04: v0 = 0
constexpr std::uint32_t kEpilogueCost = 6U;    // 0x001cba10..0x001cba24

// 0x001cb7b4 / 0x001cb8f8: the unit vector of a quad's xyz (w passes through), made as VU0 does it.
void unit_vector(guest::R5900State& cpu, const Gpr128& quad, const unsigned vf) {
    cpu.write_vu0_vector(vf, quad);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, vf, vf, n3::kMaskXyz);                                 // vmul.xyz vf2,v,v
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                     // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, vf, vf, n3::kMaskXyz, ee_cop2::kControlQ);         // vmulq.xyz v,v,q
}

// The cosine of the angle between the two quads' xyz as the raw bits VU0 leaves in lane x, clamped to [-1, 1].
std::uint32_t clamped_cosine(guest::R5900State& cpu, const Gpr128& direction, const Gpr128& reference) {
    unit_vector(cpu, direction, 3U);
    const Gpr128 first = cpu.read_vu0_vector(3U);
    unit_vector(cpu, reference, 3U);
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(3U));
    cpu.write_vu0_vector(2U, first);
    ee_cop2::vmul(cpu, 2U, 2U, 1U, n3::kMaskXyz);                                 // vmul.xyz vf2,vf2,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, true);                   // vsubw.x vf2,vf0,vf0w (-1)
    ee_cop2::vmax(cpu, 1U, 1U, 2U, kMaskX);                                       // vmax.x vf1,vf1,vf2
    ee_cop2::vmini_w(cpu, 1U, 1U, 0U, kMaskX);                                    // vminiw.x vf1,vf1,vf0w
    return cpu.read_vu0_vector(1U).words[0];
}

struct Verdict {
    bool within;
    bool declined;
    std::uint32_t retired;  // the angle block plus acosf's own count
};

// Is the angle between `direction` and the reference below the limit (degrees, raw float bits)?
template <class Call>
Verdict angle_below(Call& call, const Gpr128& direction, const std::uint32_t limit) {
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint32_t cosine = clamped_cosine(cpu, direction, n3::load_quad_any(m, kReferenceDirection));
    call.set_float_arg(0, cosine);
    const n3::Run acos = call.invoke([](auto& memory) { return n3::fn_0011e328(memory); }, kFrameBytes);
    if (acos.declined) {
        return {false, true, 0U};
    }
    const std::uint32_t scale = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kRadiansToDegrees));
    const std::uint32_t degrees = fpu::mul(scale, call.float_result());
    return {fpu::lt(degrees, limit), false, kAngleCost + acos.retired};
}

template <class Context>
n3::Run aim_within_angle(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const bool search_slots = call.arg64(1) != 0U;
    const std::uint32_t limit = call.cpu().read_fpr_bits(12U);
    std::uint32_t retired = kEntryCost;

    const auto hit = [&](const std::uint32_t count) {
        call.return_value(1U);
        return call.finish(count + kHitCost + kEpilogueCost);
    };

    // The object's own direction.
    if (m.read_u8(object + kEnabled) != 0U) {
        retired += kHandleCost;
        if (m.read_u32_le(object + kHandle) != 0U) {
            const Verdict v = angle_below(call, n3::load_quad_any(m, object + kDirection), limit);
            if (v.declined) {
                return call.decline();
            }
            retired += v.retired;
            if (v.within) {
                return hit(retired);
            }
        }
    }

    retired += kFlagCost;
    if (!search_slots) {
        call.return_value(0U);
        return call.finish(retired + kNoFlagCost + kEpilogueCost);
    }

    // The slots, last to first; the decrement of the index happens before the slot is read.
    retired += kLoopEntryCost;
    std::uint32_t remaining = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kSlotCount))));
    while (remaining != 0U) {
        retired += kLoopTestCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        remaining -= 1U;
        const GuestAddress slot = object + remaining * kSlotStride;
        retired += kSlotHeadCost;
        if (m.read_u8(slot + kSlotEnabled) == 0U) {
            continue;
        }
        retired += kSlotTypeCost;
        const auto type = static_cast<std::int8_t>(m.read_u8(m.read_u32_le(object + kTypes) + remaining + kTypeBias));
        if (type == kSkippedType) {
            continue;
        }
        const Verdict v = angle_below(call, n3::load_quad_any(m, slot + kSlotDirection), limit);
        if (v.declined) {
            return call.decline();
        }
        retired += v.retired;
        if (v.within) {
            return hit(retired);
        }
    }
    retired += kLoopTestCost + kLoopExitCost;
    call.return_value(0U);
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001cb780, 680U, 0x6caacef5f689043aULL, "AimWithinAngle", kCeiling, aim_within_angle, EngineReturn::Integer)
