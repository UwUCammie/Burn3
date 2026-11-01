// 0x001ac0a0 (a0 = the object O): OrthonormalizeInertiaFrames. A leaf that calls nothing. Its four byte-copy loops
// run exactly 8 iterations each, so every path retires the same 460 instructions, which is the declared cost.
// Part one turns the vector at O+480 into reciprocals (DIV.S) and stores the rows at O+16, O+32 and O+48.
// Part two loads the three quadwords that the pointer at O+516 addresses, transposes them through the stack frame
// and forms three VU0 macro products against the rows. Part three copies frame quadwords to O+80 and O+96 and
// forms a second set of three products against O+64, O+80 and O+96. Part four copies the last frame quadwords to
// O+80 and O+96. The object stores keep the original's order. The stack frame (928 bytes below the entry $sp)
// is written as the original writes it. Nothing is returned.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// Retired instruction count of the only path (the loops are fixed; see the header comment).
constexpr std::uint32_t kCost = 460U;
// $sp inside the function: the entry $sp less this frame (addiu $sp,$sp,-928 at entry, +928 on return).
constexpr std::uint32_t kFrameBytes = 928U;
// VU0 destination masks, x the high bit as the instructions encode them (n3::kMaskXyz, n3::kMaskXyzw in n3_support.hpp).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;
// General-purpose registers, by their names in the listing.
constexpr unsigned kV1 = 3U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr unsigned kA3 = 7U;
constexpr unsigned kT0 = 8U;
constexpr unsigned kT1 = 9U;
constexpr unsigned kT2 = 10U;
constexpr unsigned kT3 = 11U;
constexpr unsigned kT4 = 12U;
constexpr unsigned kT5 = 13U;

// The state the listing reads and writes: the CPU (GPRs, FPRs, VU0), guest memory, the object in a0 and the
// function's own $sp, so that $sp-relative offsets read as they do in the listing.
template <class Base>
struct Ctx {
    guest::R5900State& cpu;
    Base& m;
    GuestAddress object;
    GuestAddress frame;
};

// Quadword moves. lq/sq/lqc2/sqc2 ignore the low four address bits (sq/lq mask them, as the interpreter does).
void lq(auto& s, const unsigned rt, const GuestAddress address) {
    s.cpu.write_gpr(rt, n3::load_quad_any(s.m, address));
}
void sq(auto& s, const unsigned rt, const GuestAddress address) {
    n3::store_quad_any(s.m, address, s.cpu.read_gpr(rt));
}
void lqc2(auto& s, const unsigned vf, const GuestAddress address) {
    s.cpu.write_vu0_vector(vf, n3::load_quad_any(s.m, address));
}
void sqc2(auto& s, const unsigned vf, const GuestAddress address) {
    n3::store_quad_any(s.m, address, s.cpu.read_vu0_vector(vf));
}
// Whole 128-bit moves between a GPR and a VU0 register.
void qmfc2(auto& s, const unsigned rt, const unsigned vf) {
    s.cpu.write_gpr(rt, s.cpu.read_vu0_vector(vf));
}
void qmtc2(auto& s, const unsigned rt, const unsigned vf) {
    s.cpu.write_vu0_vector(vf, s.cpu.read_gpr(rt));
}
// Word moves. Like the interpreter, a 32-bit result sign-extends into the low doubleword and keeps the high one.
void lw(auto& s, const unsigned rt, const GuestAddress address) {
    s.cpu.write_gpr_sign_extended_32(rt, s.m.read_u32_le(address));
}
void lwc1(auto& s, const unsigned ft, const GuestAddress address) {
    s.cpu.write_fpr_bits(ft, s.m.read_u32_le(address));
}
void mtc1(auto& s, const unsigned fs, const unsigned rt) {
    s.cpu.write_fpr_bits(fs, s.cpu.read_gpr_u32(rt));
}
void mfc1(auto& s, const unsigned rt, const unsigned fs) {
    s.cpu.write_gpr_sign_extended_32(rt, s.cpu.read_fpr_bits(fs));
}
// addiu rt,$sp,offset (the frame), and addiu rt,$zero,value / lui (a constant).
void addiu_sp(auto& s, const unsigned rt, const std::uint32_t offset) {
    s.cpu.write_gpr_sign_extended_32(rt, s.frame + offset);
}
void set_word(auto& s, const unsigned rt, const std::uint32_t value) {
    s.cpu.write_gpr_sign_extended_32(rt, value);
}
// The scalar FPU on raw bit patterns (n3::fpu): MUL.S, ADD.S and DIV.S.
void mul_s(auto& s, const unsigned fd, const unsigned fs, const unsigned ft) {
    s.cpu.write_fpr_bits(fd, fpu::mul(s.cpu.read_fpr_bits(fs), s.cpu.read_fpr_bits(ft)));
}
void add_s(auto& s, const unsigned fd, const unsigned fs, const unsigned ft) {
    s.cpu.write_fpr_bits(fd, fpu::add(s.cpu.read_fpr_bits(fs), s.cpu.read_fpr_bits(ft)));
}
void div_s(auto& s, const unsigned fd, const unsigned fs, const unsigned ft) {
    s.cpu.write_fpr_bits(fd, fpu::div(s.cpu.read_fpr_bits(fs), s.cpu.read_fpr_bits(ft)));
}

