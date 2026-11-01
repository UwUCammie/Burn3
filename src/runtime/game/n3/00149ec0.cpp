// 0x00149ec0 (a0 = camera object, a1 = output vec4, 16-byte aligned): CameraLookDirection. Returns f0.
//
// W is the camera's world object (word at +340) and X the world's sub-object (word at W+9264). Four tests select
// PATH A: the word at W+7148 is 0, the word at W+7144 is 2, the word at X+5412 minus 1 is below 2, and the byte at
// W+7250 is not 255. Otherwise PATH B runs. The tests stop at the first failure, so the retired count depends on
// which one failed.
//
// PATH A stores quad W+7232 to the output and returns min(max(d, G), 0.5). d is the dot product of that quad's xyz
// with D = (W+7216) - (W+48) in xyz, divided by w when w > 1.0, where w is lane 3 of quad X+176 and G is the float
// at gp-30796.
//
// PATH B copies camera quad +320 (byte +337 set) or +288 (clear) as the vf2 operand, stores camera quad +304 or
// +272 to the output, and then stores normalize(80 * xyz + D) with the output's w kept, where D = copy - (W+48) in
// xyz. It returns |D|, divided by w when w > 1.0.
//
// The original's stack copies are not reproduced (the check ignores stack writes). Each read happens where the
// listing makes it relative to the output stores, so an output that overlaps W, X or the camera object reads back
// the same values.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kCameraWorld = 340U;      // camera+340: the world object W
constexpr GuestAddress kCameraFlag = 337U;       // camera+337 (byte): selects PATH B's copy and stored quads
constexpr GuestAddress kWorldFlag = 7148U;       // W+7148 (word): PATH A needs 0
constexpr GuestAddress kWorldMode = 7144U;       // W+7144 (word): PATH A needs 2
constexpr GuestAddress kWorldSub = 9264U;        // W+9264 (word): the sub-object X
constexpr GuestAddress kWorldByte = 7250U;       // W+7250 (byte): PATH A needs a value other than 255
constexpr GuestAddress kWorldStored = 7232U;     // W+7232 (quad): PATH A's output
constexpr GuestAddress kWorldPlain = 7216U;      // W+7216 (quad): PATH A's minuend of D
constexpr GuestAddress kWorldBase = 48U;         // W+48 (quad): the subtrahend of D on both paths
constexpr GuestAddress kSubMode = 5412U;         // X+5412 (word): PATH A needs (value - 1) below 2
constexpr GuestAddress kSubQuad = 176U;          // X+176 (quad): lane 3 is w
constexpr std::uint32_t kGlobalFloat = static_cast<std::uint32_t>(-30796);  // gp-relative float G

constexpr std::uint32_t kOneBits = 0x3f800000U;     // 1.0f: the w test
constexpr std::uint32_t kHalfBits = 0x3f000000U;    // 0.5f: PATH A's upper clamp (lui 0x3f00)
constexpr std::uint32_t kEightyBits = 0x42a00000U;  // 80.0f: PATH B's scale (lui 0x42a0)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kFirstTestCost = 5U;     // 0x149ec0..0x149ed0: addiu sp, lw, lw, bne, nop
constexpr std::uint32_t kSecondTestCost = 4U;    // 0x149ed4..0x149ee0
constexpr std::uint32_t kThirdTestCost = 6U;     // 0x149ee4..0x149ef8
constexpr std::uint32_t kFourthTestCost = 4U;    // 0x149efc..0x149f08
constexpr std::uint32_t kPathAHeadCost = 15U;    // 0x149f0c..0x149f44: through the bc1t and its delay slot
constexpr std::uint32_t kPathAShortCost = 12U;   // 0x149f90..0x149fbc: w <= 1.0
constexpr std::uint32_t kPathALongCost = 13U;    // 0x149f48..0x149f78: w > 1.0, with the divide
constexpr std::uint32_t kPathATailCost = 5U;     // 0x149f7c..0x149f8c: max.s, lui, mtc1, beq, min.s
constexpr std::uint32_t kPathBHeadCost = 3U;     // 0x149fc0..0x149fc8: lbu, beq, nop
constexpr std::uint32_t kPathBZeroCost = 4U;     // 0x149fe0..0x149fec: byte at +337 clear
constexpr std::uint32_t kPathBNonzeroCost = 5U;  // 0x149fcc..0x149fdc: byte at +337 set
constexpr std::uint32_t kPathBLoadCost = 15U;    // 0x149ff0..0x14a028: through the bc1t and its delay slot
constexpr std::uint32_t kPathBShortCost = 19U;   // 0x14a0d4..0x14a11c: w <= 1.0
constexpr std::uint32_t kPathBLongCost = 20U;    // 0x14a02c..0x14a078: w > 1.0, with the divide
constexpr std::uint32_t kPathBJoinCost = 22U;    // 0x14a07c..0x14a0d0
constexpr std::uint32_t kReturnCost = 2U;        // 0x14a120..0x14a124: jr ra and the sp restore

