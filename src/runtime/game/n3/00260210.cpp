// 0x00260210 (a0 = effect system S, a1 = vehicle V, a2 = shade pair address): EmitVehicleSpray. Each frame, for one
// vehicle record V, nothing happens unless the speed-like float at V+84 is below 9.0 and the spray counter at V+4268
// (signed) is below 9999. Then the attachment record at V+64 lists its entries: a pointer at +5764 to 48-byte entries
// and a byte count at +5812. For each entry the local position (+0) goes through the vehicle's transform on VU0 (the
// origin at V+48 plus the axes at V+0, V+16, V+32 times the point), the local direction (+16) through the axes alone.
// The direction is scaled by a float (f20) and SpawnParticleBurst (0x0025ea70) makes the particles, with a2 handed on
// as its t1. Returns nothing.
//
// The scalars (f20, f22, f21 and the FPU accumulator) come from the counter as a float, five gp-relative floats and the
// effect system's gain at S+0x1f84c; spray_scalars keeps the original's operation order. The original saves and
// restores f20-f22 and s0-s6 in its frame, so the native keeps them in locals and never writes them to the CPU. The
// FPU accumulator and the FCR31 condition bit (set by the c.olt.s of the speed test) are left as the original leaves
// them. The VU0 registers a pass uses are written again at the top of every pass, because the burst call changes VU0.
//
// Retired counts: the speed test fails 32, the counter test fails 36, otherwise 83 plus 38 per entry plus each burst.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0025ea70)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// The vehicle record V.
constexpr GuestAddress kAxisX = 0U;                 // quad: the transform's axes and origin, V+0 .. V+48
constexpr GuestAddress kAxisY = 16U;
constexpr GuestAddress kAxisZ = 32U;
constexpr GuestAddress kOrigin = 48U;
constexpr GuestAddress kSpeed = 84U;                // float: sprays only while below 9.0
constexpr GuestAddress kSprayCounter = 4268U;       // int: sprays only while below 9999
constexpr GuestAddress kAttachment = 64U;           // pointer to the attachment record

// The attachment record.
constexpr GuestAddress kEntries = 5764U;            // pointer to the 48-byte entries
constexpr GuestAddress kEntryCount = 5812U;         // byte: number of entries
constexpr std::uint32_t kEntryBytes = 48U;

// The effect system S.
constexpr GuestAddress kSystemGain = 0x0001f84cU;   // float at S + 0x20000 - 1972 (= S + 0x1f84c)

// gp-relative tuning floats and the burst's carry byte.
constexpr std::int32_t kRateGp = -29556;            // rate = this * the counter
constexpr std::int32_t kDirectionBiasGp = -29732;   // direction scale = bias + gain * rate
constexpr std::int32_t kDirectionGainGp = -29936;
constexpr std::int32_t kFadeGainGp = -30200;        // fade = 1 - gain * speed
constexpr std::int32_t kSpreadBaseGp = -29552;      // spread = base + 11 * rate
constexpr std::int32_t kCarryByteGp = -23684;       // gp + this: the burst's carry byte address

// This function's frame (320 bytes below the entry $sp). The burst reads its stack word and two quads here.
constexpr std::uint32_t kFrameBytes = 320U;
constexpr GuestAddress kFlagSlot = 0U;              // u64: the burst's stack argument, zero
constexpr GuestAddress kDirectionSlot = 224U;       // quad: the scaled direction (the burst's a2 and a3)
constexpr GuestAddress kPositionSlot = 240U;        // quad: the world position (the burst's a1)

