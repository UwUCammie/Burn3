// 0x0024b390 DrawLightFlareQuad (no arguments, no result): draws one camera-facing flare sprite, a unit quad of
// half-extent 1.5 centred at P + 50 * D, where D is the light direction (quad at 0x01d9b940) and P the position (quad at
// 0x00666320). The right vector R = normalise(cross((0, 1, 0), D)) and the up vector Uv = normalise(cross(R, D)) come
// from VU0 (OPMULA / OPMSUB pairs, length^2 through VMUL / VADDAy / VMADDz, VRSQRT for the scale). The four corners
// are 32-byte vertex records in the function's own frame (xyzw, s and t, colour bytes, a stale pad word) that
// AppendVertexBatch (0x00365040) uploads:
//
//     v0 = C - R' + U' (s,t = 0,0)   v1 = C + R' + U' (1,0)   v2 = C - R' - U' (0,1)   v3 = C + R' - U' (1,1)
//
// The colour bytes are the low bytes of the integer conversion of the three floats at 0x01d9b960, alpha 255. Around
// the batch the function opens a lit-vertex batch (BeginBatch 0x00365640), reserves eight quadwords
// (ReservePacketBuffer 0x001f3080) and writes one GIF packet at the cursor (gp-24240): TEST_1, ALPHA_1, the frame
// register record of the current render slot, ZBUF_1 with its mask bit set, a quadword from 0x01d9b950, TEX1_1 and
// CLAMP_1. After the vertices go out it closes the batch (EndBatchRestoreState 0x003655a0).
//
// When the byte at gp-23744 is set the function only runs the head, which loads the four vertex slots from the frame
// and forces their w lanes to zero in VF1..VF4 (the slots are not stored back usefully: the frame is dead), and
// returns after 35 instructions.
//
// The VU0 sequence runs on the CPU's VF registers so the final VF1..VF8, ACC, Q and the MAC / status flags match the
// original. The stale upper halves of some GPRs are architecturally visible through QMTC2 (VF4 after the head, the
// w lane of VF1 after the qmtc2 of the 50.0 constant), so those registers are read.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00365640)
N3_CALLEE(001f3080)
N3_CALLEE(00365040)
N3_CALLEE(003655a0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kW = n3::kMaskW;

constexpr std::uint32_t kFrameBytes = 912U;
constexpr GuestAddress kVertexBase = 768U;        // four 32-byte vertex records at sp + 768
constexpr GuestAddress kVertexStride = 32U;
constexpr GuestAddress kScratchQuad = 400U;       // (0, 1, 0, stale w): the up vector the cross product starts from

constexpr std::int32_t kSkipFlagOffset = -23744;  // byte at gp: set = draw nothing
constexpr std::int32_t kCursorOffset = -24240;    // word at gp: packet write cursor

constexpr GuestAddress kLightDirection = 0x01d9b940U;
constexpr GuestAddress kFlareColour = 0x01d9b960U;
constexpr GuestAddress kPacketQuad = 0x01d9b950U;
constexpr GuestAddress kPosition = 0x00666320U;
constexpr GuestAddress kSlotIndexWord = 0x00667050U;
constexpr GuestAddress kSlotTable = 0x00665ec0U;
constexpr std::uint32_t kSlotStride = 560U;
constexpr std::uint32_t kSlotFrameOffset = 1584U;  // E + 1584: the slot's frame register record

constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kDistance = 0x42480000U;   // 50.0
constexpr std::uint32_t kHalfSize = 0x3fc00000U;   // 1.5
constexpr std::uint32_t kAlpha = 255U;
constexpr std::uint32_t kVertexCount = 4U;

constexpr std::uint64_t kGifTag = (std::uint64_t{0x10000000U} << 32U) | 0x8007U;  // NLOOP 7, EOP, A+D
constexpr std::uint64_t kTest1 = 0x0003380dU;
constexpr std::uint64_t kAlpha1 = 68U;
constexpr std::uint64_t kZbufMask = std::uint64_t{1U} << 32U;
constexpr std::uint64_t kTex1 = 97U;
constexpr std::uint64_t kClamp1 = 5U;
constexpr std::uint32_t kPacketBytes = 128U;
constexpr std::uint32_t kReserveFlags = 0x80000000U;
constexpr std::uint32_t kReserveQuads = 8U;

// Retired instructions of the original's own code, delay slots included.
constexpr std::uint32_t kHeadCost = 31U;      // 0x24b390-0x24b408, up to the flag test and its delay slot
constexpr std::uint32_t kDrawCost = 177U;     // 0x24b40c-0x24b6cc: the VU work, the packet and the four jals
constexpr std::uint32_t kTailCost = 4U;       // 0x24b6d0-0x24b6dc
constexpr std::uint32_t kCeiling = 20000U;

// A quadword assembled from two 64-bit halves (pcpyld).
Gpr128 pair(const std::uint64_t high, const std::uint64_t low) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

// A vertex record's w lane forced to zero: VSUBw.w vf,vf0,vf0w then VADDx.w vf,vf,vf4x (VF4.x is zero here).
void clear_w(guest::R5900State& cpu, const unsigned vf) {
    ee_cop2::vadd_broadcast(cpu, vf, 0U, 0U, kW, 3U, true);
    ee_cop2::vadd_broadcast(cpu, vf, vf, 4U, kW, 0U, false);
}

// Normalises the xyz of VF4 in place: |v|^2 into VF1.x (VF2 holds the squares), Q = 1 / sqrt, VF4.xyz *= Q.
void normalise_vf4(guest::R5900State& cpu) {
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 4U, 4U, kXyz);                                      // vmul.xyz vf2,vf4,vf4
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);                             // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);    // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                  // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 4U, 4U, kXyz, ee_cop2::kControlQ);              // vmulq.xyz vf4,vf4,q
}

