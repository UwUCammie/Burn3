// 0x00272a30 (a0 = pointer to the sphere pointer, a1 = segment): SegmentHitsFacingSphere. The segment's end points are
// the quadwords at a1 (A) and a1 + 16 (B). The sphere record S = *a0 holds its centre C (xyz) and radius r (w) at S + 0
// and a direction D (xyz) at S + 16. Returns 1 when the segment touches the sphere and dot(C - B, D) < 0, else 0:
//   - a point segment (|B - A|^2 below 2^-32) touches when |C - B|^2 <= r * r;
//   - a longer segment touches when ClassifySegmentAgainstSphere (0x002bf5f0) returns non-zero.
// The original does this work on VU0 (macro mode). It is replayed with the same operations in the same order, so
// vf1..vf3, the accumulator and the MAC/STATUS flags end as the original leaves them. The stack copies it makes of
// C - B, the squares and the radius are C++ locals. Every comparison is ordered: a NaN fails it.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002bf5f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// VU0 destination masks (x is the high bit) and the broadcast lanes.
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kW = 1U;
constexpr std::size_t kLaneY = 1U;
constexpr std::size_t kLaneZ = 2U;
constexpr std::size_t kLaneW = 3U;
constexpr unsigned kZero = 0U;                       // vf0 = (0, 0, 0, 1)

constexpr std::uint32_t kPointLimit = 0x2f800000U;   // 2^-32: the squared length below which a segment is a point
constexpr std::uint32_t kFrameBytes = 352U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 25U;       // 0x272a30..0x272a90: |B - A|^2 and the point test
constexpr std::uint32_t kPointCost = 20U;       // 0x272a94..0x272ae0: |C - B|^2 against r * r
constexpr std::uint32_t kPointMissCost = 2U;    // 0x272ae4..0x272ae8: the point misses, result 0
constexpr std::uint32_t kCallCost = 5U;         // 0x272b3c..0x272b4c: the call to ClassifySegmentAgainstSphere
constexpr std::uint32_t kNoHitCost = 1U;        // 0x272b68: result 0 when the classification misses
constexpr std::uint32_t kEndCost = 6U;          // 0x272b50..0x272b64: C - B for a segment that touches
constexpr std::uint32_t kDirectionCost = 16U;   // 0x272aec..0x272b28: dot(C - B, D) and its test
constexpr std::uint32_t kResultCost = 2U;       // 0x272b2c..0x272b38: the result arm (1 or 0) and its branch
constexpr std::uint32_t kReturnCost = 5U;       // 0x272b6c..0x272b7c: restore s0, s1, ra and sp, return

// VADDw.x fd, vf0, vf0w: fd.x = 1.0; the other lanes of fd are kept.
void set_one(guest::R5900State& cpu, const unsigned fd) {
    ee_cop2::vadd_broadcast(cpu, fd, kZero, kZero, kX, kLaneW, false);
}

// The sum of the x, y and z lanes of vf(prod), added the way the VU does it: ACC.x = prod.x + prod.y, then
// vf1.x = ACC.x + base.x * prod.z with base.x = 1.0. vf1 holds `base` for that step, so its y, z and w lanes are
// the ones `base` gives. Returns vf1.
Gpr128 sum_xyz(guest::R5900State& cpu, const unsigned prod, const Gpr128& base) {
    ee_cop2::vadda_broadcast(cpu, prod, prod, kX, kLaneY);
    cpu.write_vu0_vector(1U, base);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, prod, kX, kLaneZ, false, false, false);
    return cpu.read_vu0_vector(1U);
}