// Float constants and the burst's emission kind (t0).
constexpr std::uint32_t kLimitBits = 0x41100000U;   // 9.0f
constexpr std::int32_t kCounterLimit = 9999;
constexpr std::uint32_t kOneBits = 0x3f800000U;     // 1.0f
constexpr std::uint32_t kElevenBits = 0x41300000U;  // 11.0f
constexpr std::uint32_t kEmissionKind = 10U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 19U;        // 0x260210..0x260258: frame, saves, the speed test
constexpr std::uint32_t kCounterTestCost = 4U;      // 0x26025c..0x260268: the counter test
constexpr std::uint32_t kSetupCost = 43U;           // 0x26026c..0x260314: scalars, quads, the jump to the loop test
constexpr std::uint32_t kLoopTestCost = 4U;         // 0x2603a0..0x2603ac: once per entry, and once to leave
constexpr std::uint32_t kEntryCost = 34U;           // 0x260318..0x26039c: the VU0 transform and the burst call
constexpr std::uint32_t kEpilogueCost = 13U;        // 0x2603b0..0x2603e0
constexpr std::uint32_t kSpeedSkipCost = kPrologueCost + kEpilogueCost;             // 32
constexpr std::uint32_t kCounterSkipCost = kSpeedSkipCost + kCounterTestCost;       // 36
constexpr std::uint32_t kSprayCost = kPrologueCost + kCounterTestCost + kSetupCost; // 66, before the loop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The high word of a sign-extended word: what MFC1 leaves in the upper half of a GPR.
constexpr std::uint32_t sign_word(const std::uint32_t bits) {
    return (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Memory>
std::uint32_t read_gp(const Memory& m, const std::uint32_t gp, const std::int32_t offset) {
    return m.read_u32_le(gp + static_cast<std::uint32_t>(offset));
}

// The scalars of the spray (raw float bits) and the FPU accumulator the last ADDA.S leaves.
struct Scalars {
    std::uint32_t direction_scale{};   // f20: multiplies the local direction
    std::uint32_t intensity{};         // f22: the burst's f12
    std::uint32_t size{};              // f21: the burst's f13
    std::uint32_t accumulator{};
};

// speed and count are raw bits: V+84 and the counter converted to float. Each ADDA.S starts from 0 (f5 is zero).
template <class Memory>
Scalars spray_scalars(const Memory& m, const std::uint32_t gp, const GuestAddress system, const std::uint32_t speed,
                      const std::uint32_t count) {
    const std::uint32_t rate = fpu::mul(read_gp(m, gp, kRateGp), count);
    // ACC = 0 + bias; f20 = ACC + gain * rate
    const std::uint32_t direction_scale =
        fpu::madd(fpu::add(0U, read_gp(m, gp, kDirectionBiasGp)), read_gp(m, gp, kDirectionGainGp), rate);
    // ACC = 0 + 1.0; f3 = ACC - fade gain * speed
    const std::uint32_t fade = fpu::msub(fpu::add(0U, kOneBits), read_gp(m, gp, kFadeGainGp), speed);
    // ACC = 0 + spread base; f1 = ACC + 11.0 * rate
    const std::uint32_t spread_base = read_gp(m, gp, kSpreadBaseGp);
    const std::uint32_t spread = fpu::madd(fpu::add(0U, spread_base), kElevenBits, rate);
    // f22 = gain * spread; f21 = 1 - (1 - fade * (1 - rate))^2
    const std::uint32_t intensity = fpu::mul(m.read_u32_le(system + kSystemGain), spread);
    const std::uint32_t inner = fpu::sub(kOneBits, fpu::mul(fade, fpu::sub(kOneBits, rate)));
    const std::uint32_t size = fpu::sub(kOneBits, fpu::mul(inner, inner));
    return Scalars{direction_scale, intensity, size, fpu::add(0U, spread_base)};
}

template <class Context>
n3::Run emit_vehicle_spray(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint64_t system64 = call.arg64(0U);     // a0, copied whole by the original (daddu)
    const GuestAddress system = call.arg(0U);
    const GuestAddress vehicle = call.arg(1U);
    const std::uint64_t shade_pair = call.arg64(2U);   // a2, handed on as the burst's t1 (daddu)
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // c.olt.s f10, f0 (speed < 9.0): the condition bit is set on every path that reaches it.
    const std::uint32_t speed = m.read_u32_le(vehicle + kSpeed);
    const bool slow = fpu::lt(speed, kLimitBits);
    cpu.set_fcr31_bits(slow ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                            : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
    if (!slow) {
        return call.finish(kSpeedSkipCost);
    }
    const std::uint32_t counter = m.read_u32_le(vehicle + kSprayCounter);
    if (static_cast<std::int32_t>(counter) >= kCounterLimit) {
        return call.finish(kCounterSkipCost);
    }

    const Scalars scalars = spray_scalars(m, gp, system, speed, fpu::from_int(static_cast<std::int32_t>(counter)));
    cpu.set_fpu_accumulator_bits(scalars.accumulator);

    // The transform quads are read once; the original keeps them in its frame and reloads them each pass.
    const Gpr128 axis_x = n3::load_quad_any(m, vehicle + kAxisX);
    const Gpr128 axis_y = n3::load_quad_any(m, vehicle + kAxisY);
    const Gpr128 axis_z = n3::load_quad_any(m, vehicle + kAxisZ);
    const Gpr128 origin = n3::load_quad_any(m, vehicle + kOrigin);
    const GuestAddress attachment = m.read_u32_le(vehicle + kAttachment);
    const GuestAddress entries = m.read_u32_le(attachment + kEntries);
    const std::uint32_t entry_count = m.read_u8(attachment + kEntryCount);

    std::uint32_t retired = kSprayCost;
    for (std::uint32_t pass = 0U; pass < entry_count; ++pass) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress entry = entries + pass * kEntryBytes;

        // VU0: the world position is origin + axis_x * x + axis_y * y + axis_z * z (the origin is added last).
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry));
        cpu.write_vu0_vector(4U, axis_x);
        ee_cop2::vmula_broadcast(cpu, 4U, 1U, n3::kMaskXyz, false);
        cpu.write_vu0_vector(3U, axis_y);
        ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);
        cpu.write_vu0_vector(2U, axis_z);
        ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 1U, n3::kMaskXyz, 2U, false, true, false);
        cpu.write_vu0_vector(1U, origin);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, false);
        n3::store_quad_any(m, frame + kPositionSlot, cpu.read_vu0_vector(1U));

        // VU0: the direction is the axes alone. Its w lane and the high half of the register survive into VF2:
        // MFC1 replaces only the low 64 bits (f20, sign-extended), so VF2 = {f20, sign, dir.z, dir.w}; VF2.x scales it.
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry + 16U));
        ee_cop2::vmula_broadcast(cpu, 4U, 1U, n3::kMaskXyz, false);
        ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false);
        Gpr128 scale_quad = cpu.read_vu0_vector(1U);
        scale_quad.words[0] = scalars.direction_scale;
        scale_quad.words[1] = sign_word(scalars.direction_scale);
        cpu.write_vu0_vector(2U, scale_quad);
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);
        n3::store_quad_any(m, frame + kDirectionSlot, cpu.read_vu0_vector(1U));

        // The burst's operands, as the original leaves them before its jal (the stack word in the delay slot).
        m.write_u64_le(frame + kFlagSlot, 0U);
        call.set_arg64(0U, system64);
        call.set_arg(1U, frame + kPositionSlot);
        call.set_arg(2U, frame + kDirectionSlot);
        call.set_arg(3U, frame + kDirectionSlot);
        call.set_arg(4U, kEmissionKind);
        call.set_arg64(5U, shade_pair);
        call.set_arg(6U, gp + static_cast<std::uint32_t>(kCarryByteGp));
        call.set_arg64(7U, 0U);
        call.set_float_arg(0U, scalars.intensity);
        call.set_float_arg(1U, scalars.size);
        const n3::Run burst = call.invoke([](auto& machine) { return n3::fn_0025ea70(machine); }, kFrameBytes);
        if (burst.declined) {
            return call.decline();
        }
        retired += kLoopTestCost + kEntryCost + burst.retired;
    }
    retired += kLoopTestCost + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00260210, 468U, 0x9d584a921e89f53fULL, "EmitVehicleSpray", kCeiling, emit_vehicle_spray,
         EngineReturn::Nothing)
