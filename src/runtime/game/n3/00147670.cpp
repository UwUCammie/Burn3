// 0x00147670 (a0 = strip object, a1 = position quad, a2 = quad index in its low halfword): StripSegmentDistance.
// The object's strip (u32 at +4) gives the index table (u32 at +0: two halfwords per quad, quad k's corners at
// halfwords 2k..2k+3) and the records (u32 at +4: 8 bytes each, whose first float is a running distance). The
// segment length f20 is record k less record k-1 (record 0 alone for k = 0). Vertices are 16-byte entries at u32
// 0x0064d354. The normal comes from the edge between corners 2k and 2k+1: when they differ it is the VU0 outer
// product of (v[2k] - v[2k+1]) with the constant quad at 0x004c9cf0; when they are equal (a degenerate edge) it is
// the StripQuadStep vector of the same quad (0x00147d00, written to the frame at +96). f0 is the smaller of the
// position's signed distance from corner 0's vertex along the unit normal and the segment length. The original
// keeps its temporaries in its frame and reads them back; here they stay in locals, and only the callee's output
// word is written to memory. f20 is left as it was on entry.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

N3_CALLEE(00147d00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kVertices = 0x0064d354U;    // lui 0x65, lw -11436: 16-byte vertex entries
constexpr GuestAddress kPlaneQuad = 0x004c9cf0U;   // lui 0x4d, lqc2 -25360: the constant the normal is built with
constexpr std::uint8_t kMaskX = 8U;                // VU0 destination mask x
constexpr std::uint32_t kFrameBytes = 128U;        // addiu $sp, $sp, -128
constexpr std::uint32_t kOutputOffset = 96U;       // the degenerate edge's normal goes to sp + 96
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;        // 0x147670..0x147688: frame, quad, index test and its delay slot
constexpr std::uint32_t kZeroSegmentCost = 6U;  // 0x147788..0x14779c: index 0 takes record 0
constexpr std::uint32_t kSegmentCost = 7U;      // 0x14768c..0x1476a4: record k less record k-1
constexpr std::uint32_t kCornerCost = 14U;      // 0x1476a8..0x1476dc: corner reads, position less corner 0, branch
constexpr std::uint32_t kNormalCost = 12U;      // 0x1476e0..0x14770c: normal of a non-degenerate edge
constexpr std::uint32_t kDegenerateCost = 17U;  // 0x1477a0..0x1477e0: normal of a degenerate edge, branch to 0x147710
constexpr std::uint32_t kTestCost = 2U;         // 0x147710..0x147714: the corner-pair test and its delay slot
constexpr std::uint32_t kReloadCost = 1U;       // 0x147718: the normal reloaded into VF2 (non-degenerate edge)
constexpr std::uint32_t kCallCost = 5U;         // 0x1477e4..0x1477f4: jal, its delay slot, branch back, its slot
constexpr std::uint32_t kLengthCost = 27U;      // 0x14771c..0x147784: normal length, unit normal, distance, min.s
constexpr std::uint32_t kReturnCost = 4U;       // 0x1477f8..0x147804: epilogue

// MIN.S on raw words, ordered as guest::ee_cop1::min_s orders them.
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                          : std::min(signed_left, signed_right));
}

template <class Context>
n3::Run strip_segment_distance(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint32_t index = call.arg(2) & 0xffffU;
    const GuestAddress strip = m.read_u32_le(call.arg(0) + 4U);
    const GuestAddress records = m.read_u32_le(strip + 4U);

    std::uint32_t retired = kEntryCost;
    std::uint32_t segment = 0U;
    if (index == 0U) {
        segment = m.read_u32_le(records);
        retired += kZeroSegmentCost;
    } else {
        const GuestAddress record = records + index * 8U;
        segment = fpu::sub(m.read_u32_le(record), m.read_u32_le(record - 8U));
        retired += kSegmentCost;
    }

    const GuestAddress vertices = m.read_u32_le(kVertices);
    const GuestAddress corners = m.read_u32_le(strip) + index * 4U;
    const auto vertex = [&](const std::uint32_t corner) { return vertices + (corner << 4U); };
    const std::uint32_t c0 = m.read_u16_le(corners);
    const std::uint32_t c1 = m.read_u16_le(corners + 2U);

    cpu.write_vu0_vector(2U, n3::load_quad_any(m, call.arg(1)));    // VF2 = the position quad
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex(c0)));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);      // vsub.xyz vf1,vf2,vf1
    const Gpr128 from_corner = cpu.read_vu0_vector(1U);
    retired += kCornerCost;

    Gpr128 normal{};
    if (c0 != c1) {
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertex(c0)));
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex(c1)));
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, kPlaneQuad));
        ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, n3::kMaskXyz, true);  // vsub.xyz vf2,vf3,vf2
        ee_cop2::vopmula(cpu, 2U, 1U);                              // vopmula.xyz acc,vf2,vf1
        ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                          // vopmsub.xyz vf1,vf1,vf2
        normal = cpu.read_vu0_vector(1U);
        retired += kNormalCost + kTestCost + kReloadCost;
    } else {
        const std::uint32_t c2 = m.read_u16_le(corners + 4U);
        const std::uint32_t c3 = m.read_u16_le(corners + 6U);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, kPlaneQuad));
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex(c3)));
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertex(c2)));
        ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, n3::kMaskXyz, true);  // vsub.xyz vf2,vf3,vf2
        ee_cop2::vopmula(cpu, 2U, 1U);                              // vopmula.xyz acc,vf2,vf1
        ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                          // vopmsub.xyz vf1,vf1,vf2
        // The original stores this VF1 at sp + 96 and the callee overwrites that word with its own output.
        const GuestAddress output = call.stack_pointer() - kFrameBytes + kOutputOffset;
        call.set_arg64(1, call.arg64(2));                           // daddu a1,a2,zero
        call.set_arg(2, output);                                    // addiu a2,sp,96
        const n3::Run callee = call.invoke([](auto& mm) { return n3::fn_00147d00(mm); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        normal = n3::load_quad_any(m, output);                      // lqc2 vf2,96(sp) in the delay slot
        retired += kDegenerateCost + kTestCost + kCallCost + callee.retired;
    }

    // 0x14771c..0x147784: VF2 holds the normal. VF1.x is 1, so the squared length lands in VF1.x.
    cpu.write_vu0_vector(2U, normal);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
    const Gpr128 kept = cpu.read_vu0_vector(1U);
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);                   // vmul.xyz vf2,vf2,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, kept);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                       // vrsqrt q,vf0w,vf1x (vwaitq follows)
    cpu.write_vu0_vector(1U, normal);
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);  // vmulq.xyz vf1,vf1,q
    const Gpr128 unit = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(3U, from_corner);                          // lqc2 vf3,80(sp)
    cpu.write_vu0_vector(2U, unit);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);   // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);                   // vmul.xyz vf2,vf3,vf2
    const Gpr128 kept_again = cpu.read_vu0_vector(1U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, kept_again);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];            // qmfc2 v0,vf1 ; mtc1 v0,f0
    cpu.write_fpr_bits(0U, min_bits(distance, segment));                         // min.s f0,f0,f20
    return call.finish(retired + kLengthCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00147670, 408U, 0x4bd4747e8b551ae2ULL, "StripSegmentDistance", kCeiling, strip_segment_distance,
         EngineReturn::Float)
