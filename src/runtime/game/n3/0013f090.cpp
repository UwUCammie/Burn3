// 0x0013f090 (a0 = vehicle A, a1 = neighbour record V, a2 = pointer to a quad Qa, a3 / t0 = flags, t1 = profile id,
// t2 = box pointer): ProjectNeighbourIntoRouteProfile. Decides whether the neighbour V lies in front of the vehicle
// within range and, if so, makes ONE call to ProjectRouteIntoProfile (0x0013f550) with two points and a level; it
// returns without a call at ~12 geometric rejects. Nothing visible is stored here: every sq/sqc2 of the original goes
// to its own frame, except the two quads the approach call reads through its a2/a3 pointers (frame + 528 and + 512).
//
//   A's physics record P = u32 at A+1108; Z = u32 at P+9264; W = u32 at Z+516.
//   d0 = Z[176] - Qa (xyz). Its VU length gives the unit direction `direction` (the quad at P+32 when d0 is shorter
//   than 2^-32) and the length `reach` (clamped up to the float at $gp-30796).
//   dp = V[48] - A[32] (xyz). |dp.y| must not exceed the float at 0x01e75534. `range` = |dp| and `unit` = dp/|dp|.
//   a3 != 0 ("approach"): range must be within 0x01e75530, and Z[192].unit decides: below the float at $gp-30672
//     the function just returns; otherwise the call is made with the points V[48] and V[48] + Qa * 0x01e75538,
//     level 0.5 * 0x01e75538.
//   a3 == 0 ("profile"): with t0 != 0 range must be within 0x01e7551c. along = P[6880].dp (t0 != 0: it must not be
//     below -P[9272] / 2); facing = max(direction.unit, floor). From range < 12 the level is 0, otherwise the level
//     comes from range / (facing * reach) (negative: reject; above 8 after the 0.5 * 0x01e75538 offset: reject).
//     t0 != 0 then adds the offsets at 0x01e7555c.. and clamps at 0, t0 == 0 clamps at 0 and doubles.
//   The call is made with the points V[48] / V[48] and the level and slope/1.5.
// VU0 registers VF1..VF4, ACC, Q and the COP1 accumulator end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>

N3_CALLEE(0013f550)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Records.
constexpr GuestAddress kPhysics = 1108U;           // A: P
constexpr GuestAddress kHolder = 9264U;            // P: Z
constexpr GuestAddress kTable = 516U;              // Z: W
constexpr GuestAddress kHolderPoint = 176U;        // Z: point (quad)
constexpr GuestAddress kHolderAxis = 192U;         // Z: axis (quad)
constexpr GuestAddress kHolderPointWeight = 176U + 12U;  // Z: the w lane of that quad
constexpr GuestAddress kPhysicsDirection = 32U;    // P: direction (quad)
constexpr GuestAddress kPhysicsAxis = 6880U;       // P: axis (quad)
constexpr GuestAddress kPhysicsHalfReach = 9272U;  // P: float
constexpr GuestAddress kVehiclePosition = 32U;     // A: quad
constexpr GuestAddress kNeighbourDirection = 32U;  // V: quad (passed on as t0/t1)
constexpr GuestAddress kNeighbourPosition = 48U;   // V: quad
constexpr GuestAddress kTableCentre = 48U;         // W: quad

// Floats.
constexpr GuestAddress kRangeLimitApproach = 0x01e75530U;
constexpr GuestAddress kSideLimit = 0x01e75534U;
constexpr GuestAddress kRangeLimitProfile = 0x01e7551cU;
constexpr GuestAddress kProbeOffset = 0x01e75538U;
constexpr GuestAddress kLevelBias = 0x01e7555cU;
constexpr GuestAddress kSlopeBias2 = 0x01e75560U;
constexpr GuestAddress kLevelBias2 = 0x01e75564U;
constexpr GuestAddress kSlopeBias = 0x01e75568U;
constexpr GuestAddress kFloorOffset = 0U - 30796U;     // $gp - 30796
constexpr GuestAddress kBehindOffset = 0U - 30672U;    // $gp - 30672

constexpr std::uint32_t kTiny = 0x2f800000U;           // 2^-32
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;
constexpr std::uint32_t kTwelve = 0x41400000U;
constexpr std::uint32_t kEight = 0x41000000U;
constexpr std::uint8_t kMaskX = 8U;

