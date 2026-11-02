// 0x00254c90 (a0 = record R, a1 = V, f12 = level L): UpdateCoveragePair. The rate is L - P, where P is the record's
// previous level (float at +124). Two block-coverage states of the record, at +232 and +276, advance by that rate
// through UpdateBlockCoverage (0x002556f0); the second call takes P read again after the first. Then the quad at +96
// (x, y, z, w) becomes (S * v0, y, S * v1, w), where v0 and v1 are the floats at V+0 and V+4 and S is the float at +72
// of the record whose pointer is at +228. VU0 adds +0.0 to each lane (so -0.0 becomes +0.0), and VF1..VF3 end as the
// original leaves them: the scaled words reach VF2 by mfc1 and qmtc2 through $a0 and $v1, so the upper doublewords
// those registers keep show up in VF2. The listing has no branch, so every instruction retires once: 42 of its own
// plus the two callees' counts.
// Returns nothing.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002556f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kLevel = 124U;           // P, the previous level (float)
constexpr GuestAddress kFirstCoverage = 232U;   // the state UpdateBlockCoverage takes as a0 on the first call
constexpr GuestAddress kSecondCoverage = 276U;  // and on the second call
constexpr GuestAddress kScaleRecord = 228U;     // pointer to the record that holds S
constexpr GuestAddress kScale = 72U;            // S (float) in that record
constexpr GuestAddress kQuad = 96U;             // the position quad (x, y, z, w)

constexpr std::uint32_t kFrameBytes = 64U;      // addiu $sp, $sp, -64
constexpr std::uint32_t kOwnCost = 42U;         // every instruction of the listing, delay slots included
constexpr std::uint32_t kCeiling = 20000U;

// VU0 destination masks: bit 3 is x, bit 1 is z (n3::kMaskXyz is 14).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskZ = 2U;

// mfc1 then qmtc2: the word becomes the sign-extended low doubleword of a GPR (its upper doubleword is kept), and the
// whole GPR is copied into the VU0 register.
void word_to_vu0(guest::R5900State& cpu, const std::size_t gpr, const std::size_t vf, const std::uint32_t bits) {
    cpu.write_gpr_sign_extended_32(gpr, bits);
    cpu.write_vu0_vector(vf, cpu.read_gpr(gpr));
}

template <class Context>
n3::Run update_coverage_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const std::uint64_t record_doubleword = call.arg64(0U);   // daddu $s1, $a0, $zero; a1 copies it before each call
    const GuestAddress xz = call.arg(1U);
    const std::uint32_t previous = m.read_u32_le(record + kLevel);
    const std::uint32_t rate = fpu::sub(cpu.read_fpr_bits(12U), previous);

    call.set_arg(0U, record + kFirstCoverage);
    call.set_arg64(1U, record_doubleword);
    call.set_float_arg(0U, previous);
    call.set_float_arg(1U, rate);
    const n3::Run first = call.invoke([](auto& callee) { return n3::fn_002556f0(callee); }, kFrameBytes);
    if (first.declined) {
        return call.decline();
    }

    call.set_arg(0U, record + kSecondCoverage);
    call.set_arg64(1U, record_doubleword);
    call.set_float_arg(0U, m.read_u32_le(record + kLevel));   // lwc1 $f12, 124($s1) after the first call
    call.set_float_arg(1U, rate);
    const n3::Run second = call.invoke([](auto& callee) { return n3::fn_002556f0(callee); }, kFrameBytes);
    if (second.declined) {
        return call.decline();
    }

    // The scaled pair is read only now, after both calls, as the original reads it.
    const std::uint32_t scale = m.read_u32_le(m.read_u32_le(record + kScaleRecord) + kScale);
    const std::uint32_t scaled_x = fpu::mul(scale, m.read_u32_le(xz));
    const std::uint32_t scaled_z = fpu::mul(scale, m.read_u32_le(xz + 4U));

    const Gpr128 quad = n3::load_quad_any(m, record + kQuad);
    cpu.write_vu0_vector(3U, quad);                                       // lqc2 $vf3, 96($s1)
    cpu.write_vu0_vector(1U, quad);                                       // lqc2 $vf1, 96($s1)
    word_to_vu0(cpu, 4U, 2U, scaled_x);                                   // mfc1 $a0 ; qmtc2 $a0, $vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kMaskX, 0U, false);          // vaddx.x $vf3, $vf0, $vf2x
    word_to_vu0(cpu, 3U, 2U, scaled_z);                                   // mfc1 $v1 ; qmtc2 $v1, $vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, kMaskZ, 0U, false);          // vaddx.z $vf3, $vf0, $vf2x
    ee_cop2::vadd_vector(cpu, 1U, 0U, 3U, n3::kMaskXyz, false);           // vadd.xyz $vf1, $vf0, $vf3
    n3::store_quad_any(m, record + kQuad, cpu.read_vu0_vector(1U));       // sqc2 $vf1, 96($s1)

    return call.finish(kOwnCost + first.retired + second.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00254c90, 168U, 0xbaedff64cbe0578aULL, "UpdateCoveragePair", kCeiling, update_coverage_pair,
         EngineReturn::Nothing)