// The 8-iteration lb/sb loops (0x1ac320, 0x1ac354, 0x1ac3ec, 0x1ac420). Each iteration loads two bytes from t0
// (lb a1, then lb v1), stores them to a3 (sb a1, then sb v1), and advances both pointers by two. The source and
// destination are 16-byte stack quadwords; the loads and stores keep the listing's order anyway.
void copy_quad_bytes(auto& s, const unsigned source_register, const unsigned destination_register) {
    GuestAddress source = s.cpu.read_gpr_u32(source_register);
    GuestAddress destination = s.cpu.read_gpr_u32(destination_register);
    for (unsigned iteration = 0U; iteration < 8U; ++iteration) {
        const std::uint8_t first = s.m.read_u8(source);
        const std::uint8_t second = s.m.read_u8(source + 1U);
        s.cpu.write_gpr_u64(kA1, static_cast<std::uint64_t>(static_cast<std::int8_t>(first)));
        s.cpu.write_gpr_u64(kV1, static_cast<std::uint64_t>(static_cast<std::int8_t>(second)));
        s.m.write_u8(destination, first);
        s.m.write_u8(destination + 1U, second);
        source += 2U;
        destination += 2U;
    }
    s.cpu.write_gpr_sign_extended_32(source_register, source);
    s.cpu.write_gpr_sign_extended_32(destination_register, destination);
    s.cpu.write_gpr_sign_extended_32(kA2, 0U);
}