// PATH A after its four tests passed. Returns the count of the instructions from 0x149f0c on.
std::uint32_t run_path_a(guest::R5900State& cpu, auto& m, const GuestAddress world,
                         const GuestAddress out) {
    // The output store comes first: the lqc2 of the output reads it back, and the other reads may overlap it.
    n3::store_quad_any(m, out, n3::load_quad_any(m, world + kWorldStored));          // lq a0 / sq a0, 0(a1)
    const GuestAddress sub = m.read_u32_le(world + kWorldSub);                // lw v0, 9264(a2)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, world + kWorldBase));           // lqc2 vf1, 48(a2)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, world + kWorldPlain));          // lqc2 vf2, 7216(a2)
    Gpr128 v0 = n3::load_quad_any(m, sub + kSubQuad);                             // lq v0, 176(v0)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                         // vsub.xyz vf1, vf2, vf1
    const std::uint32_t w = v0.words[3];                                      // lwc1 f1, 60(sp): lane 3 of v0
    const bool short_route = fpu::le(w, kOneBits);                            // c.ole.s f1, f0; bc1t
    const Gpr128 a0 = cpu.read_vu0_vector(1U);                                // qmfc2 a0, vf1 (delay slot)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out));                          // lqc2 vf2, 0(a1)
    cpu.write_vu0_vector(1U, a0);                                             // qmtc2 a0, vf1
    ee_cop2::vmul(cpu, 2U, 2U, 1U, 14U);                                      // vmul.xyz vf2, vf2, vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1, vf0, vf0w
    v0 = cpu.read_vu0_vector(1U);                                             // qmfc2 v0, vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, v0);                                             // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1, vf1, vf2z
    v0 = cpu.read_vu0_vector(1U);                                             // qmfc2 v0, vf1
    std::uint32_t value = v0.words[0];                                        // mtc1 v0, f0
    if (!short_route) {
        value = fpu::div(value, w);                                           // div.s f0, f0, f1
    }
    cpu.write_fpr_bits(0U, value);
    cpu.write_fpr_bits(1U, m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kGlobalFloat));  // lwc1 f1, -30796(gp)
    ee_cop1::max_s(cpu, 1U, 0U, 1U);                                          // max.s f1, f0, f1
    cpu.write_fpr_bits(0U, kHalfBits);                                        // lui v0, 0x3f00; mtc1 v0, f0
    ee_cop1::min_s(cpu, 0U, 1U, 0U);                                          // min.s f0, f1, f0
    v0.words[0] = kHalfBits;                                                  // the lui sets the low doubleword
    v0.words[1] = 0U;
    cpu.write_gpr(2U, v0);
    cpu.write_gpr(4U, a0);
    return kPathAHeadCost + (short_route ? kPathAShortCost : kPathALongCost) + kPathATailCost + kReturnCost;
}

