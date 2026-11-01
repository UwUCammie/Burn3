// 0x0025d430 (a0 = ring buffer object, a1/a2/a3/t1 = four quadwords, t0 = vector, t2 = mode, f12 = time offset,
// f13 = solver argument): PushRingBufferEntryBurst.
// The time offset is added to the float at gp-27792; the integer part of the sum is the number of entries pushed
// and its fraction is stored back. A table at 0x004bc5f0 (12 bytes per mode t2) selects a per-mode quadword set,
// a pair of 16-bit values at object+0x20000-2008 is folded (low half sign-extended into the whole word, summed
// pairwise) and turned into a float scale that seeds the VU0 random unit (VRINIT). Each of the integer-part
// iterations then draws random vectors (VRNEXT), builds two quadwords from the running state at 0x01d9d190, adjusts
// one of them when its dot product with the first quadword is negative, stores the running state back and calls
// PushRingBufferEntry (0x00365e80) with the object's ring buffer at s2+8432+64*table word.
// The original works in a 624-byte stack frame; the stack words are written (the callee reads two quadwords from
// it, and the VU0 loads read words the function itself never wrote), the VU0 register file, accumulator, Q, R and
// MAC/STATUS flags are driven through guest::ee_cop2 exactly as the instructions do. s0-s5, f20 and ra are
// restored. Only the registers listed below are live out (none: the function returns nothing).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00365e80)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Registers.
constexpr unsigned kV0 = 2U, kV1 = 3U, kA0 = 4U, kA1 = 5U, kA2 = 6U, kA3 = 7U, kT0 = 8U, kT1 = 9U, kT2 = 10U,
                   kT3 = 11U, kT4 = 12U, kS0 = 16U, kS1 = 17U, kS2 = 18U, kS3 = 19U, kS4 = 20U, kS5 = 21U;

constexpr std::uint32_t kFrameBytes = 624U;
constexpr std::int32_t kGpFraction = -27792;    // swc1 f0 / lwc1 f0 (fraction of the time)
constexpr std::int32_t kGpQuarter = -31992;     // lwc1 f6 (mode 2)
constexpr std::int32_t kGpScale = -30736;       // lw a1 (VF2 broadcast x)
constexpr std::int32_t kGpFactor = -29600;      // lwc1 f0 in the setup
constexpr GuestAddress kModeTable = 0x004bc5f0U;  // lui 0x4c, addiu -14864
constexpr GuestAddress kRandomBase = 0x004bbfb0U;  // lq a0, -16464(0x4c0000)
constexpr GuestAddress kState = 0x01d9d190U;       // lqc2 vf3, -11888(0x01da0000)
constexpr GuestAddress kPairBase = 0x00020000U;    // object + 0x20000 holds the halfword pair words
constexpr std::int32_t kPairOffset = -2008;
constexpr std::int32_t kStepOffset = -1976;        // lwc1 f12 for the callee

