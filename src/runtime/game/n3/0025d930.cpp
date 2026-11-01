// 0x0025d930 (a0 = object O, a1 = pointer to quad Q1, a2 = pointer to quad Q2, a3 = pointer to quad Q3,
// t0 = mode flag, f12 = f23 = rate, f13 = f22 = start parameter): SpawnBallisticTrailSamples.
// The three quads are copied to the frame (Q3 at 1136, Q2 at 1152, Q1 at 1168). The length of Q3.xyz squared
// (VU0 vmul/vadday/vmaddz) is compared with 2500.0: at most 2500 the scale f2 is the global float at gp-29512
// times its square root, otherwise 1.0. Two words at O+0x1f828 / O+0x1f82c are stepped as the game's generator
// does (rotate by 16, then two additions) and the high word, scaled by 2^-32, seeds the VU0 R register. Constant
// quads are built on the frame (f2-scaled combinations of fixed vectors and Q3) and then a sample count s2 is
// derived from the float at gp-27796: mode flag set uses ACC = fraction + 7.0*rate, clear uses fraction + rate;
// the integer part is the count and the fraction goes back to gp-27796. With a count above zero the loop runs
// s2 times: random vectors from vf3/vf4 (VU0 vrnext) are combined with the frame quads, the result quads go to
// the stack and the global quad at 0x01d9d190, and RecordBallisticTrailSample (0x00263c40) is called with a ring
// set at O+8688; the generator words step again and, when the lowest bit of the first is set, PushRingBufferEntry
// (0x00365e80) is called as well with O+8432. After each iteration the vector at 976 is added to the vector at
// 992 and its y word to the running parameter f24. When the count is exhausted with the flag set the flag is
// cleared and the count is derived again with the other formula. The VU0 registers, ACC and R end as the
// original leaves them; every word the original writes to the frame is mirrored because lqc2 reads stale slots.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00263c40)
N3_CALLEE(00365e80)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 1200U;           // addiu sp,sp,-1200
constexpr GuestAddress kRandomX = 0x0001f828U;         // O + 0x1f828 (lui 1 ori 0xf828; 0x20000 - 2008)
constexpr GuestAddress kRandomY = 0x0001f82cU;         // O + 0x1f82c
constexpr GuestAddress kRate = 0x0001f848U;            // O + 0x1f848: f12 of both calls
constexpr GuestAddress kTrailRing = 8688U;             // O + 8688: a0 of RecordBallisticTrailSample
constexpr GuestAddress kPushRing = 8432U;              // O + 8432: a0 of PushRingBufferEntry
constexpr GuestAddress kGlobalVector = 0x01d9d190U;    // lui 0x01da, -11888
constexpr GuestAddress kGlobalConstants = 0x004bbfb0U; // lui 0x004c, -16464
constexpr std::int32_t kScaleOffset = -29512;          // gp-relative float
constexpr std::int32_t kFractionOffset = -27796;       // gp-relative float
constexpr std::uint32_t kLengthLimit = 2500U;          // addiu v1,zero,2500
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kSeven = 0x40e00000U;          // f21
constexpr std::uint32_t kTwoToMinus32 = 0x2f800000U;
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

