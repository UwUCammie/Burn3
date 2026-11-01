// 0x003640c0 (a0 = record R): ResetInputState, a leaf with no result (EngineReturn::Nothing). It zero-fills the
// record at a0 in the listing's order, with the fixed values the listing stores: 31 in word +4 of each of the
// sixteen 16-byte entries at +0, -1.0f at +288 and +292, +0.0 (f0) at +296, and -1 as a halfword at +556. Four
// quadwords (+304, +320, +336 and +480) are the VU0 register after `vadd.xyz vfN,vf0,vf0`: x, y and z are +0 and
// the w lane keeps the register's old value, so the stored w lanes are whatever the VU0 state held. The VU0 sums
// run on the real VU0 state (VF1..VF3, the MAC and STATUS flags), in the listing's order.
//
// The five loops have fixed bounds, not data-dependent ones. Each counter is set to 0 by a daddu from $zero right
// before its loop, and only the loop body changes it, so they are plain for-loops here:
// 16 iterations (0x3640cc), 3 (0x364158), 7 (0x364188), 7 (0x3641f8) and 64 (0x364240). The path is the same on
// every call, so the retired count is a constant: 3 + 16*9 + 26 + 3*11 + 1 + 7*7 + 21 + 7*7 + 11 + 64*7 + 2 = 787.
// The quadword stores use n3::store_quad, which aligns the address down; the original's sqc2 faults on a
// misaligned address instead, so a0 is assumed to be 16-byte aligned (the offsets above are all multiples of 16).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// Retired instruction count of every call (no path depends on the input; see the header comment).
constexpr std::uint32_t kRetired = 787U;
// -1.0f, from `lui $v1,0xbf80`: stored at +288 and +292.
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;
// The value loop 1 stores at +4 of each entry (`addiu $a1,$zero,31`).
constexpr std::uint32_t kEntryFill = 31U;

