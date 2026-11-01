// 0x00349980 (a0 = output, a1 = world matrix rows, a2 = view window, a3 = view offset, f12 = near, f13 = far):
// BuildPackedViewFrustum. The matrix at a1 holds the right, up, at and position rows (quadwords at +0, +16, +32,
// +48). The view window (two floats at a2) and offset (two floats at a3, the first negated) scale and shift the
// right/up rows into four corner rays; the four corner points at the near and far distance are built in the
// function's own frame (976..1104), and the result at a0 is two plane quadwords (far at +0, near at +16, each the
// normalised vector with its distance in w) then four quadwords at +32..+80 holding the side planes' normals and
// distances one plane per lane (x, y, z lanes of the normals in rows 0..2 via the vaddx/y/z writes, distances in w
// of each row). Every normalisation is the listing's own VU0 sequence (vopmula/vopmsub cross products, dot product
// through vadday/vmaddz, VRSQRT, vmulq), executed on the real VU0 state (VF1..VF8, ACC, Q, MAC and STATUS).
//
// The 128-bit scratch GPRs are tracked in full because qmtc2 copies a whole GPR into a vector register (the upper
// doublewords survive lw, mfc1, lui, addiu, sll and slti, which write the low doubleword only). Frame words are
// written to guest memory too (the check ignores them); stale frame words (960(sp)) are read from guest memory.
// The path is straight code: the loop runs a fixed four times. 52 + 4 * 14 + 216 = 324 instructions.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 1104U;     // addiu $sp, $sp, -1104
constexpr std::uint32_t kPrologueCost = 52U;     // 0x349980..0x349a4c
constexpr std::uint32_t kLoopPassCost = 14U;     // 0x349a50..0x349a84, run four times
constexpr std::uint32_t kLoopPasses = 4U;
constexpr std::uint32_t kTailCost = 216U;        // 0x349a88..0x349de4
constexpr std::uint32_t kTotalCost = kPrologueCost + kLoopPasses * kLoopPassCost + kTailCost;
constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint8_t kX = 8U;      // destination masks: the x lane alone, and so on
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kW = 1U;
constexpr std::uint8_t kXyz = 14U;