// Register numbers.
constexpr unsigned kV0 = 2U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;
constexpr unsigned kT3 = 11U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 44U;       // 0x25d930..0x25d9dc
constexpr std::uint32_t kScaleCost = 6U;        // 0x25d9e0..0x25d9f4: sqrt path
constexpr std::uint32_t kUnitScaleCost = 4U;    // 0x25dc50..0x25dc5c: f2 = 1.0
constexpr std::uint32_t kGeneratorCost = 35U;   // 0x25d9f8..0x25da80
constexpr std::uint32_t kPositiveCost = 3U;     // 0x25da84..0x25da8c
constexpr std::uint32_t kNegativeCost = 7U;     // 0x25da90..0x25daa8
constexpr std::uint32_t kSetupCost = 77U;       // 0x25daac..0x25dbdc
constexpr std::uint32_t kCountHeadCost = 4U;    // 0x25dbe0..0x25dbec
constexpr std::uint32_t kCountFlagCost = 11U;   // 0x25dbf0..0x25dc18
constexpr std::uint32_t kCountPlainCost = 7U;   // 0x25dc1c..0x25dc34
constexpr std::uint32_t kCountTestCost = 2U;    // 0x25dc38..0x25dc3c
constexpr std::uint32_t kFlagTestCost = 2U;     // 0x25dc40..0x25dc44
constexpr std::uint32_t kFlagClearCost = 2U;    // 0x25dc48..0x25dc4c
constexpr std::uint32_t kLoopHeadCost = 12U;    // 0x25dc60..0x25dc8c
constexpr std::uint32_t kBodyCost = 89U;        // 0x25dc90..0x25ddf0 (the jal and its delay slot included)
constexpr std::uint32_t kPushCost = 48U;        // 0x25de28..0x25dee4 (the jal and its delay slot included)
constexpr std::uint32_t kEvenCost = 1U;         // 0x25ddf4
constexpr std::uint32_t kStepCost = 10U;        // 0x25ddf8..0x25de1c
constexpr std::uint32_t kLoopExitCost = 2U;     // 0x25de20..0x25de24
constexpr std::uint32_t kReturnCost = 17U;      // 0x25dee8..0x25df28

constexpr std::uint32_t kCeiling = 20000U;