template <class Context>
n3::Run reset_input_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    // 0x3640c4: mtc1 $zero,$f0. The swc1 at +296 stores these bits.
    cpu.write_fpr_bits(0U, 0U);
    const std::uint32_t zero_f0 = cpu.read_fpr_bits(0U);

    // Loop 1 (0x3640cc..0x3640e8): i = 0..15 over the 16-byte entries at record + 16*i. Store order per entry:
    // +4, +8, +0, +12.
    for (std::uint32_t i = 0U; i < 16U; ++i) {
        const GuestAddress entry = record + 16U * i;
        m.write_u32_le(entry + 4U, kEntryFill);
        m.write_u32_le(entry + 8U, 0U);
        m.write_u32_le(entry + 0U, 0U);
        m.write_u32_le(entry + 12U, 0U);
    }

    // 0x3640f0..0x364154: the straight block after loop 1, with the VU0 sums and the quadword stores.
    m.write_u32_le(record + 256U, 0U);
    m.write_u32_le(record + 260U, 0U);
    ee_cop2::vadd_vector(cpu, 3U, 0U, 0U, n3::kMaskXyz, false);  // vadd.xyz vf3,vf0,vf0
    m.write_u32_le(record + 264U, 0U);
    m.write_u8(record + 280U, 0U);
    m.write_u8(record + 281U, 0U);
    m.write_u32_le(record + 268U, 0U);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);  // vadd.xyz vf2,vf0,vf0
    m.write_u32_le(record + 272U, 0U);
    m.write_u32_le(record + 276U, 0U);
    m.write_u8(record + 282U, 0U);
    m.write_u8(record + 283U, 0U);
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // vadd.xyz vf1,vf0,vf0
    m.write_u8(record + 284U, 0U);
    m.write_u32_le(record + 288U, kMinusOneBits);
    m.write_u32_le(record + 292U, kMinusOneBits);
    m.write_u32_le(record + 296U, zero_f0);
    m.write_u32_le(record + 300U, 0U);
    n3::store_quad_any(m, record + 304U, cpu.read_vu0_vector(3U));  // sqc2 vf3,304($a0)
    n3::store_quad_any(m, record + 320U, cpu.read_vu0_vector(2U));  // sqc2 vf2,320($a0)
    n3::store_quad_any(m, record + 336U, cpu.read_vu0_vector(1U));  // sqc2 vf1,336($a0)
    m.write_u32_le(record + 380U, 0U);
    m.write_u32_le(record + 440U, 0U);
    m.write_u32_le(record + 408U, 0U);

    // Loop 2 (0x364158..0x36417c): i = 0..2 (a2 = 0 from 0x364104). Per iteration: the byte at +352+i, then the
    // words at +356, +368, +384 and +396, each plus 4*i.
    for (std::uint32_t i = 0U; i < 3U; ++i) {
        m.write_u8(record + 352U + i, 0U);
        const GuestAddress word = record + 4U * i;
        m.write_u32_le(word + 356U, 0U);
        m.write_u32_le(word + 368U, 0U);
        m.write_u32_le(word + 384U, 0U);
        m.write_u32_le(word + 396U, 0U);
    }

    // Loop 3 (0x364188..0x3641a0): i = 0..6 (a1 = 0 from 0x364184): the word at +412 + 4*i.
    for (std::uint32_t i = 0U; i < 7U; ++i) {
        m.write_u32_le(record + 412U + 4U * i, 0U);
    }

    // 0x3641a4..0x3641f4: the straight block after loop 3.
    m.write_u8(record + 444U, 0U);
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // vadd.xyz vf1,vf0,vf0 (again)
    m.write_u8(record + 445U, 0U);
    m.write_u8(record + 446U, 0U);
    m.write_u32_le(record + 448U, 0U);
    m.write_u32_le(record + 452U, 0U);
    m.write_u32_le(record + 456U, 0U);
    m.write_u32_le(record + 468U, 0U);
    m.write_u32_le(record + 472U, 0U);
    m.write_u32_le(record + 464U, 0U);
    n3::store_quad_any(m, record + 480U, cpu.read_vu0_vector(1U));  // sqc2 vf1,480($a0)
    m.write_u8(record + 496U, 0U);
    m.write_u8(record + 497U, 0U);
    m.write_u8(record + 498U, 0U);
    m.write_u8(record + 499U, 0U);
    m.write_u8(record + 500U, 0U);
    m.write_u32_le(record + 504U, 0U);
    m.write_u32_le(record + 508U, 0U);
    m.write_u32_le(record + 512U, 0U);
    m.write_u32_le(record + 544U, 0U);

    // Loop 4 (0x3641f8..0x36420c): i = 0..6 (a1 = 0 from 0x3641b0): the word at +516 + 4*i.
    for (std::uint32_t i = 0U; i < 7U; ++i) {
        m.write_u32_le(record + 516U + 4U * i, 0U);
    }

    // 0x364214..0x36423c: the halfword -1 at +556 (sh stores the low 16 bits), the byte at +558, then the words
    // at +816..+840.
    m.write_u16_le(record + 556U, 0xffffU);
    m.write_u8(record + 558U, 0U);
    m.write_u32_le(record + 816U, 0U);
    m.write_u32_le(record + 820U, 0U);
    m.write_u32_le(record + 824U, 0U);
    m.write_u32_le(record + 828U, 0U);
    m.write_u32_le(record + 832U, 0U);
    m.write_u32_le(record + 836U, 0U);
    m.write_u32_le(record + 840U, 0U);

    // Loop 5 (0x364240..0x364254): i = 0..63 (a1 = 0 from 0x364218): the word at +560 + 4*i.
    for (std::uint32_t i = 0U; i < 64U; ++i) {
        m.write_u32_le(record + 560U + 4U * i, 0U);
    }

    // The temporaries the listing leaves behind: v1 = 0 (the last slti of loop 5), a1 = 64, a2 = 3 (loop 2),
    // a3 = 16 (loop 1). f0 is +0.0 (set at the start). Nothing else is written.
    cpu.write_gpr_u64(3U, 0U);
    cpu.write_gpr_u64(5U, 64U);
    cpu.write_gpr_u64(6U, 3U);
    cpu.write_gpr_u64(7U, 16U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_003640c0{0x003640c0U, 420U, 0x95fe0f1ca44fe77dULL, "ResetInputState", 787U,
                                                  n3::live<&reset_input_state<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003640c0, reset_input_state)