// Part one: 0x001ac0a4 .. 0x001ac250.
void part_one(auto& s) {
    lqc2(s, 2U, s.object + 480U);                                    // 001ac0a4 lqc2 vf2,480(a0)
    lq(s, kA1, s.object + 464U);                                     // 001ac0a8 lq a1,464(a0)
    ee_cop2::vadd_vector(s.cpu, 1U, 0U, 2U, kMaskX, true);           // 001ac0ac vsub.x vf1,vf0,vf2
    qmfc2(s, kV1, 1U);                                               // 001ac0b0 qmfc2 v1,vf1
    mtc1(s, 1U, kA1);                                                // 001ac0b4 mtc1 a1,f1
    mtc1(s, 0U, kV1);                                                // 001ac0b8 mtc1 v1,f0
    ee_cop1::max_s(s.cpu, 3U, 1U, 0U);                               // 001ac0c0 max.s f3,f1,f0
    addiu_sp(s, kV1, 672U);                                          // 001ac0c4 addiu v1,sp,672
    ee_cop2::vadd_vector(s.cpu, 1U, 0U, 2U, kMaskY, true);           // 001ac0c8 vsub.y vf1,vf0,vf2
    sq(s, kA1, s.cpu.read_gpr_u32(kV1));                             // 001ac0cc sq a1,0(v1)
    addiu_sp(s, kV1, 656U);                                          // 001ac0d0 addiu v1,sp,656
    lwc1(s, 1U, s.frame + 676U);                                     // 001ac0d4 lwc1 f1,676(sp)
    sqc2(s, 1U, s.cpu.read_gpr_u32(kV1));                            // 001ac0d8 sqc2 vf1,0(v1)
    lwc1(s, 0U, s.frame + 660U);                                     // 001ac0dc lwc1 f0,660(sp)
    ee_cop1::max_s(s.cpu, 2U, 1U, 0U);                               // 001ac0e0 max.s f2,f1,f0
    addiu_sp(s, kV1, 640U);                                          // 001ac0e4 addiu v1,sp,640
    sq(s, kA1, s.cpu.read_gpr_u32(kV1));                             // 001ac0e8 sq a1,0(v1)
    ee_cop2::vadd_vector(s.cpu, 1U, 0U, 2U, kMaskZ, true);           // 001ac0ec vsub.z vf1,vf0,vf2
    addiu_sp(s, kV1, 624U);                                          // 001ac0f0 addiu v1,sp,624
    lwc1(s, 1U, s.frame + 648U);                                     // 001ac0f4 lwc1 f1,648(sp)
    sqc2(s, 1U, s.cpu.read_gpr_u32(kV1));                            // 001ac0f8 sqc2 vf1,0(v1)
    lwc1(s, 0U, s.frame + 632U);                                     // 001ac0fc lwc1 f0,632(sp)
    ee_cop1::max_s(s.cpu, 0U, 1U, 0U);                               // 001ac100 max.s f0,f1,f0
    s.cpu.write_gpr_u64(kT2, 0U);                                    // 001ac104 daddu t2,zero,zero
    mul_s(s, 1U, 0U, 0U);                                            // 001ac108 mul.s f1,f0,f0
    set_word(s, kT1, 0x3f000000U);                                   // 001ac10c lui t1,0x3f00
    set_word(s, kT0, 0x3f800000U);                                   // 001ac110 lui t0,0x3f80
    lqc2(s, 2U, s.frame + 912U);                                     // 001ac114 lqc2 vf2,912(sp)
    addiu_sp(s, kA3, 416U);                                          // 001ac118 addiu a3,sp,416
    addiu_sp(s, kA2, 400U);                                          // 001ac11c addiu a2,sp,400
    addiu_sp(s, kA1, 384U);                                          // 001ac120 addiu a1,sp,384
    addiu_sp(s, kV1, 368U);                                          // 001ac124 addiu v1,sp,368
    mul_s(s, 4U, 2U, 2U);                                            // 001ac128 mul.s f4,f2,f2
    lqc2(s, 1U, s.object + 16U);                                     // 001ac12c lqc2 vf1,16(a0)
    qmtc2(s, kT2, 3U);                                               // 001ac130 qmtc2 t2,vf3
    mul_s(s, 0U, 3U, 3U);                                            // 001ac134 mul.s f0,f3,f3
    add_s(s, 6U, 4U, 1U);                                            // 001ac138 add.s f6,f4,f1
    add_s(s, 5U, 0U, 4U);                                            // 001ac13c add.s f5,f0,f4
    lwc1(s, 4U, s.object + 496U);                                    // 001ac140 lwc1 f4,496(a0)
    add_s(s, 1U, 0U, 1U);                                            // 001ac144 add.s f1,f0,f1
    mtc1(s, 3U, kT1);                                                // 001ac148 mtc1 t1,f3
    mul_s(s, 0U, 4U, 6U);                                            // 001ac14c mul.s f0,f4,f6
    mul_s(s, 1U, 4U, 1U);                                            // 001ac150 mul.s f1,f4,f1
    mul_s(s, 0U, 3U, 0U);                                            // 001ac154 mul.s f0,f3,f0
    mtc1(s, 2U, kT0);                                                // 001ac158 mtc1 t0,f2
    mul_s(s, 1U, 3U, 1U);                                            // 001ac160 mul.s f1,f3,f1
    mfc1(s, kT0, 0U);                                                // 001ac164 mfc1 t0,f0
    qmtc2(s, kT0, 4U);                                               // 001ac16c qmtc2 t0,vf4
    mul_s(s, 0U, 4U, 5U);                                            // 001ac170 mul.s f0,f4,f5
    ee_cop2::vadd_broadcast(s.cpu, 2U, 0U, 4U, kMaskX, 0U, false);  // 001ac174 vaddx.x vf2,vf0,vf4x
    mul_s(s, 0U, 3U, 0U);                                            // 001ac178 mul.s f0,f3,f0
    qmfc2(s, kT0, 2U);                                               // 001ac17c qmfc2 t0,vf2
    qmtc2(s, kT0, 5U);                                               // 001ac180 qmtc2 t0,vf5
    mtc1(s, 3U, kT0);                                                // 001ac184 mtc1 t0,f3
    mfc1(s, kT0, 1U);                                                // 001ac188 mfc1 t0,f1
    div_s(s, 1U, 2U, 3U);                                            // 001ac18c div.s f1,f2,f3
    qmtc2(s, kT0, 2U);                                               // 001ac190 qmtc2 t0,vf2
    mfc1(s, kT1, 1U);                                                // 001ac194 mfc1 t1,f1
    mfc1(s, kT0, 0U);                                                // 001ac198 mfc1 t0,f0
    qmtc2(s, kT1, 4U);                                               // 001ac19c qmtc2 t1,vf4
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 4U, kMaskX, 0U, false);  // 001ac1a0 vaddx.x vf5,vf0,vf4x
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 2U, kMaskY, 0U, false);  // 001ac1a4 vaddx.y vf5,vf0,vf2x
    sqc2(s, 5U, s.cpu.read_gpr_u32(kA3));                            // 001ac1a8 sqc2 vf5,0(a3)
    lwc1(s, 0U, s.frame + 420U);                                     // 001ac1ac lwc1 f0,420(sp)
    sqc2(s, 5U, s.frame + 912U);                                     // 001ac1b0 sqc2 vf5,912(sp)
    qmtc2(s, kT0, 2U);                                               // 001ac1b4 qmtc2 t0,vf2
    div_s(s, 0U, 2U, 0U);                                            // 001ac1b8 div.s f0,f2,f0
    mfc1(s, kA3, 0U);                                                // 001ac1bc mfc1 a3,f0
    qmtc2(s, kA3, 4U);                                               // 001ac1c4 qmtc2 a3,vf4
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 4U, kMaskY, 0U, false);  // 001ac1c8 vaddx.y vf5,vf0,vf4x
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 2U, kMaskZ, 0U, false);  // 001ac1cc vaddx.z vf5,vf0,vf2x
    sqc2(s, 5U, s.cpu.read_gpr_u32(kA2));                            // 001ac1d0 sqc2 vf5,0(a2)
    lwc1(s, 0U, s.frame + 408U);                                     // 001ac1d4 lwc1 f0,408(sp)
    div_s(s, 0U, 2U, 0U);                                            // 001ac1d8 div.s f0,f2,f0
    mfc1(s, kA2, 0U);                                                // 001ac1dc mfc1 a2,f0
    qmtc2(s, kA2, 2U);                                               // 001ac1e4 qmtc2 a2,vf2
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 2U, kMaskZ, 0U, false);  // 001ac1e8 vaddx.z vf5,vf0,vf2x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 5U, kMaskX, 0U, false);  // 001ac1ec vaddx.x vf1,vf0,vf5x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskY, 0U, false);  // 001ac1f0 vaddx.y vf1,vf0,vf3x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskZ, 0U, false);  // 001ac1f4 vaddx.z vf1,vf0,vf3x
    sqc2(s, 5U, s.cpu.read_gpr_u32(kA1));                            // 001ac1f8 sqc2 vf5,0(a1)
    sqc2(s, 5U, s.cpu.read_gpr_u32(kV1));                            // 001ac1fc sqc2 vf5,0(v1)
    lw(s, kV1, s.frame + 392U);                                      // 001ac200 lw v1,392(sp)
    sqc2(s, 1U, s.object + 16U);                                     // 001ac204 sqc2 vf1,16(a0)
    lw(s, kA1, s.frame + 372U);                                      // 001ac208 lw a1,372(sp)
    lqc2(s, 1U, s.object + 32U);                                     // 001ac210 lqc2 vf1,32(a0)
    qmtc2(s, kA1, 2U);                                               // 001ac214 qmtc2 a1,vf2
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskX, 0U, false);  // 001ac218 vaddx.x vf1,vf0,vf3x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 2U, kMaskY, 0U, false);  // 001ac21c vaddx.y vf1,vf0,vf2x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskZ, 0U, false);  // 001ac220 vaddx.z vf1,vf0,vf3x
    sqc2(s, 1U, s.object + 32U);                                     // 001ac224 sqc2 vf1,32(a0)
    addiu_sp(s, kT2, 592U);                                          // 001ac22c addiu t2,sp,592
    addiu_sp(s, kT1, 608U);                                          // 001ac230 addiu t1,sp,608
    addiu_sp(s, kT0, 544U);                                          // 001ac234 addiu t0,sp,544
    addiu_sp(s, kA3, 560U);                                          // 001ac238 addiu a3,sp,560
    lqc2(s, 1U, s.object + 48U);                                     // 001ac23c lqc2 vf1,48(a0)
    qmtc2(s, kV1, 2U);                                               // 001ac240 qmtc2 v1,vf2
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskX, 0U, false);  // 001ac244 vaddx.x vf1,vf0,vf3x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 3U, kMaskY, 0U, false);  // 001ac248 vaddx.y vf1,vf0,vf3x
    ee_cop2::vadd_broadcast(s.cpu, 1U, 0U, 2U, kMaskZ, 0U, false);  // 001ac24c vaddx.z vf1,vf0,vf2x
    sqc2(s, 1U, s.object + 48U);                                     // 001ac250 sqc2 vf1,48(a0)
}

