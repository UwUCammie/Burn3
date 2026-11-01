// 0x00254a90 (a0 = object, a1 and a2 = 16-byte inputs, a3 = gate flag, t0 selects the sub-record at a0+232 when
// zero and at a0+276 otherwise, f12..f17 = floats). The sub-record keeps a write index at +24 and tables at +8
// (16-byte entries), +12 (8-byte), +16 (4-byte) and +28 (floats, one per 60 indices). With a3 zero, the call returns
// 0 without writing when the VU0 difference of the object's word +124 and the w lane of the index's entry at +8 is
// below the float at +36. Otherwise it writes one sample at the index (VU0 transforms of a1, a2 and the floats,
// clamped and packed), steps the index down (reloading it from +20 when it is zero) and returns 1.
// Every instruction is reproduced on the real CPU and memory, in order, so the VU0 registers, the flags and the
// stack words the original reads back all match.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/ee_mmi.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;
namespace ee_mmi = guest::ee_mmi;

// The lane mask for x alone (the n3 header has w, xyz and xyzw; x is the high bit).
constexpr std::uint8_t kMaskX = 8U;

// PEXTLW rd,rs,rt in the interpreter's word order: {rt.w0, rs.w0, rt.w1, rs.w1}.
inline guest::Gpr128 pextlw(const guest::Gpr128& rs, const guest::Gpr128& rt) {
    return guest::Gpr128{{rt.words[0], rs.words[0], rt.words[1], rs.words[1]}};
}

// PCPYLD rd,rs,rt: {rt.low64, rs.low64}.
inline guest::Gpr128 pcpyld(const guest::Gpr128& rs, const guest::Gpr128& rt) {
    return guest::Gpr128{{rt.words[0], rt.words[1], rs.words[0], rs.words[1]}};
}