template <class Context>
n3::Run draw_light_flare_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress vertex[kVertexCount] = {frame + kVertexBase, frame + kVertexBase + kVertexStride,
                                               frame + kVertexBase + 2U * kVertexStride,
                                               frame + kVertexBase + 3U * kVertexStride};

    // Head: VF4 takes v1 (daddu zero,zero leaves its upper half), and the vertex slots are loaded with w cleared.
    // The loaded slot values only matter in the early exit: the draw path replaces every xyz lane.
    Gpr128 zero_low = cpu.read_gpr(3U);
    zero_low.words[0] = 0U;
    zero_low.words[1] = 0U;
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex[2]));
    cpu.write_vu0_vector(4U, zero_low);
    clear_w(cpu, 1U);
    const Gpr128 slot2 = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertex[0]));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex[1]));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex[3]));
    clear_w(cpu, 3U);
    clear_w(cpu, 2U);
    clear_w(cpu, 1U);
    const Gpr128 slot0 = cpu.read_vu0_vector(3U);
    const Gpr128 slot1 = cpu.read_vu0_vector(2U);
    const Gpr128 slot3 = cpu.read_vu0_vector(1U);
    cpu.write_fpr_bits(2U, 0U);

    if (m.read_u8(gp + static_cast<std::uint32_t>(kSkipFlagOffset)) != 0U) {
        return call.finish(kHeadCost + kTailCost);
    }

    // The up vector (0, 1, 0, w) is built in the frame; its w lane is whatever the slot held.
    cpu.write_vu0_vector(1U, Gpr128{{0U, kOne, 0U, m.read_u32_le(frame + kScratchQuad + 12U)}});
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, kLightDirection));

    // R = normalise(cross(D, U0)) in VF4, kept in VF8.
    ee_cop2::vopmula(cpu, 3U, 1U);                                             // vopmula.xyz acc,vf3,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 3U);                                         // vopmsub.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(4U, cpu.read_vu0_vector(1U));
    normalise_vf4(cpu);
    cpu.write_vu0_vector(8U, cpu.read_vu0_vector(4U));

    // Uv = normalise(cross(D, R)) in VF4, kept in VF7.
    ee_cop2::vopmula(cpu, 8U, 3U);                                             // vopmula.xyz acc,vf8,vf3
    ee_cop2::vopmsub(cpu, 1U, 3U, 8U);                                         // vopmsub.xyz vf1,vf3,vf8
    cpu.write_vu0_vector(4U, cpu.read_vu0_vector(1U));
    normalise_vf4(cpu);
    cpu.write_vu0_vector(7U, cpu.read_vu0_vector(4U));

    // The colour: low bytes of the integer conversion of the three floats.
    const Gpr128 colour = n3::load_quad_any(m, kFlareColour);
    const std::uint32_t colour_word =
        (static_cast<std::uint32_t>(fpu::to_int(colour.words[0])) & 0xffU) |
        ((static_cast<std::uint32_t>(fpu::to_int(colour.words[1])) & 0xffU) << 8U) |
        ((static_cast<std::uint32_t>(fpu::to_int(colour.words[2])) & 0xffU) << 16U) | (kAlpha << 24U);
    cpu.write_fpr_bits(0U, static_cast<std::uint32_t>(fpu::to_int(colour.words[2])));
    cpu.write_fpr_bits(1U, static_cast<std::uint32_t>(fpu::to_int(colour.words[1])));

    // vf1.x = 50.0 (the upper lanes are the stale upper half of t5), vf4.x = 1.5.
    const Gpr128 stale_t5 = cpu.read_gpr(13U);
    cpu.write_vu0_vector(1U, Gpr128{{kDistance, 0U, stale_t5.words[2], stale_t5.words[3]}});
    cpu.write_vu0_vector(4U, Gpr128{{kHalfSize, 0U, cpu.read_gpr(14U).words[2], cpu.read_gpr(14U).words[3]}});
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, kXyz, 0U);                        // vmulx.xyz vf1,vf3,vf1x
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, kPosition));
    ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, kXyz, false);                        // vadd.xyz vf3,vf2,vf1: centre C
    ee_cop2::vmul_broadcast(cpu, 8U, 8U, 4U, kXyz, 0U);                        // vmulx.xyz vf8,vf8,vf4x: R'
    ee_cop2::vmul_broadcast(cpu, 7U, 7U, 4U, kXyz, 0U);                        // vmulx.xyz vf7,vf7,vf4x: U'

    // Corners, in the original's order (v0 .. v3 are the quadwords stored below).
    ee_cop2::vadd_vector(cpu, 1U, 3U, 8U, kXyz, true);                         // vsub.xyz vf1,vf3,vf8
    cpu.write_vu0_vector(6U, slot0);
    ee_cop2::vadd_vector(cpu, 1U, 1U, 7U, kXyz, false);                        // vadd.xyz vf1,vf1,vf7
    ee_cop2::vadd_vector(cpu, 6U, 0U, 1U, kXyz, false);                        // vadd.xyz vf6,vf0,vf1
    ee_cop2::vadd_vector(cpu, 1U, 3U, 8U, kXyz, false);                        // vadd.xyz vf1,vf3,vf8
    ee_cop2::vadd_vector(cpu, 1U, 1U, 7U, kXyz, false);                        // vadd.xyz vf1,vf1,vf7
    cpu.write_vu0_vector(2U, slot1);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, kXyz, false);                        // vadd.xyz vf2,vf0,vf1
    const Gpr128 corner1 = cpu.read_vu0_vector(2U);
    ee_cop2::vadd_vector(cpu, 2U, 3U, 8U, kXyz, true);                         // vsub.xyz vf2,vf3,vf8
    ee_cop2::vadd_vector(cpu, 1U, 3U, 8U, kXyz, false);                        // vadd.xyz vf1,vf3,vf8
    cpu.write_vu0_vector(4U, slot3);
    cpu.write_vu0_vector(5U, slot2);
    ee_cop2::vadd_vector(cpu, 2U, 2U, 7U, kXyz, true);                         // vsub.xyz vf2,vf2,vf7
    ee_cop2::vadd_vector(cpu, 1U, 1U, 7U, kXyz, true);                         // vsub.xyz vf1,vf1,vf7
    ee_cop2::vadd_vector(cpu, 5U, 0U, 2U, kXyz, false);                        // vadd.xyz vf5,vf0,vf2
    ee_cop2::vadd_vector(cpu, 4U, 0U, 1U, kXyz, false);                        // vadd.xyz vf4,vf0,vf1

    // Vertex records: position, s and t, colour bytes. The pad word at +28 is left as it was.
    const Gpr128 corner[kVertexCount] = {cpu.read_vu0_vector(6U), corner1, cpu.read_vu0_vector(5U),
                                         cpu.read_vu0_vector(4U)};
    constexpr std::uint32_t kS[kVertexCount] = {0U, kOne, 0U, kOne};
    constexpr std::uint32_t kT[kVertexCount] = {0U, 0U, kOne, kOne};
    for (unsigned i = 0U; i < kVertexCount; ++i) {
        n3::store_quad_any(m, vertex[i], corner[i]);
        m.write_u32_le(vertex[i] + 16U, kS[i]);
        m.write_u32_le(vertex[i] + 20U, kT[i]);
        m.write_u32_le(vertex[i] + 24U, colour_word);
    }

    std::uint32_t retired = kHeadCost + kDrawCost + kTailCost;

    // jal 0x00365640: BeginBatch.
    const n3::Run begin = call.invoke([](auto& machine) { return n3::fn_00365640(machine); }, kFrameBytes);
    if (begin.declined) {
        return call.decline();
    }
    retired += begin.retired;

    // jal 0x001f3080: ReservePacketBuffer(0x80000000, 8); s0 = the render slot's record, read before the call.
    const std::uint32_t slot = m.read_u32_le(kSlotIndexWord);
    const GuestAddress record = kSlotTable + kSlotStride * slot + kSlotFrameOffset;
    call.set_arg(0, kReserveFlags);
    call.set_arg(1, kReserveQuads);
    const n3::Run reserve = call.invoke([](auto& machine) { return n3::fn_001f3080(machine); }, kFrameBytes);
    if (reserve.declined) {
        return call.decline();
    }
    retired += reserve.retired;

    // The GIF packet at the cursor.
    const GuestAddress cursor = m.read_u32_le(gp + static_cast<std::uint32_t>(kCursorOffset));
    n3::store_quad_any(m, cursor, pair(14U, kGifTag));
    n3::store_quad_any(m, cursor + 16U, pair(71U, kTest1));
    n3::store_quad_any(m, cursor + 32U, pair(66U, kAlpha1));
    n3::store_quad_any(m, cursor + 48U, n3::load_quad_any(m, record));
    const Gpr128 zbuf = n3::load_quad_any(m, record + 16U);
    n3::store_quad_any(m, cursor + 64U,
                       pair(78U, (static_cast<std::uint64_t>(zbuf.words[1]) << 32U | zbuf.words[0]) | kZbufMask));
    n3::store_quad_any(m, cursor + 80U, n3::load_quad_any(m, kPacketQuad));
    n3::store_quad_any(m, cursor + 96U, pair(20U, kTex1));
    n3::store_quad_any(m, cursor + 112U, pair(8U, kClamp1));

    // jal 0x00365040: AppendVertexBatch(vertices, 4); the cursor moves past the packet in its delay slot.
    m.write_u32_le(gp + static_cast<std::uint32_t>(kCursorOffset), cursor + kPacketBytes);
    call.set_arg(0, vertex[0]);
    call.set_arg(1, kVertexCount);
    const n3::Run append = call.invoke([](auto& machine) { return n3::fn_00365040(machine); }, kFrameBytes);
    if (append.declined) {
        return call.decline();
    }
    retired += append.retired;

    // jal 0x003655a0: EndBatchRestoreState.
    const n3::Run end = call.invoke([](auto& machine) { return n3::fn_003655a0(machine); }, kFrameBytes);
    if (end.declined) {
        return call.decline();
    }
    retired += end.retired;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0024b390, 848U, 0xa7ef52bd0acd3a20ULL, "DrawLightFlareQuad", kCeiling, draw_light_flare_quad,
         EngineReturn::Nothing)