constexpr std::uint32_t kFrameBytes = 608U;
constexpr GuestAddress kFrameBPoint = 528U;   // V[48] copy, a2 of the approach call
constexpr GuestAddress kFrameProbe = 512U;    // probe point, a3 of the approach call
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the blocks of the listing, delay slots included (likely branches handled).
constexpr std::uint32_t kEntry = 26U;           // 0x13f090..0x13f0f4
constexpr std::uint32_t kShortDirection = 4U;   // 0x13f0f8..0x13f104
constexpr std::uint32_t kLongDirection = 24U;   // 0x13f214..0x13f270
constexpr std::uint32_t kFloorReach = 2U;       // 0x13f274..0x13f278
constexpr std::uint32_t kDelta = 13U;           // 0x13f108..0x13f138
constexpr std::uint32_t kNormalise = 21U;       // 0x13f13c..0x13f18c
constexpr std::uint32_t kApproachRange = 5U;    // 0x13f190..0x13f1a0
constexpr std::uint32_t kApproachDot = 17U;     // 0x13f1a4..0x13f1e4
constexpr std::uint32_t kApproachBehind = 2U;   // 0x13f1e8..0x13f1ec, then 0x13f544
constexpr std::uint32_t kProfileFlag = 2U;      // 0x13f1f0..0x13f1f4
constexpr std::uint32_t kProfileRange = 5U;     // 0x13f1f8..0x13f208
constexpr std::uint32_t kProfileReject = 2U;    // 0x13f20c..0x13f210
constexpr std::uint32_t kApproachCall = 40U;    // 0x13f27c..0x13f318
constexpr std::uint32_t kAlongDot = 13U;        // 0x13f324..0x13f354
constexpr std::uint32_t kAlongTest = 11U;       // 0x13f358..0x13f380
constexpr std::uint32_t kFacing = 14U;          // 0x13f384..0x13f3b8
constexpr std::uint32_t kFacingFloor = 1U;      // 0x13f3bc
constexpr std::uint32_t kNear = 6U;             // 0x13f3c0..0x13f3d4
constexpr std::uint32_t kNearZero = 3U;         // 0x13f534..0x13f53c
constexpr std::uint32_t kRatio = 9U;            // 0x13f3d8..0x13f3f8
constexpr std::uint32_t kLevel = 13U;           // 0x13f3fc..0x13f42c
constexpr std::uint32_t kSlope = 8U;            // 0x13f430..0x13f44c
constexpr std::uint32_t kMode = 2U;             // 0x13f450..0x13f454
constexpr std::uint32_t kBiased = 20U;          // 0x13f458..0x13f4a4
constexpr std::uint32_t kDoubled = 7U;          // 0x13f4a8..0x13f4c0
constexpr std::uint32_t kRouteCall = 28U;       // 0x13f4c4..0x13f530 (through the branch after the call)
constexpr std::uint32_t kAfterCall = 2U;        // 0x13f31c..0x13f320
constexpr std::uint32_t kExitTail = 3U;         // 0x13f540 (ld ra) + 0x13f544 (jr, addiu sp)
constexpr std::uint32_t kReturnTail = 2U;       // 0x13f544 alone

// MAX.S on raw words, ordered as guest::ee_cop1::max_s orders them.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// A 32-bit float moved through a GPR (mfc1) and back into a VF (qmtc2): sign-extended, upper words kept.
Gpr128 widened(const std::uint32_t bits, const Gpr128& upper) {
    return Gpr128{{bits, (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U, upper.words[2], upper.words[3]}};
}

// vaddw.x vf1,vf0,vf0w: VF1.x = 1.0, the other lanes stay.
void load_one(R5900State& cpu) { ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false); }

// vadday.x acc,vf2,vf2y ; VF1 = restore ; vmaddz.x vf1,vf1,vf2z: with VF2 = the products and VF1.x = 1 in `restore`,
// VF1.x = (x + y) + z. Returns that sum.
std::uint32_t finish_dot3(R5900State& cpu, const Gpr128& restore) {
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    cpu.write_vu0_vector(1U, restore);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    return cpu.read_vu0_vector(1U).words[0];
}

// Normalises VF3 (xyz): Q = 1/sqrt(sum of squares), VF3 *= Q. Returns the length from sqrt.s of the same sum.
std::uint32_t normalise_vf3(R5900State& cpu) {
    load_one(cpu);
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);
    const std::uint32_t squared = finish_dot3(cpu, cpu.read_vu0_vector(1U));
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
    const std::uint32_t length = fpu::sqrt(squared);
    cpu.write_vu0_vector(1U, widened(length, cpu.read_vu0_vector(1U)));
    ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);
    return length;
}