// Part two: 0x001ac254 .. 0x001ac31c. The rows at O+16..O+48 (stored by part one) and the three quadwords at the
// pointer O+516 are loaded, the latter transposed through the frame. The first product is stored to O+64 and to
// frame+80 and +480; the other two go to frame+96 and frame+112.
void part_two(auto& s) {
    lw(s, kT3, s.object + 516U);                                     // 001ac254 lw t3,516(a0)
    lq(s, kA2, s.object + 16U);                                      // 001ac258 lq a2,16(a0)
    lq(s, kA1, s.object + 32U);                                      // 001ac25c lq a1,32(a0)
    lq(s, kV1, s.object + 48U);                                      // 001ac260 lq v1,48(a0)
    lq(s, kT5, s.cpu.read_gpr_u32(kT3));                             // 001ac264 lq t5,0(t3)
    lq(s, kT4, s.cpu.read_gpr_u32(kT3) + 16U);                       // 001ac268 lq t4,16(t3)
    qmtc2(s, kT5, 5U);                                               // 001ac26c qmtc2 t5,vf5
    sq(s, kT4, s.frame + 592U);                                      // 001ac270 sq t4,592(sp)
    ee_cop2::vmove(s.cpu, 1U, 5U, n3::kMaskXyzw);                    // 001ac274 vmove.xyzw vf1,vf5
    lq(s, kT3, s.cpu.read_gpr_u32(kT3) + 32U);                       // 001ac278 lq t3,32(t3)
    sq(s, kT5, s.frame + 576U);                                      // 001ac27c sq t5,576(sp)
    lqc2(s, 4U, s.cpu.read_gpr_u32(kT2));                            // 001ac280 lqc2 vf4,0(t2)
    sq(s, kT3, s.frame + 608U);                                      // 001ac284 sq t3,608(sp)
    lqc2(s, 3U, s.cpu.read_gpr_u32(kT1));                            // 001ac288 lqc2 vf3,0(t1)
    ee_cop2::vmove(s.cpu, 2U, 4U, n3::kMaskXyzw);                    // 001ac28c vmove.xyzw vf2,vf4
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 4U, kMaskY, 0U, false);  // 001ac290 vaddx.y vf5,vf0,vf4x
    ee_cop2::vadd_broadcast(s.cpu, 5U, 0U, 3U, kMaskZ, 0U, false);  // 001ac294 vaddx.z vf5,vf0,vf3x
    ee_cop2::vadd_broadcast(s.cpu, 2U, 0U, 1U, kMaskX, 1U, false);  // 001ac298 vaddy.x vf2,vf0,vf1y
    ee_cop2::vadd_broadcast(s.cpu, 2U, 0U, 3U, kMaskZ, 1U, false);  // 001ac29c vaddy.z vf2,vf0,vf3y
    ee_cop2::vadd_broadcast(s.cpu, 3U, 0U, 1U, kMaskX, 2U, false);  // 001ac2a0 vaddz.x vf3,vf0,vf1z
    ee_cop2::vadd_broadcast(s.cpu, 3U, 0U, 4U, kMaskY, 2U, false);  // 001ac2a4 vaddz.y vf3,vf0,vf4z
    sqc2(s, 5U, s.frame + 528U);                                     // 001ac2a8 sqc2 vf5,528(sp)
    qmfc2(s, kT3, 5U);                                               // 001ac2ac qmfc2 t3,vf5
    sqc2(s, 2U, s.cpu.read_gpr_u32(kT0));                            // 001ac2b0 sqc2 vf2,0(t0)
    sqc2(s, 3U, s.cpu.read_gpr_u32(kA3));                            // 001ac2b4 sqc2 vf3,0(a3)
    qmtc2(s, kA2, 4U);                                               // 001ac2b8 qmtc2 a2,vf4
    qmtc2(s, kT3, 1U);                                               // 001ac2bc qmtc2 t3,vf1
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac2c0 vmulax.xyz acc,vf4,vf1x
    qmtc2(s, kA1, 3U);                                               // 001ac2c4 qmtc2 a1,vf3
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac2c8 vmadday.xyz acc,vf3,vf1y
    qmtc2(s, kV1, 2U);                                               // 001ac2cc qmtc2 v1,vf2
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac2d0 vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kA1, 1U);                                               // 001ac2d4 qmfc2 a1,vf1
    lqc2(s, 1U, s.frame + 544U);                                     // 001ac2d8 lqc2 vf1,544(sp)
    sq(s, kA1, s.frame + 80U);                                       // 001ac2dc sq a1,80(sp)
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac2e0 vmulax.xyz acc,vf4,vf1x
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac2e4 vmadday.xyz acc,vf3,vf1y
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac2e8 vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kV1, 1U);                                               // 001ac2ec qmfc2 v1,vf1
    lqc2(s, 1U, s.frame + 560U);                                     // 001ac2f0 lqc2 vf1,560(sp)
    sq(s, kV1, s.frame + 96U);                                       // 001ac2f4 sq v1,96(sp)
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac2f8 vmulax.xyz acc,vf4,vf1x
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac2fc vmadday.xyz acc,vf3,vf1y
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac300 vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kV1, 1U);                                               // 001ac304 qmfc2 v1,vf1
    sq(s, kV1, s.frame + 112U);                                      // 001ac308 sq v1,112(sp)
    addiu_sp(s, kT0, 96U);                                           // 001ac30c addiu t0,sp,96
    sq(s, kA1, s.frame + 480U);                                      // 001ac310 sq a1,480(sp)
    addiu_sp(s, kA3, 496U);                                          // 001ac314 addiu a3,sp,496
    sq(s, kA1, s.object + 64U);                                      // 001ac318 sq a1,64(a0)
    set_word(s, kA2, 8U);                                            // 001ac31c addiu a2,zero,8
}

