// 0x0013e920 (a0 = vehicle A): ProjectNeighboursIntoProfile3. Walks the list of other vehicles (count at
// 0x0064517c, object pointers at 0x00645070 + 4 i) and, for each one O that is ahead of A inside a lateral and a
// distance window, projects its path into A's profile id 3 with ProjectRouteIntoProfile (0x0013f550).
//
// Names: P = A->[1108] (A's physics record, whose [9264] is Z and Z->[516] is W, the quad record at W+48 is A's
// road position), WO = O->[516] (O's body record: quads at +0/+16/+32 are its axes, +48 its position). The quad at
// O+176 is O's velocity (w lane = its speed scale).
//
//   1. d = Z[176] - (WO+32) * O[188]. dir/reach: when |d|^2 is below 2^-32, dir = quad P+32 and reach = the
//      minimum reach R (float at $gp-31236, read once before the loop); otherwise dir = d / |d| and reach =
//      max(|d|, R).
//   2. dp = (WO+48) - (A+32). An element is skipped when |dp.y| exceeds the height window (0x01e75534), or when
//      dot(P+6880, dp) is below the negated P[9272].
//   3. dist = |dp| and u = dp / dist. A distance below 12 projects at once with level 0; otherwise cs = dot(u, dir)
//      must not be below R, the level is dist / (cs * reach) - 0.5 * G6 (G6 = float 0x01e75538, never below 0)
//      and must be at most 8, else the element is skipped.
//   4. Pa = WO[48] + O[176] * level and Pb = Pa + O[176] * (level + G6) go to the frame with copies of WO's
//      quads; ProjectRouteIntoProfile gets (A, 3, &Pa, &Pb, &WO[32] copy twice, box, dot(P+6880, Pa - W[48]), dist,
//      level). The box is the record at 0x00527440 + 0x12880 + 384 * (signed byte O[9259]), plus 3712.
//
// Vector work runs on the VU0 registers exactly as the original's macro-mode sequences do (dot product =
// vmul.xyz, vadday.x, vmaddz.x with VF1.x = 1; unit vector = vrsqrt and vmulq, the length from sqrt.s), so the
// results, the VU0 registers and the accumulators match. Stores: only frame quads (no other memory is written
// here); the callee reads them through the pointers.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

N3_CALLEE(0013f550)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 624U;
constexpr std::uint32_t kCeiling = 100000U;
constexpr std::uint8_t kMaskX = 8U;

// Globals.
constexpr GuestAddress kListCount = 0x0064517cU;     // 0x522660 + 0x122b1c
constexpr GuestAddress kListBase = 0x00645070U;      // 0x522660 + 0x122a10
constexpr GuestAddress kStripBoxes = 0x00539cc0U;    // 0x527440 + 0x12880: 384-byte records
constexpr GuestAddress kHeightWindow = 0x01e75534U;  // lui 0x1e7, +21812
constexpr GuestAddress kLevelStep = 0x01e75538U;     // lui 0x1e7, +21816
constexpr std::uint32_t kGpReach = 0U - 31236U;      // $gp - 31236: the minimum reach R

// Record fields.
constexpr GuestAddress kPhysics = 1108U;        // A: P
constexpr GuestAddress kPosition = 32U;         // A: quad
constexpr GuestAddress kRoadChain = 9264U;      // P: Z
constexpr GuestAddress kRoadLink = 516U;        // Z, O: body record
constexpr GuestAddress kSideAxis = 6880U;       // P: quad
constexpr GuestAddress kSideLimit = 9272U;      // P: float
constexpr GuestAddress kForward = 32U;          // P: quad (the direction for a degenerate offset)
constexpr GuestAddress kVelocity = 176U;        // O, Z: quad
constexpr GuestAddress kBoxId = 9259U;          // O: signed byte
constexpr GuestAddress kBoxSize = 384U;
constexpr GuestAddress kBoxField = 3712U;

