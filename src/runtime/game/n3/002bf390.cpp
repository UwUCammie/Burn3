// 0x002bf390 (a0 = sphere quadword (cx, cy, cz, r), a1 = clip volume): SphereVsClipVolumeClassify. Returns v0 = 0
// when the sphere is outside, 1 when it intersects and 2 when it is fully inside (EngineReturn::Integer). The volume
// holds two planes (nx, ny, nz, d) at +0 and +16, the basis vectors A, B, C at +32, +48, +64 and the offset D at +80.
// Plane i gives dist = n . centre - d in VU0: dist > r is outside; dist <= -r keeps v0 as it is, otherwise v0 = 1.
// Then p = cx*A + cy*B + cz*C - D is tested lane by lane (x, y, z, w): any lane above r is outside; if none is, any
// lane above -r gives v0 = 1, and when all four are at or below -r v0 keeps the plane result.
// The original's copies of the quadwords on its own stack frame are not observable and are not written here. The VU0
// steps follow the original's instruction order, so VF1..VF4, ACC and the MAC/STATUS flags finish as the original
// leaves them on every exit. The plane loop always runs exactly twice (a3 < 2 from zero), so its count is fixed and
// the function needs no call_cost.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kPlaneStride = 16U;        // plane i is the quadword at a1 + 16 * i
constexpr std::uint32_t kPlaneCount = 2U;
constexpr GuestAddress kBasisA = 32U;             // lqc2 at +32, +48 and +64
constexpr GuestAddress kBasisB = 48U;
constexpr GuestAddress kBasisC = 64U;
constexpr GuestAddress kOffset = 80U;             // lqc2 at +80

// Retired instruction counts from the listing, delay slots included. Each exit's total is the sum of brief.py's
// block counts along its path; the longest path (both planes outside, then the second pass fails at w) is 125.
constexpr std::uint32_t kPrologueCost = 14U;      // 0x2bf390..0x2bf3c4: copies, v0 = 2, f3 = r, f2 = -r, branch to test
constexpr std::uint32_t kLoopTestCost = 3U;       // 0x2bf418..0x2bf420: a3 < 2 and its delay slot
constexpr std::uint32_t kPlaneBodyCost = 15U;     // 0x2bf3c8..0x2bf400: dist, then the dist <= r test and its slot
constexpr std::uint32_t kPlaneSecondTestCost = 3U;  // 0x2bf404..0x2bf40c: the dist <= -r test and its slot
constexpr std::uint32_t kPlaneOutsideCost = 1U;   // 0x2bf410: dist > -r, so v0 = a0 (1)
constexpr std::uint32_t kPlaneStepCost = 1U;      // 0x2bf414: a3 += 1
constexpr std::uint32_t kCentreHeadCost = 17U;    // 0x2bf424..0x2bf464: p = cx*A + cy*B + cz*C - D and the x test
constexpr std::uint32_t kLaneCost = 6U;           // 0x2bf468..0x2bf47c and its siblings: one lane test
constexpr std::uint32_t kPassTwoXCost = 3U;       // 0x2bf4b0..0x2bf4b8: the x test against -r
constexpr std::uint32_t kEndCost = 5U;            // 0x2bf50c or 0x2bf514, 0x2bf524, 0x2bf528: v0 = 0 exit
constexpr std::uint32_t kIntersectCost = 6U;      // 0x2bf51c, 0x2bf504, 0x2bf528: v0 = 1 exit from the second pass
constexpr std::uint32_t kInsideCost = 4U;         // 0x2bf504, 0x2bf528: all four lanes at or below -r

// Sets v0 and finishes the call with the count of the exit taken.
n3::Run exit_with(auto& call, const std::uint32_t result, const std::uint32_t retired) {
    call.return_value(result);
    return call.finish(retired);
}

