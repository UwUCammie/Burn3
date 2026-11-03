// 0x0025df30 (a0 = effect object O, a1/a2/a3/t0 = addresses of four quadwords Q0..Q3, t1 = flag, f12 = time step,
// f13 = solver argument): EmitRingBufferTrail.
// The time step is added to the float at gp-27800; the integer part of the sum is the base count and its fraction
// is stored back (with a non-zero flag the count is first raised by 7.0 * step, as one more add into the fraction).
// Q2 and Q3 and the x, y, z lanes of Q1 give a length (VU0 dot product): when it is at most 2500.0 it scales the
// factor at gp-29512 by its square root, otherwise the scale is 1.0. A halfword pair at O+0x20000-2008 is folded
// (low half sign-extended, summed pairwise); the second word's sum becomes a float that seeds the VU0 random unit
// (VRINIT). The four quadwords build the box corner offsets kept in VF registers. Then once per whole unit of
// count (s0 + folded value) the loop draws random vectors (VRNEXT), forms two quadwords from the running state
// at 0x01d9d190 and from the box offsets, flips the first when its dot product with Q3 * scale is negative, stores
// the running state back and calls RecordBallisticTrailSample (0x00263c40) with O+8688 and the two quadwords.
// Finally PushRingBufferEntryBurst (0x0025d430) runs with the four quadwords and f12 = gp-29508 * step.
// The original works in a 640-byte stack frame. The quadwords the callees read are stored there (Q0..Q3 at +608,
// +592, +576, +560; the two per-iteration quadwords at +416 and +432); the VU0 loads of words the function never
// wrote (+236, +252, +268, +448..+460) read what the frame holds, as the original does. The VU0 register file,
// accumulator, Q, R and MAC/STATUS flags are driven through guest::ee_cop2 exactly as the instructions do; the
// registers a transfer through a general register carries (the doubleword the instruction leaves alone) are kept
// as the original leaves them. The function returns nothing; s0-s6, ra, f20 and f21 are left as they were.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00263c40)
N3_CALLEE(0025d430)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 640U;
constexpr GuestAddress kFrameQuad0 = 608U;   // Q0 .. Q3 are the arguments PushRingBufferEntryBurst reads
constexpr GuestAddress kFrameQuad1 = 592U;
constexpr GuestAddress kFrameQuad2 = 576U;
constexpr GuestAddress kFrameQuad3 = 560U;
constexpr GuestAddress kFrameTrail = 432U;   // second quadword passed to RecordBallisticTrailSample
constexpr GuestAddress kFrameCenter = 416U;  // first quadword
constexpr GuestAddress kFrameBox = 448U;     // stale words the first load of VF4 reads
constexpr GuestAddress kFrameStaleSix = 236U;
constexpr GuestAddress kFrameStaleEight = 252U;
constexpr GuestAddress kFrameStaleTen = 268U;

constexpr std::int32_t kGpFraction = -27800;     // swc1 / lwc1: fractional part of the accumulated time
constexpr std::int32_t kGpScale = -30736;        // lw a1: scale of Q3
constexpr std::int32_t kGpLengthFactor = -29512; // lwc1 f0: factor of the square root
constexpr std::int32_t kGpBurstFactor = -29508;  // lwc1 f0: factor of the burst step
constexpr GuestAddress kRandomQuad = 0x004bbfb0U;   // lq a0, -16464(0x4c0000)
constexpr GuestAddress kState = 0x01d9d190U;        // lqc2 vf3, -11888(0x01da0000)
constexpr GuestAddress kObjectPairs = 0x00020000U - 2008U;  // halfword pair words
constexpr GuestAddress kObjectStep = 0x00020000U - 1976U;   // lwc1 f12 for RecordBallisticTrailSample
constexpr GuestAddress kObjectRing = 8688U;                 // a0 of RecordBallisticTrailSample

