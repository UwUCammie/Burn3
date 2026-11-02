// 0x002202d0 (a0 = sphere quad x, y, z, r): ClassifySphereInCullingFrustum. Returns 0 when the sphere lies outside the
// culling frustum, 1 when it straddles the frustum and 2 when it lies inside. The frustum at 0x0067d5a0 has two depth
// planes (+0 and +16: nx, ny, nz, d) and two side sets of four planes stored transposed: the outer set (x +32, y +48,
// z +64, d +80) is tested with +r and the inner set (x +96, y +112, z +128, d +144) with -r. A side lane is the VU0
// chain d - X*x - Y*y - Z*z + radius. A set-wide sign bit (-0.0 included) in the outer set means outside; in the
// inner set it means the sphere reaches into the frustum (result 1). The lanes come from the VU0 macro helpers, so
// they round and clamp as the VU does. A depth plane's dot product
// X*nx + Y*ny + Z*nz is also VU0 work; dist = dot - d is FPU work: dist > r is outside, dist <= -r is inside this
// plane, anything else straddles.
//
// The VU0 state the original leaves behind (VF1..VF6, ACC and the MAC/STATUS flags of its last operation) is produced
// by the same VU0 operations, in the original's order, on the real register file. The stack copies of the quads are
// not reproduced: each one holds the value its quad was loaded from.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kFrustum = 0x0067d5a0U;       // lui 0x0068, addiu -10848
constexpr GuestAddress kDepthPlaneStride = 16U;      // the two depth planes at +0 and +16
constexpr GuestAddress kOuterSet = kFrustum + 32U;   // side planes tested with +r: x +32, y +48, z +64, d +80
constexpr GuestAddress kInnerSet = kFrustum + 96U;   // side planes tested with -r: x +96, y +112, z +128, d +144
constexpr std::uint32_t kSignBit = 0x80000000U;

constexpr std::uint32_t kOutside = 0U;
constexpr std::uint32_t kIntersect = 1U;
constexpr std::uint32_t kInside = 2U;

// VU0 destination masks: bit 3 is x, bit 0 is w (n3::kMaskW, n3::kMaskXyz and n3::kMaskXyzw come from n3_support.hpp).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskYz = 6U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kSideBlock = 32U;         // 0x002202d0..0x0022034c: frame, sphere copies, outer side test
constexpr std::uint32_t kSideOutsideExit = 2U;    // 0x00220350..0x00220354: v0 = 0, jump to the return
constexpr std::uint32_t kReturnBlock = 2U;        // 0x0022043c..0x00220440: jr $ra and its delay slot
constexpr std::uint32_t kDepthSetup = 6U;         // 0x00220358..0x0022036c: VF4 = sphere, VF2.x = 1, loop entry
constexpr std::uint32_t kPlaneTest = 3U;          // 0x002203c4..0x002203cc: the loop test before each plane
constexpr std::uint32_t kPlaneBody = 14U;         // 0x00220370..0x002203a4: the dot product and the r test
constexpr std::uint32_t kPlaneOutsideExit = 2U;   // 0x002203a8..0x002203ac: v0 = 0, jump to the return
constexpr std::uint32_t kPlaneNegativeTest = 3U;  // 0x002203b0..0x002203b8: the -r test and its delay slot
constexpr std::uint32_t kPlaneStraddle = 1U;      // 0x002203bc: v0 = 1, only when the plane is not inside
constexpr std::uint32_t kPlaneIncrement = 1U;     // 0x002203c0: the loop counter
constexpr std::uint32_t kLoopEnd = 3U;            // the loop test that ends the loop after the second plane
constexpr std::uint32_t kResultSelect = 3U;       // 0x002203d0..0x002203d8: v0 == 1 test and its delay slot
constexpr std::uint32_t kInnerTest = 23U;         // 0x002203dc..0x00220434: the inner side test
constexpr std::uint32_t kInnerNegative = 1U;      // 0x00220438: v0 = 1 when an inner lane is negative

enum class PlaneSide : std::uint8_t { Outside, Straddle, Inside };

// Whether a lane of the four has its sign bit set (the original's psraw/or test).
bool any_sign(const Gpr128& lanes) {
    return ((lanes.words[0] | lanes.words[1] | lanes.words[2] | lanes.words[3]) & kSignBit) != 0U;
}

