// 0x00263120 (a0 = output block of six quadwords, a1 = camera/view record, f12 = scalar): BuildProjectionMatrix.
// A straight 137-instruction path with no branches and no calls. The rows at a0+0/16/32/48 and the quadwords at
// a0+64/80 are read and written back in place under VU0 destination masks, so lanes a step does not write keep
// their old values. The original's scratch frame lies 80 bytes below the entry $sp: four floats are stored there,
// read back as one quadword, copied to the frame's first quadword and partly re-read as a word. Those stores go to
// guest memory at the same addresses, so the frame matches the original.
//
// Register semantics follow the interpreter. qmtc2 copies all 128 bits of a GPR into a VU0 register, while lui,
// addiu and mfc1 rewrite only the low 64 bits of a GPR (write_gpr_sign_extended_32). So the upper words a later
// qmtc2 copies are the caller's, or those of the quadword that lq loaded. Each step is the listing instruction at
// the address in its comment, using the same primitives as the reference interpreter.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// The frame the original reserves with addiu $sp,$sp,-80 (and releases on return).
constexpr std::uint32_t kFrameBytes = 80U;

// MIN.S on the raw sign/magnitude patterns, ordered as guest::ee_cop1::min_s orders them (no operand clamp).
std::uint32_t ee_min(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}

