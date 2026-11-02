// 0x00257e10 EmitCarTrailParticles (a0 = effect system, a1 = source record R). Returns nothing.
//
// Does nothing unless the intensity at R+692 is strictly between 0 and 1.5. Otherwise it builds the trail's quads and
// shading floats and calls EmitTrailParticles (0x0025f0c0) with emission kind 8:
//   A = quad R+176 (the spread), B = quad at (word R+516) + 48 (the end),
//   D = B - A * scale on xyz (the start), scale = word a0 + 0x20000 - 1972; VU0 computes vf3 = A * scale,
//   f12 = |A * scale| (VU0 square root), f = 1.0 - G * intensity (G a gp-relative global), f13 = 0.75 + 0.25 * f,
//   f14 = 0.5 * f, and the shading floats m and k at frame +88 and +92, computed from the floats at P3 + 92 and
//   P3 + 96, where P3 is the word at (word R+688) + 3264.
// The callee reads its quads, floats and flag word through the pointers and the stack it is given, so they are laid
// out in the caller's frame here; those stores are below the entry $sp and not observable. This function's VU0
// sequence runs on the real VU0 state in the listing's order, so vf1..vf3, ACC, Q and the status and MAC flags end
// as the listing leaves them after it. EmitTrailParticles (0x0025f0c0) keeps its own VU0 work in local vectors, so
// the VU0 state the original's callee leaves behind is not reproduced by the call chain.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