// One set of four side planes against the sphere. Leaves VF2 = d, VF3 = x, VF4 = y, VF5 = z, VF6 = the sphere,
// ACC = d - X*x - Y*y - Z*z and VF1 = ACC + radius in every lane, and returns VF1.
template <class Memory>
Gpr128 side_lanes(guest::R5900State& cpu, const Memory& m, const GuestAddress set, const Gpr128& sphere,
                  const std::uint32_t radius) {
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, set + 48U));          // lqc2 $vf2, d
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, set));                // lqc2 $vf3, x
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, set + 16U));          // lqc2 $vf4, y
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, set + 32U));          // lqc2 $vf5, z
    cpu.write_vu0_vector(6U, sphere);                                   // lqc2 $vf6, the sphere quad
    cpu.write_vu0_vector(1U, Gpr128{{radius, 0U, 0U, 0U}});             // qmtc2 radius, $vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 1U, kMaskYz, 0U, false);       // vaddx.yz $vf1,$vf0,$vf1x
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 1U, n3::kMaskW, 0U);           // vmulx.w $vf1,$vf0,$vf1x
    ee_cop2::vadda_broadcast(cpu, 2U, 0U, n3::kMaskXyzw, 0U);           // vaddax.xyzw $acc,$vf2,$vf0x
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 6U, n3::kMaskXyzw, 0U, false, true, true);   // vmsubax $acc,$vf3,$vf6x
    ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 6U, n3::kMaskXyzw, 1U, false, true, true);   // vmsubay $acc,$vf4,$vf6y
    ee_cop2::vmadd_broadcast(cpu, 0U, 5U, 6U, n3::kMaskXyzw, 2U, false, true, true);   // vmsubaz $acc,$vf5,$vf6z
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyzw, 3U, false, false, false); // vmaddw $vf1,$vf1,$vf0w
    return cpu.read_vu0_vector(1U);
}

// One depth plane. VF3 = the plane's quad; the dot product X*nx + Y*ny + Z*nz comes out of VU0 in VF1.x (VF4 is the
// sphere and VF2.x = 1.0), and the plane distance dot - d is FPU work against the radius.
template <class Memory>
PlaneSide depth_plane(guest::R5900State& cpu, const Memory& m, const GuestAddress plane, const std::uint32_t radius,
                      const std::uint32_t negative_radius) {
    const Gpr128 quad = n3::load_quad_any(m, plane);
    cpu.write_vu0_vector(3U, quad);                                                     // lqc2 $vf3, plane
    ee_cop2::vmul(cpu, 1U, 4U, 3U, n3::kMaskXyz);                                       // vmul.xyz $vf1,$vf4,$vf3
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                                  // vadday.x $acc,$vf1,$vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, kMaskX, 2U, false, false, false);         // vmaddz.x $vf1,$vf2,$vf1z
    const std::uint32_t dot = cpu.read_vu0_vector(1U).words[0];
    const std::uint32_t distance = fpu::sub(dot, quad.words[3]);
    if (!fpu::le(distance, radius)) {
        return PlaneSide::Outside;
    }
    if (!fpu::le(distance, negative_radius)) {
        return PlaneSide::Straddle;
    }
    return PlaneSide::Inside;
}

template <class Context>
n3::Run classify_sphere(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    const auto& m = call.memory();

    const Gpr128 sphere = n3::load_quad_any(m, call.arg(0));   // lq $a1, 0($a0)
    const std::uint32_t radius = sphere.words[3];
    const std::uint32_t negative_radius = fpu::neg(radius);

    // The outer side planes: a lane below zero (or -0.0) puts the sphere outside the frustum.
    if (any_sign(side_lanes(cpu, m, kOuterSet, sphere, radius))) {
        call.return_value(kOutside);
        return call.finish(kSideBlock + kSideOutsideExit + kReturnBlock);
    }

    cpu.write_vu0_vector(4U, sphere);                                     // lqc2 $vf4, the sphere quad
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, false);          // vaddw.x $vf2,$vf0,$vf0w

    // The two depth planes: the loop has a fixed count of two (slti i, 2), so it is straight code.
    std::uint32_t retired = kSideBlock + kDepthSetup;
    std::uint32_t result = kInside;
    for (GuestAddress plane = kFrustum; plane < kFrustum + 2U * kDepthPlaneStride; plane += kDepthPlaneStride) {
        retired += kPlaneTest + kPlaneBody;
        switch (depth_plane(cpu, m, plane, radius, negative_radius)) {
        case PlaneSide::Outside:
            call.return_value(kOutside);
            return call.finish(retired + kPlaneOutsideExit + kReturnBlock);
        case PlaneSide::Straddle:
            result = kIntersect;
            retired += kPlaneNegativeTest + kPlaneStraddle + kPlaneIncrement;
            break;
        case PlaneSide::Inside:
            retired += kPlaneNegativeTest + kPlaneIncrement;
            break;
        }
    }
    retired += kLoopEnd + kResultSelect;
    if (result == kIntersect) {
        call.return_value(kIntersect);
        return call.finish(retired + kReturnBlock);
    }

    // The inner side planes, tested with -r: a negative lane means the sphere reaches into the frustum.
    const bool reaches_inside = any_sign(side_lanes(cpu, m, kInnerSet, sphere, negative_radius));
    call.return_value(reaches_inside ? kIntersect : kInside);
    return call.finish(retired + kInnerTest + (reaches_inside ? kInnerNegative : 0U) + kReturnBlock);
}

} // namespace

} // namespace takedown::runtime

// Longest path: both depth planes inside and a negative inner lane (112 original instructions).
N3_ENTRY_PLAIN(002202d0, 372U, 0x783167a9ff7a407aULL, "ClassifySphereInCullingFrustum", 112U, classify_sphere,
               EngineReturn::Integer)