template <class Context>
n3::Run build_projection_matrix(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress out = call.arg(0);
    const GuestAddress view = call.arg(1);
    // $sp after the prologue. The native never changes $sp itself, so it still holds the entry value on return.
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // 263120 addiu $sp,$sp,-80 is the `frame` above.
    cpu.write_fpr_bits(1U, m.read_u32_le(view + 412U));                                // 263124 lwc1 f1,412(a1)
    cpu.write_fpr_bits(0U, m.read_u32_le(gp - 30380U));                                // 263128 lwc1 f0,-30380(gp)
    cpu.write_fpr_bits(4U, m.read_u32_le(view + 396U));                                // 26312c lwc1 f4,396(a1)
    cpu.write_fpr_bits(5U, ee_min(cpu.read_fpr_bits(1U), cpu.read_fpr_bits(0U)));      // 263130 min.s f5,f1,f0
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 48U));                             // 263134 lqc2 vf1,48(a0)
    cpu.write_gpr_sign_extended_32(3U, 0x3f800000U);                                   // 263138 lui v1,0x3f80
    cpu.write_gpr_sign_extended_32(6U, frame);                                         // 26313c addiu a2,sp,0
    cpu.write_fpr_bits(3U, cpu.read_gpr_u32(3U));                                      // 263140 mtc1 v1,f3 (1.0)
    cpu.write_fpr_bits(8U, m.read_u32_le(view + 536U));                                // 263144 lwc1 f8,536(a1)
    cpu.write_gpr_sign_extended_32(3U, 0xc0000000U);                                   // 263148 lui v1,0xc000
    cpu.write_vu0_vector(7U, n3::load_quad_any(m, view + 224U));                           // 26314c lqc2 vf7,224(a1)
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(3U));                                      // 263150 mtc1 v1,f0 (-2.0)
    cpu.write_fpr_bits(7U, m.read_u32_le(view + 540U));                                // 263154 lwc1 f7,540(a1)
    cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(0U), cpu.read_fpr_bits(4U)));    // 263158 mul.s f0,f0,f4
    cpu.write_gpr_sign_extended_32(3U, 0x43800000U);                                   // 26315c lui v1,0x4380
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 1U, 3U, true);                            // 263160 vsubw.w vf1,vf0,vf0w
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(1U));                             // 263164 sqc2 vf1,48(a0)
    cpu.write_fpr_bits(1U, fpu::mul(cpu.read_fpr_bits(0U), cpu.read_fpr_bits(5U)));    // 263168 mul.s f1,f0,f5
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 0U));                              // 26316c lqc2 vf1,0(a0)
    cpu.write_fpr_bits(0U, fpu::sub(cpu.read_fpr_bits(4U), cpu.read_fpr_bits(5U)));    // 263170 sub.s f0,f4,f5
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, view + 240U));                           // 263174 lqc2 vf6,240(a1)
    cpu.write_fpr_bits(3U, fpu::div(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(0U)));    // 263178 div.s f3,f3,f0
    ee_cop2::vadd_vector(cpu, 1U, 7U, 7U, 12U, false);                                 // 26317c vadd.xy vf1,vf7,vf7
    n3::store_quad_any(m, out + 0U, cpu.read_vu0_vector(1U));                              // 263180 sqc2 vf1,0(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 16U));                             // 263184 lqc2 vf1,16(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, view + 256U));                           // 263188 lqc2 vf5,256(a1)
    ee_cop2::vadd_vector(cpu, 1U, 6U, 6U, 12U, false);                                 // 26318c vadd.xy vf1,vf6,vf6
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(1U));                             // 263190 sqc2 vf1,16(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 32U));                             // 263194 lqc2 vf1,32(a0)
    cpu.write_fpr_bits(0U, fpu::add(cpu.read_fpr_bits(4U), cpu.read_fpr_bits(5U)));    // 263198 add.s f0,f4,f5
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, view + 272U));                           // 26319c lqc2 vf4,272(a1)
    ee_cop2::vadd_vector(cpu, 1U, 5U, 5U, 12U, false);                                 // 2631a0 vadd.xy vf1,vf5,vf5
    cpu.write_fpr_bits(6U, cpu.read_gpr_u32(3U));                                      // 2631a4 mtc1 v1,f6 (256.0)
    cpu.write_fpr_bits(1U, fpu::mul(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(1U)));    // 2631a8 mul.s f1,f3,f1
    cpu.write_gpr_sign_extended_32(3U, 0x3f000000U);                                   // 2631ac lui v1,0x3f00
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(1U));                             // 2631b0 sqc2 vf1,32(a0)
    cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(0U)));    // 2631b4 mul.s f0,f3,f0
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out + 48U));                             // 2631b8 lqc2 vf3,48(a0)
    cpu.write_fpr_bits(5U, fpu::add(cpu.read_fpr_bits(6U), cpu.read_fpr_bits(8U)));    // 2631bc add.s f5,f6,f8
    ee_cop2::vadd_vector(cpu, 3U, 4U, 4U, 12U, false);                                 // 2631c0 vadd.xy vf3,vf4,vf4
    cpu.write_fpr_bits(5U, fpu::div(cpu.read_fpr_bits(8U), cpu.read_fpr_bits(5U)));    // 2631c4 div.s f5,f8,f5
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(3U));                             // 2631c8 sqc2 vf3,48(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 0U));                              // 2631cc lqc2 vf1,0(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 3U, 7U, 1U, 2U, false);                           // 2631d0 vaddz.w vf1,vf3,vf7z
    n3::store_quad_any(m, out + 0U, cpu.read_vu0_vector(1U));                              // 2631d4 sqc2 vf1,0(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 48U));                             // 2631d8 lqc2 vf1,48(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out + 16U));                             // 2631dc lqc2 vf3,16(a0)
    ee_cop2::vadd_broadcast(cpu, 3U, 1U, 6U, 1U, 2U, false);                           // 2631e0 vaddz.w vf3,vf1,vf6z
    m.write_u32_le(frame + 16U, cpu.read_fpr_bits(5U));                                // 2631e4 swc1 f5,16(sp)
    cpu.write_fpr_bits(5U, fpu::add(cpu.read_fpr_bits(6U), cpu.read_fpr_bits(7U)));    // 2631e8 add.s f5,f6,f7
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(3U));                             // 2631ec sqc2 vf3,16(a0)
    cpu.write_fpr_bits(5U, fpu::div(cpu.read_fpr_bits(7U), cpu.read_fpr_bits(5U)));    // 2631f0 div.s f5,f7,f5
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 48U));                             // 2631f4 lqc2 vf1,48(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out + 32U));                             // 2631f8 lqc2 vf3,32(a0)
    ee_cop2::vadd_broadcast(cpu, 3U, 1U, 5U, 1U, 2U, false);                           // 2631fc vaddz.w vf3,vf1,vf5z
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(3U));                             // 263200 sqc2 vf3,32(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 48U));                             // 263204 lqc2 vf1,48(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 4U, 1U, 2U, false);                           // 263208 vaddz.w vf1,vf1,vf4z
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(1U));                             // 26320c sqc2 vf1,48(a0)
    cpu.write_fpr_bits(2U, cpu.read_gpr_u32(3U));                                      // 263210 mtc1 v1,f2 (0.5)
    m.write_u32_le(frame + 28U, cpu.read_fpr_bits(1U));                                // 263214 swc1 f1,28(sp)
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(12U));                        // 263218 mfc1 v1,f12
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 0U));                              // 26321c lqc2 vf1,0(a0)
    m.write_u32_le(frame + 24U, cpu.read_fpr_bits(0U));                                // 263220 swc1 f0,24(sp)
    cpu.write_fpr_bits(1U, m.read_u32_le(view + 520U));                                // 263224 lwc1 f1,520(a1)
    cpu.write_fpr_bits(0U, m.read_u32_le(view + 524U));                                // 263228 lwc1 f0,524(a1)
    m.write_u32_le(frame + 20U, cpu.read_fpr_bits(5U));                                // 26322c swc1 f5,20(sp)
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));                                        // 263230 qmtc2 v1,vf2
    cpu.write_fpr_bits(4U, m.read_u32_le(view + 512U));                                // 263234 lwc1 f4,512(a1)
    cpu.write_fpr_bits(3U, m.read_u32_le(view + 516U));                                // 263238 lwc1 f3,516(a1)
    cpu.write_fpr_bits(1U, fpu::mul(cpu.read_fpr_bits(8U), cpu.read_fpr_bits(1U)));    // 26323c mul.s f1,f8,f1
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 1U, 12U, 3U, true);                           // 263240 vsubw.xy vf1,vf1,vf1w
    cpu.write_gpr(3U, n3::load_quad_any(m, frame + 16U));                                  // 263244 lq v1,16(sp)
    n3::store_quad_any(m, out + 0U, cpu.read_vu0_vector(1U));                              // 263248 sqc2 vf1,0(a0)
    cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(7U), cpu.read_fpr_bits(0U)));    // 26324c mul.s f0,f7,f0
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 16U));                             // 263250 lqc2 vf1,16(a0)
    cpu.write_fpr_bits(1U, fpu::mul(cpu.read_fpr_bits(2U), cpu.read_fpr_bits(1U)));    // 263254 mul.s f1,f2,f1
    cpu.write_vu0_vector(7U, cpu.read_gpr(3U));                                        // 263258 qmtc2 v1,vf7
    cpu.write_fpr_bits(0U, fpu::mul(cpu.read_fpr_bits(2U), cpu.read_fpr_bits(0U)));    // 26325c mul.s f0,f2,f0
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 1U, 12U, 3U, true);                           // 263260 vsubw.xy vf1,vf1,vf1w
    n3::store_quad_any(m, cpu.read_gpr_u32(6U), cpu.read_gpr(3U));                         // 263264 sq v1,0(a2)
    cpu.write_fpr_bits(2U, cpu.read_gpr_u32(3U));                                      // 263268 mtc1 v1,f2
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(1U));                             // 26326c sqc2 vf1,16(a0)
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(1U));                         // 263270 mfc1 v1,f1
    cpu.write_fpr_bits(2U, fpu::div(cpu.read_fpr_bits(4U), cpu.read_fpr_bits(2U)));    // 263274 div.s f2,f4,f2
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 32U));                             // 263278 lqc2 vf1,32(a0)
    cpu.write_vu0_vector(3U, cpu.read_gpr(3U));                                        // 26327c qmtc2 v1,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 1U, 12U, 3U, true);                           // 263280 vsubw.xy vf1,vf1,vf1w
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(1U));                             // 263284 sqc2 vf1,32(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 48U));                             // 263288 lqc2 vf1,48(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 1U, 12U, 3U, true);                           // 26328c vsubw.xy vf1,vf1,vf1w
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(1U));                             // 263290 sqc2 vf1,48(a0)
    cpu.write_fpr_bits(1U, m.read_u32_le(frame + 4U));                                 // 263294 lwc1 f1,4(sp)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 0U));                              // 263298 lqc2 vf1,0(a0)
    cpu.write_gpr_sign_extended_32(6U, cpu.read_fpr_bits(2U));                         // 26329c mfc1 a2,f2
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(0U));                         // 2632a0 mfc1 v1,f0
    cpu.write_vu0_vector(6U, cpu.read_gpr(6U));                                        // 2632a4 qmtc2 a2,vf6
    cpu.write_fpr_bits(1U, fpu::div(cpu.read_fpr_bits(3U), cpu.read_fpr_bits(1U)));    // 2632a8 div.s f1,f3,f1
    ee_cop2::vmul(cpu, 1U, 1U, 7U, 12U);                                               // 2632ac vmul.xy vf1,vf1,vf7
    n3::store_quad_any(m, out + 0U, cpu.read_vu0_vector(1U));                              // 2632b0 sqc2 vf1,0(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 16U));                             // 2632b4 lqc2 vf5,16(a0)
    cpu.write_vu0_vector(1U, cpu.read_gpr(3U));                                        // 2632b8 qmtc2 v1,vf1
    ee_cop2::vmul(cpu, 5U, 5U, 7U, 12U);                                               // 2632bc vmul.xy vf5,vf5,vf7
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(5U));                             // 2632c0 sqc2 vf5,16(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 32U));                             // 2632c4 lqc2 vf5,32(a0)
    cpu.write_gpr_sign_extended_32(5U, cpu.read_fpr_bits(1U));                         // 2632c8 mfc1 a1,f1
    ee_cop2::vmul(cpu, 5U, 5U, 7U, 12U);                                               // 2632cc vmul.xy vf5,vf5,vf7
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(5U));                             // 2632d0 sqc2 vf5,32(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 48U));                             // 2632d4 lqc2 vf5,48(a0)
    cpu.write_vu0_vector(4U, cpu.read_gpr(5U));                                        // 2632d8 qmtc2 a1,vf4
    ee_cop2::vmul(cpu, 5U, 5U, 7U, 12U);                                               // 2632dc vmul.xy vf5,vf5,vf7
    ee_cop2::vmul_broadcast(cpu, 5U, 7U, 5U, 2U, 3U);                                  // 2632e0 vmulw.z vf5,vf7,vf5w
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(5U));                             // 2632e4 sqc2 vf5,48(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 0U));                              // 2632e8 lqc2 vf5,0(a0)
    ee_cop2::vmul_broadcast(cpu, 5U, 7U, 5U, 2U, 3U);                                  // 2632ec vmulw.z vf5,vf7,vf5w
    n3::store_quad_any(m, out + 0U, cpu.read_vu0_vector(5U));                              // 2632f0 sqc2 vf5,0(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 16U));                             // 2632f4 lqc2 vf5,16(a0)
    ee_cop2::vmul_broadcast(cpu, 5U, 7U, 5U, 2U, 3U);                                  // 2632f8 vmulw.z vf5,vf7,vf5w
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(5U));                             // 2632fc sqc2 vf5,16(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 32U));                             // 263300 lqc2 vf5,32(a0)
    ee_cop2::vmul_broadcast(cpu, 5U, 7U, 5U, 2U, 3U);                                  // 263304 vmulw.z vf5,vf7,vf5w
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(5U));                             // 263308 sqc2 vf5,32(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 48U));                             // 26330c lqc2 vf5,48(a0)
    ee_cop2::vadd_broadcast(cpu, 5U, 5U, 7U, 2U, 3U, false);                           // 263310 vaddw.z vf5,vf5,vf7w
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(5U));                             // 263314 sqc2 vf5,48(a0)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, out + 64U));                             // 263318 lqc2 vf5,64(a0)
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 6U, 8U, 0U, false);                           // 26331c vaddx.x vf5,vf0,vf6x
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 4U, 4U, 0U, false);                           // 263320 vaddx.y vf5,vf0,vf4x
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 3U, 2U, 0U, false);                           // 263324 vaddx.z vf5,vf0,vf3x
    ee_cop2::vmul_broadcast(cpu, 5U, 0U, 1U, 1U, 0U);                                  // 263328 vmulx.w vf5,vf0,vf1x
    n3::store_quad_any(m, out + 64U, cpu.read_vu0_vector(5U));                             // 26332c sqc2 vf5,64(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + 80U));                             // 263330 lqc2 vf1,80(a0)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, 8U, 0U, false);                           // 263334 vaddx.x vf1,vf0,vf2x
    n3::store_quad_any(m, out + 80U, cpu.read_vu0_vector(1U));                             // 263338 sqc2 vf1,80(a0)

    // 263340 addiu $sp,$sp,80 (delay slot of jr $ra): $sp is back at its entry value.
    return call.finish(137U);
}

} // namespace

extern constinit const EngineFunction n3_00263120{0x00263120U, 548U, 0x01b65f53af0cd78aULL, "BuildProjectionMatrix",
                                                  137U, n3::live<&build_projection_matrix<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00263120, build_projection_matrix)