template <class Context>
n3::Run sphere_vs_clip_volume(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress volume = call.arg(1);

    // lq a2,0(a0) loads the sphere quadword. vaddw.x vf2,vf0,vf0w sets VF2.x to 1.0 (y, z, w keep their values).
    // The original's frame copy of the quadword is only ever reloaded (into VF3 and VF4), so it is not written here.
    // The radius is the copy's w word, and f2 is its negation.
    const guest::Gpr128 sphere = n3::load_quad_any(m, call.arg(0));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf2,vf0,vf0w
    cpu.write_vu0_vector(4U, sphere);                                         // lqc2 vf4 from the frame copy
    const std::uint32_t radius = sphere.words[3];                             // lwc1 f3 at the copy's +12
    const std::uint32_t negative_radius = fpu::neg(radius);                   // neg.s f2,f3

    std::uint32_t result = 2U;
    std::uint32_t retired = kPrologueCost;

    // dist = cx*nx + cy*ny + cz*nz - d for each plane. Each VU0 step below is the original's, in its order.
    for (std::uint32_t plane = 0U; plane < kPlaneCount; ++plane) {
        retired += kLoopTestCost;
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, volume + plane * kPlaneStride));  // lqc2 vf3,0(v1)
        ee_cop2::vmul(cpu, 1U, 4U, 3U, 14U);                                  // vmul.xyz vf1,vf4,vf3
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                        // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf2,vf1z
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 3U, 8U, 3U, true);               // vsubw.x vf1,vf1,vf3w
        const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];      // qmfc2 v1,vf1; mtc1 v1,f0
        retired += kPlaneBodyCost;
        if (!fpu::le(distance, radius)) {
            return exit_with(call, 0U, retired + kEndCost);
        }
        retired += kPlaneSecondTestCost;
        if (!fpu::le(distance, negative_radius)) {
            result = 1U;                                                      // daddu v0,a0,zero (a0 = 1)
            retired += kPlaneOutsideCost;
        }
        retired += kPlaneStepCost;
    }
    retired += kLoopTestCost;                                                 // a3 = 2: the loop test falls through

    // p = cx*A + cy*B + cz*C - D over all four lanes, in VF1. VF3 is the sphere copy again (lqc2 vf3,576(sp)).
    cpu.write_vu0_vector(3U, sphere);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, volume + kBasisA));             // lqc2 vf2,32(a1)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, volume + kBasisB));             // lqc2 vf1,48(a1)
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, 15U, 0U);                        // vmulx.xyzw vf2,vf2,vf3x
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 15U, 1U);                        // vmuly.xyzw vf1,vf1,vf3y
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 15U, false);                        // vadd.xyzw vf2,vf2,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, volume + kBasisC));             // lqc2 vf1,64(a1)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, volume + kOffset));             // lqc2 vf4,80(a1)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 15U, 2U);                        // vmulz.xyzw vf1,vf1,vf3z
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 15U, false);                        // vadd.xyzw vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 15U, true);                         // vsub.xyzw vf1,vf1,vf4
    const guest::Gpr128 centre = cpu.read_vu0_vector(1U);
    retired += kCentreHeadCost;

    // Every lane against +r, in order x, y, z, w: a lane above r means outside. x was tested in the head.
    if (!fpu::le(centre.words[0], radius)) {
        return exit_with(call, 0U, retired + kEndCost);
    }
    for (std::size_t lane = 1U; lane < 4U; ++lane) {
        retired += kLaneCost;
        if (!fpu::le(centre.words[lane], radius)) {
            return exit_with(call, 0U, retired + kEndCost);
        }
    }

    // Every lane against -r, in order x, y, z, w: a lane above -r means the sphere intersects.
    retired += kPassTwoXCost;
    if (!fpu::le(centre.words[0], negative_radius)) {
        return exit_with(call, 1U, retired + kIntersectCost);
    }
    for (std::size_t lane = 1U; lane < 4U; ++lane) {
        retired += kLaneCost;
        if (!fpu::le(centre.words[lane], negative_radius)) {
            return exit_with(call, 1U, retired + kIntersectCost);
        }
    }
    return exit_with(call, result, retired + kInsideCost);
}

} // namespace

extern constinit const EngineFunction n3_002bf390{0x002bf390U, 416U, 0x71648ddd3c7485a9ULL, "SphereVsClipVolumeClassify",
                                                  125U, n3::live<&sphere_vs_clip_volume<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002bf390, sphere_vs_clip_volume)