// PATH B after its tests failed. Returns the count of the instructions from 0x149fc0 on.
std::uint32_t run_path_b(guest::R5900State& cpu, auto& m, const GuestAddress camera,
                         const GuestAddress out) {
    const bool flagged = m.read_u8(camera + kCameraFlag) != 0U;                  // lbu v0, 337(a0)
    const Gpr128 copy = n3::load_quad_any(m, camera + (flagged ? 320U : 288U));      // lq v1, 320(a0) or 288(a0)
    const Gpr128 stored = n3::load_quad_any(m, camera + (flagged ? 304U : 272U));    // lq v0, 304(a0) or 272(a0)
    n3::store_quad_any(m, out, stored);                                              // sq v0, 0(a1)
    const GuestAddress world = m.read_u32_le(camera + kCameraWorld);             // lw a0, 340(a0)
    cpu.write_vu0_vector(2U, copy);                                              // lqc2 vf2, 192(sp)
    const GuestAddress sub = m.read_u32_le(world + kWorldSub);                   // lw v0, 9264(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, world + kWorldBase));              // lqc2 vf1, 48(a0)
    Gpr128 v0 = n3::load_quad_any(m, sub + kSubQuad);                                // lq v0, 176(v0)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, true);                            // vsub.xyz vf1, vf2, vf1
    const Gpr128 d = cpu.read_vu0_vector(1U);                                    // sqc2 vf1, 0(sp) (read back below)
    const std::uint32_t w = v0.words[3];                                         // lwc1 f1, 28(sp): lane 3 of v0
    cpu.write_fpr_bits(1U, w);
    const bool short_route = fpu::le(w, kOneBits);                               // c.ole.s f1, f0; bc1t
    const Gpr128 a0 = d;                                                         // qmfc2 a0, vf1 (delay slot)
    // The length of D: one VU0 sequence on both routes, which differ only by the divide after it.
    cpu.write_vu0_vector(1U, a0);                                                // qmtc2 a0, vf1
    ee_cop2::vmul(cpu, 2U, 1U, 1U, 14U);                                         // vmul.xyz vf2, vf1, vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                     // vaddw.x vf1, vf0, vf0w
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                               // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);      // vmaddz.x vf1, vf1, vf2z
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1
    ee_cop2::vsqrt(cpu, 1U, 0U);                                                 // vsqrt q, vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                     // vaddw.x vf1, vf0, vf0w
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1 (after vwaitq)
    ee_cop2::vmul_control(cpu, 1U, 1U, 8U, ee_cop2::kControlQ);                  // vmulq.x vf1, vf1, q
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    std::uint32_t length = v0.words[0];                                          // mtc1 v0, f0
    if (!short_route) {
        length = fpu::div(length, w);                                            // div.s f0, f0, f1
    }
    cpu.write_fpr_bits(0U, length);
    // Join: the output's xyz scaled by 80 plus D, normalised; the output's w is kept.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out));                             // lqc2 vf3, 0(a1)
    v0.words[0] = kEightyBits;                                                   // lui v0, 0x42a0
    v0.words[1] = 0U;
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1
    ee_cop2::vmul_broadcast(cpu, 3U, 3U, 1U, 14U, 0U);                           // vmulx.xyz vf3, vf3, vf1x
    cpu.write_vu0_vector(2U, d);                                                 // lqc2 vf2, 0(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                     // vaddw.x vf1, vf0, vf0w
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, 14U, false);                           // vadd.xyz vf3, vf3, vf2
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    n3::store_quad_any(m, out, cpu.read_vu0_vector(3U));                             // sqc2 vf3, 0(a1)
    ee_cop2::vmul(cpu, 2U, 3U, 3U, 14U);                                         // vmul.xyz vf2, vf3, vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                               // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);      // vmaddz.x vf1, vf1, vf2z
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1
    v0 = n3::load_quad_any(m, out);                                                  // lq v0, 0(a1)
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                    // vrsqrt q, vf0w, vf1x
    cpu.write_vu0_vector(1U, v0);                                                // qmtc2 v0, vf1 (after vwaitq)
    ee_cop2::vmul_control(cpu, 1U, 1U, 14U, ee_cop2::kControlQ);                 // vmulq.xyz vf1, vf1, q
    v0 = cpu.read_vu0_vector(1U);                                                // qmfc2 v0, vf1
    n3::store_quad_any(m, out, v0);                                                  // sq v0, 0(a1)
    cpu.write_gpr(2U, v0);
    cpu.write_gpr(4U, a0);
    return kPathBHeadCost + (flagged ? kPathBNonzeroCost : kPathBZeroCost) + kPathBLoadCost +
           (short_route ? kPathBShortCost : kPathBLongCost) + kPathBJoinCost + kReturnCost;
}

template <class Context>
n3::Run camera_look_direction(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress camera = call.arg(0);
    const GuestAddress out = call.arg(1);
    // Each test is reached only when the ones before it passed; a failing test leaves for PATH B with the count
    // of the instructions it reached.
    std::uint32_t retired = kFirstTestCost;
    const GuestAddress world = m.read_u32_le(camera + kCameraWorld);
    bool path_a = m.read_u32_le(world + kWorldFlag) == 0U;
    if (path_a) {
        retired += kSecondTestCost;
        path_a = m.read_u32_le(world + kWorldMode) == 2U;
    }
    if (path_a) {
        retired += kThirdTestCost;
        const GuestAddress sub = m.read_u32_le(world + kWorldSub);
        path_a = (m.read_u32_le(sub + kSubMode) - 1U) < 2U;
    }
    if (path_a) {
        retired += kFourthTestCost;
        path_a = m.read_u8(world + kWorldByte) != 255U;
    }
    if (path_a) {
        return call.finish(retired + run_path_a(cpu, m, world, out));
    }
    return call.finish(retired + run_path_b(cpu, m, camera, out));
}

} // namespace

// Declared cost 86: the longest path (the fourth test fails, then PATH B's long route with the byte at +337 set).
extern constinit const EngineFunction n3_00149ec0{0x00149ec0U, 616U, 0x6c1b7fe27f080799ULL, "CameraLookDirection",
                                                  86U, n3::live<&camera_look_direction<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(00149ec0, camera_look_direction)
