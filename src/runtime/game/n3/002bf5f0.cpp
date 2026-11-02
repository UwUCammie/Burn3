// 0x002bf5f0 (a0 = segment, a1 = sphere): ClassifySegmentAgainstSphere. The segment's end points A and B are the
// quadwords at a0 and a0 + 16, the sphere is the quadword at a1 (centre xyz, radius in w). The result (v0) is
//   2  both end points are inside the sphere (squared distance to the centre <= r * r),
//   1  one end point is inside, or the point of the segment closest to the centre (t = dot(C - A, e) / dot(e, e),
//      e = B - A, only when 0 <= t <= 1) is within the radius,
//   0  the segment misses the sphere.
// All comparisons are ordered (a NaN distance fails them and falls to the later tests). The vector work runs on the
// VU0 registers exactly as the original does: vf1..vf3, the accumulator and the MAC flags it leaves behind are
// visible state, so each step is done with the same VU0 operation on the same register (the lanes the original
// leaves untouched keep their old contents). The copies the original keeps on its stack (sphere at sp + 256,
// A at sp, C - A at sp + 240, squares at sp + 96) are C++ locals.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kW = 1U;
constexpr std::size_t kLaneY = 1U;
constexpr std::size_t kLaneZ = 2U;
constexpr std::size_t kLaneW = 3U;
constexpr unsigned kZero = 0U;   // vf0 = (0, 0, 0, 1)

constexpr std::uint32_t kOneBits = 0x3f800000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 36U;        // 0x2bf5f0..0x2bf67c, up to the first bc1f
constexpr std::uint32_t kBothTestCost = 3U;      // 0x2bf680: second compare
constexpr std::uint32_t kBothInsideCost = 2U;    // 0x2bf68c: result 2
constexpr std::uint32_t kEndTestCost = 3U;       // 0x2bf694: B inside?
constexpr std::uint32_t kStartTestCost = 3U;     // 0x2bf6a0: A inside?
constexpr std::uint32_t kInsideResultCost = 2U;  // 0x2bf788 / 0x2bf790: result 1
constexpr std::uint32_t kClosestCost = 28U;      // 0x2bf6ac..0x2bf718: e, dot products, divide, t < 0 test
constexpr std::uint32_t kUpperTestCost = 6U;     // 0x2bf71c: t <= 1 test
constexpr std::uint32_t kPointCost = 19U;        // 0x2bf734: closest point, distance, compare
constexpr std::uint32_t kNearResultCost = 2U;    // 0x2bf780: result 1
constexpr std::uint32_t kMissResultCost = 1U;    // 0x2bf798: result 0
constexpr std::uint32_t kReturnCost = 2U;        // 0x2bf79c: jr / frame pop

// vf(fd) = (1.0, ...): VADDw.x fd, vf0, vf0w.
void set_one(guest::R5900State& cpu, const unsigned fd) {
    ee_cop2::vadd_broadcast(cpu, fd, kZero, kZero, kX, kLaneW, false);
}

// The tail every three-lane dot product shares: acc.x = prod.x + prod.y, then x of vf1 (loaded with `one`, whose
// x is 1.0) = acc.x + 1.0 * prod.z. Returns the full vf1 quadword that QMFC2 reads.
Gpr128 finish_dot(guest::R5900State& cpu, const unsigned prod, const Gpr128& one) {
    ee_cop2::vadda_broadcast(cpu, prod, prod, kX, kLaneY);
    cpu.write_vu0_vector(1U, one);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, prod, kX, kLaneZ, false, false, false);
    return cpu.read_vu0_vector(1U);
}

