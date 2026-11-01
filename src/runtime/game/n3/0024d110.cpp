// 0x0024d110 (a0 = direction vector, a1 = colour quad, f12 and f13 size the flare): ProjectSun.
// VU0 normalises the direction at a0 (VF1.x receives its squared length, VRSQRT puts 1 / length in Q), and the unit
// vector is stored at 0x01d9b940. The colour at a1 is stored times 128 at 0x01d9b960 and times its alpha times 128
// at 0x01d9b970. The count at 0x00667050 selects a 560-byte row at 0x00665ec0 + 560 * count + 1136, whose quads and
// floats are read at offsets 208..551 (for a count of 0 or less, the quad at offset 208 of the row is used in
// place of the quad at 0x00666320). The flag byte at $gp-23744 is 1 when the float at $sp+72 is at most the
// float at $gp-31236, else 0. Retires 68 instructions when the flag is set and 142 otherwise. Returns nothing.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

// MIPS register numbers used by the listing.
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;
constexpr unsigned kSp = 29U;

constexpr std::uint32_t kFrameBytes = 304U;                  // addiu $sp, $sp, -304 (and +304 on the way out)
constexpr GuestAddress kCountAddress = 0x00667050U;          // lw 28752($v1), $v1 = 0x00660000
constexpr GuestAddress kCurveQuadAddress = 0x00666320U;      // lq 25376($a0), $a0 = 0x00660000 (count > 0)
constexpr GuestAddress kRowBase = 0x00665EC0U;               // addiu 24256 on $a0 = 0x00660000
constexpr std::uint32_t kRowStride = 560U;                   // (count * 7) * 4 + count * 7, shifted left by 4
constexpr std::uint32_t kRowBias = 1136U;                    // addiu $v1, $v1, 1136
constexpr GuestAddress kSunDirectionAddress = 0x01D9B940U;   // -18112 on $a0 = 0x01da0000
constexpr GuestAddress kSunColourAddress = 0x01D9B960U;      // -18080 on $a1 = 0x01da0000
constexpr GuestAddress kSunPremultipliedAddress = 0x01D9B970U;  // -18064 on $a0 = 0x01da0000
constexpr std::int32_t kGpFirst = -31236;                    // lowest $gp-relative access (lwc1 $f0)
constexpr std::int32_t kGpLast = -23744;                     // highest $gp-relative access (the flag byte)
constexpr std::int32_t kFlagOffset = -23744;                 // sb $v1 / sb $zero

// Base plus a signed offset, wrapping the way 32-bit address arithmetic does.
constexpr GuestAddress offset_address(const GuestAddress base, const std::int32_t offset) noexcept {
    return base + static_cast<GuestAddress>(offset);
}

// dsra32 with a shift of 32: the arithmetic shift of the whole doubleword.
constexpr std::uint64_t arithmetic_shift_right_32(const std::uint64_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value) >> 32U);
}

