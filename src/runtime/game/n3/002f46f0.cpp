// 0x002f46f0 (a0 = ring record R, a1/a2/a3/t0 = quads P/Q/S/C, f12..f15 = floats): PushImpactSpriteRingEntry.
// R holds the entry array base at +20, its capacity at +24 and the cursor at +28. The 64-byte entry at
// base + (cursor << 6) takes P.xyz at +0 and Q.xyz at +16. Each of those two quads gets its w lane from the
// sprite setup: the square-root path when 0 < f5 * f5 - g0 * f14 < (g1 + f5)^2, else the y lanes of P and Q.
// +32 is S.xyz with f12 as its w, +48 holds f15, +52 the packed colour C.xyz * (the quad at 0x004c4240)
// as x | y << 8 | z << 16 | 0x80000000, and +56 holds f13. The cursor then advances and wraps to 0 at the
// capacity. The original's round trips through its stack frame are kept in registers (the frame is scratch).
// Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kColourScale = 0x004c4240U;  // lui 0x4c, lqc2 16960(v1)
constexpr std::uint8_t kMaskY = 4U;                 // destination lane y (x = 8, z = 2, w = 1)

// $gp-relative float constants (lwc1 fN, offset($gp)).
constexpr std::int32_t kG0 = -29488;
constexpr std::int32_t kG1 = -29484;
constexpr std::int32_t kG2 = -29480;
constexpr std::int32_t kG3 = -29476;
constexpr std::int32_t kG4 = -29472;
constexpr std::int32_t kG5 = -29468;

// The VU0 register that qmtc2 fills from a GPR that mfc1 has just written: the float is the low word and its
// sign-extension the next. The upper doubleword is zero. Each such register is read only through its x lane
// (the y lane of vf1 is produced by the operation that reads it), so those zeros are never observed.
guest::Gpr128 vector_from_float(const std::uint32_t bits) {
    const std::uint32_t high = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return guest::Gpr128{{bits, high, 0U, 0U}};
}