constexpr std::uint32_t kSeven = 0x40e00000U;       // 7.0f
constexpr std::uint32_t kOne = 0x3f800000U;         // 1.0f
constexpr std::uint32_t kQuarter = 0x3e800000U;     // 0.25f
constexpr std::uint32_t kHalf = 0x3f000000U;        // 0.5f
constexpr std::uint32_t kThree = 0x40400000U;       // 3.0f
constexpr std::uint32_t kMinusThree = 0xc0400000U;  // -3.0f
constexpr std::uint32_t kThirteen = 0x41500000U;    // 13.0f
constexpr std::uint32_t kTwoToMinus32 = 0x2f800000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 32U;          // 0x25df30..0x25dfac
constexpr std::uint32_t kFlagCost = 12U;          // 0x25e394..0x25e3c0 (flag set)
constexpr std::uint32_t kLengthCost = 16U;        // 0x25dfb0..0x25dfec
constexpr std::uint32_t kSqrtCost = 6U;           // 0x25e000..0x25e014 (length <= 2500.0)
constexpr std::uint32_t kUnitCost = 4U;           // 0x25dff0..0x25dffc (length > 2500.0)
constexpr std::uint32_t kSetupCost = 38U;         // 0x25e018..0x25e0ac
constexpr std::uint32_t kFoldPositive = 3U;       // 0x25e0b0..0x25e0b8
constexpr std::uint32_t kFoldNegative = 7U;       // 0x25e0bc..0x25e0d4
constexpr std::uint32_t kBoxCost = 62U;           // 0x25e0d8..0x25e1cc
constexpr std::uint32_t kCounterCost = 3U;        // 0x25e350..0x25e358
constexpr std::uint32_t kStepCost = 66U;          // 0x25e1d0..0x25e2d4
constexpr std::uint32_t kFlipCost = 7U;           // 0x25e3c4..0x25e3dc
constexpr std::uint32_t kTrailCost = 30U;         // 0x25e2d8..0x25e34c, up to the jal and its delay slot
constexpr std::uint32_t kBurstCost = 14U;         // 0x25e35c..0x25e390, up to the jal and its delay slot
constexpr std::uint32_t kExitCost = 11U;          // 0x25e3e0..0x25e408
constexpr std::uint32_t kCeiling = 20000U;