template <class Context>
n3::Run append_sample_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const guest::Gpr128 zero{};

    // addiu $sp,$sp,-64 (the frame lies below the entry $sp; the original's stores there are reproduced too).
    const GuestAddress frame = call.stack_pointer() - 64U;
    // 0x254a94..0x254aa4: lq $v1,0($a1); lq $v0,0($a2); sq $v1,48($sp); sq $v0,32($sp) (a delay slot).
    cpu.write_gpr(3U, n3::load_quad_any(m, cpu.read_gpr_u32(5U)));
    cpu.write_gpr(2U, n3::load_quad_any(m, cpu.read_gpr_u32(6U)));
    n3::store_quad_any(m, frame + 48U, cpu.read_gpr(3U));
    n3::store_quad_any(m, frame + 32U, cpu.read_gpr(2U));

    // 0x254aa0: beq $t0,$zero (64-bit). The zero case retires 0x254ab0 alone; the other retires 0x254aa8 and its
    // delay slot before joining at 0x254ab4.
    const bool t0_zero = cpu.read_gpr_u64(8U) == 0U;
    std::uint32_t retired = t0_zero ? 7U : 8U;
    const GuestAddress record = cpu.read_gpr_u32(4U) + (t0_zero ? 232U : 276U);
    cpu.write_gpr_sign_extended_32(3U, record);

    // 0x254ab4..0x254acc: index = +24; the entry offset is index << 4 added to the table at +8; v0 = 0x20000000
    // ORed with that address (the uncached alias, as the original forms it); the delay slot is the OR.
    const std::uint32_t index = m.read_u32_le(record + 24U);
    cpu.write_gpr_sign_extended_32(8U, index);
    cpu.write_gpr_sign_extended_32(2U, 0x20000000U);
    cpu.write_gpr_sign_extended_32(5U, m.read_u32_le(record + 8U));
    cpu.write_gpr_sign_extended_32(6U, index << 4U);
    cpu.write_gpr_sign_extended_32(5U, cpu.read_gpr_u32(5U) + cpu.read_gpr_u32(6U));
    const bool a3_zero = cpu.read_gpr_u64(7U) == 0U;
    cpu.write_gpr_u64(2U, cpu.read_gpr_u64(5U) | cpu.read_gpr_u64(2U));
    retired += 7U;

    if (a3_zero) {
        // 0x254ad0..0x254afc: the gate. VF1.x = object word +124 (VF2.x) minus the w lane of the quadword at v0.
        cpu.write_gpr_sign_extended_32(5U, m.read_u32_le(cpu.read_gpr_u32(4U) + 124U));  // lw $a1,124($a0)
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, cpu.read_gpr_u32(2U)));             // lqc2 $vf1,0($v0)
        cpu.write_fpr_bits(0U, m.read_u32_le(record + 36U));                              // lwc1 $f0,36($v1)
        cpu.write_vu0_vector(2U, cpu.read_gpr(5U));                                       // qmtc2 $a1,$vf2
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskX, 3U, true);                        // vsubw.x $vf1,$vf2,$vf1w
        cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                                       // qmfc2 $v0,$vf1
        cpu.write_fpr_bits(1U, cpu.read_gpr_u32(2U));                                     // mtc1 $v0,$f1
        const bool below = fpu::lt(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U));         // c.olt.s $f1,$f0
        retired += 12U;
        if (below) {
            // 0x254b00..0x254b04 (beq, daddu $v0,$zero,$zero) and the return block: nothing is written.
            cpu.write_gpr_u64(2U, 0U);
            return call.finish(retired + 4U);
        }
    }

    // 0x254b08..0x254c58: the sample, 85 instructions without a branch.
    retired += 85U;
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame + 48U));                          // lqc2 $vf2,48($sp)
    cpu.write_gpr_sign_extended_32(2U, cpu.read_fpr_bits(17U));                           // mfc1 $v0,$f17
    cpu.write_gpr_sign_extended_32(9U, cpu.read_fpr_bits(12U));                           // mfc1 $t1,$f12
    cpu.write_gpr_u64(5U, 0xffffU);                                                       // ori $a1,$zero,0xffff
    cpu.write_gpr_sign_extended_32(6U, 0xffff8000U);                                      // addiu $a2,$zero,-32768
    cpu.write_gpr_u64(5U, cpu.read_gpr_u64(5U) << 16U);                                   // dsll $a1,$a1,16
    cpu.write_gpr_u64(7U, cpu.read_gpr_u64(6U) << 32U);                                   // dsll32 $a3,$a2,0
    cpu.write_gpr_u64(5U, cpu.read_gpr_u64(5U) | 0x8000U);                                // ori $a1,$a1,0x8000
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 16U));                          // lqc2 $vf3,16($sp)
    cpu.write_gpr_u64(5U, cpu.read_gpr_u64(5U) | cpu.read_gpr_u64(7U));                   // or $a1,$a1,$a3
    cpu.write_gpr_sign_extended_32(6U, 32767U);                                           // addiu $a2,$zero,32767
    cpu.write_gpr(7U, pcpyld(zero, cpu.read_gpr(5U)));                                    // pcpyld $a3,$zero,$a1
    cpu.write_gpr_u64(5U, cpu.read_gpr_u64(6U) << 32U);                                   // dsll32 $a1,$a2,0
    cpu.write_vu0_vector(5U, cpu.read_gpr(2U));                                           // qmtc2 $v0,$vf5
    cpu.write_gpr_u64(5U, cpu.read_gpr_u64(6U) | cpu.read_gpr_u64(5U));                   // or $a1,$a2,$a1
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, n3::kMaskXyz, false);                           // vadd.xyz $vf3,$vf0,$vf2
    cpu.write_gpr_sign_extended_32(2U, 0x004c0000U);                                      // lui $v0,0x004c
    cpu.write_vu0_vector(1U, cpu.read_gpr(9U));                                           // qmtc2 $t1,$vf1
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, n3::kMaskW, 3U, true);                       // vsubw.w $vf3,$vf0,$vf0w
    ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, n3::kMaskW, 0U, false);                      // vaddx.w $vf3,$vf3,$vf1x
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, cpu.read_gpr_u32(2U) - 16528U));        // lqc2 $vf2,-16528($v0)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, frame + 32U));                          // lqc2 $vf4,32($sp)
    n3::store_quad_any(m, frame + 16U, cpu.read_vu0_vector(3U));                          // sqc2 $vf3,16($sp)
    ee_cop2::vmul_broadcast(cpu, 1U, 4U, 2U, n3::kMaskXyz, 2U);                           // vmulz.xyz $vf1,$vf4,$vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(3U));                                           // qmfc2 $v0,$vf3
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 5U, n3::kMaskW, 0U);                             // vmulx.w $vf1,$vf2,$vf5x
    cpu.write_gpr(6U, cpu.read_vu0_vector(1U));                                           // qmfc2 $a2,$vf1
    cpu.write_gpr(13U, pextlw(cpu.read_gpr(7U), cpu.read_gpr(7U)));                       // pextlw $t5,$a3,$a3
    cpu.write_gpr(12U, pextlw(cpu.read_gpr(5U), cpu.read_gpr(5U)));                       // pextlw $t4,$a1,$a1
    cpu.write_vu0_vector(1U, cpu.read_gpr(6U));                                           // qmtc2 $a2,$vf1
    cpu.write_gpr_sign_extended_32(7U, 255U);                                             // addiu $a3,$zero,255
    cpu.write_gpr_sign_extended_32(9U, cpu.read_gpr_u32(8U) << 3U);                       // sll $t1,$t0,3
    cpu.write_gpr_sign_extended_32(5U, 0x004c0000U);                                      // lui $a1,0x004c
    cpu.write_fpr_bits(0U, fpu::add(cpu.read_fpr_bits(13U), cpu.read_fpr_bits(13U)));     // add.s $f0,$f13,$f13
    cpu.write_gpr_sign_extended_32(8U, 0x30000000U);                                      // lui $t0,0x3000
    cpu.write_gpr_u64(6U, cpu.read_gpr_u64(7U) << 32U);                                   // dsll32 $a2,$a3,0
    cpu.write_vu0_vector(4U, cpu.read_gpr(4U));                                           // qmtc2 $a0,$vf4
    cpu.write_gpr_u64(7U, cpu.read_gpr_u64(7U) | cpu.read_gpr_u64(6U));                   // or $a3,$a3,$a2
    cpu.write_gpr(10U, pextlw(cpu.read_gpr(7U), cpu.read_gpr(7U)));                       // pextlw $t2,$a3,$a3
    ee_cop2::vftoi(cpu, 1U, 1U, n3::kMaskXyzw, 0U);                                       // vftoi0.xyzw $vf1,$vf1
    cpu.write_gpr_sign_extended_32(7U, m.read_u32_le(record + 12U));                      // lw $a3,12($v1)
    cpu.write_fpr_bits(0U, fpu::add(cpu.read_fpr_bits(14U), cpu.read_fpr_bits(0U)));     // add.s $f0,$f14,$f0
    cpu.write_gpr_sign_extended_32(6U, 60U);                                              // addiu $a2,$zero,60
    cpu.write_gpr(11U, cpu.read_vu0_vector(1U));                                          // qmfc2 $t3,$vf1
    m.write_u32_le(frame + 8U, cpu.read_fpr_bits(16U));                                   // swc1 $f16,8($sp)
    cpu.write_gpr_sign_extended_32(7U, cpu.read_gpr_u32(7U) + cpu.read_gpr_u32(9U));      // addu $a3,$a3,$t1
    m.write_u32_le(frame + 12U, cpu.read_fpr_bits(15U));                                  // swc1 $f15,12($sp)
    cpu.write_gpr(9U, ee_mmi::pmaxw(cpu.read_gpr(11U), cpu.read_gpr(13U), true));         // pmaxw $t1,$t3,$t5
    cpu.write_gpr(4U, ee_mmi::pmaxw(cpu.read_gpr(9U), cpu.read_gpr(12U), false));         // pminw $a0,$t1,$t4
    cpu.write_gpr(9U, ee_mmi::ppach(zero, cpu.read_gpr(4U)));                             // ppach $t1,$zero,$a0
    cpu.write_gpr_u64(4U, cpu.read_gpr_u64(7U) | cpu.read_gpr_u64(8U));                   // or $a0,$a3,$t0
    m.write_u64_le(cpu.read_gpr_u32(4U), cpu.read_gpr_u64(9U));                           // sd $t1,0($a0)
    m.write_u32_le(frame + 4U, cpu.read_fpr_bits(14U));                                   // swc1 $f14,4($sp)
    m.write_u32_le(frame + 0U, cpu.read_fpr_bits(0U));                                    // swc1 $f0,0($sp)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, cpu.read_gpr_u32(5U) - 16512U));        // lqc2 $vf2,-16512($a1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 0U));                           // lqc2 $vf3,0($sp)
    ee_cop2::vmove(cpu, 1U, 4U, n3::kMaskXyzw);                                           // vmove.xyzw $vf1,$vf4
    cpu.write_gpr_sign_extended_32(5U, m.read_u32_le(record + 24U));                      // lw $a1,24($v1)
    cpu.write_gpr_sign_extended_32(4U, m.read_u32_le(record + 16U));                      // lw $a0,16($v1)
    ee_cop2::vmul(cpu, 4U, 3U, 2U, n3::kMaskXyzw);                                        // vmul.xyzw $vf4,$vf3,$vf2
    cpu.write_gpr_sign_extended_32(5U, cpu.read_gpr_u32(5U) << 2U);                       // sll $a1,$a1,2
    cpu.write_gpr_sign_extended_32(4U, cpu.read_gpr_u32(4U) + cpu.read_gpr_u32(5U));      // addu $a0,$a0,$a1
    ee_cop2::vftoi(cpu, 1U, 4U, n3::kMaskXyzw, 0U);                                       // vftoi0.xyzw $vf1,$vf4
    cpu.write_gpr_u64(4U, cpu.read_gpr_u64(4U) | cpu.read_gpr_u64(8U));                   // or $a0,$a0,$t0
    cpu.write_gpr(5U, cpu.read_vu0_vector(1U));                                           // qmfc2 $a1,$vf1
    cpu.write_gpr(5U, ee_mmi::pmaxw(cpu.read_gpr(5U), cpu.read_gpr(10U), false));         // pminw $a1,$a1,$t2
    cpu.write_gpr(5U, ee_mmi::ppach(zero, cpu.read_gpr(5U)));                             // ppach $a1,$zero,$a1
    cpu.write_gpr(5U, ee_mmi::ppacb(zero, cpu.read_gpr(5U)));                             // ppacb $a1,$zero,$a1
    m.write_u32_le(cpu.read_gpr_u32(4U), cpu.read_gpr_u32(5U));                           // sw $a1,0($a0)
    cpu.write_gpr_sign_extended_32(5U, m.read_u32_le(record + 24U));                      // lw $a1,24($v1)
    cpu.write_gpr_sign_extended_32(4U, m.read_u32_le(record + 8U));                       // lw $a0,8($v1)
    cpu.write_gpr_sign_extended_32(5U, cpu.read_gpr_u32(5U) << 4U);                       // sll $a1,$a1,4
    cpu.write_gpr_sign_extended_32(4U, cpu.read_gpr_u32(4U) + cpu.read_gpr_u32(5U));      // addu $a0,$a0,$a1
    cpu.write_gpr_u64(4U, cpu.read_gpr_u64(4U) | cpu.read_gpr_u64(8U));                   // or $a0,$a0,$t0
    n3::store_quad_any(m, cpu.read_gpr_u32(4U), cpu.read_gpr(2U));                        // sq $v0,0($a0)
    cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(record + 24U));                      // lw $v0,24($v1)

    // divu $v0,$a2 (LO = quotient, HI = remainder, unsigned 32-bit) and mflo $a0.
    const std::uint32_t dividend = cpu.read_gpr_u32(2U);
    const std::uint32_t divisor = cpu.read_gpr_u32(6U);
    cpu.write_lo(0U, dividend / divisor);
    cpu.write_hi(0U, dividend % divisor);
    cpu.write_gpr_u64(4U, cpu.read_lo(0U));                                               // mflo $a0
    cpu.write_gpr_sign_extended_32(4U, cpu.read_gpr_u32(4U) << 2U);                       // sll $a0,$a0,2
    cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(record + 28U));                      // lw $v0,28($v1)
    cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(2U) + cpu.read_gpr_u32(4U));      // addu $v0,$v0,$a0
    m.write_u32_le(cpu.read_gpr_u32(2U), cpu.read_fpr_bits(12U));                         // swc1 $f12,0($v0)
    cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(record + 24U));                      // lw $v0,24($v1)

    // 0x254c54: beq $v0,$zero,0x254c70. A zero index is first reloaded from +20 (stored back to +24).
    if (cpu.read_gpr_u64(2U) == 0U) {
        cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(record + 20U));                  // lw $v0,20($v1)
        m.write_u32_le(record + 24U, cpu.read_gpr_u32(2U));                               // sw $v0,24($v1)
        retired += 3U;
    }
    // 0x254c5c..0x254c6c: step the index down and return 1; 0x254c7c..0x254c80 is the return itself.
    cpu.write_gpr_sign_extended_32(4U, m.read_u32_le(record + 24U));                      // lw $a0,24($v1)
    cpu.write_gpr_sign_extended_32(2U, 1U);                                               // addiu $v0,$zero,1
    cpu.write_gpr_sign_extended_32(4U, cpu.read_gpr_u32(4U) - 1U);                       // addiu $a0,$a0,-1
    m.write_u32_le(record + 24U, cpu.read_gpr_u32(4U));                                   // sw $a0,24($v1)
    return call.finish(retired + 5U + 2U);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00254a90, 500U, 0xdf69c48ba7c76e17ULL, "AppendSampleRecord", 122U, append_sample_record,
               EngineReturn::Integer)