// Every load and store of the listing, checked as the interpreter checks it (Lq and Sq mask the address to 16
// bytes; Lqc2 and Sqc2 need 16-byte alignment; Lwc1, Sw and Swc1 need 4 bytes; Sb needs one byte; every access
// must lie in guest memory). The accesses inside each region (the sun stores, the frame, the row, the $gp
// globals) are covered by one span check per region. A declined call runs the original, which faults by itself.
template <class Memory>
bool accesses_ok(const Memory& m, const GuestAddress vector_address, const GuestAddress quad_address,
                 const GuestAddress gp, const GuestAddress frame, const GuestAddress row) {
    constexpr auto kGlobalSpan = static_cast<std::size_t>(kGpLast - kGpFirst + 1);
    return (vector_address & 15U) == 0U && m.contains(vector_address, 16U) &&     // lqc2 $vf3, 0($a0)
           m.contains(quad_address & ~15U, 16U) &&                                // lq $a1, 0($a1)
           m.contains(kCurveQuadAddress, 16U) &&                                  // lq from 0x00666320
           m.contains(kSunDirectionAddress, 64U) &&                               // the three VU0 stores
           (frame & 15U) == 0U && m.contains(frame, kFrameBytes) &&               // the frame
           m.contains(row + 208U, 344U) &&                                        // the row, offsets 208..551
           (gp & 3U) == 0U && m.contains(offset_address(gp, kGpFirst), kGlobalSpan);  // the $gp globals
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Stores are made in the
// listing's order, so every load sees the bytes that the original's load sees.
template <class Context>
n3::Run project_sun(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // lw $a2, 28752($v1) is the first load, read before anything is written.
    if (!m.contains(kCountAddress, 4U)) {
        return call.decline();
    }
    const std::uint32_t count = m.read_u32_le(kCountAddress);
    const GuestAddress vector_address = call.arg(0U);
    const GuestAddress quad_address = call.arg(1U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress row = kRowBase + kRowStride * count + kRowBias;
    if (!accesses_ok(m, vector_address, quad_address, gp, frame, row)) {
        return call.decline();
    }

    // 0x0024d110..0x0024d18c: the direction is normalised on VU0, and the unit vector and the colour are stored.
    cpu.write_gpr_sign_extended_32(kSp, frame);                                                // addiu $sp, $sp, -304
    cpu.write_gpr_sign_extended_32(kV1, 0x00660000U);                                          // lui $v1, 0x0066
    cpu.write_gpr(kA1, n3::load_quad_any(m, quad_address));                                    // lq $a1, 0($a1)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                                   // vaddw.x $vf1, $vf0, $vf0w
    cpu.write_gpr_sign_extended_32(kA2, count);                                                // lw $a2, 28752($v1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vector_address));                            // lqc2 $vf3, 0($a0)
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kA2) << 3U);                          // sll $v1, $a2, 3
    cpu.write_gpr(kA3, cpu.read_vu0_vector(1U));                                               // qmfc2 $a3, $vf1
    cpu.write_gpr_sign_extended_32(kA0, 0x00660000U);                                          // lui $a0, 0x0066
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_gpr_u32(kA0) + 24256U);                       // addiu $a0, $a0, 24256
    n3::store_quad_any(m, frame + 272U, cpu.read_gpr(kA1));                                    // sq $a1, 272($sp)
    ee_cop2::vmul(cpu, 1U, 3U, 3U, 14U);                                                       // vmul.xyz $vf1, $vf3, $vf3
    cpu.write_gpr_sign_extended_32(kA1, cpu.read_gpr_u32(kV1) - cpu.read_gpr_u32(kA2));        // subu $a1, $v1, $a2
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kA1) << 2U);                          // sll $v1, $a1, 2
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kA1) + cpu.read_gpr_u32(kV1));        // addu $v1, $a1, $v1
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kV1) << 4U);                          // sll $v1, $v1, 4
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kA0) + cpu.read_gpr_u32(kV1));        // addu $v1, $a0, $v1
    cpu.write_gpr(kA0, cpu.read_vu0_vector(1U));                                               // qmfc2 $a0, $vf1
    cpu.write_gpr_sign_extended_32(kV1, cpu.read_gpr_u32(kV1) + kRowBias);                     // addiu $v1, $v1, 1136
    cpu.write_vu0_vector(2U, cpu.read_gpr(kA0));                                               // qmtc2 $a0, $vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                             // vadday.x $acc, $vf2, $vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA3));                                               // qmtc2 $a3, $vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);                    // vmaddz.x $vf1, $vf1, $vf2z
    cpu.write_gpr(kA0, cpu.read_vu0_vector(1U));                                               // qmfc2 $a0, $vf1
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                               // qmtc2 $a0, $vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                                 // vrsqrt $q, $vf0w, $vf1x
    // vwaitq: VU0 runs synchronously, so Q is already final and there is nothing to wait for.
    ee_cop2::vmul_control(cpu, 3U, 3U, 14U, ee_cop2::kControlQ);                               // vmulq.xyz $vf3, $vf3, $q
    cpu.write_gpr(kA1, cpu.read_vu0_vector(3U));                                               // qmfc2 $a1, $vf3
    cpu.write_gpr_sign_extended_32(kA0, 0x01da0000U);                                          // lui $a0, 0x01da
    // blez $a2 (taken for a count of 0 or less); its delay slot stores the unit vector on either path.
    n3::store_quad_any(m, kSunDirectionAddress, cpu.read_gpr(kA1));                            // sq $a1, -18112($a0)

    // 0x0024d190..0x0024d198 (count > 0) or 0x0024d34c..0x0024d354 (count <= 0): one quad, stored at 240($sp).
    if (static_cast<std::int32_t>(count) > 0) {
        cpu.write_gpr_sign_extended_32(kA0, 0x00660000U);                                      // lui $a0, 0x0066
        cpu.write_gpr(kA0, n3::load_quad_any(m, kCurveQuadAddress));                           // lq $a0, 25376($a0)
    } else {
        cpu.write_gpr(kA0, n3::load_quad_any(m, row + 208U));                                  // lq $a0, 208($v1)
    }
    n3::store_quad_any(m, frame + 240U, cpu.read_gpr(kA0));                                    // sq $a0, 240($sp)

    // 0x0024d19c..0x0024d200: the colour and the row's quads go through VU0, and the compare of the listing is made.
    cpu.write_gpr_sign_extended_32(kA0, 0x01da0000U);                                          // lui $a0, 0x01da
    cpu.write_gpr_sign_extended_32(kA1, 0x42480000U);                                          // lui $a1, 0x4248
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, kSunDirectionAddress));                      // lqc2 $vf1, -18112($a0)
    cpu.write_vu0_vector(2U, cpu.read_gpr(kA1));                                               // qmtc2 $a1, $vf2
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, frame + 240U));                              // lqc2 $vf4, 240($sp)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);                                         // vmulx.xyz $vf1, $vf1, $vf2x
    cpu.write_gpr(kA2, n3::load_quad_any(m, row + 240U));                                      // lq $a2, 240($v1)
    cpu.write_gpr(kA1, n3::load_quad_any(m, row + 256U));                                      // lq $a1, 256($v1)
    cpu.write_gpr(kA0, n3::load_quad_any(m, row + 272U));                                      // lq $a0, 272($v1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, row + 224U));                                // lqc2 $vf3, 224($v1)
    ee_cop2::vadd_vector(cpu, 2U, 4U, 1U, 14U, false);                                         // vadd.xyz $vf2, $vf4, $vf1
    ee_cop2::vmula_broadcast(cpu, 3U, 2U, 14U, false);                                         // vmulax.xyz $acc, $vf3, $vf2x
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA2));                                               // qmtc2 $a2, $vf1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 1U, false, true, false);                    // vmadday.xyz $acc, $vf1, $vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));                                               // qmtc2 $a1, $vf1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, 14U, 2U, false, true, false);                    // vmaddaz.xyz $acc, $vf1, $vf2z
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA0));                                               // qmtc2 $a0, $vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, 14U, 3U, false, false, false);                   // vmaddw.xyz $vf1, $vf1, $vf0w
    cpu.write_gpr(kA1, cpu.read_vu0_vector(1U));                                               // qmfc2 $a1, $vf1
    cpu.write_gpr_sign_extended_32(kA0, frame + 64U);                                          // addiu $a0, $sp, 64
    cpu.write_fpr_bits(0U, m.read_u32_le(offset_address(gp, -31236)));                         // lwc1 $f0, -31236($gp)
    n3::store_quad_any(m, frame + 64U, cpu.read_gpr(kA1));                                     // sq $a1, 0($a0)
    cpu.write_fpr_bits(1U, m.read_u32_le(frame + 72U));                                        // lwc1 $f1, 72($sp)
    ee_cop1::compare_s(cpu, ee_cop1::Compare::LessEqual, 1U, 0U);                              // c.ole.s $f1, $f0
    cpu.write_vu0_vector(1U, cpu.read_gpr(kA1));                                               // qmtc2 $a1, $vf1 (bc1f slot)

    if ((cpu.fcr31_bits() & ee_cop1::kFcr31Condition) != 0U) {
        // The compare held, so bc1f falls through: 0x0024d204..0x0024d20c, then 0x0024d344..0x0024d348.
        cpu.write_gpr_sign_extended_32(kV1, 1U);                                               // addiu $v1, $zero, 1
        m.write_u8(offset_address(gp, kFlagOffset), static_cast<std::uint8_t>(cpu.read_gpr_u32(kV1)));  // sb $v1, -23744($gp)
        cpu.write_gpr_sign_extended_32(kSp, frame + kFrameBytes);                              // addiu $sp, $sp, 304
        return call.finish(68U);
    }

    // The compare failed, so bc1f is taken to 0x0024d210 (the flag is cleared).
    // 0x0024d210..0x0024d340: the colour is scaled, the premultiplied colour and the quads are stored, and the
    // floats of the row are combined and stored, then 0x0024d344..0x0024d348 restores $sp.
    cpu.write_gpr_u64(kA0, 0xffffULL);                                                         // ori $a0, $zero, 0xffff
    cpu.write_gpr(kT2, cpu.read_vu0_vector(1U));                                               // qmfc2 $t2, $vf1
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA0) << 32U);                                      // dsll32 $a1, $a0, 0
    cpu.write_gpr_sign_extended_32(kA3, 0x3f800000U);                                          // lui $a3, 0x3f80
    cpu.write_gpr_sign_extended_32(kA0, 0x41800000U);                                          // lui $a0, 0x4180
    cpu.write_fpr_bits(2U, m.read_u32_le(row + 536U));                                         // lwc1 $f2, 536($v1)
    cpu.write_fpr_bits(4U, cpu.read_gpr_u32(kA0));                                             // mtc1 $a0, $f4
    cpu.write_gpr(kA2, n3::load_quad_any(m, row + 480U));                                      // lq $a2, 480($v1)
    cpu.write_fpr_bits(1U, m.read_u32_le(row + 544U));                                         // lwc1 $f1, 544($v1)
    m.write_u8(offset_address(gp, kFlagOffset), static_cast<std::uint8_t>(0U));                // sb $zero, -23744($gp)
    cpu.write_gpr_sign_extended_32(kA0, frame + 16U);                                          // addiu $a0, $sp, 16
    n3::store_quad_any(m, frame + 16U, cpu.read_vu0_vector(1U));                               // sqc2 $vf1, 0($a0)
    cpu.write_fpr_bits(0U, m.read_u32_le(row + 540U));                                         // lwc1 $f0, 540($v1)
    cpu.write_fpr_bits(5U, m.read_u32_le(row + 548U));                                         // lwc1 $f5, 548($v1)
    ee_cop1::cvt_w_s(cpu, 7U, 2U);                                                             // cvt.w.s $f7, $f2
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA2) & cpu.read_gpr_u64(kA1));                     // and $a1, $a2, $a1
    cpu.write_gpr_sign_extended_32(kA0, frame);                                                // addiu $a0, $sp, 0
    cpu.write_gpr_u64(kA2, cpu.read_gpr_u64(kA2) & 0xffffU);                                   // andi $a2, $a2, 0xffff
    n3::store_quad_any(m, frame, cpu.read_vu0_vector(1U));                                     // sqc2 $vf1, 0($a0)
    cpu.write_gpr_u64(kT1, cpu.read_gpr_u64(kA2) << 32U);                                      // dsll32 $t1, $a2, 0
    cpu.write_gpr_sign_extended_32(kA2, 0x43000000U);                                          // lui $a2, 0x4300
    cpu.write_gpr_u64(kT1, arithmetic_shift_right_32(cpu.read_gpr_u64(kT1)));                  // dsra32 $t1, $t1, 0
    cpu.write_gpr_u64(kA1, cpu.read_gpr_u64(kA1) >> 32U);                                      // dsrl32 $a1, $a1, 0
    ee_cop1::mul_s(cpu, 3U, 4U, 1U);                                                           // mul.s $f3, $f4, $f1
    cpu.write_gpr_u64(kT0, cpu.read_gpr_u64(kA1) << 32U);                                      // dsll32 $t0, $a1, 0
    cpu.write_gpr_u64(kT0, arithmetic_shift_right_32(cpu.read_gpr_u64(kT0)));                  // dsra32 $t0, $t0, 0
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 272U));                              // lqc2 $vf1, 272($sp)
    cpu.write_gpr_sign_extended_32(kA0, 0x01da0000U);                                          // lui $a0, 0x01da
    cpu.write_gpr_sign_extended_32(kA1, 0x01da0000U);                                          // lui $a1, 0x01da
    ee_cop1::cvt_w_s(cpu, 6U, 0U);                                                             // cvt.w.s $f6, $f0
    cpu.write_vu0_vector(3U, cpu.read_gpr(kA2));                                               // qmtc2 $a2, $vf3
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 3U, 15U, 0U);                                         // vmulx.xyzw $vf2, $vf1, $vf3x
    m.write_u32_le(offset_address(gp, -23776), cpu.read_gpr_u32(kT1));                         // sw $t1, -23776($gp)
    m.write_u32_le(offset_address(gp, -23772), cpu.read_gpr_u32(kT0));                         // sw $t0, -23772($gp)
    m.write_u32_le(offset_address(gp, -23792), cpu.read_fpr_bits(1U));                         // swc1 $f1, -23792($gp)
    cpu.write_fpr_bits(2U, m.read_u32_le(frame + 20U));                                        // lwc1 $f2, 20($sp)
    cpu.write_fpr_bits(1U, m.read_u32_le(frame + 8U));                                         // lwc1 $f1, 8($sp)
    n3::store_quad_any(m, kSunColourAddress, cpu.read_vu0_vector(2U));                         // sqc2 $vf2, -18080($a1)
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(kA3));                                             // mtc1 $a3, $f0
    ee_cop1::cvt_w_s(cpu, 3U, 3U);                                                             // cvt.w.s $f3, $f3
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 1U, 15U, 3U);                                         // vmulw.xyzw $vf1, $vf1, $vf1w
    ee_cop1::div_s(cpu, 0U, 0U, 1U);                                                           // div.s $f0, $f0, $f1
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 15U, 0U);                                         // vmulx.xyzw $vf1, $vf1, $vf3x
    n3::store_quad_any(m, kSunPremultipliedAddress, cpu.read_vu0_vector(1U));                  // sqc2 $vf1, -18064($a0)
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_fpr_bits(3U));                                // mfc1 $a0, $f3
    ee_cop1::mul_s(cpu, 1U, 4U, 5U);                                                           // mul.s $f1, $f4, $f5
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_gpr_u32(kT1) + cpu.read_gpr_u32(kA0));        // addu $a0, $t1, $a0
    m.write_u32_le(offset_address(gp, -23768), cpu.read_gpr_u32(kA0));                         // sw $a0, -23768($gp)
    ee_cop1::cvt_w_s(cpu, 1U, 1U);                                                             // cvt.w.s $f1, $f1
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_fpr_bits(1U));                                // mfc1 $a0, $f1
    cpu.write_fpr_bits(8U, cpu.read_gpr_u32(kT2));                                             // mtc1 $t2, $f8
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_gpr_u32(kT0) + cpu.read_gpr_u32(kA0));        // addu $a0, $t0, $a0
    ee_cop1::mul_s(cpu, 1U, 8U, 0U);                                                           // mul.s $f1, $f8, $f0
    m.write_u32_le(offset_address(gp, -23764), cpu.read_gpr_u32(kA0));                         // sw $a0, -23764($gp)
    ee_cop1::mul_s(cpu, 0U, 2U, 0U);                                                           // mul.s $f0, $f2, $f0
    m.write_u32_le(offset_address(gp, -23804), cpu.read_fpr_bits(0U));                         // swc1 $f0, -23804($gp)
    m.write_u32_le(offset_address(gp, -23808), cpu.read_fpr_bits(1U));                         // swc1 $f1, -23808($gp)
    ee_cop1::cvt_s_w(cpu, 0U, 7U);                                                             // cvt.s.w $f0, $f7
    ee_cop1::cvt_s_w(cpu, 1U, 6U);                                                             // cvt.s.w $f1, $f6
    m.write_u32_le(offset_address(gp, -23800), cpu.read_fpr_bits(0U));                         // swc1 $f0, -23800($gp)
    ee_cop1::div_s(cpu, 0U, 1U, 0U);                                                           // div.s $f0, $f1, $f0
    cpu.write_fpr_bits(3U, m.read_u32_le(row + 524U));                                         // lwc1 $f3, 524($v1)
    m.write_u32_le(offset_address(gp, -23796), cpu.read_fpr_bits(1U));                         // swc1 $f1, -23796($gp)
    cpu.write_fpr_bits(1U, m.read_u32_le(row + 520U));                                         // lwc1 $f1, 520($v1)
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_fpr_bits(7U));                                // mfc1 $a0, $f7
    cpu.write_fpr_bits(2U, m.read_u32_le(row + 512U));                                         // lwc1 $f2, 512($v1)
    m.write_u32_le(offset_address(gp, -23784), cpu.read_gpr_u32(kA0));                         // sw $a0, -23784($gp)
    m.write_u32_le(offset_address(gp, -23748), cpu.read_fpr_bits(13U));                        // swc1 $f13, -23748($gp)
    ee_cop1::mul_s(cpu, 0U, 3U, 0U);                                                           // mul.s $f0, $f3, $f0
    ee_cop1::mul_s(cpu, 0U, 2U, 0U);                                                           // mul.s $f0, $f2, $f0
    m.write_u32_le(offset_address(gp, -23760), cpu.read_fpr_bits(0U));                         // swc1 $f0, -23760($gp)
    ee_cop1::mul_s(cpu, 0U, 12U, 1U);                                                          // mul.s $f0, $f12, $f1
    cpu.write_gpr_sign_extended_32(kA0, cpu.read_fpr_bits(6U));                                // mfc1 $a0, $f6
    m.write_u32_le(offset_address(gp, -23788), cpu.read_fpr_bits(5U));                         // swc1 $f5, -23788($gp)
    m.write_u32_le(offset_address(gp, -23780), cpu.read_gpr_u32(kA0));                         // sw $a0, -23780($gp)
    m.write_u32_le(offset_address(gp, -23756), cpu.read_fpr_bits(1U));                         // swc1 $f1, -23756($gp)
    m.write_u32_le(offset_address(gp, -23752), cpu.read_fpr_bits(0U));                         // swc1 $f0, -23752($gp)
    cpu.write_gpr_sign_extended_32(kSp, frame + kFrameBytes);                                  // addiu $sp, $sp, 304
    return call.finish(142U);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(0024d110, 592U, 0x09c54075b328e293ULL, "ProjectSun", 142U, project_sun, EngineReturn::Nothing)
