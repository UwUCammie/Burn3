// 0x0014a1c0 (a0 = record R, a1 = output O, 16-byte aligned): NormalizedOffsetLength. Returns f0.
//
// R's byte at +400 selects the path. When it is clear, the original calls CameraLookDirection (0x00149ec0) with
// a0 = R+16 and a1 = O, and returns that call's f0. When it is set, D = (quad R+384).xyz - (quad W+48).xyz, where
// W is the word at R+4 and D's w lane is the w of quad W+48. D is stored to O, then O is rewritten with D's xyz
// normalised. The result f0 is |D|, divided by lane 3 of quad X+176 when that lane is above 1.0, where X is the
// word at W+9264. The reads of X come after the store to O, as the listing orders them.
//
// The VU0 work goes through the helpers of guest/ee_cop2_macro.hpp in the listing's order, so vf1, vf2, ACC, Q and
// the MAC/status flags end as the original leaves them. The original's frame writes (the saved ra and the quad
// spilled to sp+16) are not reproduced: the spilled lane 3 is read from the quad itself.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00149ec0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kRecordWorld = 4U;        // R+4 (word): the world object W
constexpr GuestAddress kRecordPoint = 384U;      // R+384 (quad): the point D starts from
constexpr GuestAddress kRecordFlag = 400U;       // R+400 (byte): clear selects the call
constexpr GuestAddress kWorldBase = 48U;         // W+48 (quad): the point D is measured to
constexpr GuestAddress kWorldSub = 9264U;        // W+9264 (word): the sub-object X
constexpr GuestAddress kSubQuad = 176U;          // X+176 (quad): lane 3 is w
constexpr GuestAddress kCalleeRecord = 16U;      // the jal's delay slot: addiu a0, a0, 16
constexpr std::uint32_t kCalleeFrame = 128U;     // the caller's frame: addiu sp, sp, -128
constexpr std::uint8_t kMaskX = 8U;              // destination mask of the x lane
constexpr std::uint32_t kOneBits = 0x3f800000U;  // 1.0f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 4U;      // 0x0014a1c0..0x0014a1cc: addiu sp, lbu, beq and sd (delay slot)
constexpr std::uint32_t kCallCost = 2U;          // 0x0014a278..0x0014a27c: jal and its delay slot
constexpr std::uint32_t kBodyCost = 37U;        // 0x0014a1d0..0x0014a260: the VU0 sequence through bc1t's delay slot
constexpr std::uint32_t kDivideCost = 4U;        // 0x0014a264..0x0014a270: nop, nop, div.s and the beq
constexpr std::uint32_t kReturnCost = 3U;        // ld ra, jr ra and the delay slot of jr (0x0014a280..0x0014a288)

template <class Context>
n3::Run normalized_offset_length(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    if (m.read_u8(record + kRecordFlag) == 0U) {
        call.set_arg(0, record + kCalleeRecord);
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00149ec0(machine); }, kCalleeFrame);
        if (callee.declined) {
            return call.decline();
        }
        return call.finish(kPrologueCost + kCallCost + callee.retired + kReturnCost);
    }

    const GuestAddress out = call.arg(1);
    const GuestAddress world = m.read_u32_le(record + kRecordWorld);

    // vaddw.x vf1, vf0, vf0w: vf1.x = 1.0 and the other lanes keep their values (the listing saves them in v0).
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);
    const Gpr128 vf1_before = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + kRecordPoint));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, world + kWorldBase));
    // vsub.xyz vf1, vf2, vf1 gives D; the listing stores it to O before anything else reads O.
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
    const Gpr128 difference = cpu.read_vu0_vector(1U);
    n3::store_quad_any(m, out, difference);
    // vmul.xyz vf2, vf1, vf1; vadday.x acc, vf2, vf2y; vmaddz.x vf1, vf1, vf2z: vf1.x = |D| squared.
    ee_cop2::vmul(cpu, 2U, 1U, 1U, n3::kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    cpu.write_vu0_vector(1U, vf1_before);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    const Gpr128 squared = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, squared);
    // vrsqrt q, vf0w, vf1x; sqrt.s f0 (the length); vwaitq; qmtc2 v0, vf1 (D again); vmulq.xyz vf1, vf1, q.
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);
    const std::uint32_t length = fpu::sqrt(squared.words[0]);
    cpu.write_vu0_vector(1U, difference);
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);
    n3::store_quad_any(m, out, cpu.read_vu0_vector(1U));

    // lw a0, 9264(v1); lq v0, 176(a0): the quad is spilled to the frame and its lane 3 reloaded as f2.
    const GuestAddress sub = m.read_u32_le(world + kWorldSub);
    const Gpr128 sub_quad = n3::load_quad_any(m, sub + kSubQuad);
    const std::uint32_t w = sub_quad.words[3];
    cpu.write_gpr(2U, sub_quad);
    cpu.write_fpr_bits(1U, kOneBits);
    cpu.write_fpr_bits(2U, w);
    const bool short_route = fpu::le(w, kOneBits);
    if (short_route) {
        cpu.write_fpr_bits(0U, length);
        return call.finish(kPrologueCost + kBodyCost + kReturnCost);
    }
    cpu.write_fpr_bits(0U, fpu::div(length, w));
    return call.finish(kPrologueCost + kBodyCost + kDivideCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014a1c0, 204U, 0xf354d1f2c5852663ULL, "NormalizedOffsetLength", kCeiling, normalized_offset_length,
         EngineReturn::Float)