// Part three: 0x001ac320 .. 0x001ac3e8. Byte loop one copies frame+96 to frame+496, which is stored to O+80. Byte
// loop two copies frame+112 to frame+512, which is stored to O+96. A second set of three products is then formed
// from the object quadwords at O+64, O+80 and O+96, stored to frame+32, +48, +64, and the first also to O+64.
void part_three(auto& s) {
    copy_quad_bytes(s, kT0, kA3);                                    // 001ac320 .. 001ac338 byte loop (t0 -> a3)
    lq(s, kV1, s.frame + 496U);                                      // 001ac340 lq v1,496(sp)
    addiu_sp(s, kT0, 112U);                                          // 001ac344 addiu t0,sp,112
    addiu_sp(s, kA3, 512U);                                          // 001ac348 addiu a3,sp,512
    set_word(s, kA2, 8U);                                            // 001ac34c addiu a2,zero,8
    sq(s, kV1, s.object + 80U);                                      // 001ac350 sq v1,80(a0)
    copy_quad_bytes(s, kT0, kA3);                                    // 001ac354 .. 001ac370 byte loop (t0 -> a3)
    lq(s, kA2, s.frame + 512U);                                      // 001ac374 lq a2,512(sp)
    lq(s, kA1, s.cpu.read_gpr_u32(kT2));                             // 001ac378 lq a1,0(t2)
    lq(s, kV1, s.cpu.read_gpr_u32(kT1));                             // 001ac37c lq v1,0(t1)
    lqc2(s, 4U, s.frame + 576U);                                     // 001ac380 lqc2 vf4,576(sp)
    sq(s, kA2, s.object + 96U);                                      // 001ac384 sq a2,96(a0)
    lqc2(s, 1U, s.object + 64U);                                     // 001ac388 lqc2 vf1,64(a0)
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac38c vmulax.xyz acc,vf4,vf1x
    qmtc2(s, kA1, 3U);                                               // 001ac390 qmtc2 a1,vf3
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac394 vmadday.xyz acc,vf3,vf1y
    qmtc2(s, kV1, 2U);                                               // 001ac398 qmtc2 v1,vf2
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac39c vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kA1, 1U);                                               // 001ac3a0 qmfc2 a1,vf1
    lqc2(s, 1U, s.object + 80U);                                     // 001ac3a4 lqc2 vf1,80(a0)
    sq(s, kA1, s.frame + 32U);                                       // 001ac3a8 sq a1,32(sp)
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac3ac vmulax.xyz acc,vf4,vf1x
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac3b0 vmadday.xyz acc,vf3,vf1y
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac3b4 vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kV1, 1U);                                               // 001ac3b8 qmfc2 v1,vf1
    lqc2(s, 1U, s.object + 96U);                                     // 001ac3bc lqc2 vf1,96(a0)
    sq(s, kV1, s.frame + 48U);                                       // 001ac3c0 sq v1,48(sp)
    ee_cop2::vmula_broadcast(s.cpu, 4U, 1U, n3::kMaskXyz, false);   // 001ac3c4 vmulax.xyz acc,vf4,vf1x
    ee_cop2::vmadd_broadcast(s.cpu, 0U, 3U, 1U, n3::kMaskXyz, 1U, false, true, false);  // 001ac3c8 vmadday.xyz acc,vf3,vf1y
    ee_cop2::vmadd_broadcast(s.cpu, 1U, 2U, 1U, n3::kMaskXyz, 2U, false, false, false); // 001ac3cc vmaddz.xyz vf1,vf2,vf1z
    qmfc2(s, kV1, 1U);                                               // 001ac3d0 qmfc2 v1,vf1
    sq(s, kV1, s.frame + 64U);                                       // 001ac3d4 sq v1,64(sp)
    addiu_sp(s, kT0, 48U);                                           // 001ac3d8 addiu t0,sp,48
    sq(s, kA1, s.frame + 432U);                                      // 001ac3dc sq a1,432(sp)
    addiu_sp(s, kA3, 448U);                                          // 001ac3e0 addiu a3,sp,448
    sq(s, kA1, s.object + 64U);                                      // 001ac3e4 sq a1,64(a0)
    set_word(s, kA2, 8U);                                            // 001ac3e8 addiu a2,zero,8
}