// A 32-bit write to a GPR (lw, mfc1, lui, addiu, sll, slti): the low doubleword is the sign-extended value, the
// upper doubleword stays as it was.
void set32(Gpr128& reg, const std::uint32_t value) {
    reg.words[0] = value;
    reg.words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

// The interpreter's faults: lqc2/sqc2 need a 16-byte aligned address inside guest memory, lw/lwc1 a 4-byte one.
template <class Memory>
bool quads_ok(const Memory& m, const GuestAddress address, const std::uint32_t bytes) {
    return (address & 15U) == 0U && m.contains(address, bytes);
}

template <class Memory>
bool words_ok(const Memory& m, const GuestAddress address, const std::uint32_t bytes) {
    return (address & 3U) == 0U && m.contains(address, bytes);
}

template <class Context>
n3::Run build_packed_view_frustum(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const GuestAddress out = call.arg(0);
    const GuestAddress matrix = call.arg(1);
    const GuestAddress window = call.arg(2);
    const GuestAddress offset = call.arg(3);
    const GuestAddress sp = call.stack_pointer() - kFrameBytes;
    if (!quads_ok(m, out, 96U) || !quads_ok(m, matrix, 64U) || !quads_ok(m, sp, kFrameBytes) ||
        !words_ok(m, window, 8U) || !words_ok(m, offset, 8U)) {
        return call.decline();
    }

    Gpr128 v1 = cpu.read_gpr(3U);
    Gpr128 a0 = call.arg128(0U);
    Gpr128 a1 = call.arg128(1U);
    Gpr128 a2 = call.arg128(2U);
    Gpr128 a3 = call.arg128(3U);
    Gpr128 t0 = call.arg128(4U);
    Gpr128 t1 = call.arg128(5U);
    Gpr128 t2 = call.arg128(6U);
    Gpr128 t3 = call.arg128(7U);
    (void)a0;

    const auto lqc2 = [&](const unsigned vf, const GuestAddress address) {
        cpu.write_vu0_vector(vf, n3::load_quad_any(m, address));
    };
    const auto sqc2 = [&](const unsigned vf, const GuestAddress address) {
        n3::store_quad_any(m, address, cpu.read_vu0_vector(vf));
    };
    const auto qmtc2 = [&](const unsigned vf, const Gpr128& reg) { cpu.write_vu0_vector(vf, reg); };
    const auto qmfc2 = [&](Gpr128& reg, const unsigned vf) { reg = cpu.read_vu0_vector(vf); };
    // Instruction families by name: vector (xyz) add/sub, broadcast forms, and the normalisation's pieces.
    const auto vadd_xyz = [&](const unsigned fd, const unsigned fs, const unsigned ft) {
        ee_cop2::vadd_vector(cpu, fd, fs, ft, kXyz, false);
    };
    const auto vsub_xyz = [&](const unsigned fd, const unsigned fs, const unsigned ft) {
        ee_cop2::vadd_vector(cpu, fd, fs, ft, kXyz, true);
    };
    const auto vmul_xyz = [&](const unsigned fd, const unsigned fs, const unsigned ft) {
        ee_cop2::vmul(cpu, fd, fs, ft, kXyz);
    };
    const auto vmulb = [&](const unsigned fd, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                           const std::size_t lane) { ee_cop2::vmul_broadcast(cpu, fd, fs, ft, mask, lane); };
    const auto vaddb = [&](const unsigned fd, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                           const std::size_t lane) { ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, false); };
    // vaddw.x fd,vf0,vf0w: the constant 1.0 into the x lane.
    const auto vone_x = [&](const unsigned fd) { ee_cop2::vadd_broadcast(cpu, fd, 0U, 0U, kX, 3U, false); };
    // vadday.x acc,fs,fsy; vmaddz.x fd,fd,fsz: the x lane becomes the dot-product tail.
    const auto vadday_x = [&](const unsigned fs) { ee_cop2::vadda_broadcast(cpu, fs, fs, kX, 1U); };
    const auto vmaddz_x = [&](const unsigned fd, const unsigned fs) {
        ee_cop2::vmadd_broadcast(cpu, fd, fd, fs, kX, 2U, false, false, false);
    };
    const auto vrsqrt = [&](const unsigned ft) { ee_cop2::vdiv(cpu, 0U, 3U, ft, 0U, true); };  // vwaitq: no-op

    // --- 0x349980..0x349a4c: the scaled rays and the four corner points ---------------------------------------
    lqc2(3U, matrix);                                                  // lqc2 vf3,0(a1)
    std::uint32_t f0 = m.read_u32_le(offset);                          // lwc1 f0,0(a3)
    f0 = fpu::neg(f0);                                                 // neg.s f0,f0
    set32(a3, m.read_u32_le(offset + 4U));                             // lw a3,4(a3)
    set32(v1, f0);                                                     // mfc1 v1,f0
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vmulb(4U, 3U, 1U, kXyz, 0U);                                       // vmulx.xyz vf4,vf3,vf1x
    set32(v1, m.read_u32_le(window));                                  // lw v1,0(a2)
    lqc2(2U, matrix + 16U);                                            // lqc2 vf2,16(a1)
    qmtc2(1U, a3);                                                     // qmtc2 a3,vf1
    vmulb(1U, 2U, 1U, kXyz, 0U);                                       // vmulx.xyz vf1,vf2,vf1x
    vadd_xyz(4U, 4U, 1U);                                              // vadd.xyz vf4,vf4,vf1
    qmfc2(t2, 4U);                                                     // qmfc2 t2,vf4
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    set32(t1, m.read_u32_le(window + 4U));                             // lw t1,4(a2)
    vmulb(3U, 3U, 1U, kXyz, 0U);                                       // vmulx.xyz vf3,vf3,vf1x
    v1 = n3::load_quad_any(m, matrix + 48U);                           // lq v1,48(a1)
    set32(t0, sp + 992U);                                              // addiu t0,sp,992
    set32(a3, sp + 1024U);                                             // addiu a3,sp,1024
    t3.words[0] = 0U;                                                  // daddu t3,zero,zero
    t3.words[1] = 0U;
    qmtc2(1U, t1);                                                     // qmtc2 t1,vf1
    set32(a2, 0x40000000U);                                            // lui a2,0x4000
    vmulb(2U, 2U, 1U, kXyz, 0U);                                       // vmulx.xyz vf2,vf2,vf1x
    n3::store_quad_any(m, sp + 48U, v1);                               // sq v1,48(sp)
    n3::store_quad_any(m, sp + 32U, v1);                               // sq v1,32(sp)
    lqc2(1U, matrix + 32U);                                            // lqc2 vf1,32(a1)
    qmtc2(4U, a2);                                                     // qmtc2 a2,vf4
    vadd_xyz(1U, 1U, 3U);                                              // vadd.xyz vf1,vf1,vf3
    vadd_xyz(1U, 1U, 2U);                                              // vadd.xyz vf1,vf1,vf2
    vmulb(3U, 3U, 4U, kXyz, 0U);                                       // vmulx.xyz vf3,vf3,vf4x
    sqc2(1U, sp + 976U);                                               // sqc2 vf1,976(sp)
    vsub_xyz(1U, 1U, 3U);                                              // vsub.xyz vf1,vf1,vf3
    vmulb(2U, 2U, 4U, kXyz, 0U);                                       // vmulx.xyz vf2,vf2,vf4x
    sqc2(1U, t0.words[0]);                                             // sqc2 vf1,0(t0)
    vsub_xyz(1U, 1U, 2U);                                              // vsub.xyz vf1,vf1,vf2
    sqc2(1U, sp + 1008U);                                              // sqc2 vf1,1008(sp)
    vadd_xyz(1U, 1U, 3U);                                              // vadd.xyz vf1,vf1,vf3
    sqc2(1U, a3.words[0]);                                             // sqc2 vf1,0(a3)
    lqc2(2U, sp + 48U);                                                // lqc2 vf2,48(sp)
    set32(a2, cpu.read_fpr_bits(12U));                                 // mfc1 a2,f12
    set32(v1, cpu.read_fpr_bits(13U));                                 // mfc1 v1,f13
    qmtc2(6U, t2);                                                     // qmtc2 t2,vf6
    lqc2(1U, sp + 32U);                                                // lqc2 vf1,32(sp)
    qmtc2(5U, a2);                                                     // qmtc2 a2,vf5
    qmtc2(3U, v1);                                                     // qmtc2 v1,vf3
    vadd_xyz(4U, 6U, 2U);                                              // vadd.xyz vf4,vf6,vf2
    vadd_xyz(1U, 6U, 1U);                                              // vadd.xyz vf1,vf6,vf1

    // --- 0x349a50..0x349a84: four passes, each one corner of the near face and the matching far corner -----------
    // (t3 counts 0..3; the loop is not data dependent, so the count is fixed: kLoopPasses * kLoopPassCost.)
    for (std::uint32_t pass = 0U; pass < kLoopPasses; ++pass) {
        set32(v1, t3.words[0] << 4U);                                  // sll v1,t3,4
        set32(v1, v1.words[0] + sp);                                   // addu v1,v1,sp
        set32(t3, t3.words[0] + 1U);                                   // addiu t3,t3,1
        set32(a2, v1.words[0] + 976U);                                 // addiu a2,v1,976
        lqc2(2U, a2.words[0]);                                         // lqc2 vf2,0(a2)
        set32(v1, t3.words[0] < 4U ? 1U : 0U);                         // slti v1,t3,4
        vsub_xyz(7U, 2U, 6U);                                          // vsub.xyz vf7,vf2,vf6
        vmulb(2U, 7U, 5U, kXyz, 0U);                                   // vmulx.xyz vf2,vf7,vf5x
        vadd_xyz(2U, 4U, 2U);                                          // vadd.xyz vf2,vf4,vf2
        sqc2(2U, a2.words[0]);                                         // sqc2 vf2,0(a2)
        vmulb(2U, 7U, 3U, kXyz, 0U);                                   // vmulx.xyz vf2,vf7,vf3x
        vadd_xyz(2U, 1U, 2U);                                          // vadd.xyz vf2,vf1,vf2
        sqc2(2U, a2.words[0] + 64U);                                   // sqc2 vf2,64(a2) (delay slot)
    }

    // --- 0x349a88..0x349b54: the far plane (normal from the position row, distance) -------------------------------
    vone_x(1U);                                                        // vaddw.x vf1,vf0,vf0w
    set32(v1, sp + 1040U);                                             // addiu v1,sp,1040
    qmfc2(a2, 1U);                                                     // qmfc2 a2,vf1
    lqc2(2U, matrix + 32U);                                            // lqc2 vf2,32(a1)
    lqc2(3U, sp + 960U);                                               // lqc2 vf3,960(sp) (stale frame word in w)
    lqc2(1U, v1.words[0]);                                             // lqc2 vf1,0(v1)
    vadd_xyz(3U, 0U, 2U);                                              // vadd.xyz vf3,vf0,vf2
    qmfc2(t1, 3U);                                                     // qmfc2 t1,vf3
    vmul_xyz(2U, 1U, 2U);                                              // vmul.xyz vf2,vf1,vf2
    vadday_x(2U);                                                      // vadday.x acc,vf2,vf2y
    qmtc2(1U, a2);                                                     // qmtc2 a2,vf1
    vmaddz_x(1U, 2U);                                                  // vmaddz.x vf1,vf1,vf2z
    qmfc2(a2, 1U);                                                     // qmfc2 a2,vf1
    lqc2(2U, matrix + 32U);                                            // lqc2 vf2,32(a1)
    qmtc2(4U, t1);                                                     // qmtc2 t1,vf4
    qmtc2(1U, a2);                                                     // qmtc2 a2,vf1
    vmulb(4U, 0U, 1U, kW, 0U);                                         // vmulx.w vf4,vf0,vf1x
    vsub_xyz(3U, 0U, 2U);                                              // vsub.xyz vf3,vf0,vf2
    sqc2(4U, out);                                                     // sqc2 vf4,0(a0)
    vadd_xyz(4U, 0U, 3U);                                              // vadd.xyz vf4,vf0,vf3
    lqc2(1U, sp + 976U);                                               // lqc2 vf1,976(sp)
    vsub_xyz(2U, 0U, 2U);                                              // vsub.xyz vf2,vf0,vf2
    vmul_xyz(2U, 1U, 2U);                                              // vmul.xyz vf2,vf1,vf2
    vone_x(1U);                                                        // vaddw.x vf1,vf0,vf0w
    qmfc2(a2, 4U);                                                     // qmfc2 a2,vf4
    qmfc2(a1, 1U);                                                     // qmfc2 a1,vf1
    vadday_x(2U);                                                      // vadday.x acc,vf2,vf2y
    qmtc2(1U, a1);                                                     // qmtc2 a1,vf1
    vmaddz_x(1U, 2U);                                                  // vmaddz.x vf1,vf1,vf2z
    qmfc2(a1, 1U);                                                     // qmfc2 a1,vf1
    qmtc2(2U, a2);                                                     // qmtc2 a2,vf2
    qmtc2(1U, a1);                                                     // qmtc2 a1,vf1
    vmulb(2U, 0U, 1U, kW, 0U);                                         // vmulx.w vf2,vf0,vf1x
    lqc2(6U, sp + 1056U);                                              // lqc2 vf6,1056(sp)
    lqc2(3U, t0.words[0]);                                             // lqc2 vf3,0(t0)
    lqc2(1U, sp + 1072U);                                              // lqc2 vf1,1072(sp)
    sqc2(2U, out + 16U);                                               // sqc2 vf2,16(a0)
    qmfc2(a2, 2U);                                                     // qmfc2 a2,vf2
    vsub_xyz(2U, 1U, 6U);                                              // vsub.xyz vf2,vf1,vf6
    vsub_xyz(7U, 3U, 6U);                                              // vsub.xyz vf7,vf3,vf6
    ee_cop2::vopmula(cpu, 7U, 2U);                                     // vopmula.xyz acc,vf7,vf2
    ee_cop2::vopmsub(cpu, 2U, 2U, 7U);                                 // vopmsub.xyz vf2,vf2,vf7
    qmfc2(a1, 2U);                                                     // qmfc2 a1,vf2
    qmtc2(4U, a1);                                                     // qmtc2 a1,vf4
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    vmul_xyz(3U, 4U, 4U);                                              // vmul.xyz vf3,vf4,vf4
    qmfc2(a1, 2U);                                                     // qmfc2 a1,vf2
    vadday_x(3U);                                                      // vadday.x acc,vf3,vf3y
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2
    vmaddz_x(2U, 3U);                                                  // vmaddz.x vf2,vf2,vf3z
    qmfc2(a1, 2U);                                                     // qmfc2 a1,vf2
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2
    vrsqrt(2U);                                                        // vrsqrt q,vf0w,vf2x
    ee_cop2::vmul_control(cpu, 4U, 4U, kXyz, ee_cop2::kControlQ);      // vmulq.xyz vf4,vf4,q
    qmfc2(a1, 4U);                                                     // qmfc2 a1,vf4
    qmtc2(3U, a2);                                                     // qmtc2 a2,vf3
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2
    vadd_xyz(3U, 0U, 2U);                                              // vadd.xyz vf3,vf0,vf2
    lqc2(5U, t0.words[0]);                                             // lqc2 vf5,0(t0)
    qmfc2(a2, 3U);                                                     // qmfc2 a2,vf3
    vmul_xyz(3U, 5U, 2U);                                              // vmul.xyz vf3,vf5,vf2
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    qmfc2(a1, 2U);                                                     // qmfc2 a1,vf2
    vadday_x(3U);                                                      // vadday.x acc,vf3,vf3y
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2
    vmaddz_x(2U, 3U);                                                  // vmaddz.x vf2,vf2,vf3z
    qmfc2(a1, 2U);                                                     // qmfc2 a1,vf2
    qmtc2(8U, a2);                                                     // qmtc2 a2,vf8
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2

    // --- 0x349ba0..0x349bd8: the first side plane's column (x lane of rows 0..3) ------------------------------
    lqc2(3U, out + 32U);                                               // lqc2 vf3,32(a0)
    vmulb(8U, 0U, 2U, kW, 0U);                                         // vmulx.w vf8,vf0,vf2x
    vaddb(3U, 0U, 8U, kX, 0U);                                         // vaddx.x vf3,vf0,vf8x
    sqc2(3U, out + 32U);                                               // sqc2 vf3,32(a0)
    lqc2(3U, out + 48U);                                               // lqc2 vf3,48(a0)
    lqc2(4U, v1.words[0]);                                             // lqc2 vf4,0(v1)
    vaddb(3U, 0U, 8U, kX, 1U);                                         // vaddy.x vf3,vf0,vf8y
    sqc2(3U, out + 48U);                                               // sqc2 vf3,48(a0)
    lqc2(3U, out + 64U);                                               // lqc2 vf3,64(a0)
    vsub_xyz(2U, 4U, 6U);                                              // vsub.xyz vf2,vf4,vf6
    vaddb(3U, 0U, 8U, kX, 2U);                                         // vaddz.x vf3,vf0,vf8z
    sqc2(3U, out + 64U);                                               // sqc2 vf3,64(a0)
    lqc2(3U, out + 80U);                                               // lqc2 vf3,80(a0)
    vaddb(3U, 0U, 8U, kX, 3U);                                         // vaddw.x vf3,vf0,vf8w
    sqc2(3U, out + 80U);                                               // sqc2 vf3,80(a0)

    // --- 0x349bdc..0x349c8c: the second plane, its y lane column ------------------------------------------------
    ee_cop2::vopmula(cpu, 2U, 7U);                                     // vopmula.xyz acc,vf2,vf7
    ee_cop2::vopmsub(cpu, 2U, 7U, 2U);                                 // vopmsub.xyz vf2,vf7,vf2
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    qmtc2(6U, v1);                                                     // qmtc2 v1,vf6
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    vmul_xyz(3U, 6U, 6U);                                              // vmul.xyz vf3,vf6,vf6
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    vadday_x(3U);                                                      // vadday.x acc,vf3,vf3y
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vmaddz_x(2U, 3U);                                                  // vmaddz.x vf2,vf2,vf3z
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vrsqrt(2U);                                                        // vrsqrt q,vf0w,vf2x
    ee_cop2::vmul_control(cpu, 6U, 6U, kXyz, ee_cop2::kControlQ);      // vmulq.xyz vf6,vf6,q
    qmfc2(v1, 6U);                                                     // qmfc2 v1,vf6
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vadd_xyz(8U, 0U, 2U);                                              // vadd.xyz vf8,vf0,vf2
    vmul_xyz(3U, 5U, 2U);                                              // vmul.xyz vf3,vf5,vf2
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    qmfc2(a1, 8U);                                                     // qmfc2 a1,vf8
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    vadday_x(3U);                                                      // vadday.x acc,vf3,vf3y
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vmaddz_x(2U, 3U);                                                  // vmaddz.x vf2,vf2,vf3z
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    qmtc2(7U, a1);                                                     // qmtc2 a1,vf7
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    lqc2(3U, out + 32U);                                               // lqc2 vf3,32(a0)
    vmulb(7U, 0U, 2U, kW, 0U);                                         // vmulx.w vf7,vf0,vf2x
    lqc2(5U, sp + 1088U);                                              // lqc2 vf5,1088(sp)
    vaddb(3U, 0U, 7U, kY, 0U);                                         // vaddx.y vf3,vf0,vf7x
    sqc2(3U, out + 32U);                                               // sqc2 vf3,32(a0)
    vsub_xyz(2U, 4U, 5U);                                              // vsub.xyz vf2,vf4,vf5
    lqc2(4U, out + 48U);                                               // lqc2 vf4,48(a0)
    lqc2(3U, a3.words[0]);                                             // lqc2 vf3,0(a3)
    vaddb(4U, 0U, 7U, kY, 1U);                                         // vaddy.y vf4,vf0,vf7y
    sqc2(4U, out + 48U);                                               // sqc2 vf4,48(a0)
    vsub_xyz(6U, 3U, 5U);                                              // vsub.xyz vf6,vf3,vf5
    lqc2(3U, out + 64U);                                               // lqc2 vf3,64(a0)
    vaddb(3U, 0U, 7U, kY, 2U);                                         // vaddz.y vf3,vf0,vf7z
    sqc2(3U, out + 64U);                                               // sqc2 vf3,64(a0)
    lqc2(3U, out + 80U);                                               // lqc2 vf3,80(a0)
    vaddb(3U, 0U, 7U, kY, 3U);                                         // vaddw.y vf3,vf0,vf7w
    sqc2(3U, out + 80U);                                               // sqc2 vf3,80(a0)

    // --- 0x349c90..0x349d38: the third plane, its z lane column --------------------------------------------------
    ee_cop2::vopmula(cpu, 6U, 2U);                                     // vopmula.xyz acc,vf6,vf2
    ee_cop2::vopmsub(cpu, 2U, 2U, 6U);                                 // vopmsub.xyz vf2,vf2,vf6
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    qmtc2(4U, v1);                                                     // qmtc2 v1,vf4
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    vmul_xyz(3U, 4U, 4U);                                              // vmul.xyz vf3,vf4,vf4
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    vadday_x(3U);                                                      // vadday.x acc,vf3,vf3y
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vmaddz_x(2U, 3U);                                                  // vmaddz.x vf2,vf2,vf3z
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vrsqrt(2U);                                                        // vrsqrt q,vf0w,vf2x
    ee_cop2::vmul_control(cpu, 4U, 4U, kXyz, ee_cop2::kControlQ);      // vmulq.xyz vf4,vf4,q
    qmfc2(v1, 4U);                                                     // qmfc2 v1,vf4
    lqc2(3U, a3.words[0]);                                             // lqc2 vf3,0(a3)
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vadd_xyz(7U, 0U, 2U);                                              // vadd.xyz vf7,vf0,vf2
    vmul_xyz(4U, 3U, 2U);                                              // vmul.xyz vf4,vf3,vf2
    vone_x(2U);                                                        // vaddw.x vf2,vf0,vf0w
    qmfc2(a1, 7U);                                                     // qmfc2 a1,vf7
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    vadday_x(4U);                                                      // vadday.x acc,vf4,vf4y
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vmaddz_x(2U, 4U);                                                  // vmaddz.x vf2,vf2,vf4z
    qmfc2(v1, 2U);                                                     // qmfc2 v1,vf2
    vsub_xyz(1U, 1U, 5U);                                              // vsub.xyz vf1,vf1,vf5
    qmtc2(5U, a1);                                                     // qmtc2 a1,vf5
    qmtc2(2U, v1);                                                     // qmtc2 v1,vf2
    vmulb(5U, 0U, 2U, kW, 0U);                                         // vmulx.w vf5,vf0,vf2x
    lqc2(2U, out + 32U);                                               // lqc2 vf2,32(a0)
    vaddb(2U, 0U, 5U, kZ, 0U);                                         // vaddx.z vf2,vf0,vf5x
    sqc2(2U, out + 32U);                                               // sqc2 vf2,32(a0)
    lqc2(2U, out + 48U);                                               // lqc2 vf2,48(a0)
    vaddb(2U, 0U, 5U, kZ, 1U);                                         // vaddy.z vf2,vf0,vf5y
    sqc2(2U, out + 48U);                                               // sqc2 vf2,48(a0)
    lqc2(2U, out + 64U);                                               // lqc2 vf2,64(a0)
    vaddb(2U, 0U, 5U, kZ, 2U);                                         // vaddz.z vf2,vf0,vf5z
    sqc2(2U, out + 64U);                                               // sqc2 vf2,64(a0)
    lqc2(2U, out + 80U);                                               // lqc2 vf2,80(a0)
    vaddb(2U, 0U, 5U, kZ, 3U);                                         // vaddw.z vf2,vf0,vf5w
    sqc2(2U, out + 80U);                                               // sqc2 vf2,80(a0)

    // --- 0x349d3c..0x349ddc: the fourth plane, its w lane column ---------------------------------------------------
    ee_cop2::vopmula(cpu, 1U, 6U);                                     // vopmula.xyz acc,vf1,vf6
    ee_cop2::vopmsub(cpu, 1U, 6U, 1U);                                 // vopmsub.xyz vf1,vf6,vf1
    qmfc2(v1, 1U);                                                     // qmfc2 v1,vf1
    qmtc2(4U, v1);                                                     // qmtc2 v1,vf4
    vone_x(1U);                                                        // vaddw.x vf1,vf0,vf0w
    vmul_xyz(2U, 4U, 4U);                                              // vmul.xyz vf2,vf4,vf4
    qmfc2(v1, 1U);                                                     // qmfc2 v1,vf1
    vadday_x(2U);                                                      // vadday.x acc,vf2,vf2y
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vmaddz_x(1U, 2U);                                                  // vmaddz.x vf1,vf1,vf2z
    qmfc2(v1, 1U);                                                     // qmfc2 v1,vf1
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vrsqrt(1U);                                                        // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 4U, 4U, kXyz, ee_cop2::kControlQ);      // vmulq.xyz vf4,vf4,q
    qmfc2(v1, 4U);                                                     // qmfc2 v1,vf4
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vadd_xyz(5U, 0U, 1U);                                              // vadd.xyz vf5,vf0,vf1
    vmul_xyz(2U, 3U, 1U);                                              // vmul.xyz vf2,vf3,vf1
    vone_x(1U);                                                        // vaddw.x vf1,vf0,vf0w
    qmfc2(a1, 5U);                                                     // qmfc2 a1,vf5
    qmfc2(v1, 1U);                                                     // qmfc2 v1,vf1
    vadday_x(2U);                                                      // vadday.x acc,vf2,vf2y
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vmaddz_x(1U, 2U);                                                  // vmaddz.x vf1,vf1,vf2z
    qmfc2(v1, 1U);                                                     // qmfc2 v1,vf1
    qmtc2(2U, a1);                                                     // qmtc2 a1,vf2
    qmtc2(1U, v1);                                                     // qmtc2 v1,vf1
    vmulb(2U, 0U, 1U, kW, 0U);                                         // vmulx.w vf2,vf0,vf1x
    lqc2(1U, out + 32U);                                               // lqc2 vf1,32(a0)
    vmulb(1U, 0U, 2U, kW, 0U);                                         // vmulx.w vf1,vf0,vf2x
    sqc2(1U, out + 32U);                                               // sqc2 vf1,32(a0)
    lqc2(1U, out + 48U);                                               // lqc2 vf1,48(a0)
    vmulb(1U, 0U, 2U, kW, 1U);                                         // vmuly.w vf1,vf0,vf2y
    sqc2(1U, out + 48U);                                               // sqc2 vf1,48(a0)
    lqc2(1U, out + 64U);                                               // lqc2 vf1,64(a0)
    vmulb(1U, 0U, 2U, kW, 2U);                                         // vmulz.w vf1,vf0,vf2z
    sqc2(1U, out + 64U);                                               // sqc2 vf1,64(a0)
    lqc2(1U, out + 80U);                                               // lqc2 vf1,80(a0)
    vmulb(1U, 0U, 2U, kW, 3U);                                         // vmulw.w vf1,vf0,vf2w
    sqc2(1U, out + 80U);                                               // sqc2 vf1,80(a0)
    cpu.write_fpr_bits(0U, f0);                                        // f0 as neg.s left it
    return call.finish(kTotalCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00349980, 1128U, 0x37745a2a1bd2225fULL, "BuildPackedViewFrustum", kTotalCost, build_packed_view_frustum,
         EngineReturn::Nothing)