// Frame layout the callee reads (offsets from the entry sp minus 624).
constexpr GuestAddress kAxesA = 496U;           // WO[0], WO[16], WO[32], Pb
constexpr GuestAddress kQ5 = 528U;              // WO[32] (a copy), t1
constexpr GuestAddress kQ3 = 544U;              // Pb, a3
constexpr GuestAddress kAxesB = 560U;           // WO[0], WO[16], WO[32], Pa
constexpr GuestAddress kQ4 = 592U;              // WO[32], t0
constexpr GuestAddress kPoint = 608U;           // Pa, a2

constexpr std::uint32_t kTiny = 0x2f800000U;    // 2^-32
constexpr std::uint32_t kNear = 0x41400000U;    // 12.0
constexpr std::uint32_t kHalf = 0x3f000000U;    // 0.5
constexpr std::uint32_t kMostLevel = 0x41000000U;  // 8.0
constexpr std::uint32_t kProfileId = 3U;

// Retired counts from the listing (delay slots included).
constexpr std::uint32_t kEntryCost = 34U;
constexpr std::uint32_t kHeadCost = 7U;
constexpr std::uint32_t kReturnCost = 15U;
constexpr std::uint32_t kNextCost = 2U;         // 0x13ed00: i++ and the branch back
constexpr std::uint32_t kOffsetCost = 29U;      // 0x13e9c4..: d and |d|^2, the tiny test
constexpr std::uint32_t kTinyCost = 4U;         // 0x13ea38: dir = P+32, reach = R
constexpr std::uint32_t kUnitCost = 23U;        // 0x13ea48: dir = d / |d|, the reach test
constexpr std::uint32_t kClampCost = 1U;        // 0x13eaa4: reach = R
constexpr std::uint32_t kWindowCost = 12U;      // 0x13eaa8: dp and the height test
constexpr std::uint32_t kLateralCost = 16U;     // 0x13ead8: the lateral test
constexpr std::uint32_t kDistanceCost = 23U;    // 0x13eb18: |dp|, u and the near test
constexpr std::uint32_t kNearCost = 2U;         // 0x13eb74: level 0, to the call
constexpr std::uint32_t kAngleCost = 14U;       // 0x13eb7c: cs and its test
constexpr std::uint32_t kRatioCost = 8U;        // 0x13ebb4: the quotient and its test
constexpr std::uint32_t kLevelCost = 9U;        // 0x13ebd4: the level and the lower clamp test
constexpr std::uint32_t kLevelClampCost = 1U;   // 0x13ebf8: level = 0
constexpr std::uint32_t kUpperCost = 3U;        // 0x13ebfc: the upper limit test
constexpr std::uint32_t kCallCost = 62U;        // 0x13ec08: the points, the arguments, jal and its delay slot