// Part four: 0x001ac3ec .. 0x001ac444. Byte loop three copies frame+48 to frame+448, which is stored to O+80. Byte
// loop four copies frame+64 to frame+464, which is stored to O+96. The jr $ra at 0x001ac448 is the return.
void part_four(auto& s) {
    copy_quad_bytes(s, kT0, kA3);                                    // 001ac3ec .. 001ac408 byte loop (t0 -> a3)
    lq(s, kV1, s.frame + 448U);                                      // 001ac40c lq v1,448(sp)
    addiu_sp(s, kT0, 64U);                                           // 001ac410 addiu t0,sp,64
    addiu_sp(s, kA3, 464U);                                          // 001ac414 addiu a3,sp,464
    set_word(s, kA2, 8U);                                            // 001ac418 addiu a2,zero,8
    sq(s, kV1, s.object + 80U);                                      // 001ac41c sq v1,80(a0)
    copy_quad_bytes(s, kT0, kA3);                                    // 001ac420 .. 001ac438 byte loop (t0 -> a3)
    lq(s, kV1, s.frame + 464U);                                      // 001ac440 lq v1,464(sp)
    sq(s, kV1, s.object + 96U);                                      // 001ac444 sq v1,96(a0)
}

template <class Context>
n3::Run orthonormalize_inertia_frames(Context& context) {
    auto call = n3::guest_call(context);
    Ctx s{call.cpu(), call.memory(), call.arg(0), call.stack_pointer() - kFrameBytes};
    part_one(s);
    part_two(s);
    part_three(s);
    part_four(s);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_001ac0a0{0x001ac0a0U, 944U, 0x44e0a80270903cc4ULL,
                                                  "OrthonormalizeInertiaFrames", kCost,
                                                  n3::live<&orthonormalize_inertia_frames<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001ac0a0, orthonormalize_inertia_frames)