template <class Context>
n3::Run classify_segment_against_sphere(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const GuestAddress segment = call.arg(0);
    const GuestAddress sphere_address = call.arg(1);

    const Gpr128 sphere = n3::load_quad_any(m, sphere_address);
    const Gpr128 end_b = n3::load_quad_any(m, segment + 16U);
    const Gpr128 start_a = n3::load_quad_any(m, segment);

    // |C - B|^2 and r * r (VMULw.w leaves the squared components in xyz of vf1).
    cpu.write_vu0_vector(1U, end_b);
    set_one(cpu, 2U);
    cpu.write_vu0_vector(3U, sphere);
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, kXyz, true);
    ee_cop2::vmul(cpu, 1U, 1U, 1U, kXyz);
    const Gpr128 squares_b = cpu.read_vu0_vector(1U);
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 3U, kW, kLaneW);
    const std::uint32_t radius_squared = cpu.read_vu0_vector(1U).words[3];
    const Gpr128 one_b = cpu.read_vu0_vector(2U);
    cpu.write_vu0_vector(2U, squares_b);
    const std::uint32_t dist_b = finish_dot(cpu, 2U, one_b).words[0];

    // |C - A|^2; C - A is kept for the closest-point dot product.
    set_one(cpu, 1U);
    const Gpr128 one_a = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, start_a);
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, kXyz, true);
    const Gpr128 centre_minus_a = cpu.read_vu0_vector(1U);
    ee_cop2::vmul(cpu, 2U, 1U, 1U, kXyz);
    const std::uint32_t dist_a = finish_dot(cpu, 2U, one_a).words[0];

    const bool end_inside = fpu::le(dist_b, radius_squared);
    const bool start_inside = fpu::le(dist_a, radius_squared);

    std::uint32_t retired = kEntryCost;
    std::uint32_t result = 0U;
    if (end_inside) {
        retired += kBothTestCost;
        if (start_inside) {
            call.return_value(2U);
            return call.finish(retired + kBothInsideCost + kReturnCost);
        }
    }
    retired += kEndTestCost;
    if (end_inside) {
        call.return_value(1U);
        return call.finish(retired + kInsideResultCost + kReturnCost);
    }
    retired += kStartTestCost;
    if (start_inside) {
        call.return_value(1U);
        return call.finish(retired + kInsideResultCost + kReturnCost);
    }

    // Neither end point is inside: the closest point of the segment to the centre.
    retired += kClosestCost;
    set_one(cpu, 2U);
    const Gpr128 one_n = cpu.read_vu0_vector(2U);
    cpu.write_vu0_vector(1U, start_a);
    cpu.write_vu0_vector(2U, end_b);
    cpu.write_vu0_vector(3U, centre_minus_a);
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, true);                 // e = B - A
    ee_cop2::vmul(cpu, 3U, 3U, 2U, kXyz);                              // (C - A) * e
    const std::uint32_t numerator = finish_dot(cpu, 3U, one_n).words[0];
    set_one(cpu, 1U);
    const Gpr128 one_d = cpu.read_vu0_vector(1U);
    ee_cop2::vmul(cpu, 3U, 2U, 2U, kXyz);                              // e * e
    const Gpr128 den_quad = finish_dot(cpu, 3U, one_d);
    const std::uint32_t t = fpu::div(numerator, den_quad.words[0]);

    if (fpu::lt(t, 0U)) {
        call.return_value(0U);
        return call.finish(retired + kMissResultCost + kReturnCost);
    }
    retired += kUpperTestCost;
    if (!fpu::le(t, kOneBits)) {
        call.return_value(0U);
        return call.finish(retired + kMissResultCost + kReturnCost);
    }

    // P = A + e * t; the squared distance from P to the centre against r * r. MTC2 of the sign-extended t keeps the
    // upper two lanes of the register QMFC2 last read (vf1 after the denominator).
    retired += kPointCost;
    cpu.write_vu0_vector(3U, start_a);
    cpu.write_vu0_vector(1U, Gpr128{{t, (t & 0x80000000U) != 0U ? 0xffffffffU : 0U, den_quad.words[2],
                                     den_quad.words[3]}});
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, kXyz, 0U);                // e * t
    cpu.write_vu0_vector(2U, sphere);
    ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, kXyz, false);                // A + e * t
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, kXyz, true);                 // - C
    ee_cop2::vmul(cpu, 2U, 1U, 1U, kXyz);
    set_one(cpu, 1U);
    const Gpr128 one_p = cpu.read_vu0_vector(1U);
    const std::uint32_t dist_p = finish_dot(cpu, 2U, one_p).words[0];

    if (fpu::le(dist_p, radius_squared)) {
        result = 1U;
        retired += kNearResultCost;
    } else {
        retired += kMissResultCost;
    }
    call.return_value(result);
    return call.finish(retired + kReturnCost);
}

}  // namespace
}  // namespace takedown::runtime

N3_ENTRY_PLAIN(002bf5f0, 436U, 0xd2cd8025702e8459ULL, "ClassifySegmentAgainstSphere", 102U,
               classify_segment_against_sphere, EngineReturn::Integer)