N3_CALLEE(0025f0c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;
using Vec = guest::Gpr128;

// The source record (a1 = R).
constexpr GuestAddress kIntensity = 692U;      // float
constexpr GuestAddress kSpreadQuad = 176U;     // quad A
constexpr GuestAddress kEndPointer = 516U;     // word: the end quad B is 48 bytes into the block it points at
constexpr GuestAddress kEndQuadOffset = 48U;
constexpr GuestAddress kShadePointer = 688U;   // word: points at P2; P3 is the word at P2 + 3264
constexpr GuestAddress kShadeRecord = 3264U;
constexpr GuestAddress kGlowWord = 92U;        // float at P3 + 92 (the u side)
constexpr GuestAddress kFadeWord = 96U;        // float at P3 + 96 (the w side)
constexpr GuestAddress kCarryByte = 699U;      // the byte EmitTrailParticles keeps its fraction in

// The effect system's scale word.
constexpr GuestAddress kScaleWord = 0x00020000U - 1972U;

// The caller's frame, as offsets from its stack pointer after the 96-byte allocation.
constexpr std::uint32_t kFrameBytes = 96U;
constexpr GuestAddress kFlagSlot = 0U;         // 64-bit: 1, the "feed both trails" flag
constexpr GuestAddress kSpreadSlot = 32U;      // quad A
constexpr GuestAddress kEndSlot = 48U;         // quad B
constexpr GuestAddress kStartSlot = 64U;       // quad D
constexpr GuestAddress kShadeSlot = 88U;       // float m; float k follows at +92

// gp-relative float globals.
constexpr std::int32_t kFalloffGp = -29436;    // G in f = 1.0 - G * intensity
constexpr std::int32_t kGlowFloorGp = -23688;
constexpr std::int32_t kGlowGainGp = -27844;
constexpr std::int32_t kFadeFloorGp = -27840;
constexpr std::int32_t kFadeGainGp = -27836;
constexpr std::int32_t kShadeGainGp = -27828;
constexpr std::int32_t kOffsetGainGp = -27832;

// Raw float bits, the x-lane destination mask (bit 3 is the x lane), the emission kind and the callee ceiling.
constexpr std::uint32_t kZero = 0U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kThreeQuarters = 0x3f400000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kOneAndAHalf = 0x3fc00000U;
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint32_t kEmissionKind = 8U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kLowIntensityCost = 10U;     // 0x257e10..0x257e28, then the epilogue
constexpr std::uint32_t kHighIntensityCost = 16U;    // the same, and 0x257e2c..0x257e40
constexpr std::uint32_t kPrefixCost = 70U;           // 0x257e10..0x257f24: up to the glow/fade choice
constexpr std::uint32_t kGlowBelowFadeCost = 2U;     // 0x257f28..0x257f2c (glow < fade)
constexpr std::uint32_t kGlowAtLeastFadeCost = 1U;   // 0x257f30 (glow >= fade)
constexpr std::uint32_t kTailCost = 38U;             // 0x257f34..0x257fbc (the jal and its delay slot), epilogue

// MIN.S and MAX.S on raw encodings, as ee_cop1::min_s and max_s: the words compare as signed integers, reversed when
// both carry the sign bit. Operands are not normalised.
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// The clamp the listing applies to each shading value: MAX.S against +0, then MIN.S against +1.0.
std::uint32_t clamp01(const std::uint32_t value) { return min_bits(max_bits(value, kZero), kOne); }

struct VuTrail {
    Vec start;             // vf1 after the subtraction: B - A * scale on xyz, w = B.w
    std::uint32_t length;  // f12: the length of A * scale
};

// The VU0 sequence of the listing on the real VU0 state, one kernel per instruction, in the listing's order.
VuTrail vu0_trail(guest::R5900State& cpu, const Vec& scale_register, const Vec& spread, const Vec& end) {
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);     // vaddw.x vf1 = vf0.x + vf0.w (1.0)
    const Vec saved = cpu.read_vu0_vector(1U);                       // qmfc2 $v1, $vf1 (restored below)
    cpu.write_vu0_vector(2U, spread);                                // lqc2 vf2: the quad A stored earlier
    cpu.write_vu0_vector(1U, scale_register);                        // qmtc2 $a2: whole register, scale in word 0
    ee_cop2::vmul_broadcast(cpu, 3U, 2U, 1U, n3::kMaskXyz, 0U);     // vmulx.xyz vf3 = vf2 * vf1.x
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                    // vmul.xyz vf2 = vf3 * vf3
    cpu.write_vu0_vector(1U, end);                                   // qmtc2 $v0: the quad B
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, true);       // vsub.xyz vf1 = vf1 - vf3
    const Vec start = cpu.read_vu0_vector(1U);                       // sqc2 vf1: the quad D
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);               // vadday.x ACC.x = vf2.x + vf2.y
    cpu.write_vu0_vector(1U, saved);                                 // qmtc2 $v1: vf1 back, x = 1.0
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x: ACC.x + vf1.x * vf2.z
    ee_cop2::vsqrt(cpu, 1U, 0U);                                     // vsqrt Q, vf1.x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);     // vaddw.x vf1 = vf0.x + vf0.w (1.0)
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);  // vmulq.x vf1.x = vf1.x * Q
    return VuTrail{start, cpu.read_vu0_vector(1U).words[0]};        // mtc1 $v0, $f12
}

// Everything the call needs, decided before the frame is laid out.
struct Trail {
    Vec spread;             // a3: A
    Vec end;                // a2: B
    Vec start;              // a1: D
    std::uint32_t length;   // f12
    std::uint32_t rate;     // f13
    std::uint32_t size;     // f14
    std::uint32_t shade_m;  // frame +88
    std::uint32_t shade_k;  // frame +92
};