// A 32-bit result (lui, addiu, lw, mfc1, ...) replaces the low doubleword, sign-extended, and keeps the high one.
void write32(Gpr128& reg, const std::uint32_t bits) {
    reg.words[0] = bits;
    reg.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Context>
n3::Run spawn_ballistic_trail_samples(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress sp = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    Gpr128 r[32];
    for (unsigned n = 0U; n < 32U; ++n) {
        r[n] = cpu.read_gpr(n);
    }
    std::uint32_t retired = 0U;

    // --- 0x25d930..0x25d9dc: entry. s4 = O, s3 = flag (a doubleword), f23 = rate, f22 = parameter.
    const GuestAddress object = r[kA0].words[0];
    std::uint64_t flag = static_cast<std::uint64_t>(r[kT0].words[0]) | (static_cast<std::uint64_t>(r[kT0].words[1]) << 32U);
    r[kV1] = n3::load_quad_any(m, r[kA3].words[0]);
    r[kA0] = n3::load_quad_any(m, r[kA2].words[0]);
    r[kA1] = n3::load_quad_any(m, r[kA1].words[0]);
    std::uint32_t f23 = cpu.read_fpr_bits(12U);
    std::uint32_t f22 = cpu.read_fpr_bits(13U);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                 // vaddw.x vf1,vf0,vf0w
    n3::store_quad_any(m, sp + 1136U, r[kV1]);
    r[kA2] = cpu.read_vu0_vector(1U);                                           // qmfc2 a2,vf1
    cpu.write_vu0_vector(1U, r[kV1]);                                           // qmtc2 v1,vf1
    ee_cop2::vmul(cpu, 1U, 1U, 1U, n3::kMaskXyz);                               // vmul.xyz vf1,vf1,vf1
    n3::store_quad_any(m, sp + 1168U, r[kA1]);
    n3::store_quad_any(m, sp + 1152U, r[kA0]);
    r[kV1] = cpu.read_vu0_vector(1U);                                           // qmfc2 v1,vf1
    cpu.write_vu0_vector(2U, r[kV1]);                                           // qmtc2 v1,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                          // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, r[kA2]);                                           // qmtc2 a2,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false); // vmaddz.x vf1,vf1,vf2z
    r[kV1] = cpu.read_vu0_vector(1U);                                           // qmfc2 v1,vf1
    std::uint32_t f1 = r[kV1].words[0];
    std::uint32_t f0 = fpu::from_int(static_cast<std::int32_t>(kLengthLimit));
    std::uint32_t f2 = 0U;
    retired += kEntryCost;
    if (fpu::le(f1, f0)) {
        retired += kScaleCost;
        f1 = fpu::sqrt(f1);
        f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kScaleOffset));
        f2 = fpu::mul(f0, f1);
    } else {
        retired += kUnitScaleCost;
        f2 = kOne;
    }

    // --- 0x25d9f8..0x25daa8: the generator words of O step; the high word becomes f0.
    {
        const GuestAddress x_address = object + kRandomX;
        const GuestAddress y_address = object + kRandomY;
        std::uint32_t x = m.read_u32_le(x_address);
        x = (x << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> 16);
        m.write_u32_le(x_address, x);
        const std::uint32_t t2 = m.read_u32_le(x_address);
        const std::uint32_t t1 = m.read_u32_le(y_address);
        m.write_u32_le(x_address, t2 + t1);
        const std::uint32_t y = m.read_u32_le(y_address);
        const std::uint32_t xn = m.read_u32_le(x_address);
        m.write_u32_le(y_address, y + xn);
        const std::uint32_t v1 = m.read_u32_le(x_address);
        retired += kGeneratorCost;
        f1 = kTwoToMinus32;
        if (static_cast<std::int32_t>(v1) < 0) {
            retired += kNegativeCost;
            const std::uint32_t halved = (v1 >> 1U) | (v1 & 1U);
            f0 = fpu::from_int(static_cast<std::int32_t>(halved));
            f0 = fpu::add(f0, f0);
        } else {
            retired += kPositiveCost;
            f0 = fpu::from_int(static_cast<std::int32_t>(v1));
        }
        f0 = fpu::mul(f1, f0);
        write32(r[kV1], f0);                                                     // mfc1 v1,f0
    }

    // --- 0x25daac..0x25dbdc: R seed and the constant quads on the frame.
    cpu.write_vu0_vector(1U, r[kV1]);                                           // qmtc2 v1,vf1
    ee_cop2::vrinit(cpu, 1U, 0U, false);                                        // vrinit $r,vf1x
    write32(r[kA0], 0xc0400000U);
    write32(r[kT1], 0x3e800000U);
    m.write_u32_le(sp + 624U, 0xc0400000U);
    write32(r[kT0], 0x3f000000U);
    m.write_u32_le(sp + 632U, 0xc0400000U);
    write32(r[kA3], 0x40400000U);
    write32(r[kA0], f2);                                                         // mfc1 a0,f2
    write32(r[kA2], 0xc1000000U);
    write32(r[kA1], 0x41000000U);
    write32(r[kV1], 0x41500000U);
    cpu.write_vu0_vector(9U, r[kA0]);                                           // qmtc2 a0,vf9
    write32(r[kT3], f2);
    write32(r[kT2], f2);
    const GuestAddress fp_address = sp + 1120U;
    m.write_u32_le(sp + 612U, 0x41500000U);
    write32(r[kA0], kOne);
    m.write_u32_le(sp + 640U, 0x3e800000U);
    m.write_u32_le(sp + 580U, 0x41040000U);
    const std::uint32_t f20 = kOne;
    write32(r[kV1], f2);                                                         // mfc1 v1,f2
    m.write_u32_le(sp + 644U, 0x3e800000U);
    m.write_u32_le(sp + 648U, 0x3e800000U);
    r[kA0] = n3::load_quad_any(m, sp + 640U);                                   // lq a0,640(sp)
    m.write_u32_le(sp + 628U, 0x3f000000U);
    cpu.write_vu0_vector(11U, r[kV1]);                                          // qmtc2 v1,vf11
    cpu.write_vu0_vector(10U, n3::load_quad_any(m, sp + 624U));                 // lqc2 vf10,624(sp)
    m.write_u32_le(sp + 608U, 0x40400000U);
    m.write_u32_le(sp + 616U, 0x40400000U);
    cpu.write_vu0_vector(8U, n3::load_quad_any(m, sp + 608U));                  // lqc2 vf8,608(sp)
    m.write_u32_le(sp + 592U, 0xc1000000U);
    ee_cop2::vmul_broadcast(cpu, 10U, 10U, 11U, n3::kMaskXyz, 0U);              // vmulx.xyz vf10,vf10,vf11x
    m.write_u32_le(sp + 600U, 0xc1000000U);
    m.write_u32_le(sp + 596U, 0x3e800000U);
    m.write_u32_le(sp + 576U, 0x41000000U);
    ee_cop2::vmul_broadcast(cpu, 8U, 8U, 9U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf8,vf8,vf9x
    m.write_u32_le(sp + 584U, 0x41000000U);
    n3::store_quad_any(m, sp + 1104U, r[kA0]);
    cpu.write_vu0_vector(7U, n3::load_quad_any(m, sp + 592U));                  // lqc2 vf7,592(sp)
    cpu.write_vu0_vector(1U, r[kT3]);                                           // qmtc2 t3,vf1
    ee_cop2::vmul_broadcast(cpu, 7U, 7U, 1U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf7,vf7,vf1x
    ee_cop2::vadd_vector(cpu, 1U, 8U, 10U, n3::kMaskXyz, true);                 // vsub.xyz vf1,vf8,vf10
    n3::store_quad_any(m, sp + 1088U, cpu.read_vu0_vector(1U));                 // sqc2 vf1,0(v1)
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, sp + 576U));                  // lqc2 vf6,576(sp)
    cpu.write_vu0_vector(5U, r[kT2]);                                           // qmtc2 t2,vf5
    ee_cop2::vmul_broadcast(cpu, 6U, 6U, 5U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf6,vf6,vf5x
    ee_cop2::vadd_vector(cpu, 1U, 6U, 7U, n3::kMaskXyz, true);                  // vsub.xyz vf1,vf6,vf7
    n3::store_quad_any(m, sp + 1024U, cpu.read_vu0_vector(1U));                 // sqc2 vf1,0(v1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, sp + 1136U));                 // lqc2 vf3,1136(sp)
    cpu.write_vu0_vector(4U, r[kT0]);                                           // qmtc2 t0,vf4
    cpu.write_vu0_vector(2U, r[kT1]);                                           // qmtc2 t1,vf2
    ee_cop2::vmul_broadcast(cpu, 4U, 3U, 4U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf4,vf3,vf4x
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 2U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf1,vf3,vf2x
    ee_cop2::vadd_vector(cpu, 1U, 4U, 1U, n3::kMaskXyz, true);                  // vsub.xyz vf1,vf4,vf1
    cpu.write_vu0_vector(5U, r[kA0]);                                           // qmtc2 a0,vf5
    n3::store_quad_any(m, sp + 1056U, cpu.read_vu0_vector(1U));                 // sqc2 vf1,0(v1)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 5U, n3::kMaskXyz, true);                  // vsub.xyz vf1,vf0,vf5
    ee_cop2::vadd_vector(cpu, 1U, 5U, 1U, n3::kMaskXyz, true);                  // vsub.xyz vf1,vf5,vf1
    n3::store_quad_any(m, sp + 1072U, cpu.read_vu0_vector(8U));                 // sqc2 vf8,1072(sp)
    n3::store_quad_any(m, sp + 1008U, cpu.read_vu0_vector(6U));                 // sqc2 vf6,1008(sp)
    n3::store_quad_any(m, sp + 1040U, cpu.read_vu0_vector(4U));                 // sqc2 vf4,1040(sp)
    n3::store_quad_any(m, fp_address, cpu.read_vu0_vector(1U));                 // sqc2 vf1,0(fp)
    retired += kSetupCost;

    // --- 0x25dbe0..: the sample count s2 (and the flag-clearing retry), then the sample loop.
    std::int32_t s2 = 0;
    Gpr128 s1{};
    std::uint32_t f24 = 0U;
    const GuestAddress rate_address = object + kRate;
    for (;;) {
        retired += kCountHeadCost;
        if (flag != 0U) {
            retired += kCountFlagCost;
            f1 = m.read_u32_le(gp + static_cast<std::uint32_t>(kFractionOffset));
            f0 = 0U;
            const std::uint32_t acc = fpu::add(f0, f1);                           // adda.s f0,f1
            cpu.set_fpu_accumulator_bits(acc);
            f1 = fpu::madd(acc, kSeven, f23);                                    // madd.s f1,f21,f23
            f0 = static_cast<std::uint32_t>(fpu::to_int(f1));                    // cvt.w.s f0,f1
            s2 = static_cast<std::int32_t>(f0);                                  // mfc1 s2,f0
            f0 = fpu::from_int(s2);
            f0 = fpu::sub(f1, f0);
            m.write_u32_le(gp + static_cast<std::uint32_t>(kFractionOffset), f0);
        } else {
            retired += kCountPlainCost;
            f0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kFractionOffset));
            f1 = fpu::add(f0, f23);
            f0 = static_cast<std::uint32_t>(fpu::to_int(f1));
            s2 = static_cast<std::int32_t>(f0);
            f0 = fpu::from_int(s2);
            f0 = fpu::sub(f1, f0);
            m.write_u32_le(gp + static_cast<std::uint32_t>(kFractionOffset), f0);
        }
        retired += kCountTestCost;
        if (s2 > 0) {
            retired += kLoopHeadCost;
            f0 = fpu::from_int(s2);
            r[kV0] = n3::load_quad_any(m, sp + 1168U);                           // lq v0,1168(sp)
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1152U));          // lqc2 vf1,1152(sp)
            f24 = f22;
            n3::store_quad_any(m, sp + 992U, r[kV0]);
            f0 = fpu::div(f20, f0);
            write32(r[kV0], f0);                                                 // mfc1 v0,f0
            cpu.write_vu0_vector(2U, r[kV0]);                                    // qmtc2 v0,vf2
            ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);          // vmulx.xyz vf1,vf1,vf2x
            n3::store_quad_any(m, sp + 976U, cpu.read_vu0_vector(1U));           // sqc2 vf1,976(sp)
            for (;;) {
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                // --- 0x25dc90..0x25ddf0: the random combination of the frame quads, RecordBallisticTrailSample.
                retired += kBodyCost;
                cpu.write_vu0_vector(3U, n3::load_quad_any(m, kGlobalVector));       // lqc2 vf3,-11888(v1)
                ee_cop2::vrget(cpu, 1U, kMaskX, true);                               // vrnext.x vf1
                cpu.write_vu0_vector(4U, n3::load_quad_any(m, kGlobalConstants));    // lqc2 vf4,-16464(v0)
                ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, kMaskX, 0U);                // vmulx.x vf1,vf3,vf1x
                ee_cop2::vadda_broadcast(cpu, 4U, 1U, kMaskX, 0U);                   // vaddax.x acc,vf4,vf1x
                ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, kMaskX, 3U, false, true, true);   // vmsubaw.x acc,vf4,vf0w
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kMaskX, 3U, false, false, true);  // vmsubw.x vf1,vf1,vf0w
                r[kV1] = cpu.read_vu0_vector(1U);                                    // qmfc2 v1,vf1
                write32(r[kV0], sp + 1056U);
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1040U));          // lqc2 vf1,1040(sp)
                s1 = n3::load_quad_any(m, sp + 1056U);                               // lq s1,0(v0)
                ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);             // vaddax.xyz acc,vf1,vf0x
                cpu.write_vu0_vector(2U, r[kV1]);                                    // qmtc2 v1,vf2
                cpu.write_vu0_vector(1U, s1);                                        // qmtc2 s1,vf1
                ee_cop2::vmadd_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U, false, false, false);  // vmaddx.xyz vf2,vf1,vf2x
                r[kA0] = cpu.read_vu0_vector(2U);                                    // qmfc2 a0,vf2
                ee_cop2::vrget(cpu, 1U, kMaskX, true);                               // vrnext.x vf1
                ee_cop2::vrget(cpu, 1U, kMaskY, true);                               // vrnext.y vf1
                ee_cop2::vmr32(cpu, 3U, 3U, n3::kMaskXyzw);                          // vmr32.xyzw vf3,vf3
                ee_cop2::vrget(cpu, 1U, kMaskZ, true);                               // vrnext.z vf1
                ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);                        // vmul.xyz vf1,vf3,vf1
                ee_cop2::vadda(cpu, 1U, 4U, n3::kMaskXyz);                           // vadda.xyz acc,vf1,vf4
                ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, n3::kMaskXyz, 3U, false, true, true);   // vmsubaw.xyz acc,vf4,vf0w
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, true);  // vmsubw.xyz vf1,vf1,vf0w
                r[kV1] = cpu.read_vu0_vector(1U);                                    // qmfc2 v1,vf1
                write32(r[kV0], sp + 1088U);
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1072U));          // lqc2 vf1,1072(sp)
                r[kV0] = n3::load_quad_any(m, sp + 1088U);                           // lq v0,0(v0)
                ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);             // vaddax.xyz acc,vf1,vf0x
                cpu.write_vu0_vector(2U, r[kV1]);                                    // qmtc2 v1,vf2
                cpu.write_vu0_vector(1U, r[kV0]);                                    // qmtc2 v0,vf1
                ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);                // vmadd.xyz vf2,vf1,vf2
                r[kV0] = cpu.read_vu0_vector(2U);                                    // qmfc2 v0,vf2
                cpu.write_vu0_vector(2U, r[kV0]);                                    // qmtc2 v0,vf2
                cpu.write_vu0_vector(1U, r[kA0]);                                    // qmtc2 a0,vf1
                ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);          // vadd.xyz vf1,vf2,vf1
                n3::store_quad_any(m, sp + 960U, cpu.read_vu0_vector(1U));           // sqc2 vf1,960(sp)
                ee_cop2::vrget(cpu, 1U, kMaskX, true);                               // vrnext.x vf1
                ee_cop2::vrget(cpu, 1U, kMaskY, true);                               // vrnext.y vf1
                ee_cop2::vmr32(cpu, 3U, 3U, n3::kMaskXyzw);                          // vmr32.xyzw vf3,vf3
                ee_cop2::vrget(cpu, 1U, kMaskZ, true);                               // vrnext.z vf1
                ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);                        // vmul.xyz vf1,vf3,vf1
                ee_cop2::vadda(cpu, 1U, 4U, n3::kMaskXyz);                           // vadda.xyz acc,vf1,vf4
                ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, n3::kMaskXyz, 3U, false, true, true);   // vmsubaw.xyz acc,vf4,vf0w
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, true);  // vmsubw.xyz vf1,vf1,vf0w
                r[kV1] = cpu.read_vu0_vector(1U);                                    // qmfc2 v1,vf1
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1104U));          // lqc2 vf1,1104(sp)
                r[kV0] = n3::load_quad_any(m, fp_address);                           // lq v0,0(fp)
                ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);             // vaddax.xyz acc,vf1,vf0x
                cpu.write_vu0_vector(2U, r[kV1]);                                    // qmtc2 v1,vf2
                cpu.write_vu0_vector(1U, r[kV0]);                                    // qmtc2 v0,vf1
                ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);                // vmadd.xyz vf2,vf1,vf2
                r[kV1] = cpu.read_vu0_vector(2U);                                    // qmfc2 v1,vf2
                ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                          // vmr32.xyzw vf1,vf3
                write32(r[kV0], 0x01da0000U);
                write32(r[kA0], object + kTrailRing);
                write32(r[kA1], sp + 944U);
                write32(r[kA2], sp + 960U);
                n3::store_quad_any(m, kGlobalVector, cpu.read_vu0_vector(1U));       // sqc2 vf1,-11888(v0)
                cpu.write_vu0_vector(2U, n3::load_quad_any(m, sp + 992U));           // lqc2 vf2,992(sp)
                cpu.write_vu0_vector(1U, r[kV1]);                                    // qmtc2 v1,vf1
                ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);          // vadd.xyz vf1,vf2,vf1
                n3::store_quad_any(m, sp + 944U, cpu.read_vu0_vector(1U));           // sqc2 vf1,944(sp)
                call.set_arg(0U, object + kTrailRing);
                call.set_arg(1U, sp + 944U);
                call.set_arg128(2U, r[kA2]);
                call.set_arg64(3U, flag);
                call.set_float_arg(0U, m.read_u32_le(rate_address));
                call.set_float_arg(1U, f24);
                const n3::Run trail = call.invoke([](auto& machine) { return n3::fn_00263c40(machine); }, kFrameBytes);
                if (trail.declined) {
                    return call.decline();
                }
                retired += trail.retired;
                // RecordBallisticTrailSample ends with a2 = its first output quad and a0 = the second.
                r[kA2] = n3::load_quad_any(m, sp - 288U + 16U);
                r[kA0] = n3::load_quad_any(m, sp - 288U);
                // --- 0x25dda8..0x25dde8: the generator words step again.
                {
                    const GuestAddress x_address = object + kRandomX;
                    const GuestAddress y_address = object + kRandomY;
                    write32(r[kA2], x_address);
                    write32(r[kA1], y_address);
                    std::uint32_t x = m.read_u32_le(x_address);
                    x = (x << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> 16);
                    m.write_u32_le(x_address, x);
                    const std::uint32_t x_again = m.read_u32_le(x_address);
                    const std::uint32_t y = m.read_u32_le(y_address);
                    m.write_u32_le(x_address, x_again + y);
                    const std::uint32_t y_again = m.read_u32_le(y_address);
                    const std::uint32_t x_third = m.read_u32_le(x_address);
                    m.write_u32_le(y_address, y_again + x_third);
                    const std::uint32_t odd = m.read_u32_le(x_address) & 1U;
                    if (odd != 0U) {
                        // --- 0x25de28..0x25dee4: PushRingBufferEntry.
                        retired += kPushCost;
                        cpu.write_vu0_vector(3U, n3::load_quad_any(m, kGlobalVector));       // lqc2 vf3,-11888(v1)
                        ee_cop2::vrget(cpu, 1U, kMaskX, true);                               // vrnext.x vf1
                        cpu.write_vu0_vector(4U, n3::load_quad_any(m, kGlobalConstants));    // lqc2 vf4,-16464(v0)
                        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, kMaskX, 0U);                // vmulx.x vf1,vf3,vf1x
                        ee_cop2::vadda_broadcast(cpu, 4U, 1U, kMaskX, 0U);                   // vaddax.x acc,vf4,vf1x
                        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, kMaskX, 3U, false, true, true);   // vmsubaw.x
                        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kMaskX, 3U, false, false, true);  // vmsubw.x
                        r[kV0] = cpu.read_vu0_vector(1U);                                    // qmfc2 v0,vf1
                        cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1040U));          // lqc2 vf1,1040(sp)
                        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);             // vaddax.xyz acc,vf1,vf0x
                        cpu.write_vu0_vector(2U, r[kV0]);                                    // qmtc2 v0,vf2
                        cpu.write_vu0_vector(1U, s1);                                        // qmtc2 s1,vf1
                        ee_cop2::vmadd_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U, false, false, false);
                        r[kA3] = cpu.read_vu0_vector(2U);                                    // qmfc2 a3,vf2
                        ee_cop2::vrget(cpu, 1U, kMaskX, true);                               // vrnext.x vf1
                        ee_cop2::vrget(cpu, 1U, kMaskY, true);                               // vrnext.y vf1
                        ee_cop2::vmr32(cpu, 3U, 3U, n3::kMaskXyzw);                          // vmr32.xyzw vf3,vf3
                        ee_cop2::vrget(cpu, 1U, kMaskZ, true);                               // vrnext.z vf1
                        ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);                        // vmul.xyz vf1,vf3,vf1
                        ee_cop2::vadda(cpu, 1U, 4U, n3::kMaskXyz);                           // vadda.xyz acc,vf1,vf4
                        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, n3::kMaskXyz, 3U, false, true, true);
                        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, true);
                        r[kV1] = cpu.read_vu0_vector(1U);                                    // qmfc2 v1,vf1
                        write32(r[kV0], sp + 1024U);
                        cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 1008U));          // lqc2 vf1,1008(sp)
                        r[kV0] = n3::load_quad_any(m, sp + 1024U);                           // lq v0,0(v0)
                        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);             // vaddax.xyz acc,vf1,vf0x
                        cpu.write_vu0_vector(2U, r[kV1]);                                    // qmtc2 v1,vf2
                        cpu.write_vu0_vector(1U, r[kV0]);                                    // qmtc2 v0,vf1
                        ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);                // vmadd.xyz vf2,vf1,vf2
                        r[kV1] = cpu.read_vu0_vector(2U);                                    // qmfc2 v1,vf2
                        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                          // vmr32.xyzw vf1,vf3
                        write32(r[kV0], 0x01da0000U);
                        write32(r[kA0], object + kPushRing);
                        write32(r[kA1], sp + 944U);
                        write32(r[kA2], sp + 960U);
                        n3::store_quad_any(m, kGlobalVector, cpu.read_vu0_vector(1U));       // sqc2 vf1,-11888(v0)
                        cpu.write_vu0_vector(2U, r[kV1]);                                    // qmtc2 v1,vf2
                        cpu.write_vu0_vector(1U, r[kA3]);                                    // qmtc2 a3,vf1
                        ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);          // vadd.xyz vf1,vf2,vf1
                        n3::store_quad_any(m, sp + 960U, cpu.read_vu0_vector(1U));           // sqc2 vf1,960(sp)
                        call.set_arg128(0U, r[kA0]);
                        call.set_arg(1U, sp + 944U);
                        call.set_arg(2U, sp + 960U);
                        call.set_arg128(3U, r[kA3]);
                        call.set_float_arg(0U, m.read_u32_le(rate_address));
                        call.set_float_arg(1U, f24);
                        const n3::Run push = call.invoke([](auto& machine) { return n3::fn_00365e80(machine); }, kFrameBytes);
                        if (push.declined) {
                            return call.decline();
                        }
                        retired += push.retired;
                        r[kV1] = n3::load_quad_any(m, sp + 976U);                            // delay slot: lq v1,976(sp)
                    } else {
                        retired += kEvenCost;
                        r[kV1] = n3::load_quad_any(m, sp + 976U);                            // lq v1,976(sp)
                    }
                }
                // --- 0x25ddf8..0x25de1c: advance the position by the step; the y word joins f24.
                retired += kStepCost;
                cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp + 992U));               // lqc2 vf1,992(sp)
                s2 -= 1;
                cpu.write_vu0_vector(2U, r[kV1]);                                        // qmtc2 v1,vf2
                n3::store_quad_any(m, sp + 368U, r[kV1]);
                ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, false);              // vadd.xyz vf1,vf1,vf2
                f0 = m.read_u32_le(sp + 372U);
                n3::store_quad_any(m, sp + 992U, cpu.read_vu0_vector(1U));
                f24 = fpu::add(f24, f0);
                if (s2 == 0) {
                    break;
                }
            }
            retired += kLoopExitCost;
        }
        retired += kFlagTestCost;
        if (flag == 0U) {
            break;
        }
        retired += kFlagClearCost;
        flag = 0U;
    }
    retired += kReturnCost;
    cpu.write_fpr_bits(0U, f0);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025d930, 1532U, 0x83d6f823bb160bf4ULL, "SpawnBallisticTrailSamples", kCeiling,
         spawn_ballistic_trail_samples, EngineReturn::Nothing)