// C.cond.S leaves its result in the FCR31 condition bit.
bool compare(guest::R5900State& cpu, const bool result) {
    cpu.set_fcr31_bits(result ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                              : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
    return result;
}

std::uint32_t sign_word(const std::uint32_t value) { return (value & 0x80000000U) != 0U ? 0xffffffffU : 0U; }

// vmul.xyz vf2,a,b ; vadday.x acc,vf2,vf2y ; vf1 = one ; vmaddz.x vf1,vf1,vf2z: the dot product of a and b in
// VF1.x, where `one` is VF1 with its x lane set to 1.0.
std::uint32_t dot3(guest::R5900State& cpu, const unsigned a, const unsigned b, const Gpr128& one) {
    ee_cop2::vmul(cpu, 2U, a, b, n3::kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    cpu.write_vu0_vector(1U, one);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    return cpu.read_vu0_vector(1U).words[0];
}

struct Unit {
    std::uint32_t length;   // sqrt.s of the squared length
    Gpr128 vector;          // the vector times Q = 1 / sqrt(squared length), as VF3 leaves it
};

// vf3 -> its length and its unit vector (0x13ea48.. and 0x13eb18..).
Unit normalise(guest::R5900State& cpu) {
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
    const Gpr128 one = cpu.read_vu0_vector(1U);
    const std::uint32_t squared = dot3(cpu, 3U, 3U, one);
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                      // vrsqrt q,vf0w,vf1x
    const std::uint32_t length = fpu::sqrt(squared);
    Gpr128 length_quad = cpu.read_vu0_vector(1U);                  // mfc1 a0,f0 ; qmtc2 a0,vf1
    length_quad.words[0] = length;
    length_quad.words[1] = sign_word(length);
    cpu.write_vu0_vector(1U, length_quad);
    ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);   // vmulq.xyz vf3,vf3,q
    return Unit{length, cpu.read_vu0_vector(3U)};
}

template <class Context>
n3::Run project_neighbours(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress vehicle = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t reach_floor = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kGpReach);

    std::uint32_t retired = kEntryCost;
    for (std::int32_t index = 0;; ++index) {
        retired += kHeadCost;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        if (index >= static_cast<std::int32_t>(m.read_u32_le(kListCount))) {
            break;
        }
        const GuestAddress other = m.read_u32_le(kListBase + 4U * static_cast<std::uint32_t>(index));
        const GuestAddress other_body = m.read_u32_le(other + kRoadLink);
        const GuestAddress physics = m.read_u32_le(vehicle + kPhysics);

        // d = Z[176] - (WO+32) * O[188]
        const Gpr128 velocity = n3::load_quad_any(m, other + kVelocity);
        const std::uint32_t speed = velocity.words[3];  // lw 188(sp): the stored copy's w lane
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
        const Gpr128 one = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(2U, Gpr128{{speed, sign_word(speed), velocity.words[2], velocity.words[3]}});
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, other_body + 32U));
        ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U);    // vmulx.xyz vf2,vf1,vf2x
        const GuestAddress chain = m.read_u32_le(physics + kRoadChain);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, chain + kVelocity));
        ee_cop2::vadd_vector(cpu, 3U, 1U, 2U, n3::kMaskXyz, true);     // vsub.xyz vf3,vf1,vf2
        const std::uint32_t offset_squared = dot3(cpu, 3U, 3U, one);

        std::uint32_t used = kOffsetCost;
        Gpr128 direction;
        std::uint32_t reach;
        if (compare(cpu, fpu::lt(offset_squared, kTiny))) {
            direction = n3::load_quad_any(m, physics + kForward);
            reach = reach_floor;
            used += kTinyCost;
        } else {
            const Unit unit = normalise(cpu);
            direction = unit.vector;
            reach = unit.length;
            used += kUnitCost;
            if (compare(cpu, fpu::lt(reach, reach_floor))) {
                reach = reach_floor;
                used += kClampCost;
            }
        }

        // dp = WO[48] - A[32]; the height window.
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, other_body + 48U));
        const std::uint32_t height_window = m.read_u32_le(kHeightWindow);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, vehicle + kPosition));
        ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, n3::kMaskXyz, true);     // vsub.xyz vf3,vf2,vf1
        used += kWindowCost;
        if (!compare(cpu, fpu::le(fpu::abs(cpu.read_vu0_vector(3U).words[1]), height_window))) {
            retired += used + kNextCost;
            continue;
        }

        // The lateral window.
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
        const Gpr128 one_lateral = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, physics + kSideAxis));
        const std::uint32_t lateral = dot3(cpu, 1U, 3U, one_lateral);
        const std::uint32_t lateral_limit = fpu::neg(m.read_u32_le(physics + kSideLimit));
        used += kLateralCost;
        if (compare(cpu, fpu::lt(lateral, lateral_limit))) {
            retired += used + kNextCost;
            continue;
        }

        // dist = |dp|, u = dp / dist, and the level.
        const Unit toward = normalise(cpu);
        const std::uint32_t distance = toward.length;
        std::uint32_t level = 0U;
        used += kDistanceCost;
        if (compare(cpu, fpu::lt(distance, kNear))) {
            used += kNearCost;
        } else {
            cpu.write_vu0_vector(2U, direction);
            ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
            const Gpr128 one_angle = cpu.read_vu0_vector(1U);
            const std::uint32_t angle = dot3(cpu, 3U, 2U, one_angle);
            used += kAngleCost;
            if (compare(cpu, fpu::lt(angle, reach_floor))) {
                retired += used + kNextCost;
                continue;
            }
            const std::uint32_t ratio = fpu::div(distance, fpu::mul(angle, reach));
            used += kRatioCost;
            if (compare(cpu, fpu::lt(ratio, 0U))) {
                retired += used + kNextCost;
                continue;
            }
            const std::uint32_t accumulator = fpu::add(0U, ratio);                  // adda.s f0,f2
            cpu.set_fpu_accumulator_bits(accumulator);
            level = fpu::msub(accumulator, kHalf, m.read_u32_le(kLevelStep));        // msub.s f14,f21,f1
            used += kLevelCost;
            if (compare(cpu, fpu::lt(level, 0U))) {
                level = 0U;
                used += kLevelClampCost;
            }
            used += kUpperCost;
            if (!compare(cpu, fpu::le(level, kMostLevel))) {
                retired += used + kNextCost;
                continue;
            }
        }

        // Pa = WO[48] + O[176] * level, Pb = Pa + O[176] * (level + G6); the quads the callee reads.
        const std::uint32_t level_step = m.read_u32_le(kLevelStep);
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
        const Gpr128 one_point = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(3U, Gpr128{{level, sign_word(level), 0U, 0U}});
        cpu.write_vu0_vector(2U, Gpr128{{fpu::add(level, level_step), 0U, 0U, 0U}});
        const Gpr128 axis_x = n3::load_quad_any(m, other_body);
        const Gpr128 axis_y = n3::load_quad_any(m, other_body + 16U);
        const Gpr128 axis_z = n3::load_quad_any(m, other_body + 32U);
        const Gpr128 origin = n3::load_quad_any(m, other_body + 48U);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, other + kVelocity));
        cpu.write_vu0_vector(4U, origin);
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, n3::kMaskXyz, 0U);    // vmulx.xyz vf1,vf1,vf3x
        ee_cop2::vadd_vector(cpu, 4U, 4U, 1U, n3::kMaskXyz, false);    // vadd.xyz vf4,vf4,vf1
        const Gpr128 near_point = cpu.read_vu0_vector(4U);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, other + kVelocity));
        ee_cop2::vmove(cpu, 3U, 4U, n3::kMaskXyzw);                    // vmove.xyzw vf3,vf4
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);    // vmulx.xyz vf1,vf1,vf2x
        ee_cop2::vadd_vector(cpu, 3U, 4U, 1U, n3::kMaskXyz, false);    // vadd.xyz vf3,vf4,vf1
        const Gpr128 far_point = cpu.read_vu0_vector(3U);

        n3::store_quad_any(m, frame + kAxesB, axis_x);
        n3::store_quad_any(m, frame + kAxesB + 16U, axis_y);
        n3::store_quad_any(m, frame + kQ4, axis_z);
        n3::store_quad_any(m, frame + kAxesA, axis_x);
        n3::store_quad_any(m, frame + kAxesA + 16U, axis_y);
        n3::store_quad_any(m, frame + kQ5, axis_z);
        n3::store_quad_any(m, frame + kPoint, near_point);
        n3::store_quad_any(m, frame + kQ3, far_point);

        // f12 = dot(P+6880, Pa - W[48]).
        const GuestAddress road_link = m.read_u32_le(m.read_u32_le(physics + kRoadChain) + kRoadLink);
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, physics + kSideAxis));
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, road_link + 48U));
        ee_cop2::vadd_vector(cpu, 1U, 4U, 1U, n3::kMaskXyz, true);     // vsub.xyz vf1,vf4,vf1
        const std::uint32_t side = dot3(cpu, 2U, 1U, one_point);

        const auto box_id = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(other + kBoxId)));
        const GuestAddress box = m.read_u32_le(kStripBoxes + static_cast<std::uint32_t>(box_id) * kBoxSize);
        call.set_arg(0, vehicle);
        call.set_arg(1, kProfileId);
        call.set_arg(2, frame + kPoint);
        call.set_arg(3, frame + kQ3);
        call.set_arg(4, frame + kQ4);
        call.set_arg(5, frame + kQ5);
        call.set_arg(6, box + kBoxField);
        call.set_float_arg(0, side);
        call.set_float_arg(1, distance);
        call.set_float_arg(2, level);
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0013f550(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += used + kCallCost + callee.retired + kNextCost;
    }
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013e920, 1060U, 0xfa98e45a74296766ULL, "ProjectNeighboursIntoProfile3", kCeiling, project_neighbours,
         EngineReturn::Nothing)