// Retired counts of the original's straight-line blocks, delay slots included.
constexpr std::uint32_t kFirstBlock = 38U;     // 0x25d430..0x25d4c4
constexpr std::uint32_t kSelectBlock = 4U;     // 0x25d4c8..0x25d4d4 or 0x25d4d8..0x25d4e4
constexpr std::uint32_t kScaleBlock = 8U;      // 0x25d4e8..0x25d504
constexpr std::uint32_t kLengthMode3 = 19U;    // 0x25d508..0x25d550
constexpr std::uint32_t kLengthOther = 18U;    // 0x25d554..0x25d598
constexpr std::uint32_t kPairBlock = 33U;      // 0x25d59c..0x25d61c
constexpr std::uint32_t kPairPositive = 3U;    // 0x25d620..0x25d628
constexpr std::uint32_t kPairNegative = 7U;    // 0x25d62c..0x25d644
constexpr std::uint32_t kSetupBlock = 68U;     // 0x25d648..0x25d754
constexpr std::uint32_t kLoopHead = 66U;       // 0x25d758..0x25d85c
constexpr std::uint32_t kLoopFlip = 7U;        // 0x25d8ec..0x25d904
constexpr std::uint32_t kCallBlock = 30U;      // 0x25d860..0x25d8d4, up to the jal and its delay slot
constexpr std::uint32_t kLoopTail = 3U;        // 0x25d8d8..0x25d8e0
constexpr std::uint32_t kExitBlock = 11U;      // 0x25d8e4..0x25d928
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run push_ring_buffer_entry_burst(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const auto rd = [&](const unsigned r) { return cpu.read_gpr_u32(r); };
    const auto set32 = [&](const unsigned r, const std::uint32_t v) { cpu.write_gpr_sign_extended_32(r, v); };
    const auto set64 = [&](const unsigned r, const std::uint64_t v) { cpu.write_gpr_u64(r, v); };
    const auto lq = [&](const unsigned r, const GuestAddress a) { cpu.write_gpr(r, n3::load_quad_any(m, a)); };
    const auto sq = [&](const unsigned r, const GuestAddress a) { n3::store_quad_any(m, a, cpu.read_gpr(r)); };
    const auto lqc2 = [&](const unsigned vf, const GuestAddress a) {
        cpu.write_vu0_vector(vf, n3::load_quad_any(m, a));
    };
    const auto sqc2 = [&](const unsigned vf, const GuestAddress a) {
        n3::store_quad_any(m, a, cpu.read_vu0_vector(vf));
    };
    const auto qmtc2 = [&](const unsigned r, const unsigned vf) { cpu.write_vu0_vector(vf, cpu.read_gpr(r)); };
    const auto qmfc2 = [&](const unsigned r, const unsigned vf) { cpu.write_gpr(r, cpu.read_vu0_vector(vf)); };

    // What the original saves and restores.
    const Gpr128 saved_s[6] = {cpu.read_gpr(kS0), cpu.read_gpr(kS1), cpu.read_gpr(kS2),
                               cpu.read_gpr(kS3), cpu.read_gpr(kS4), cpu.read_gpr(kS5)};
    const std::uint32_t saved_f20 = cpu.read_fpr_bits(20U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto gp_at = [&](const std::int32_t offset) { return gp + static_cast<std::uint32_t>(offset); };
    const std::uint32_t f12_in = cpu.read_fpr_bits(12U);
    const std::uint32_t f13_in = cpu.read_fpr_bits(13U);

    // --- 0x25d430..0x25d4c4
    lq(kT3, rd(kA2));
    std::uint32_t f0 = m.read_u32_le(gp_at(kGpFraction));
    lq(kT4, rd(kA1));
    set32(kV1, rd(kT2) << 1U);
    set32(kV1, rd(kV1) + rd(kT2));
    std::uint32_t f1 = fpu::add(f0, f12_in);
    set32(kA1, rd(kV1) << 2U);
    lq(kA3, rd(kA3));
    set32(kV1, 0x004c0000U);
    lq(kA2, rd(kT1));
    set32(kV1, rd(kV1) + static_cast<std::uint32_t>(-14864));
    f0 = static_cast<std::uint32_t>(fpu::to_int(f1));
    set32(kV1, rd(kV1) + rd(kA1));
    set32(kA1, m.read_u32_le(rd(kV1)));
    set64(kS2, cpu.read_gpr_u64(kA0));
    set32(kA0, 2U);
    sq(kT4, frame + 592U);
    sq(kT3, frame + 576U);
    sq(kA3, frame + 560U);
    sq(kA2, frame + 544U);
    set32(kS1, f0);
    set32(kA1, rd(kA1) << 6U);
    f0 = fpu::from_int(static_cast<std::int32_t>(f0));
    set32(kA1, rd(kS2) + rd(kA1));
    set32(kS0, rd(kA1) + 8432U);
    f0 = fpu::sub(f1, f0);
    cpu.write_fpr_bits(20U, f13_in);                                // mov.s f20, f13
    m.write_u32_le(gp_at(kGpFraction), f0);                         // swc1 f0 (bne's delay slot)
    std::uint32_t retired = kFirstBlock;

    std::uint32_t f5 = 0U;
    std::uint32_t f6 = 0U;
    std::uint32_t f7 = 0U;
    if (rd(kT2) != 2U) {
        set32(kA1, 0x3e800000U);
        set32(kA0, 0x3f000000U);
        f6 = rd(kA1);
        f5 = rd(kA0);
    } else {
        set32(kA0, 0x3f800000U);
        f5 = rd(kA0);
        f6 = m.read_u32_le(gp_at(kGpQuarter));
    }
    retired += kSelectBlock;

    // --- 0x25d4e8: VF1 = Q-quadword * scale
    set32(kA1, m.read_u32_le(gp_at(kGpScale)));
    lqc2(1U, frame + 544U);
    set32(kA0, 3U);
    qmtc2(kA1, 2U);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);     // vmulx.xyz vf1, vf1, vf2x
    sqc2(1U, frame + 384U);
    retired += kScaleBlock;

    // The length of a vector (mode 3: the vector at t0; otherwise the second quadword) as a float in f7.
    const auto length_to_f7 = [&](const unsigned vf2_source_is_t0) {
        if (vf2_source_is_t0 != 0U) {
            lqc2(2U, rd(kT0));
        } else {
            lqc2(2U, frame + 576U);
        }
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf1, vf0, vf0w
        if (vf2_source_is_t0 != 0U) {
            ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);           // vmul.xyz vf2, vf2, vf2
            qmfc2(kA0, 1U);
        } else {
            qmfc2(kA0, 1U);
            ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);
        }
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);              // vadday.x acc, vf2, vf2y
        qmtc2(kA0, 1U);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);  // vmaddz.x vf1, vf1, vf2z
        qmfc2(kA0, 1U);
        qmtc2(kA0, 1U);
        ee_cop2::vsqrt(cpu, 1U, 0U);                                // vsqrt q, vf1x
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);    // vaddw.x vf1, vf0, vf0w
        qmfc2(kA0, 1U);
        qmtc2(kA0, 1U);                                              // (vwaitq has nothing to wait for)
        ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ); // vmulq.x vf1, vf1, q
        qmfc2(kA0, 1U);
        f7 = rd(kA0);
        set32(kA1, 0x00020000U);
    };
    if (rd(kA0) == rd(kT2)) {
        length_to_f7(1U);
        retired += kLengthMode3;
    } else {
        length_to_f7(0U);
        retired += kLengthOther;
    }

    // --- 0x25d59c: fold the halfword pair words at object + 0x20000 - 2008
    set32(kT2, 0x00020000U);
    set64(kA0, cpu.read_gpr_u64(kA1));
    set32(kT1, 0x00020000U);
    set32(kA1, rd(kS2) + rd(kA1));
    set32(kT0, 0x00020000U);
    set32(kT3, m.read_u32_le(rd(kA1) + static_cast<std::uint32_t>(kPairOffset)));
    set32(kA3, 0x00020000U);
    set32(kA2, 0x00020000U);
    set32(kA0, rd(kS2) + rd(kA0));
    set32(kT2, rd(kS2) + rd(kT2));
    set32(kT1, rd(kS2) + rd(kT1));
    set32(kT0, rd(kS2) + rd(kT0));
    set32(kA3, rd(kS2) + rd(kA3));
    set32(kA2, rd(kS2) + rd(kA2));
    set32(kT4, rd(kT3) << 16U);
    set32(kA1, 0x00020000U);
    set32(kT3, static_cast<std::uint32_t>(static_cast<std::int32_t>(rd(kT3)) >> 16));
    set32(kA1, rd(kS2) + rd(kA1));
    set32(kT3, rd(kT4) + rd(kT3));
    m.write_u32_le(rd(kA0) + static_cast<std::uint32_t>(kPairOffset), rd(kT3));
    set32(kT2, m.read_u32_le(rd(kT2) + static_cast<std::uint32_t>(kPairOffset)));
    set32(kA0, 0x00020000U);
    set32(kT1, m.read_u32_le(rd(kT1) + static_cast<std::uint32_t>(kPairOffset + 4)));
    set32(kA0, rd(kS2) + rd(kA0));
    set32(kT1, rd(kT2) + rd(kT1));
    m.write_u32_le(rd(kT0) + static_cast<std::uint32_t>(kPairOffset), rd(kT1));
    set32(kA3, m.read_u32_le(rd(kA3) + static_cast<std::uint32_t>(kPairOffset + 4)));
    set32(kA2, m.read_u32_le(rd(kA2) + static_cast<std::uint32_t>(kPairOffset)));
    set32(kA2, rd(kA3) + rd(kA2));
    m.write_u32_le(rd(kA1) + static_cast<std::uint32_t>(kPairOffset + 4), rd(kA2));
    set32(kA0, m.read_u32_le(rd(kA0) + static_cast<std::uint32_t>(kPairOffset)));
    retired += kPairBlock;
    if ((rd(kA0) & 0x80000000U) != 0U) {
        set32(kA1, rd(kA0) >> 1U);
        set32(kA0, rd(kA0) & 1U);
        set32(kA1, rd(kA1) | rd(kA0));
        f0 = rd(kA1);
        f1 = fpu::from_int(static_cast<std::int32_t>(f0));
        f1 = fpu::add(f1, f1);
        retired += kPairNegative;
    } else {
        f0 = rd(kA0);
        f1 = fpu::from_int(static_cast<std::int32_t>(f0));
        retired += kPairPositive;
    }

    // --- 0x25d648: seed the random unit and build the offset quadwords
    set32(kA0, 0x2f800000U);
    f0 = rd(kA0);
    f0 = fpu::mul(f0, f1);
    set32(kA0, f0);
    qmtc2(kA0, 1U);
    qmfc2(kA0, 1U);
    qmtc2(kA0, 1U);
    ee_cop2::vrinit(cpu, 1U, 0U, false);                            // vrinit r, vf1x
    set32(kA0, 0x3e800000U);
    lqc2(3U, frame + 592U);
    std::uint32_t f3 = m.read_u32_le(rd(kV1) + 4U);
    set32(kA2, frame + 464U);
    f0 = m.read_u32_le(gp_at(kGpFactor));
    set32(kA1, frame + 528U);
    f1 = m.read_u32_le(rd(kV1) + 8U);
    m.write_u32_le(frame + 240U, rd(kA0));
    std::uint32_t f4 = rd(kA0);
    m.write_u32_le(frame + 244U, rd(kA0));
    m.write_u32_le(frame + 248U, rd(kA0));
    lqc2(2U, frame + 240U);
    std::uint32_t f2 = fpu::neg(f3);
    f0 = fpu::mul(f0, f7);
    set32(kV1, frame + 496U);
    m.write_u32_le(frame + 228U, rd(kA0));
    lq(kA0, frame + 560U);
    ee_cop2::vadd_vector(cpu, 4U, 3U, 2U, n3::kMaskXyz, false);     // vadd.xyz vf4, vf3, vf2
    f1 = fpu::add(f4, f1);
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, n3::kMaskXyz, true);      // vsub.xyz vf2, vf3, vf2
    m.write_u32_le(frame + 224U, f2);
    sq(kA0, frame + 480U);
    m.write_u32_le(frame + 232U, f2);
    set32(kA3, f0);
    ee_cop2::vadd_vector(cpu, 2U, 4U, 2U, n3::kMaskXyz, true);      // vsub.xyz vf2, vf4, vf2
    set32(kT0, f0);
    m.write_u32_le(frame + 208U, f3);
    m.write_u32_le(frame + 216U, f3);
    sqc2(4U, frame + 448U);
    m.write_u32_le(frame + 212U, f1);
    f0 = fpu::add(f6, f6);
    lqc2(6U, frame + 224U);
    f1 = fpu::sub(f0, f5);
    qmtc2(kA3, 4U);
    f0 = fpu::sub(f5, f6);
    ee_cop2::vmul_broadcast(cpu, 6U, 6U, 4U, n3::kMaskXyz, 0U);     // vmulx.xyz vf6, vf6, vf4x
    set32(kA3, f1);
    sqc2(2U, rd(kA2));
    lqc2(5U, frame + 208U);
    set32(kA2, f0);
    qmtc2(kT0, 4U);
    ee_cop2::vmul_broadcast(cpu, 5U, 5U, 4U, n3::kMaskXyz, 0U);     // vmulx.xyz vf5, vf5, vf4x
    qmfc2(kS5, 2U);
    ee_cop2::vadd_vector(cpu, 2U, 5U, 6U, n3::kMaskXyz, true);      // vsub.xyz vf2, vf5, vf6
    sqc2(2U, rd(kA1));
    qmfc2(kS4, 2U);
    lqc2(1U, frame + 576U);
    qmtc2(kA0, 2U);
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);      // vsub.xyz vf1, vf2, vf1
    sqc2(1U, rd(kV1));
    qmfc2(kS3, 1U);
    lqc2(3U, frame + 432U);
    qmtc2(kA3, 1U);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, 4U, 0U, false);        // vaddx.y vf3, vf0, vf1x
    qmtc2(kA2, 1U);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, 8U, 0U, false);        // vaddx.x vf3, vf0, vf1x
    sqc2(5U, frame + 512U);
    sqc2(3U, frame + 432U);
    retired += kSetupBlock;

    // --- the loop: one PushRingBufferEntry per whole unit of time
    for (;;) {
        // 0x25d8d8: v1 = s1; bgtz v1 (s1 counts down in the delay slot)
        cpu.write_gpr_u64(kV1, cpu.read_gpr_u64(kS1));
        const bool again = static_cast<std::int64_t>(cpu.read_gpr_u64(kV1)) > 0;
        set32(kS1, rd(kS1) - 1U);
        retired += kLoopTail;
        if (!again) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }

        // --- 0x25d758
        set32(kV1, 0x01da0000U);
        set32(kV0, 0x004c0000U);
        lq(kA0, kRandomBase);
        ee_cop2::vrget(cpu, 1U, 8U, true);                          // vrnext.x vf1, r
        lqc2(3U, kState);
        qmtc2(kA0, 4U);
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, 8U, 0U);           // vmulx.x vf1, vf3, vf1x
        ee_cop2::vadda_broadcast(cpu, 4U, 1U, 8U, 0U);              // vaddax.x acc, vf4, vf1x
        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, 8U, 3U, false, true, true);   // vmsubaw.x acc, vf4, vf0w
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 8U, 3U, false, false, true);  // vmsubw.x vf1, vf1, vf0w
        qmfc2(kV0, 1U);
        lqc2(1U, frame + 480U);
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);    // vaddax.xyz acc, vf1, vf0x
        qmtc2(kV0, 2U);
        qmtc2(kS3, 1U);
        ee_cop2::vmadd_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U, false, false, false);  // vmaddx.xyz vf2, vf1, vf2x
        qmfc2(kA1, 2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                 // vmr32.xyzw vf1, vf3
        qmfc2(kV1, 1U);
        ee_cop2::vrget(cpu, 1U, 8U, true);                          // vrnext.x vf1, r
        lqc2(2U, frame + 432U);
        qmfc2(kV0, 1U);
        ee_cop2::vadda_broadcast(cpu, 0U, 2U, 8U, 1U);              // vadday.x acc, vf0, vf2y
        qmtc2(kV0, 1U);
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 0U, false, false, false);  // vmaddx.x vf1, vf2, vf1x
        qmfc2(kV0, 1U);
        qmtc2(kA1, 2U);
        qmtc2(kV0, 1U);
        ee_cop2::vmul_broadcast(cpu, 2U, 2U, 1U, n3::kMaskXyz, 0U); // vmulx.xyz vf2, vf2, vf1x
        ee_cop2::vrget(cpu, 1U, 8U, true);                          // vrnext.x vf1, r
        ee_cop2::vrget(cpu, 1U, 4U, true);                          // vrnext.y vf1, r
        qmtc2(kV1, 3U);
        ee_cop2::vrget(cpu, 1U, 2U, true);                          // vrnext.z vf1, r
        qmfc2(kA1, 2U);
        ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);               // vmul.xyz vf1, vf3, vf1
        ee_cop2::vadda(cpu, 1U, 4U, n3::kMaskXyz);                  // vadda.xyz acc, vf1, vf4
        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, n3::kMaskXyz, 3U, false, true, true);   // vmsubaw.xyz acc, vf4, vf0w
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, true);  // vmsubw.xyz vf1, vf1, vf0w
        qmfc2(kV0, 1U);
        lqc2(1U, frame + 512U);
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);    // vaddax.xyz acc, vf1, vf0x
        qmtc2(kV0, 2U);
        qmtc2(kS4, 1U);
        ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);       // vmadd.xyz vf2, vf1, vf2
        qmfc2(kV1, 2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                 // vmr32.xyzw vf1, vf3
        set32(kV0, 0x01da0000U);
        sqc2(1U, kState);
        qmtc2(kA1, 4U);
        qmtc2(kV1, 1U);
        ee_cop2::vadd_vector(cpu, 4U, 4U, 1U, n3::kMaskXyz, false); // vadd.xyz vf4, vf4, vf1
        lqc2(2U, frame + 384U);
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);    // vaddw.x vf1, vf0, vf0w
        sqc2(4U, frame + 416U);
        ee_cop2::vmul(cpu, 3U, 2U, 4U, n3::kMaskXyz);               // vmul.xyz vf3, vf2, vf4
        qmfc2(kV0, 1U);
        ee_cop2::vadda_broadcast(cpu, 3U, 3U, 8U, 1U);              // vadday.x acc, vf3, vf3y
        qmtc2(kV0, 1U);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 3U, 8U, 2U, false, false, false);  // vmaddz.x vf1, vf1, vf3z
        qmfc2(kV0, 1U);
        f1 = rd(kV0);
        f0 = 0U;                                                    // mtc1 zero, f0
        retired += kLoopHead;
        if (fpu::lt(f1, f0)) {                                      // c.olt.s f1, f0 ; bc1t 0x25d8ec
            set32(kV0, f1);
            qmtc2(kV0, 1U);
            ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, n3::kMaskXyz, 0U);   // vmulx.xyz vf1, vf2, vf1x
            ee_cop2::vadd_vector(cpu, 4U, 4U, 1U, n3::kMaskXyz, true);    // vsub.xyz vf4, vf4, vf1
            sqc2(4U, frame + 416U);
            retired += kLoopFlip;
        }

        // --- 0x25d860: the second quadword and the call
        ee_cop2::vrget(cpu, 2U, 8U, true);                          // vrnext.x vf2, r
        set32(kV0, 0x01da0000U);
        ee_cop2::vrget(cpu, 2U, 4U, true);                          // vrnext.y vf2, r
        lqc2(3U, kState);
        ee_cop2::vrget(cpu, 2U, 2U, true);                          // vrnext.z vf2, r
        qmtc2(kA0, 1U);
        ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);               // vmul.xyz vf2, vf3, vf2
        ee_cop2::vadda(cpu, 2U, 1U, n3::kMaskXyz);                  // vadda.xyz acc, vf2, vf1
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, n3::kMaskXyz, 3U, false, true, true);   // vmsubaw.xyz acc, vf1, vf0w
        ee_cop2::vmadd_broadcast(cpu, 2U, 2U, 0U, n3::kMaskXyz, 3U, false, false, true);  // vmsubw.xyz vf2, vf2, vf0w
        qmfc2(kV0, 2U);
        lqc2(1U, frame + 448U);
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);    // vaddax.xyz acc, vf1, vf0x
        qmtc2(kV0, 2U);
        qmtc2(kS5, 1U);
        ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);       // vmadd.xyz vf2, vf1, vf2
        qmfc2(kV0, 2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                 // vmr32.xyzw vf1, vf3
        set32(kV1, 0x01da0000U);
        sq(kV0, frame + 400U);
        cpu.write_gpr_u64(kA0, cpu.read_gpr_u64(kS0));              // daddu a0, s0, zero
        set32(kV0, 0x00020000U);
        set32(kA1, frame + 400U);
        set32(kV0, rd(kS2) + rd(kV0));
        set32(kA2, frame + 416U);
        sqc2(1U, kState);
        cpu.write_fpr_bits(13U, cpu.read_fpr_bits(20U));            // mov.s f13, f20
        cpu.write_fpr_bits(12U, m.read_u32_le(rd(kV0) + static_cast<std::uint32_t>(kStepOffset)));
        ee_cop2::vrget(cpu, 0U, 8U, true);                          // vrnext.x vf0, r (jal's delay slot)
        retired += kCallBlock;
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00365e80(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += callee.retired;
    }
    retired += kExitBlock;

    for (unsigned i = 0U; i < 6U; ++i) {
        cpu.write_gpr(kS0 + i, saved_s[i]);
    }
    cpu.write_fpr_bits(20U, saved_f20);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025d430, 1276U, 0x0c7a78310c69d784ULL, "PushRingBufferEntryBurst", kCeiling,
         push_ring_buffer_entry_burst, EngineReturn::Nothing)
