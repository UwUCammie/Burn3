// 0x0013dd30 (a0 = object S, a1 = road R, a2 = segment G (low 16 bits compared), a3 = quad, t0/t1/t2 = output
// words): RoadSegmentShadeLookup. The three output words are cleared first. With S's float F (+1092) zero the
// result is 0. Otherwise d is the road-link distance from R's segment G (0x00148140), and a tiny d (exponent field
// 111 or less) gives 0. S's record (+1108) names its own road (u32 at +6324) and segment (u16 at +6328); when either
// differs from R and G, f20 is the distance from the record's own pair instead. The chain entry (0x00146880,
// direction 1, quad to sp+160) and the step (0x00147cb0, quad to sp+176) come next: the step is made a unit vector
// in VU0 and dotted with (the a3 quad - the chain entry), and that dot over d is blended with (F - S+1100) / F. The
// index is 5 * f20 * that difference, negated, offset by 128, truncated and clamped to 0..255. 0x0013da30 writes the
// three output words from the index, and the result is the signed byte at S + index + 64.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

N3_CALLEE(00148140)
N3_CALLEE(00146880)
N3_CALLEE(00147cb0)
N3_CALLEE(0013da30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kScale = 1092U;          // S: F
constexpr GuestAddress kBlend = 1100U;          // S: the float subtracted from F in the blend
constexpr GuestAddress kRecord = 1108U;         // S: the record pointer
constexpr GuestAddress kRecordRoad = 6324U;     // record: road (u32)
constexpr GuestAddress kRecordSegment = 6328U;  // record: segment (u16)
constexpr GuestAddress kShadeBytes = 64U;       // the result: the signed byte at S + index + 64
constexpr std::uint8_t kMaskX = 8U;             // VU0 destination mask x
constexpr std::uint8_t kMaskY = 4U;             // VU0 destination mask y
constexpr std::uint32_t kFrameBytes = 208U;     // addiu $sp,$sp,-208
constexpr std::uint32_t kChainQuad = 160U;      // 0x00146880 writes its quad at sp + 160
constexpr std::uint32_t kStepQuad = 176U;       // 0x00147cb0 writes its quad at sp + 176
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kTinyBelow = 0x37800001U;            // exponent field 111 or less: |d| < 2^-15
constexpr std::uint32_t kFive = 0x40a00000U;                 // 5.0
constexpr std::uint32_t kMinusOneTwentyEight = 0xc3000000U;  // -128.0
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 25U;   // 0x0013dd30..0x0013dd90: frame, quad, F, the three stores, bc1f
constexpr std::uint32_t kZeroCost = 2U;        // 0x0013dd94..0x0013dd98: F is zero, v0 = 0
constexpr std::uint32_t kFirstCallCost = 11U;  // 0x0013dd9c..0x0013ddc4: the first distance call, f21, the tiny test
constexpr std::uint32_t kTinyCost = 2U;        // 0x0013df0c..0x0013df10: tiny distance, v0 = 0
constexpr std::uint32_t kRecordCost = 5U;      // 0x0013ddc8..0x0013ddd8: the record and its segment test
constexpr std::uint32_t kRoadCost = 3U;        // 0x0013dddc..0x0013dde4: the record's road test
constexpr std::uint32_t kMatchCost = 1U;       // 0x0013dde8: the record names R and G, f20 = d
constexpr std::uint32_t kReloadCost = 6U;      // 0x0013df14..0x0013df28: the record's own distance call
constexpr std::uint32_t kTailCost = 72U;       // 0x0013ddec..0x0013df08: chain, step, VU0, blend, window, lb
constexpr std::uint32_t kEpilogueCost = 11U;   // 0x0013df2c..0x0013df54

// A 32-bit word as a 64-bit register sees it after lw (sign-extended).
constexpr std::uint64_t sign_extended(const std::uint32_t word) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
}

// 0x0013df14: the road-link distance on the record's own road and segment (a2 = a1, the halfword).
template <class Memory>
n3::Run record_distance(n3::Call<Memory>& call, const std::uint32_t road, const std::uint32_t segment) {
    call.set_arg(0, road);
    call.set_arg(1, segment);
    call.set_arg64(2, segment);
    return call.invoke([](auto& memory) { return n3::fn_00148140(memory); }, kFrameBytes);
}

// 0x0013de10..0x0013de80 on the real VU0 state: the step quad is made a unit vector with its y lane forced to zero
// (VF3, then VF2), and the dot of that vector with (the a3 quad - the chain entry) comes back as f0's bits.
// The accumulator order is the original's: the x and y products first, then the z product.
template <class Memory>
std::uint32_t unit_dot(guest::R5900State& cpu, const Memory& memory, const GuestAddress frame,
                       const Gpr128& input_quad) {
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, frame + kStepQuad));      // lqc2 vf3,176(sp)
    cpu.write_vu0_vector(2U, Gpr128{});                                          // qmtc2 of a zero v0, vf2
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                 // vaddw.x vf1,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kMaskY, 0U, false);                 // vaddx.y vf3,vf0,vf2x
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                                // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                           // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                    // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);        // vmulq.xyz vf3,vf3,q
    const Gpr128 unit = cpu.read_vu0_vector(3U);                                 // qmfc2 v0,vf3
    cpu.write_vu0_vector(4U, input_quad);                                        // lqc2 vf4,192(sp)
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, frame + kChainQuad));     // lqc2 vf3,160(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                 // vaddw.x vf1,vf0,vf0w
    cpu.write_vu0_vector(2U, unit);                                              // qmtc2 v0,vf2
    ee_cop2::vadd_vector(cpu, 3U, 4U, 3U, n3::kMaskXyz, true);                   // vsub.xyz vf3,vf4,vf3
    ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);                                // vmul.xyz vf2,vf3,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                           // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U).words[0];                                     // qmfc2 v0,vf1 ; mtc1 v0,f0
}