// dot(E, D) < 0 for E = C - B (centre_minus_end) and the direction D. The VU0 steps are the original's: vf1.x = 1.0
// (its other lanes kept), vf2 = E, vf1 = D, vf2.xyz = E * D, then the sum over vf2. Returns true for a negative dot.
bool points_against_direction(guest::R5900State& cpu, const Gpr128& centre_minus_end, const Gpr128& direction) {
    set_one(cpu, 1U);
    const Gpr128 kept = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(2U, centre_minus_end);
    cpu.write_vu0_vector(1U, direction);
    ee_cop2::vmul(cpu, 2U, 2U, 1U, kXyz);
    return fpu::lt(sum_xyz(cpu, 2U, kept).words[0], 0U);
}

template <class Context>
n3::Run segment_hits_facing_sphere(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& memory = call.memory();

    const GuestAddress sphere_slot = call.arg(0);
    const GuestAddress segment = call.arg(1);
    const std::uint64_t segment_register = call.arg64(1);   // a1 as a whole register: the original copies it to a0
    const GuestAddress sphere = memory.read_u32_le(sphere_slot);
    const Gpr128 start = n3::load_quad_any(memory, segment);
    const Gpr128 end = n3::load_quad_any(memory, segment + 16U);

    // |B - A|^2 in vf1.x: the squared differences sit in vf2 until the sum is taken.
    set_one(cpu, 1U);
    const Gpr128 one_x = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(3U, end);
    cpu.write_vu0_vector(2U, start);
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, kXyz, true);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, kXyz);
    const std::uint32_t length_squared = sum_xyz(cpu, 2U, one_x).words[0];

    if (fpu::lt(length_squared, kPointLimit)) {
        // A point: |C - B|^2 against r * r. one_squares (vf2 with x = 1.0) is the base the sum below starts from.
        set_one(cpu, 2U);
        const Gpr128 one_squares = cpu.read_vu0_vector(2U);
        cpu.write_vu0_vector(1U, end);
        cpu.write_vu0_vector(2U, n3::load_quad_any(memory, sphere));
        ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kXyz, true);
        const Gpr128 centre_minus_end = cpu.read_vu0_vector(1U);
        ee_cop2::vmul(cpu, 3U, 1U, 1U, kXyz);
        const std::uint32_t distance_squared = sum_xyz(cpu, 3U, one_squares).words[0];
        ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kW, kLaneW);
        const std::uint32_t radius_squared = cpu.read_vu0_vector(1U).words[3];
        if (!fpu::le(distance_squared, radius_squared)) {
            call.return_value(0U);
            return call.finish(kEntryCost + kPointCost + kPointMissCost + kReturnCost);
        }
        const bool facing = points_against_direction(cpu, centre_minus_end, n3::load_quad_any(memory, sphere + 16U));
        call.return_value(facing ? 1U : 0U);
        return call.finish(kEntryCost + kPointCost + kDirectionCost + kResultCost + kReturnCost);
    }

    // A longer segment: ClassifySegmentAgainstSphere (a0 = the segment, a1 = the sphere) reports whether it touches.
    call.set_arg64(0, segment_register);
    call.set_arg(1, sphere);
    const n3::Run classify = call.invoke([](auto& m) { return n3::fn_002bf5f0(m); }, kFrameBytes);
    if (classify.declined) {
        return call.decline();
    }
    if (call.result() == 0U) {
        call.return_value(0U);
        return call.finish(kEntryCost + kCallCost + kNoHitCost + kReturnCost + classify.retired);
    }
    // E = C - B again. The callee writes no guest memory, so `end` still holds B and the sphere is read as before.
    cpu.write_vu0_vector(1U, end);
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, sphere));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kXyz, true);
    const Gpr128 centre_minus_end = cpu.read_vu0_vector(1U);
    const bool facing = points_against_direction(cpu, centre_minus_end, n3::load_quad_any(memory, sphere + 16U));
    call.return_value(facing ? 1U : 0U);
    return call.finish(kEntryCost + kCallCost + kEndCost + kDirectionCost + kResultCost + kReturnCost +
                       classify.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00272a30, 336U, 0x794f3d1f2d580edeULL, "SegmentHitsFacingSphere", kCeiling, segment_hits_facing_sphere,
         EngineReturn::Integer)