template <class Context>
n3::Run push_impact_sprite_ring_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress ring = call.arg(0);
    const guest::Gpr128 p_quad = n3::load_quad_any(m, call.arg(1));
    const guest::Gpr128 q_quad = n3::load_quad_any(m, call.arg(2));
    const guest::Gpr128 s_quad = n3::load_quad_any(m, call.arg(3));
    const guest::Gpr128 c_quad = n3::load_quad_any(m, call.arg(4));
    const std::uint32_t cursor = m.read_u32_le(ring + 28U);
    const GuestAddress entry = m.read_u32_le(ring + 20U) + (cursor << 6U);
    std::uint32_t f5 = q_quad.words[1];
    const std::uint32_t f12 = cpu.read_fpr_bits(12U);
    const std::uint32_t f13 = cpu.read_fpr_bits(13U);
    const std::uint32_t f14 = cpu.read_fpr_bits(14U);
    const std::uint32_t f15 = cpu.read_fpr_bits(15U);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto gp_word = [&m, gp](const std::int32_t offset) {
        return m.read_u32_le(gp + static_cast<std::uint32_t>(offset));
    };

    // 19 retired up to the first branch (0x2f46f0..0x2f4738). f5 (Q.y) below zero with f14 <= 0 is negated.
    std::uint32_t retired = 19U;
    if (fpu::lt(f5, 0U)) {
        retired += 3U;
        if (fpu::le(f14, 0U)) {
            retired += 1U;
            f5 = fpu::neg(f5);
        }
    }
    retired += 14U;

    // mula.s ACC = f5 * f5; msub.s f1 = ACC - g0 * f14.
    const std::uint32_t accumulator = fpu::mul(f5, f5);
    cpu.set_fpu_accumulator_bits(accumulator);
    const std::uint32_t f1 = fpu::msub(accumulator, gp_word(kG0), f14);

    // Entry +0 and +16 get P.xyz and Q.xyz first, each w kept until the path below overwrites it.
    cpu.write_vu0_vector(3U, p_quad);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, entry));
    cpu.write_vu0_vector(1U, q_quad);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 3U, n3::kMaskXyz, false);
    n3::store_quad_any(m, entry, cpu.read_vu0_vector(2U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, entry + 16U));
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, false);
    n3::store_quad_any(m, entry + 16U, cpu.read_vu0_vector(2U));

    // Square-root path when f1 > 0 and f1 < (g1 + f5)^2; otherwise the ALT path.
    bool square_root = false;
    if (!fpu::le(f1, 0U)) {
        retired += 6U;
        const std::uint32_t sum = fpu::add(gp_word(kG1), f5);
        const std::uint32_t bound = fpu::mul(sum, sum);
        square_root = fpu::lt(f1, bound);
    }

    if (square_root) {
        retired += 40U;
        const std::uint32_t root = fpu::sqrt(f1);                  // sqrt.s f0, f1
        const std::uint32_t f4a = fpu::add(root, f5);              // add.s f4, f0, f5
        const std::uint32_t f4 = fpu::mul(gp_word(kG2), f4a);      // lwc1 f3 (g2); mul.s f4, f3, f4
        const std::uint32_t f3 = fpu::mul(gp_word(kG3), f4);       // lwc1 f2 (g3); mul.s f3, f2, f4
        const std::uint32_t f2a = fpu::add(f5, f3);                // add.s f2, f5, f3
        const std::uint32_t f1a = fpu::mul(gp_word(kG4), f2a);     // lwc1 f1 (g4); mul.s f1, f1, f2
        const std::uint32_t f2 = fpu::sub(f1a, f3);                // sub.s f2, f1, f3
        const std::uint32_t f1b = fpu::mul(f2, f4);                // mul.s f1, f2, f4
        const std::uint32_t f0a = fpu::mul(gp_word(kG5), f4);      // lwc1 f0 (g5); mul.s f0, f0, f4
        const std::uint32_t f0b = fpu::mul(f0a, f4);               // mul.s f0, f0, f4

        // Y lane of the sprite setup: P.y - f14 - f1b - f0b, through vf2 and vf1 as the VU does it.
        cpu.write_vu0_vector(1U, vector_from_float(f14));                         // qmtc2 a1, vf1
        ee_cop2::vadd_broadcast(cpu, 2U, 3U, 1U, kMaskY, 0U, true);              // vsubx.y vf2, vf3, vf1x
        cpu.write_vu0_vector(1U, vector_from_float(f1b));                         // qmtc2 a2, vf1
        ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, kMaskY, 0U, true);              // vsubx.y vf2, vf2, vf1x
        cpu.write_vu0_vector(1U, vector_from_float(f0b));                         // qmtc2 a1, vf1
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskY, 0U, true);              // vsubx.y vf1, vf2, vf1x
        // The original stores vf1 to its frame and reads the y lane back (lw a1, 68(sp)).
        const std::uint32_t y_lane = cpu.read_vu0_vector(1U).words[1];
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry));                        // lqc2 vf1, 0(t0)
        cpu.write_vu0_vector(3U, vector_from_float(y_lane));                      // qmtc2 a1, vf3
        ee_cop2::vmul_broadcast(cpu, 1U, 0U, 3U, n3::kMaskW, 0U);                // vmulx.w vf1, vf0, vf3x
        n3::store_quad_any(m, entry, cpu.read_vu0_vector(1U));                        // sqc2 vf1, 0(t0)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry + 16U));                  // lqc2 vf1, 16(t0)
        cpu.write_vu0_vector(2U, vector_from_float(f2));                          // mfc1 v1, f2; qmtc2 v1, vf2
        ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);                // vmulx.w vf1, vf0, vf2x
        n3::store_quad_any(m, entry + 16U, cpu.read_vu0_vector(1U));                  // sqc2 vf1, 16(t0)
    } else {
        retired += 9U;
        cpu.write_vu0_vector(2U, p_quad);                                         // lqc2 vf2, 416(sp)
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, entry));                        // lqc2 vf3, 0(t0)
        cpu.write_vu0_vector(1U, q_quad);                                         // lqc2 vf1, 400(sp)
        ee_cop2::vmul_broadcast(cpu, 3U, 0U, 2U, n3::kMaskW, 1U);                // vmuly.w vf3, vf0, vf2y
        n3::store_quad_any(m, entry, cpu.read_vu0_vector(3U));                        // sqc2 vf3, 0(t0)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, entry + 16U));                  // lqc2 vf2, 16(t0)
        ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 1U);                // vmuly.w vf2, vf0, vf1y
        n3::store_quad_any(m, entry + 16U, cpu.read_vu0_vector(2U));                  // sqc2 vf2, 16(t0)
    }

    // Common tail (0x2f483c): S.xyz with f12 as w at +32, f15 at +48.
    cpu.write_vu0_vector(2U, s_quad);                                             // lqc2 vf2, 384(sp)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, entry + 32U));                      // lqc2 vf3, 32(t0)
    cpu.write_vu0_vector(1U, vector_from_float(f12));                             // mfc1 t1, f12; qmtc2 t1, vf1
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, n3::kMaskXyz, false);                   // vadd.xyz vf3, vf0, vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, n3::kMaskW, 3U, true);              // vsubw.w vf3, vf0, vf0w
    ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, n3::kMaskW, 0U, false);             // vaddx.w vf3, vf3, vf1x
    n3::store_quad_any(m, entry + 32U, cpu.read_vu0_vector(3U));                      // sqc2 vf3, 32(t0)
    m.write_u32_le(entry + 48U, f15);                                             // swc1 f15, 48(t0)

    // Colour: C.xyz * K.xyz, each lane converted with cvt.w.s and packed.
    cpu.write_vu0_vector(2U, c_quad);                                             // lqc2 vf2, 368(sp)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, kColourScale));                     // lqc2 vf1, 16960(v1)
    ee_cop2::vmul(cpu, 1U, 2U, 1U, n3::kMaskXyz);                                 // vmul.xyz vf1, vf2, vf1
    const guest::Gpr128 colour = cpu.read_vu0_vector(1U);
    const std::uint32_t x_int = static_cast<std::uint32_t>(fpu::to_int(colour.words[0]));
    const std::uint32_t y_int = static_cast<std::uint32_t>(fpu::to_int(colour.words[1]));
    const std::uint32_t z_int = static_cast<std::uint32_t>(fpu::to_int(colour.words[2]));
    m.write_u32_le(entry + 52U, x_int | (y_int << 8U) | (z_int << 16U) | 0x80000000U);  // sw v1, 52(t0)
    m.write_u32_le(entry + 56U, f13);                                                   // swc1 f13, 56(t0)
    cpu.write_fpr_bits(0U, z_int);                                                      // f0: last cvt.w.s

    // Cursor: advance, reread, and wrap to zero when it reaches the capacity.
    const std::uint32_t advanced = m.read_u32_le(ring + 28U) + 1U;                // lw v1, 28(a0); addiu
    m.write_u32_le(ring + 28U, advanced);                                         // sw v1, 28(a0)
    const std::uint32_t reread = m.read_u32_le(ring + 28U);                       // lw a1, 28(a0)
    // 0x2f483c..0x2f48ec (45 retired, including the cursor test), then 0x2f48f0..0x2f48f4 and 0x2f4924..0x2f4928.
    retired += 45U + 4U;
    if (reread == m.read_u32_le(ring + 24U)) {                                    // lw v1, 24(a0); beq
        retired += 2U;                                                            // 0x2f491c..0x2f4920
        m.write_u32_le(ring + 28U, 0U);                                           // sw zero, 28(a0)
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002f46f0{0x002f46f0U, 572U, 0x1222efc300cca876ULL,
                                                  "PushImpactSpriteRingEntry", 134U,
                                                  n3::live<&push_impact_sprite_ring_entry<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f46f0, push_impact_sprite_ring_entry)