template <class Context>
n3::Run project_neighbour_into_route_profile(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress vehicle = call.arg(0U);
    const GuestAddress neighbour = call.arg(1U);
    const bool approach = call.arg64(3U) != 0U;
    const bool variant = call.arg64(4U) != 0U;
    const std::uint64_t profile_id = call.arg64(5U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress physics = m.read_u32_le(vehicle + kPhysics);
    const GuestAddress holder = m.read_u32_le(physics + kHolder);
    const GuestAddress table = m.read_u32_le(holder + kTable);
    const GuestAddress neighbour_direction = neighbour + kNeighbourDirection;
    const std::uint32_t floor = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kFloorOffset);

    // d0 = Z[176] - Qa, and the sum of its squares.
    const Gpr128 probe_direction = n3::load_quad_any(m, call.arg(2U));
    load_one(cpu);
    const Gpr128 one_lane = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, probe_direction);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, holder + kHolderPoint));
    ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, n3::kMaskXyz, true);
    ee_cop2::vmul(cpu, 1U, 3U, 3U, n3::kMaskXyz);
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));
    const std::uint32_t squared = finish_dot3(cpu, one_lane);

    Gpr128 direction{};
    std::uint32_t reach = floor;
    std::uint32_t retired = kEntry;
    if (fpu::lt(squared, kTiny)) {
        direction = n3::load_quad_any(m, physics + kPhysicsDirection);
        retired += kShortDirection;
    } else {
        const std::uint32_t length = normalise_vf3(cpu);
        direction = cpu.read_vu0_vector(3U);
        retired += kLongDirection;
        if (fpu::lt(length, floor)) {
            retired += kFloorReach;
        } else {
            reach = length;
        }
    }

    // dp = V[48] - A[32]; the side limit.
    const Gpr128 neighbour_point = n3::load_quad_any(m, neighbour + kNeighbourPosition);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vehicle + kVehiclePosition));
    cpu.write_vu0_vector(4U, neighbour_point);
    ee_cop2::vadd_vector(cpu, 3U, 4U, 1U, n3::kMaskXyz, true);
    const Gpr128 delta = cpu.read_vu0_vector(3U);
    retired += kDelta;
    if (!fpu::le(fpu::abs(delta.words[1]), m.read_u32_le(kSideLimit))) {
        return call.finish(retired + kExitTail);
    }

    // range = |dp|, unit = dp / |dp|.
    const std::uint32_t range = normalise_vf3(cpu);
    const Gpr128 unit = cpu.read_vu0_vector(3U);
    retired += kNormalise;

    std::uint32_t level = 0U;
    std::uint32_t slope = 0U;
    if (approach) {
        retired += kApproachRange;
        if (!fpu::le(range, m.read_u32_le(kRangeLimitApproach))) {
            return call.finish(retired + kExitTail);
        }
        retired += kApproachDot;
        load_one(cpu);
        const Gpr128 saved = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(3U, unit);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, holder + kHolderAxis));
        ee_cop2::vmul(cpu, 2U, 1U, 3U, n3::kMaskXyz);
        const std::uint32_t behind = finish_dot3(cpu, saved);
        if (fpu::lt(behind, m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kBehindOffset))) {
            return call.finish(retired + kApproachBehind + kReturnTail);
        }

        // The approach call: probe = V[48] + Qa * scale, level = 0.5 * scale, both quads handed over in the frame.
        cpu.write_vu0_vector(2U, direction);
        load_one(cpu);
        const Gpr128 one_again = cpu.read_vu0_vector(1U);
        ee_cop2::vmul(cpu, 2U, 2U, 3U, n3::kMaskXyz);
        (void)finish_dot3(cpu, one_again);          // dead in the original, kept for the VU0 end state
        const std::uint32_t scale = m.read_u32_le(kProbeOffset);
        n3::store_quad_any(m, frame + kFrameBPoint, neighbour_point);
        cpu.write_vu0_vector(1U, probe_direction);
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, false);
        const Gpr128 two_saved = cpu.read_vu0_vector(2U);
        level = fpu::mul(kHalf, scale);
        cpu.write_vu0_vector(2U, widened(scale, one_again));
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
        ee_cop2::vadd_vector(cpu, 1U, 4U, 1U, n3::kMaskXyz, false);
        n3::store_quad_any(m, frame + kFrameProbe, cpu.read_vu0_vector(1U));
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + kFrameBPoint));
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, physics + kPhysicsAxis));
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, table + kTableCentre));
        ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, n3::kMaskXyz, true);
        ee_cop2::vmul(cpu, 2U, 2U, 1U, n3::kMaskXyz);
        const std::uint32_t along = finish_dot3(cpu, two_saved);

        call.set_arg(4U, neighbour_direction);
        call.set_arg64(1U, profile_id);
        call.set_arg(2U, frame + kFrameBPoint);
        call.set_arg(3U, frame + kFrameProbe);
        call.set_arg(5U, neighbour_direction);
        call.set_float_arg(0U, along);
        call.set_float_arg(1U, range);
        call.set_float_arg(2U, level);
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_0013f550(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        return call.finish(retired + kApproachCall + callee.retired + kAfterCall + kExitTail);
    }

    retired += kProfileFlag;
    if (variant) {
        retired += kProfileRange;
        if (!fpu::le(range, m.read_u32_le(kRangeLimitProfile))) {
            return call.finish(retired + kProfileReject + kExitTail);
        }
    }

    // along = P[6880] . dp
    load_one(cpu);
    const Gpr128 one_third = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(2U, delta);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, physics + kPhysicsAxis));
    ee_cop2::vmul(cpu, 2U, 1U, 2U, n3::kMaskXyz);
    const std::uint32_t along = finish_dot3(cpu, one_third);
    retired += kAlongDot;
    if (variant) {
        retired += kAlongTest;
        const std::uint32_t lower = fpu::div(fpu::neg(m.read_u32_le(physics + kPhysicsHalfReach)), kTwo);
        if (fpu::lt(along, lower)) {
            return call.finish(retired + kExitTail);
        }
    }

    // facing = max(direction . unit, floor)
    cpu.write_vu0_vector(3U, direction);
    cpu.write_vu0_vector(2U, unit);
    load_one(cpu);
    const Gpr128 one_fourth = cpu.read_vu0_vector(1U);
    ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);
    std::uint32_t facing = finish_dot3(cpu, one_fourth);
    retired += kFacing;
    if (fpu::lt(facing, floor)) {
        facing = floor;
        retired += kFacingFloor;
    }

    retired += kNear;
    if (fpu::lt(range, kTwelve)) {
        retired += kNearZero;
    } else {
        retired += kRatio;
        const std::uint32_t ratio = fpu::div(range, fpu::mul(facing, reach));
        if (fpu::lt(ratio, 0U)) {
            return call.finish(retired + kExitTail);
        }
        retired += kLevel;
        const std::uint32_t accumulated = fpu::add(0U, ratio);
        cpu.set_fpu_accumulator_bits(accumulated);
        level = fpu::msub(accumulated, kHalf, m.read_u32_le(kProbeOffset));
        if (!fpu::le(level, kEight)) {
            return call.finish(retired + kExitTail);
        }
        retired += kSlope;
        slope = fpu::mul(facing, fpu::mul(ratio, m.read_u32_le(holder + kHolderPointWeight)));
    }

    retired += kMode;
    if (variant) {
        retired += kBiased;
        const std::uint32_t base = fpu::add(0U, m.read_u32_le(kLevelBias2));
        cpu.set_fpu_accumulator_bits(base);
        const std::uint32_t lift = fpu::madd(base, kOne, m.read_u32_le(kLevelBias));
        const std::uint32_t raised = fpu::add(level, lift);
        const std::uint32_t tilted = fpu::add(slope, m.read_u32_le(kSlopeBias));
        const std::uint32_t tilt_base = fpu::add(0U, tilted);
        cpu.set_fpu_accumulator_bits(tilt_base);
        const std::uint32_t tilt = fpu::madd(tilt_base, kOne, m.read_u32_le(kSlopeBias2));
        level = max_bits(raised, 0U);
        slope = max_bits(tilt, 0U);
    } else {
        retired += kDoubled;
        level = fpu::mul(max_bits(level, 0U), kTwo);
    }

    // The route call: A's profile from V[48] to V[48] at `level`, slope / 1.5.
    const GuestAddress route_start = neighbour + kNeighbourPosition;
    load_one(cpu);
    const Gpr128 one_fifth = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, route_start));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, physics + kPhysicsAxis));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, table + kTableCentre));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
    ee_cop2::vmul(cpu, 2U, 3U, 1U, n3::kMaskXyz);
    const std::uint32_t reference = finish_dot3(cpu, one_fifth);
    call.set_arg64(1U, profile_id);
    call.set_arg(2U, route_start);
    call.set_arg(4U, neighbour_direction);
    call.set_arg(3U, route_start);
    call.set_arg(5U, neighbour_direction);
    call.set_float_arg(0U, reference);
    call.set_float_arg(1U, fpu::div(slope, kOneAndHalf));
    call.set_float_arg(2U, level);
    const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_0013f550(mm); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(retired + kRouteCall + callee.retired + kAfterCall + kExitTail);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013f090, 1212U, 0x475f4c205ea29ff2ULL, "ProjectNeighbourIntoRouteProfile", kCeiling,
         project_neighbour_into_route_profile, EngineReturn::Nothing)
