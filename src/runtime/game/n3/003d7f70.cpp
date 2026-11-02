// 0x003d7f70 (a0 = the 8 corner quads of a box, a1 = the 224-byte volume: corners at +0..+127, face planes at
// +128..+223): BuildRegionVolume. Quad k of the source is copied to volume + 16k, in order, so an overlapping source
// reads what was copied before it, as the original does. Face f then takes its corner indices A, B, C from the table
// at 0x004c9d10 (four signed bytes per face, the fourth unused) and stores the plane {n.xyz, d}, where
// n = -((A - B) x (C - B)) normalised by VRSQRT and VMULQ, and d = n . A. Both loops have fixed trip counts (8 and 6),
// so this is straight code and the retired count is 360 on every path. The VU0 registers, ACC, Q and the MAC/STATUS
// flags end as the original leaves them: each VU operation is the same ee_cop2 helper, in the same order.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kFaceTable = 0x004c9d10U;  // lui 0x4d, addiu -25328: six faces of {i0, i1, i2, unused}
constexpr std::uint32_t kCorners = 8U;
constexpr std::uint32_t kFaces = 6U;
constexpr GuestAddress kPlanes = 128U;            // the planes follow the corners in the volume
constexpr std::uint8_t kMaskX = 8U;

// Retired instruction counts of the original's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 1U;          // daddu $t0 (0x003d7f70)
constexpr std::uint32_t kCornerCost = 8U;         // one corner copy: 0x003d7f74..0x003d7f90
constexpr std::uint32_t kSetupCost = 5U;          // 0x003d7f94..0x003d7fa4: vaddw.x vf2, lui, daddu, addiu, vaddw.x vf6
constexpr std::uint32_t kFaceCost = 48U;          // one face: 0x003d7fa8..0x003d8064
constexpr std::uint32_t kReturnCost = 2U;         // 0x003d8068..0x003d806c: jr $ra and its delay slot
constexpr std::uint32_t kRetired =
    kEntryCost + kCorners * kCornerCost + kSetupCost + kFaces * kFaceCost + kReturnCost;
static_assert(kRetired == 360U);

// The volume address of corner `field` (0 = A, 1 = B, 2 = C) of face `face`: the table byte, a signed index,
// scaled to a quad offset (sll 4).
template <class Memory>
GuestAddress face_corner(const Memory& m, const GuestAddress volume, const GuestAddress face,
                         const GuestAddress field) {
    const auto index = static_cast<std::int8_t>(m.read_u8(kFaceTable + 4U * face + field));
    return volume + (static_cast<GuestAddress>(static_cast<std::int32_t>(index)) << 4U);
}

// One plane {n.xyz, d} at `plane` from the corners at a, b and c. The VU0 operations are the original's, in its
// order, on the same registers, so the VU0 state left behind is the original's too.
template <class Memory>
void build_face_plane(Memory& m, guest::R5900State& cpu, const GuestAddress plane, const GuestAddress a,
                      const GuestAddress b, const GuestAddress c) {
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, a));                                  // lqc2 vf4 (A)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, b));                                  // lqc2 vf3 (B)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, c));                                  // lqc2 vf1 (C)
    ee_cop2::vadd_vector(cpu, 5U, 4U, 3U, n3::kMaskXyz, true);                          // vsub.xyz vf5,vf4,vf3
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, true);                          // vsub.xyz vf1,vf1,vf3
    ee_cop2::vopmula(cpu, 5U, 1U);                                                      // vopmula.xyz acc,vf5,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 5U);                         // vopmsub.xyz vf1,vf1,vf5: (A - B) x (C - B)
    ee_cop2::vadd_vector(cpu, 3U, 0U, 1U, n3::kMaskXyz, true);                          // vsub.xyz vf3,vf0,vf1
    ee_cop2::vmul(cpu, 1U, 3U, 3U, n3::kMaskXyz);                                       // vmul.xyz vf1,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                                  // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, kMaskX, 2U, false, false, false);         // vmaddz.x vf1,vf2,vf1z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                           // vrsqrt Q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);               // vmulq.xyz vf3,vf3,Q

    ee_cop2::vmul(cpu, 1U, 4U, 3U, n3::kMaskXyz);                                       // vmul.xyz vf1,vf4,vf3
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, plane));                              // lqc2 vf4 (old plane)
    ee_cop2::vadd_vector(cpu, 4U, 0U, 3U, n3::kMaskXyz, false);                         // vadd.xyz vf4,vf0,vf3
    n3::store_quad_any(m, plane, cpu.read_vu0_vector(4U));   // sqc2 vf4: xyz = n, w still the old word
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                                  // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 6U, 1U, kMaskX, 2U, false, false, false);         // vmaddz.x vf1,vf6,vf1z
    cpu.write_vu0_vector(3U, cpu.read_vu0_vector(4U));                                  // lqc2 vf3: reads back the store
    ee_cop2::vmul_broadcast(cpu, 3U, 0U, 1U, n3::kMaskW, 0U);                           // vmulx.w vf3,vf0,vf1x
    n3::store_quad_any(m, plane, cpu.read_vu0_vector(3U));                              // sqc2 vf3: w = d
}

template <class Context>
n3::Run build_region_volume(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress corners = call.arg(0);
    const GuestAddress volume = call.arg(1);

    for (std::uint32_t corner = 0U; corner < kCorners; ++corner) {
        n3::store_quad_any(m, volume + 16U * corner, n3::load_quad_any(m, corners + 16U * corner));
    }

    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, kMaskX, 3U, false);  // vaddw.x vf2,vf0,vf0w: vf2.x = 1.0
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 0U, kMaskX, 3U, false);  // vaddw.x vf6,vf0,vf0w: vf6.x = 1.0

    for (std::uint32_t face = 0U; face < kFaces; ++face) {
        build_face_plane(m, cpu, volume + kPlanes + 16U * face, face_corner(m, volume, face, 0U),
                         face_corner(m, volume, face, 1U), face_corner(m, volume, face, 2U));
    }

    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003d7f70, 256U, 0x52d0648a8564c5f3ULL, "BuildRegionVolume", kRetired, build_region_volume,
               EngineReturn::Nothing)