// An integer result written to a general register replaces its low doubleword (sign-extended) and keeps the
// high one; that high doubleword travels with the register when qmtc2 copies it into a VU0 register.
Gpr128 with_low_word(Gpr128 value, const std::uint32_t bits) {
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

template <class Context>
n3::Run emit_ring_buffer_trail(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // Everything the later calls overwrite is read first.
    const std::uint64_t object64 = call.arg64(0U);
    const GuestAddress object = call.arg(0U);
    const std::uint64_t flag = call.arg64(5U);
    const std::uint32_t step_time = cpu.read_fpr_bits(12U);     // f21
    const std::uint32_t solver_arg = cpu.read_fpr_bits(13U);    // f20
    const Gpr128 entry_v0 = cpu.read_gpr(2U);
    const Gpr128 entry_a2 = call.arg128(2U);
    const Gpr128 entry_a3 = call.arg128(3U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto gp_at = [gp](const std::int32_t offset) { return gp + static_cast<std::uint32_t>(offset); };

    const Gpr128 quad3 = n3::load_quad_any(m, call.arg(4U));
    const Gpr128 quad0 = n3::load_quad_any(m, call.arg(1U));
    const Gpr128 quad2 = n3::load_quad_any(m, call.arg(3U));
    const Gpr128 quad1 = n3::load_quad_any(m, call.arg(2U));

    // --- 0x25df30: whole units of time and the stored fraction
    const std::uint32_t total = fpu::add(m.read_u32_le(gp_at(kGpFraction)), step_time);
    const std::int32_t base = fpu::to_int(total);
    n3::store_quad_any(m, frame + kFrameQuad0, quad0);
    n3::store_quad_any(m, frame + kFrameQuad1, quad1);
    n3::store_quad_any(m, frame + kFrameQuad2, quad2);
    n3::store_quad_any(m, frame + kFrameQuad3, quad3);
    m.write_u32_le(gp_at(kGpFraction), fpu::sub(total, fpu::from_int(base)));
    std::uint32_t retired = kHeadCost;

    // With the flag set the count is raised by 7.0 * step and the fraction stored again.
    std::int32_t extra = 0;
    if (flag != 0U) {
        const std::uint32_t fraction = m.read_u32_le(gp_at(kGpFraction));
        const std::uint32_t raised = fpu::madd(fpu::add(0U, fraction), kSeven, step_time);
        cpu.set_fpu_accumulator_bits(fpu::add(0U, fraction));
        extra = fpu::to_int(raised);
        m.write_u32_le(gp_at(kGpFraction), fpu::sub(raised, fpu::from_int(extra)));
        retired += kFlagCost;
    }

    // --- 0x25dfb0: squared length of Q1 (x, y, z) through VF1 (its x lane) and the ACC
    cpu.write_vu0_vector(2U, quad1);                                               // lqc2 vf2, 592(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                       // vaddw.x vf1, vf0, vf0w
    ee_cop2::vmul(cpu, 2U, 2U, 2U, n3::kMaskXyz);                                  // vmul.xyz vf2, vf2, vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                 // vadday.x acc, vf2, vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);        // vmaddz.x vf1, vf1, vf2z
    Gpr128 vf1_copy = cpu.read_vu0_vector(1U);                                     // qmfc2 v1, vf1
    const std::uint32_t length_squared = vf1_copy.words[0];
    std::uint32_t factor = kOne;
    if (fpu::le(length_squared, fpu::from_int(2500))) {
        factor = fpu::mul(m.read_u32_le(gp_at(kGpLengthFactor)), fpu::sqrt(length_squared));
        retired += kLengthCost + kSqrtCost;
    } else {
        retired += kLengthCost + kUnitCost;
    }

    // --- 0x25e018: Q3 scaled by gp-30736, kept at +400; the folded halfword pair words
    const std::uint32_t scale = m.read_u32_le(gp_at(kGpScale));
    cpu.write_vu0_vector(1U, quad3);                                               // lqc2 vf1, 560(sp)
    cpu.write_vu0_vector(2U, with_low_word(quad1, scale));                         // qmtc2 a1, vf2 (a1 = Q1 high half)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                    // vmulx.xyz vf1, vf1, vf2x
    const Gpr128 scaled_q3 = cpu.read_vu0_vector(1U);                              // sqc2 vf1, 400(sp)

    const GuestAddress pairs = object + kObjectPairs;
    const std::uint32_t pair0 = m.read_u32_le(pairs);
    const std::uint32_t folded = (pair0 << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(pair0) >> 16);
    m.write_u32_le(pairs, folded);
    const std::uint32_t pair1 = m.read_u32_le(pairs + 4U);
    const std::uint32_t sum = folded + pair1;
    m.write_u32_le(pairs, sum);
    m.write_u32_le(pairs + 4U, pair1 + sum);

    // The sum as an unsigned float, halved to fit cvt.s.w when its top bit is set, then doubled.
    std::uint32_t sum_float = 0U;
    if ((sum & 0x80000000U) != 0U) {
        sum_float = fpu::from_int(static_cast<std::int32_t>((sum >> 1U) | (sum & 1U)));
        sum_float = fpu::add(sum_float, sum_float);
        retired += kSetupCost + kFoldNegative;
    } else {
        sum_float = fpu::from_int(static_cast<std::int32_t>(sum));
        retired += kSetupCost + kFoldPositive;
    }

    // --- 0x25e0d8: seed the random unit, build the box offsets
    const Gpr128 seed = with_low_word(vf1_copy, fpu::mul(kTwoToMinus32, sum_float));   // v1 keeps its high half
    cpu.write_vu0_vector(1U, seed);                                                // qmtc2 v1, vf1
    ee_cop2::vrinit(cpu, 1U, 0U, false);                                           // vrinit r, vf1x
    std::int32_t remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(extra));                                        // s2
    const std::int32_t first = base;                                               // s0

    cpu.write_vu0_vector(4U, n3::load_quad_any(m, frame + kFrameBox));             // lqc2 vf4, 448(sp)
    cpu.write_vu0_vector(5U, Gpr128{{0U, 0U, entry_a2.words[2], entry_a2.words[3]}});   // qmtc2 a2, vf5 (a2 = 0)
    cpu.write_vu0_vector(1U, with_low_word(entry_v0, factor));                     // qmtc2 v0, vf1
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 5U, 4U, 0U, false);                       // vaddx.y vf4, vf0, vf5x
    cpu.write_vu0_vector(10U, Gpr128{{kMinusThree, kHalf, kMinusThree, m.read_u32_le(frame + kFrameStaleTen)}});
    cpu.write_vu0_vector(3U, with_low_word(seed, kQuarter));                       // qmtc2 v1, vf3
    ee_cop2::vmul_broadcast(cpu, 10U, 10U, 1U, n3::kMaskXyz, 0U);                  // vmulx.xyz vf10, vf10, vf1x
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 3U, 8U, 0U, false);                       // vaddx.x vf4, vf0, vf3x
    cpu.write_vu0_vector(8U, Gpr128{{kThree, kThirteen, kThree, m.read_u32_le(frame + kFrameStaleEight)}});
    cpu.write_vu0_vector(9U, with_low_word(entry_a3, factor));                     // qmtc2 a3, vf9
    ee_cop2::vmul_broadcast(cpu, 8U, 8U, 9U, n3::kMaskXyz, 0U);                    // vmulx.xyz vf8, vf8, vf9x
    cpu.write_vu0_vector(6U, Gpr128{{kQuarter, kQuarter, kQuarter, m.read_u32_le(frame + kFrameStaleSix)}});
    cpu.write_vu0_vector(7U, quad0);                                               // lqc2 vf7, 608(sp)
    const Gpr128 box_base = cpu.read_vu0_vector(4U);                               // sqc2 vf4, 448(sp)
    ee_cop2::vadd_vector(cpu, 4U, 7U, 6U, n3::kMaskXyz, false);                    // vadd.xyz vf4, vf7, vf6
    ee_cop2::vadd_vector(cpu, 3U, 7U, 6U, n3::kMaskXyz, true);                     // vsub.xyz vf3, vf7, vf6
    ee_cop2::vadd_vector(cpu, 3U, 4U, 3U, n3::kMaskXyz, true);                     // vsub.xyz vf3, vf4, vf3
    cpu.write_vu0_vector(2U, quad2);                                               // lqc2 vf2, 576(sp)
    cpu.write_vu0_vector(1U, quad1);                                               // lqc2 vf1, 592(sp)
    const Gpr128 box_far = cpu.read_vu0_vector(8U);                                // sqc2 vf8, 496(sp)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);                     // vsub.xyz vf1, vf2, vf1
    ee_cop2::vadd_vector(cpu, 2U, 8U, 10U, n3::kMaskXyz, true);                    // vsub.xyz vf2, vf8, vf10
    const Gpr128 box_top = cpu.read_vu0_vector(4U);                                // sqc2 vf4, 528(sp)
    const Gpr128 side3 = cpu.read_vu0_vector(3U);                                        // s1
    const Gpr128 side2 = cpu.read_vu0_vector(2U);                                        // s5
    const Gpr128 side1 = cpu.read_vu0_vector(1U);                                        // s6
    const Gpr128 box_corner = quad2;                                               // sq v1, 464(sp)
    retired += kBoxCost;

    // --- the loop: one RecordBallisticTrailSample per whole unit of count
    for (;;) {
        retired += kCounterCost;
        const bool again = remaining != 0;
        remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(remaining) - 1U);
        if (!again) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        // --- 0x25e1d0: the first quadword, offset from the running state by random vectors
        const Gpr128 random_quad = n3::load_quad_any(m, kRandomQuad);               // a0
        ee_cop2::vrget(cpu, 1U, 8U, true);                                          // vrnext.x vf1, r
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, kState));                     // lqc2 vf3, state
        cpu.write_vu0_vector(4U, random_quad);                                      // qmtc2 a0, vf4
        ee_cop2::vmul_broadcast(cpu, 1U, 3U, 1U, 8U, 0U);                           // vmulx.x vf1, vf3, vf1x
        ee_cop2::vadda_broadcast(cpu, 4U, 1U, 8U, 0U);                              // vaddax.x acc, vf4, vf1x
        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, 8U, 3U, false, true, true);       // vmsubaw.x acc, vf4, vf0w
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 8U, 3U, false, false, true);      // vmsubw.x vf1, vf1, vf0w
        Gpr128 v0 = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(1U, box_corner);                                       // lqc2 vf1, 464(sp)
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);                    // vaddax.xyz acc, vf1, vf0x
        cpu.write_vu0_vector(2U, v0);                                               // qmtc2 v0, vf2
        cpu.write_vu0_vector(1U, side1);                                            // qmtc2 s6, vf1
        ee_cop2::vmadd_broadcast(cpu, 2U, 1U, 2U, n3::kMaskXyz, 0U, false, false, false);   // vmaddx.xyz vf2, vf1, vf2x
        Gpr128 a1 = cpu.read_vu0_vector(2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                                 // vmr32.xyzw vf1, vf3
        Gpr128 v1 = cpu.read_vu0_vector(1U);
        ee_cop2::vrget(cpu, 1U, 8U, true);                                          // vrnext.x vf1, r
        cpu.write_vu0_vector(2U, box_base);                                         // lqc2 vf2, 448(sp)
        v0 = cpu.read_vu0_vector(1U);
        ee_cop2::vadda_broadcast(cpu, 0U, 2U, 8U, 1U);                              // vadday.x acc, vf0, vf2y
        cpu.write_vu0_vector(1U, v0);                                               // qmtc2 v0, vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 0U, false, false, false);     // vmaddx.x vf1, vf2, vf1x
        v0 = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(2U, a1);                                               // qmtc2 a1, vf2
        cpu.write_vu0_vector(1U, v0);                                               // qmtc2 v0, vf1
        ee_cop2::vmul_broadcast(cpu, 2U, 2U, 1U, n3::kMaskXyz, 0U);                 // vmulx.xyz vf2, vf2, vf1x
        ee_cop2::vrget(cpu, 1U, 8U, true);                                          // vrnext.x vf1, r
        ee_cop2::vrget(cpu, 1U, 4U, true);                                          // vrnext.y vf1, r
        cpu.write_vu0_vector(3U, v1);                                               // qmtc2 v1, vf3
        ee_cop2::vrget(cpu, 1U, 2U, true);                                          // vrnext.z vf1, r
        a1 = cpu.read_vu0_vector(2U);
        ee_cop2::vmul(cpu, 1U, 3U, 1U, n3::kMaskXyz);                               // vmul.xyz vf1, vf3, vf1
        ee_cop2::vadda(cpu, 1U, 4U, n3::kMaskXyz);                                  // vadda.xyz acc, vf1, vf4
        ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 0U, n3::kMaskXyz, 3U, false, true, true);     // vmsubaw.xyz acc, vf4, vf0w
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyz, 3U, false, false, true);    // vmsubw.xyz vf1, vf1, vf0w
        v0 = cpu.read_vu0_vector(1U);
        cpu.write_vu0_vector(1U, box_far);                                          // lqc2 vf1, 496(sp)
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);                    // vaddax.xyz acc, vf1, vf0x
        cpu.write_vu0_vector(2U, v0);                                               // qmtc2 v0, vf2
        cpu.write_vu0_vector(1U, side2);                                            // qmtc2 s5, vf1
        ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);                       // vmadd.xyz vf2, vf1, vf2
        v1 = cpu.read_vu0_vector(2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                                 // vmr32.xyzw vf1, vf3
        n3::store_quad_any(m, kState, cpu.read_vu0_vector(1U));                     // sqc2 vf1, state
        cpu.write_vu0_vector(4U, a1);                                               // qmtc2 a1, vf4
        cpu.write_vu0_vector(1U, v1);                                               // qmtc2 v1, vf1
        ee_cop2::vadd_vector(cpu, 4U, 4U, 1U, n3::kMaskXyz, false);                 // vadd.xyz vf4, vf4, vf1
        cpu.write_vu0_vector(2U, scaled_q3);                                        // lqc2 vf2, 400(sp)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1, vf0, vf0w
        Gpr128 trail = cpu.read_vu0_vector(4U);                                     // sqc2 vf4, 432(sp)
        n3::store_quad_any(m, frame + kFrameTrail, trail);
        ee_cop2::vmul(cpu, 3U, 2U, 4U, n3::kMaskXyz);                               // vmul.xyz vf3, vf2, vf4
        ee_cop2::vadda_broadcast(cpu, 3U, 3U, 8U, 1U);                              // vadday.x acc, vf3, vf3y
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 3U, 8U, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf3z
        const Gpr128 dot = cpu.read_vu0_vector(1U);
        retired += kStepCost;
        if (fpu::lt(dot.words[0], 0U)) {                                            // c.olt.s f1, f0 ; bc1t 0x25e3c4
            // The dot product is negative: the offset is subtracted once more along Q3 * scale.
            cpu.write_vu0_vector(1U, with_low_word(dot, dot.words[0]));             // qmtc2 v0, vf1 (v0 = mfc1 f1)
            ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, n3::kMaskXyz, 0U);             // vmulx.xyz vf1, vf2, vf1x
            ee_cop2::vadd_vector(cpu, 4U, 4U, 1U, n3::kMaskXyz, true);              // vsub.xyz vf4, vf4, vf1
            trail = cpu.read_vu0_vector(4U);
            n3::store_quad_any(m, frame + kFrameTrail, trail);                      // sqc2 vf4, 432(sp)
            retired += kFlipCost;
        }

        // --- 0x25e2d8: the second quadword and the call
        ee_cop2::vrget(cpu, 2U, 8U, true);                                          // vrnext.x vf2, r
        ee_cop2::vrget(cpu, 2U, 4U, true);                                          // vrnext.y vf2, r
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, kState));                     // lqc2 vf3, state
        ee_cop2::vrget(cpu, 2U, 2U, true);                                          // vrnext.z vf2, r
        cpu.write_vu0_vector(1U, random_quad);                                      // qmtc2 a0, vf1
        ee_cop2::vmul(cpu, 2U, 3U, 2U, n3::kMaskXyz);                               // vmul.xyz vf2, vf3, vf2
        ee_cop2::vadda(cpu, 2U, 1U, n3::kMaskXyz);                                  // vadda.xyz acc, vf2, vf1
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, n3::kMaskXyz, 3U, false, true, true);     // vmsubaw.xyz acc, vf1, vf0w
        ee_cop2::vmadd_broadcast(cpu, 2U, 2U, 0U, n3::kMaskXyz, 3U, false, false, true);    // vmsubw.xyz vf2, vf2, vf0w
        v0 = cpu.read_vu0_vector(2U);
        cpu.write_vu0_vector(1U, box_top);                                          // lqc2 vf1, 528(sp)
        ee_cop2::vadda_broadcast(cpu, 1U, 0U, n3::kMaskXyz, 0U);                    // vaddax.xyz acc, vf1, vf0x
        cpu.write_vu0_vector(2U, v0);                                               // qmtc2 v0, vf2
        cpu.write_vu0_vector(1U, side3);                                            // qmtc2 s1, vf1
        ee_cop2::vmadd(cpu, 2U, 1U, 2U, n3::kMaskXyz, false);                       // vmadd.xyz vf2, vf1, vf2
        v0 = cpu.read_vu0_vector(2U);
        ee_cop2::vmr32(cpu, 1U, 3U, n3::kMaskXyzw);                                 // vmr32.xyzw vf1, vf3
        n3::store_quad_any(m, frame + kFrameCenter, v0);                            // sq v0, 416(sp)
        n3::store_quad_any(m, kState, cpu.read_vu0_vector(1U));                     // sqc2 vf1, state
        call.set_arg(0U, object + kObjectRing);
        call.set_arg(1U, frame + kFrameCenter);
        call.set_arg(2U, frame + kFrameTrail);
        call.set_arg(3U, static_cast<std::int32_t>(first) < remaining ? 1U : 0U);
        call.set_float_arg(0U, m.read_u32_le(object + kObjectStep));
        call.set_float_arg(1U, solver_arg);
        retired += kTrailCost;
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00263c40(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        retired += callee.retired;
    }

    // --- 0x25e35c: the burst of ring buffer entries
    call.set_arg64(0U, object64);
    call.set_float_arg(0U, fpu::mul(m.read_u32_le(gp_at(kGpBurstFactor)), step_time));
    call.set_float_arg(1U, solver_arg);
    call.set_arg64(7U, flag);
    call.set_arg(1U, frame + kFrameQuad0);
    call.set_arg(2U, frame + kFrameQuad1);
    call.set_arg(3U, frame + kFrameQuad2);
    call.set_arg64(4U, 0U);
    call.set_arg(5U, frame + kFrameQuad3);
    call.set_arg64(6U, 0U);
    retired += kBurstCost;
    const n3::Run burst = call.invoke([](auto& machine) { return n3::fn_0025d430(machine); }, kFrameBytes);
    if (burst.declined) {
        return call.decline();
    }
    retired += burst.retired + kExitCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025df30, 1244U, 0x4309340cad7c04bfULL, "EmitRingBufferTrail", kCeiling, emit_ring_buffer_trail,
         EngineReturn::Nothing)