// Lays the quads, shading floats and flag word out in the caller's frame, sets the argument registers as the listing
// does and makes the call. `own` is the caller's count up to the jal's delay slot and its epilogue.
template <class Memory>
n3::Run emit_trail(n3::Call<Memory>& call, const GuestAddress record, const Trail& trail, const std::uint32_t own) {
    auto& mem = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    n3::store_quad_any(mem, frame + kSpreadSlot, trail.spread);
    n3::store_quad_any(mem, frame + kEndSlot, trail.end);
    n3::store_quad_any(mem, frame + kStartSlot, trail.start);
    mem.write_u32_le(frame + kShadeSlot, trail.shade_m);
    mem.write_u32_le(frame + kShadeSlot + 4U, trail.shade_k);
    mem.write_u64_le(frame + kFlagSlot, 1U);
    call.set_arg(1, frame + kStartSlot);
    call.set_arg(2, frame + kEndSlot);
    call.set_arg(3, frame + kSpreadSlot);
    call.set_arg(4, kEmissionKind);
    call.set_arg(5, frame + kShadeSlot);
    call.set_arg(6, record + kCarryByte);
    call.set_arg64(7, 0U);
    call.set_float_arg(0, trail.length);
    call.set_float_arg(1, trail.rate);
    call.set_float_arg(2, trail.size);
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0025f0c0(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(own + callee.retired);
}

template <class Context>
n3::Run emit_car_trail_particles(Context& context) {
    auto call = n3::guest_call(context);
    auto& mem = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress record = call.arg(1);
    const std::uint32_t intensity = mem.read_u32_le(record + kIntensity);
    if (fpu::le(intensity, kZero)) {
        return call.finish(kLowIntensityCost);
    }
    if (!fpu::lt(intensity, kOneAndAHalf)) {
        return call.finish(kHighIntensityCost);
    }
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto global = [&mem, gp](const std::int32_t offset) {
        return mem.read_u32_le(gp + static_cast<std::uint32_t>(offset));
    };

    // f = 1.0 - G * intensity (ADDA.S of 1.0, then MSUB.S): scales the rate and the size of the trail.
    const std::uint32_t falloff = fpu::msub(kOne, global(kFalloffGp), intensity);

    // VU0 part: the scale word is loaded into a2, the quads A and B come from the record.
    const Vec spread = n3::load_quad_any(mem, record + kSpreadQuad);
    const Vec end = n3::load_quad_any(mem, mem.read_u32_le(record + kEndPointer) + kEndQuadOffset);
    call.set_arg(2U, mem.read_u32_le(call.arg(0) + kScaleWord));
    const VuTrail vu = vu0_trail(cpu, call.arg128(2U), spread, end);

    // Shading: P3 = word at (word R+688) + 3264. u and w are the clamped glow and fade terms of its two floats.
    const GuestAddress shade = mem.read_u32_le(mem.read_u32_le(record + kShadePointer) + kShadeRecord);
    const std::uint32_t glow_in = mem.read_u32_le(shade + kGlowWord);
    const std::uint32_t fade_in = mem.read_u32_le(shade + kFadeWord);
    const std::uint32_t lower = min_bits(fade_in, glow_in);
    const std::uint32_t u = clamp01(fpu::mul(global(kGlowGainGp), fpu::sub(glow_in, global(kGlowFloorGp))));
    const std::uint32_t w = clamp01(fpu::mul(global(kFadeGainGp), fpu::sub(fade_in, global(kFadeFloorGp))));
    const std::uint32_t total = fpu::add(u, w);
    const bool glow_below_fade = fpu::lt(u, w);
    const std::uint32_t mixed = fpu::add(fpu::mul(total, total), glow_below_fade ? u : w);
    const std::uint32_t k = clamp01(fpu::mul(mixed, global(kShadeGainGp)));
    const std::uint32_t m = min_bits(max_bits(fpu::mul(lower, global(kOffsetGainGp)), kZero), fpu::sub(kOne, k));

    // f13 = 0.75 (the accumulator, from ADDA.S) + 0.25 * f (MADD.S); f14 = 0.5 * f. The accumulator ends at 0.75.
    const std::uint32_t rate = fpu::madd(kThreeQuarters, kQuarter, falloff);
    const std::uint32_t size = fpu::mul(kHalf, falloff);
    cpu.set_fpu_accumulator_bits(kThreeQuarters);

    const Trail trail{spread, end, vu.start, vu.length, rate, size, m, k};
    const std::uint32_t own = kPrefixCost + (glow_below_fade ? kGlowBelowFadeCost : kGlowAtLeastFadeCost) + kTailCost;
    return emit_trail(call, record, trail, own);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00257e10, 444U, 0x4d6534f6fdb821bbULL, "EmitCarTrailParticles", kCeiling, emit_car_trail_particles,
         EngineReturn::Nothing)