template <class Context>
n3::Run road_segment_shade_lookup(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // The entry values, read before anything is stored: the original reads the a3 quad and F first.
    const GuestAddress object = call.arg(0);
    const std::uint64_t object64 = call.arg64(0);
    const std::uint64_t road64 = call.arg64(1);
    const std::uint64_t segment64 = call.arg64(2);
    const auto segment = static_cast<std::uint32_t>(segment64 & 0xffffU);
    const Gpr128 input_quad = n3::load_quad_any(m, call.arg(3));
    const std::uint32_t scale = m.read_u32_le(object + kScale);
    const std::uint64_t out0 = call.arg64(4);
    const std::uint64_t out1 = call.arg64(5);
    const std::uint64_t out2 = call.arg64(6);
    m.write_u32_le(static_cast<GuestAddress>(out0), 0U);
    m.write_u32_le(static_cast<GuestAddress>(out1), 0U);
    m.write_u32_le(static_cast<GuestAddress>(out2), 0U);
    if (fpu::eq(0U, scale)) {
        call.return_value(0U);
        return call.finish(kPrologueCost + kZeroCost + kEpilogueCost);
    }

    // The distance from R's segment G. a2 is still the entry's G, as the original leaves it.
    call.set_arg64(0, road64);
    call.set_arg64(1, segment64);
    const n3::Run first = call.invoke([](auto& memory) { return n3::fn_00148140(memory); }, kFrameBytes);
    if (first.declined) {
        return call.decline();
    }
    const std::uint32_t distance = call.float_result();
    std::uint32_t retired = kPrologueCost + kFirstCallCost + first.retired;
    if ((distance & kExponentMask) < kTinyBelow) {
        call.return_value(0U);
        return call.finish(retired + kTinyCost + kEpilogueCost);
    }

    // The record's segment must be G and its road R, else f20 is the distance on the record's own road and segment.
    const GuestAddress record = m.read_u32_le(object + kRecord);
    const std::uint32_t record_segment = m.read_u16_le(record + kRecordSegment);
    retired += kRecordCost;
    bool reload = record_segment != segment;
    if (!reload) {
        retired += kRoadCost;
        reload = road64 != sign_extended(m.read_u32_le(record + kRecordRoad));
    }
    std::uint32_t shade = distance;
    if (reload) {
        retired += kReloadCost;
        const n3::Run second = record_distance(call, m.read_u32_le(record + kRecordRoad), record_segment);
        if (second.declined) {
            return call.decline();
        }
        shade = call.float_result();
        retired += second.retired;
    } else {
        retired += kMatchCost;
    }

    // The chain entry and the step vector, both written by callees into this frame, then the VU0 dot.
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    retired += kTailCost;
    call.set_arg64(0, road64);
    call.set_arg64(1, segment64);
    call.set_arg(2, 1U);
    call.set_arg(3, frame + kChainQuad);
    const n3::Run chain = call.invoke([](auto& memory) { return n3::fn_00146880(memory); }, kFrameBytes);
    if (chain.declined) {
        return call.decline();
    }
    call.set_arg64(0, road64);
    call.set_arg64(1, segment64);
    call.set_arg(2, frame + kStepQuad);
    const n3::Run step = call.invoke([](auto& memory) { return n3::fn_00147cb0(memory); }, kFrameBytes);
    if (step.declined) {
        return call.decline();
    }
    retired += chain.retired + step.retired;
    const std::uint32_t dot = unit_dot(cpu, m, frame, input_quad);

    // The index: 5 * f20 * (dot / d - (F - S+1100) / F), negated, plus 128, truncated, clamped to 0..255 (pmaxw,
    // pminw). The window statistics then write the output words and the result is read from the object.
    const std::uint32_t blend_float = m.read_u32_le(object + kBlend);
    const std::uint32_t quotient = fpu::div(dot, distance);
    const std::uint32_t blend = fpu::div(fpu::sub(scale, blend_float), scale);
    const std::uint32_t product = fpu::mul(shade, fpu::sub(quotient, blend));
    const std::uint32_t scaled = fpu::neg(fpu::mul(kFive, product));
    const std::int32_t truncated = fpu::to_int(fpu::sub(scaled, kMinusOneTwentyEight));
    const auto index = static_cast<std::uint32_t>(std::min(std::max(truncated, 0), 255));

    call.set_arg64(0, object64);
    call.set_arg64(1, index);
    call.set_arg64(2, out0);
    call.set_arg64(3, out1);
    call.set_arg64(4, out2);
    const n3::Run window = call.invoke([](auto& memory) { return n3::fn_0013da30(memory); }, kFrameBytes);
    if (window.declined) {
        return call.decline();
    }
    retired += window.retired;
    const auto byte = static_cast<std::int8_t>(m.read_u8(object + index + kShadeBytes));
    call.return_value(static_cast<std::uint32_t>(static_cast<std::int32_t>(byte)));
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0013dd30, 552U, 0xf642af634eb040a3ULL, "RoadSegmentShadeLookup", kCeiling, road_segment_shade_lookup,
         EngineReturn::Integer)
