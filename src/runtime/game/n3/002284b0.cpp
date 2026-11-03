// 0x002284b0 (a0 = object): builds the five control-quad layers of a triangle-strip mesh, then the strip itself.
//
// The object's u32 at +4504 is the strip buffer and +4508 its running offset in 16-byte blocks; +4500 is set to 6 on
// the way out and +4512 + 4*i (i = 0..4) receives the buffer address layer i's strip starts at. Layer 0's control
// quads are the homogeneous identity basis (1,0,0,1) (0,1,0,1) (0,0,1,1) (0,0,0,1); four VU0 blocks build layers 1..4
// from the direction quads at 0x004b9dc0 / 0x004b9dd0, an angle the caller keeps through $gp and a polynomial of it.
//
// Each layer bumps the object's offset by 200 blocks. BuildGridTriangleStrip (0x002294d0, always n = 10) writes a
// 10x10 grid of (position, uv) quads to 0x70000000 and returns its 196 entries; the layer writes a 64-byte DMA tag
// first (block count, 16, 84, 82 and two tag words), then 196 entries of 16 bytes: the grid's uv pair and two 16-bit
// coordinates. An entry's position is the layer's four control quads blended by the grid's (u, v, w) weights and 1,
// normalised, scaled by 256 and offset by 256*16, then quantised.
//
// The halfword counts at +4532..+4568 are 7 everywhere except the fourth and fifth layers' first count (6). The
// retired count is fixed: 1538 for the four blocks, then five passes of 3 + 69 + 14703 + 1 plus the two callees,
// then 3 + 18 for the closing check and the epilogue. The VU0 runs on the real VU0 registers, as the original does;
// the quads the original copies through its stack frame are C++ locals here, and its byte-wise quad copies are gone.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

N3_CALLEE(002294d0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

constexpr std::uint32_t kCeiling = 200000U;
constexpr std::uint32_t kFrameBytes = 7968U;             // the original's addiu $sp,$sp,-7968
constexpr std::uint32_t kGridSize = 10U;                 // addiu $a0,$zero,10
constexpr std::uint32_t kEntryBytes = 16U;
constexpr std::uint32_t kGridQuadBytes = 32U;            // BuildGridTriangleStrip's entry: position then uv
constexpr std::uint32_t kLayers = 5U;
constexpr std::uint32_t kScratchA = 2016U;               // lqc2 $vf2,2016($sp)
constexpr std::uint32_t kScratchB = 1552U;               // lqc2 $vf2,1552($sp)
constexpr std::uint32_t kScratchC = 1088U;               // lqc2 $vf2,1088($sp)
constexpr std::uint32_t kScratchD = 624U;                // lqc2 $vf2,624($sp)
constexpr GuestAddress kScratchpad = 0x70000000U;        // lui $fp,0x7000: the grid
constexpr GuestAddress kPacketBuffer = 0x01d6d880U;      // lui 0x01d7, addiu -10112
constexpr std::uint32_t kPacketSize = 26U;

// Object fields.
constexpr std::uint32_t kLayerCountOut = 4500U;
constexpr std::uint32_t kBufferBase = 4504U;
constexpr std::uint32_t kBufferOffset = 4508U;
constexpr std::uint32_t kLayerBuffer = 4512U;
constexpr std::uint32_t kHalfwordCountA = 4532U;
constexpr std::uint32_t kHalfwordCountB = 4552U;

// Globals: the direction quads and the three floats the caller keeps through $gp.
constexpr GuestAddress kDirectionQuad0 = 0x004b9dc0U;    // lui 0x004c, lq/lqc2 -25152
constexpr GuestAddress kDirectionQuad1 = 0x004b9dd0U;    // lui 0x004c, lq/lqc2 -25136
constexpr std::uint32_t kAngleOffset = 0xffff8414U;      // lwc1 -31724($gp)
constexpr std::uint32_t kAngleOffsetB = 0xffff8b78U;     // lwc1 -29832($gp)
constexpr std::uint32_t kScaleOffset = 0xffff8b7cU;      // lwc1 -29828($gp) (f22)

// Constants the original builds in registers and loads into the I register and the FPRs.
constexpr std::uint32_t kOne = 0x3f800000U;              // lui 0x3f80
constexpr std::uint32_t kHalf = 0x3f000000U;             // lui 0x3f00
constexpr std::uint32_t kQuarter = 0x3e800000U;          // lui 0x3e80
constexpr std::uint32_t kHalfPi = 0x3fc90fdbU;           // lui 0x3fc9, ori 0x0fdb
constexpr std::uint32_t kNegInvTwoPi = 0xbe22f983U;      // lui 0xbe22, ori 0xf983
constexpr std::uint32_t kRoundMagic = 0x4b400000U;       // lui 0x4b40
constexpr std::uint32_t kTwoPi = 0x40c90fdaU;            // lui 0x40c9, ori 0x0fda
constexpr std::uint32_t kPolyCube = 0xc2992661U;         // lui 0xc299, ori 0x2661
constexpr std::uint32_t kPolyQuadratic = 0xc2255de0U;    // lui 0xc225, ori 0x5de0
constexpr std::uint32_t kPolyLinear = 0x42a33457U;       // lui 0x42a3, ori 0x3457
constexpr std::uint32_t kPolyConstant = 0x421ed7b7U;     // lui 0x421e, ori 0xd7b7
constexpr std::uint32_t kScaleGain = 0x43800000U;        // lui 0x4380: f21
constexpr std::uint32_t kUnitGain = 0x41800000U;         // lui 0x4180: f20

// Instruction counts: the four layer blocks (0x002284b0..0x00229234, the twelve eight-pair quad copies included),
// the strip loop's pieces (0x00229238..0x00229468) and the epilogue (0x00229488..0x002294cc).
constexpr std::uint32_t kLayer1Cost = 577U;              // 210 + 3*(8*8) + 2*5 + 165
constexpr std::uint32_t kLayer2Cost = 366U;              // 3*(8*8) + 2*5 + 164
constexpr std::uint32_t kLayer3Cost = 367U;              // 3*(8*8) + 2*5 + 165
constexpr std::uint32_t kLayer4Cost = 228U;              // 3*(8*8) + 2*5 + 26
constexpr std::uint32_t kOuterCheckCost = 3U;
constexpr std::uint32_t kOuterBodyCost = 69U;
constexpr std::uint32_t kEntryCost = 72U;
constexpr std::uint32_t kInnerCheckCost = 3U;
constexpr std::uint32_t kNextLayerCost = 1U;
constexpr std::uint32_t kEpilogueCost = 18U;

struct Layer {
    Gpr128 p0;
    Gpr128 p1;
    Gpr128 p2;
    Gpr128 p3;
};

// The direction quad scaled by 1/|quad|: q = rsqrt of the sum of its squared lanes, as the block's VADDAy/VMADDz
// pair leaves it.
Gpr128 normalized_direction(guest::R5900State& cpu, const Gpr128& quad) {
    cpu.write_vu0_vector(1U, Gpr128{{kOne, 0U, 0U, 0U}});               // qmtc2 $a0,$vf1 (a0 = 1.0)
    cpu.write_vu0_vector(2U, Gpr128{});                                 // lqc2 $vf2,6992($sp): its other lanes are dead
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);    // vaddx.x $vf2,$vf0,$vf1x
    cpu.write_vu0_vector(1U, quad);                                     // lqc2 $vf1,-25152($v1)
    ee_cop2::vmul(cpu, 1U, 1U, 1U, n3::kMaskXyz);                       // vmul.xyz $vf1,$vf1,$vf1
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));                  // qmfc2 $a0,$vf1 ; qmtc2 $a0,$vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);              // vadday.x $acc,$vf2,$vf2y
    cpu.write_vu0_vector(1U, Gpr128{{kOne, 0U, 0U, kOne}});             // qmfc2/qmtc2 $v1,$vf1 ((1,0,0,1))
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x $vf1,$vf1,$vf2z
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                           // vrsqrt $q,$vf0w,$vf1x (vwaitq is immediate)
    cpu.write_vu0_vector(1U, quad);                                     // qmtc2 $a1,$vf1
    ee_cop2::vmul_control(cpu, 1U, 1U, n3::kMaskXyz, ee_cop2::kControlQ);   // vmulq.xyz $vf1,$vf1,$q
    return cpu.read_vu0_vector(1U);                                     // qmfc2 $t2,$vf1
}

// The angle polynomial: the angle's fractional turn, shifted by a quarter turn, through the four-term polynomial
// x*(2pi + x^2*(c + x^2*(b + x^2*a))) that the listing's VMULi/VMADD chain evaluates in vf3. `scratch` is the quad
// the block loads from the caller's stack at 2016/1552/1088/624($sp): its z and w lanes ride through the polynomial
// and the w lane ends up in $t0, which the callee copies into the grid quads' w lanes.
Gpr128 angle_polynomial(guest::R5900State& cpu, const std::uint32_t angle, const Gpr128& scratch,
                        Gpr128& eighth_power) {
    cpu.write_vu0_vector(1U, Gpr128{{angle, 0U, 0U, 0U}});              // qmtc2 $a0,$vf1
    cpu.write_vu0_vector(2U, scratch);                                 // lqc2 $vf2,2016/1552/1088/624($sp)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskX, 0U, false);    // vaddx.x $vf2,$vf0,$vf1x
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 2U, kMaskY, 0U, false);    // vaddx.y $vf2,$vf0,$vf2x
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(2U));                  // qmfc2/qmtc2 $v1,$vf2
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kHalfPi);                    // ctc2 $t1,$i
    ee_cop2::vsub_i(cpu, 1U, 1U, 0x0aU);                                // vsubi.xz $vf1,$vf1,$i
    ee_cop2::vabs(cpu, 1U, 1U, n3::kMaskXyzw);                          // vabs.xyzw $vf1,$vf1
    const Gpr128 phase = cpu.read_vu0_vector(1U);                       // qmfc2 $a2,$vf1
    (void)ee_cop2::vmax_w(cpu, 1U, 0U, 0U, n3::kMaskXyzw);              // vmaxw.xyzw $vf1,$vf0,$vf0w
    const Gpr128 ones = cpu.read_vu0_vector(1U);                        // qmfc2 $a0,$vf1
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kNegInvTwoPi);               // ctc2 $t0,$i
    cpu.write_vu0_vector(2U, phase);                                    // qmtc2 $a2,$vf2
    ee_cop2::vmula_broadcast(cpu, 2U, 0U, n3::kMaskXyzw, true);         // vmulai.xyzw $acc,$vf2,$i
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kRoundMagic);                // ctc2 $v1,$i
    cpu.write_vu0_vector(1U, ones);                                     // qmtc2 $a0,$vf1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, n3::kMaskXyzw, 0U, true, true, true);   // vmsubai $acc,$vf1,$i
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, n3::kMaskXyzw, 0U, true, true, false);  // vmaddai $acc,$vf1,$i
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kNegInvTwoPi);               // ctc2 $t0,$i
    ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 0U, n3::kMaskXyzw, 0U, true, true, true);   // vmsubai $acc,$vf2,$i
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kHalf);                      // ctc2 $a1,$i
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyzw, 0U, true, false, true);  // vmsubi $vf1,$vf1,$i
    ee_cop2::vabs(cpu, 1U, 1U, n3::kMaskXyzw);                          // vabs.xyzw $vf1,$vf1
    const Gpr128 fraction = cpu.read_vu0_vector(1U);                    // qmfc2 $a0,$vf1
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kQuarter);                   // ctc2 $v1,$i
    cpu.write_vu0_vector(1U, fraction);                                 // qmtc2 $a0,$vf1
    ee_cop2::vsub_i(cpu, 1U, 1U, n3::kMaskXyzw);                        // vsubi.xyzw $vf1,$vf1,$i
    cpu.write_vu0_vector(3U, cpu.read_vu0_vector(1U));                  // qmfc2/qmtc2 $v1,$vf3

    ee_cop2::vmul(cpu, 4U, 3U, 3U, n3::kMaskXyzw);                      // vmul $vf4,$vf3,$vf3
    ee_cop2::vmul(cpu, 2U, 4U, 4U, n3::kMaskXyzw);                      // vmul $vf2,$vf4,$vf4
    ee_cop2::vmul(cpu, 1U, 2U, 2U, n3::kMaskXyzw);                      // vmul $vf1,$vf2,$vf2
    const Gpr128 eighth = cpu.read_vu0_vector(1U);                      // qmfc2 $s0,$vf1
    eighth_power = eighth;
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kPolyCube);                  // ctc2 $a3,$i
    ee_cop2::vmul_control(cpu, 1U, 3U, n3::kMaskXyzw, ee_cop2::kControlI);   // vmuli $vf1,$vf3,$i
    ee_cop2::vmul(cpu, 1U, 1U, 4U, n3::kMaskXyzw);                      // vmul $vf1,$vf1,$vf4
    const Gpr128 cubic = cpu.read_vu0_vector(1U);                       // qmfc2 $t7,$vf1
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kPolyQuadratic);             // ctc2 $a2,$i
    ee_cop2::vmul_control(cpu, 1U, 3U, n3::kMaskXyzw, ee_cop2::kControlI);   // vmuli $vf1,$vf3,$i
    const Gpr128 quadratic = cpu.read_vu0_vector(1U);                   // qmfc2 $t6,$vf1
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kPolyLinear);                // ctc2 $a1,$i
    ee_cop2::vmul_control(cpu, 1U, 3U, n3::kMaskXyzw, ee_cop2::kControlI);   // vmuli $vf1,$vf3,$i
    const Gpr128 linear = cpu.read_vu0_vector(1U);                      // qmfc2 $t5,$vf1
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kPolyConstant);              // ctc2 $a0,$i
    ee_cop2::vmul_control(cpu, 1U, 3U, n3::kMaskXyzw, ee_cop2::kControlI);   // vmuli $vf1,$vf3,$i
    const Gpr128 constant = cpu.read_vu0_vector(1U);                    // qmfc2 $t4,$vf1
    cpu.write_vu0_vector(1U, quadratic);                                // qmtc2 $t6,$vf1
    ee_cop2::vmula(cpu, 1U, 4U, n3::kMaskXyzw);                         // vmula $acc,$vf1,$vf4
    cpu.write_vu0_vector(1U, cubic);                                    // qmtc2 $t7,$vf1
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, n3::kMaskXyzw, true);                        // vmadda $acc,$vf1,$vf2
    ee_cop2::ctc2(cpu, ee_cop2::kControlI, kTwoPi);                     // ctc2 $v1,$i
    cpu.write_vu0_vector(1U, linear);                                   // qmtc2 $t5,$vf1
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, n3::kMaskXyzw, true);                        // vmadda $acc,$vf1,$vf2
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 0U, n3::kMaskXyzw, 0U, true, true, false);  // vmaddai $acc,$vf3,$i
    cpu.write_vu0_vector(2U, constant);                                 // qmtc2 $t4,$vf2
    cpu.write_vu0_vector(1U, eighth);                                   // qmtc2 $s0,$vf1
    ee_cop2::vmadd(cpu, 1U, 2U, 1U, n3::kMaskXyzw, false);              // vmadd $vf1,$vf2,$vf1
    return cpu.read_vu0_vector(1U);                                     // qmfc2 $t7,$vf1
}

// The three transformed quads of a layer: the polynomial's y and x lanes and the normalised direction build the
// basis in vf4, vf7 and vf8; the first quad takes x, z, y of its lanes, the second z, x, x, and vf5 y, x, x.
struct BlockQuads {
    Gpr128 first;
    Gpr128 second;
    Gpr128 third;
};

BlockQuads build_layer_quads(guest::R5900State& cpu, const Gpr128& direction, const Gpr128& poly) {
    cpu.write_vu0_vector(5U, Gpr128{{kOne, 0U, 0U, 0U}});               // qmtc2 $t3,$vf5 (the quad the block left)
    cpu.write_vu0_vector(3U, poly);                                     // qmtc2 $t7,$vf3
    cpu.write_vu0_vector(6U, Gpr128{});                                 // lqc2 $vf6,0($t6): its other lanes are dead
    cpu.write_vu0_vector(7U, Gpr128{});                                 // lqc2 $vf7,6944($sp): its other lanes are dead
    cpu.write_vu0_vector(2U, direction);                                // qmtc2 $t2,$vf2

    ee_cop2::vadd_broadcast(cpu, 1U, 5U, 3U, kMaskX, 1U, true);     // vsuby.x $vf1,$vf5,$vf3y
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, kMaskZ, 0U, false);    // vaddx.z $vf3,$vf0,$vf1x
    ee_cop2::vmul(cpu, 1U, 2U, 2U, n3::kMaskXyz);                       // vmul.xyz $vf1,$vf2,$vf2
    ee_cop2::vadd_vector(cpu, 8U, 0U, 1U, n3::kMaskXyz, true);          // vsub.xyz $vf8,$vf0,$vf1
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kMaskY, 2U);           // vmulz.y $vf1,$vf2,$vf2z
    ee_cop2::vadd_broadcast(cpu, 7U, 0U, 1U, kMaskX, 1U, false);    // vaddy.x $vf7,$vf0,$vf1y
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kMaskZ, 0U);           // vmulx.z $vf1,$vf2,$vf2x
    ee_cop2::vadd_broadcast(cpu, 7U, 0U, 1U, kMaskY, 2U, false);    // vaddz.y $vf7,$vf0,$vf1z
    ee_cop2::vadd_broadcast(cpu, 8U, 8U, 5U, n3::kMaskXyz, 0U, false);  // vaddx.xyz $vf8,$vf8,$vf5x
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kMaskX, 1U);           // vmuly.x $vf1,$vf2,$vf2y
    ee_cop2::vadd_broadcast(cpu, 7U, 0U, 1U, kMaskZ, 0U, false);    // vaddx.z $vf7,$vf0,$vf1x
    ee_cop2::vmul_broadcast(cpu, 8U, 8U, 3U, n3::kMaskXyz, 2U);         // vmulz.xyz $vf8,$vf8,$vf3z
    ee_cop2::vmul_broadcast(cpu, 4U, 2U, 3U, n3::kMaskXyz, 0U);         // vmulx.xyz $vf4,$vf2,$vf3x
    ee_cop2::vmul_broadcast(cpu, 7U, 7U, 3U, n3::kMaskXyz, 2U);         // vmulz.xyz $vf7,$vf7,$vf3z

    ee_cop2::vadd_broadcast(cpu, 1U, 5U, 8U, kMaskX, 0U, true);     // vsubx.x $vf1,$vf5,$vf8x
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 1U, kMaskX, 0U, false);    // vaddx.x $vf6,$vf0,$vf1x
    ee_cop2::vadd_broadcast(cpu, 1U, 7U, 4U, kMaskZ, 2U, false);    // vaddz.z $vf1,$vf7,$vf4z
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 1U, kMaskY, 2U, false);    // vaddz.y $vf6,$vf0,$vf1z
    ee_cop2::vadd_broadcast(cpu, 1U, 7U, 4U, kMaskY, 1U, true);     // vsuby.y $vf1,$vf7,$vf4y
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 1U, kMaskZ, 1U, false);    // vaddy.z $vf6,$vf0,$vf1y
    const Gpr128 first = cpu.read_vu0_vector(6U);                       // sqc2 $vf6,7040/7488/7632($sp)

    ee_cop2::vadd_broadcast(cpu, 3U, 7U, 4U, kMaskZ, 2U, true);     // vsubz.z $vf3,$vf7,$vf4z
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 3U, kMaskX, 2U, false);    // vaddz.x $vf6,$vf0,$vf3z
    ee_cop2::vadd_broadcast(cpu, 2U, 5U, 8U, kMaskX, 1U, true);     // vsuby.x $vf2,$vf5,$vf8y
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 2U, kMaskY, 0U, false);    // vaddx.y $vf6,$vf0,$vf2x
    ee_cop2::vadd_broadcast(cpu, 1U, 5U, 8U, kMaskX, 2U, true);     // vsubz.x $vf1,$vf5,$vf8z
    ee_cop2::vadd_broadcast(cpu, 2U, 7U, 4U, kMaskX, 0U, false);    // vaddx.x $vf2,$vf7,$vf4x
    ee_cop2::vadd_broadcast(cpu, 6U, 0U, 2U, kMaskZ, 0U, false);    // vaddx.z $vf6,$vf0,$vf2x
    ee_cop2::vadd_broadcast(cpu, 3U, 7U, 4U, kMaskY, 1U, false);    // vaddy.y $vf3,$vf7,$vf4y
    ee_cop2::vadd_broadcast(cpu, 2U, 7U, 4U, kMaskX, 0U, true);     // vsubx.x $vf2,$vf7,$vf4x
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 3U, kMaskX, 1U, false);    // vaddy.x $vf5,$vf0,$vf3y
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 2U, kMaskY, 0U, false);    // vaddx.y $vf5,$vf0,$vf2x
    ee_cop2::vadd_vector(cpu, 4U, 0U, 0U, n3::kMaskXyz, false);         // vadd.xyz $vf4,$vf0,$vf0
    ee_cop2::vadd_broadcast(cpu, 5U, 0U, 1U, kMaskZ, 0U, false);    // vaddx.z $vf5,$vf0,$vf1x
    const Gpr128 second = cpu.read_vu0_vector(6U);                      // sqc2 $vf6,0($t6)
    const Gpr128 third = cpu.read_vu0_vector(5U);                       // sqc2 $vf5,0($t3)
    return {first, second, third};
}

// One layer's control quads from the direction quad at `direction_address` and the angle bits `angle`.
template <class Memory>
Layer build_layer(guest::R5900State& cpu, const Memory& m, const GuestAddress direction_address,
                  const std::uint32_t angle, const Gpr128& scratch, Gpr128& eighth_power) {
    const Gpr128 direction = normalized_direction(cpu, n3::load_quad_any(m, direction_address));
    const Gpr128 poly = angle_polynomial(cpu, angle, scratch, eighth_power);
    const BlockQuads quads = build_layer_quads(cpu, direction, poly);
    return {quads.first, quads.second, quads.third, Gpr128{{0U, 0U, 0U, kOne}}};
}

template <class Context>
n3::Run build_five_layer_strip(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t angle_a = m.read_u32_le(gp + kAngleOffset);
    const std::uint32_t angle_b = m.read_u32_le(gp + kAngleOffsetB);
    const std::uint32_t scale_bits = m.read_u32_le(gp + kScaleOffset);

    // The object's per-layer halfword counts, and the layer count the object ends with.
    for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
        m.write_u32_le(object + kHalfwordCountA + 4U * layer, layer < 3U ? 7U : 6U);
        m.write_u32_le(object + kHalfwordCountB + 4U * layer, 7U);
    }
    m.write_u32_le(object + kLayerCountOut, 6U);

    Layer layers[kLayers];
    layers[0] = Layer{Gpr128{{kOne, 0U, 0U, kOne}}, Gpr128{{0U, kOne, 0U, kOne}},
                      Gpr128{{0U, 0U, kOne, kOne}}, Gpr128{{0U, 0U, 0U, kOne}}};
    // The four blocks read a quad of the caller's stack (2016, 1552, 1088 and 624 bytes into the frame); its z
    // and w lanes ride through the angle polynomial, and the last block's x^8 result is left in $t0, which the
    // callee moves into the grid quads' w lanes.
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    Gpr128 eighth_power{};
    Gpr128 unused_power{};
    layers[1] = build_layer(cpu, m, kDirectionQuad0, angle_a, n3::load_quad_any(m, frame + kScratchA), unused_power);
    layers[2] = build_layer(cpu, m, kDirectionQuad0, angle_b, n3::load_quad_any(m, frame + kScratchB), unused_power);
    layers[3] = build_layer(cpu, m, kDirectionQuad1, angle_a, n3::load_quad_any(m, frame + kScratchC), unused_power);
    layers[4] = build_layer(cpu, m, kDirectionQuad1, angle_b, n3::load_quad_any(m, frame + kScratchD), eighth_power);
    cpu.write_gpr(8U, eighth_power);
    std::uint32_t retired = kLayer1Cost + kLayer2Cost + kLayer3Cost + kLayer4Cost;

    const std::uint32_t buffer_base = m.read_u32_le(object + kBufferBase);
    std::uint32_t buffer_offset = m.read_u32_le(object + kBufferOffset);

    for (std::uint32_t layer = 0U; layer < kLayers; ++layer) {
        retired += kOuterCheckCost;
        const GuestAddress layer_slot = object + kLayerBuffer + 4U * layer;
        m.write_u32_le(layer_slot, 0U);                                  // sw $zero,4512($v0)

        // BuildGridTriangleStrip(10): the grid this layer samples.
        call.set_arg(0U, kGridSize);
        call.set_arg(1U, 0U);
        const n3::Run grid = call.invoke([](auto& machine) { return n3::fn_002294d0(machine); }, kFrameBytes);
        if (grid.declined) {
            return call.decline();
        }
        const std::uint32_t entries = call.result();
        const std::uint32_t blocks = (entries * kEntryBytes + 15U) >> 4U;
        const GuestAddress layer_buffer = buffer_base + buffer_offset * 16U;
        m.write_u32_le(layer_slot, layer_buffer);                        // sw $v0,0($s5)
        const std::uint32_t next_offset = buffer_offset + blocks + 4U;
        m.write_u32_le(object + kBufferOffset, next_offset);             // sw $v0,4508($s2)
        retired += kOuterBodyCost + grid.retired;

        // 0x0022928c: the packet build at 0x00222300, which has no n3 file yet.
        // n3-missing-callee
        return call.decline();

        // The layer's 64-byte tag, written before its entries.
        m.write_u64_le(layer_buffer + 0U, (blocks + 3U) | 0x60000000U);
        m.write_u64_le(layer_buffer + 8U, static_cast<std::uint64_t>((blocks + 3U) | 0x50000000U) << 32U);
        m.write_u64_le(layer_buffer + 16U, 0x2400000000000001ULL);
        m.write_u64_le(layer_buffer + 24U, 16U);
        m.write_u64_le(layer_buffer + 32U, 84U);
        m.write_u64_le(layer_buffer + 40U, 0x3f80000080808080ULL);
        m.write_u64_le(layer_buffer + 48U, static_cast<std::uint64_t>(entries) | 0x2400000000008000ULL);
        m.write_u64_le(layer_buffer + 56U, 82U);

        // The layer's control quads, in the registers the strip loop reads them from.
        const Layer& current = layers[layer];
        cpu.write_vu0_vector(6U, current.p0);                            // lqc2 $vf6,0($a0)
        cpu.write_vu0_vector(5U, current.p1);                            // lqc2 $vf5,16($a0)
        cpu.write_vu0_vector(4U, current.p2);                            // lqc2 $vf4,32($a0)
        cpu.write_vu0_vector(3U, current.p3);                            // lqc2 $vf3,48($a0)
        cpu.write_vu0_vector(9U, Gpr128{{kOne, 0U, 0U, 0U}});            // qmtc2 $t3,$vf9 (t3 = 1.0)
        cpu.write_vu0_vector(7U, Gpr128{{kOne, 0U, 0U, 0U}});            // qmtc2 $t2,$vf7 (t2 = 1.0)
        cpu.write_vu0_vector(2U, Gpr128{{kOne, 0U, 0U, kOne}});          // vaddw.x $vf2,$vf0,$vf0w
        cpu.set_fpu_accumulator_bits(fpu::add(0U, kScaleGain));          // adda.s $f1,$f21 (f1 = 0)
        GuestAddress strip = layer_buffer + 64U;
        for (std::uint32_t entry = 0U; entry < entries; ++entry) {
            const GuestAddress grid_entry = kScratchpad + entry * kGridQuadBytes;
            const Gpr128 weights = n3::load_quad_any(m, grid_entry);      // lqc2 $vf1,0($t2)
            const Gpr128 uv = n3::load_quad_any(m, grid_entry + kEntryBytes);  // lq $t2,16($t2)
            cpu.write_vu0_vector(1U, weights);
            ee_cop2::vmula_broadcast(cpu, 6U, 1U, n3::kMaskXyz, false);                  // vmulax.xyz $acc,$vf6,$vf1x
            ee_cop2::vmadd_broadcast(cpu, 0U, 5U, 1U, n3::kMaskXyz, 1U, false, true, false);  // vmadday.xyz $acc,$vf5,$vf1y
            ee_cop2::vmadd_broadcast(cpu, 0U, 4U, 1U, n3::kMaskXyz, 2U, false, true, false);  // vmaddaz.xyz $acc,$vf4,$vf1z
            ee_cop2::vmadd_broadcast(cpu, 1U, 3U, 0U, n3::kMaskXyz, 3U, false, false, false);  // vmaddw.xyz $vf1,$vf3,$vf0w
            const Gpr128 blended = cpu.read_vu0_vector(1U);                                // qmfc2 $t3,$vf1
            cpu.write_vu0_vector(8U, blended);                                             // qmtc2 $t3,$vf8
            ee_cop2::vmul(cpu, 1U, 8U, 8U, n3::kMaskXyz);                                  // vmul.xyz $vf1,$vf8,$vf8
            ee_cop2::vadda_broadcast(cpu, 1U, 1U, kMaskX, 1U);                         // vadday.x $acc,$vf1,$vf1y
            ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, kMaskX, 2U, false, false, false);  // vmaddz.x $vf1,$vf2,$vf1z
            ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                      // vrsqrt $q,$vf0w,$vf1x
            ee_cop2::vmul_control(cpu, 8U, 8U, n3::kMaskXyz, ee_cop2::kControlQ);          // vmulq.xyz $vf8,$vf8,$q
            const Gpr128 normal = cpu.read_vu0_vector(8U);                                 // qmfc2 $t3,$vf8
            cpu.write_vu0_vector(10U, normal);                                             // qmtc2 $t3,$vf10
            const std::uint32_t normal_x = normal.words[0];                                // mtc1 $t3,$f4
            ee_cop2::vmul_broadcast(cpu, 1U, 10U, 10U, kMaskX, 0U);                    // vmulx.x $vf1,$vf10,$vf10x
            const std::uint32_t square_x = cpu.read_vu0_vector(1U).words[0];               // qmfc2 $t2,$vf1 / mtc1 $t2,$f2
            ee_cop2::vadd_broadcast(cpu, 8U, 10U, 9U, kMaskZ, 0U, false);              // vaddx.z $vf8,$vf10,$vf9x
            ee_cop2::vmul_broadcast(cpu, 1U, 10U, 10U, kMaskY, 1U);                    // vmuly.y $vf1,$vf10,$vf10y
            const std::uint32_t xy = cpu.read_vu0_vector(1U).words[1];                     // sqc2/lwc1 $f0,7188($sp)
            ee_cop2::vadd_broadcast(cpu, 1U, 10U, 7U, kMaskZ, 0U, false);              // vaddx.z $vf1,$vf10,$vf7x
            ee_cop2::vmul_broadcast(cpu, 1U, 8U, 1U, kMaskZ, 2U);                      // vmulz.z $vf1,$vf8,$vf1z
            const std::uint32_t zz = cpu.read_vu0_vector(1U).words[2];                     // sqc2/lwc1 $f0,7144($sp)
            ee_cop2::vadd_vector(cpu, 1U, 0U, 10U, kMaskY, true);                      // vsub.y $vf1,$vf0,$vf10
            const std::uint32_t minus_y = cpu.read_vu0_vector(1U).words[1];                // sqc2/lwc1 $f2,3188($sp)
            const std::uint32_t sum = fpu::add(fpu::add(square_x, xy), zz);                // add.s $f2,$f2,$f0 ; add.s $f3
            const std::uint32_t length = fpu::sqrt(sum);                                   // sqrt.s $f3,$f3
            const std::uint32_t lateral = fpu::div(minus_y, length);                       // div.s $f2,$f2,$f3
            const std::uint32_t forward = fpu::div(normal_x, length);                      // div.s $f0,$f4,$f3
            m.write_u64_le(strip, (static_cast<std::uint64_t>(uv.words[1]) << 32U) | uv.words[0]);  // sd $t2,0($t1)
            std::uint32_t first = fpu::mul(forward, scale_bits);                           // mul.s $f0,$f0,$f22
            first = fpu::madd(cpu.fpu_accumulator_bits(), kScaleGain, first);              // madd.s $f0,$f21,$f0
            first = fpu::mul(kUnitGain, first);                                            // mul.s $f0,$f20,$f0
            const std::uint32_t packed_first = static_cast<std::uint32_t>(fpu::to_int(first));  // cvt.w.s $f0 / mfc1
            std::uint32_t second = fpu::mul(lateral, scale_bits);                          // mul.s $f2,$f2,$f22
            cpu.set_fpu_accumulator_bits(fpu::add(0U, kScaleGain));                        // adda.s $f1,$f21
            second = fpu::madd(cpu.fpu_accumulator_bits(), kScaleGain, second);            // madd.s $f0,$f21,$f2
            second = fpu::mul(kUnitGain, second);                                          // mul.s $f0,$f20,$f0
            const std::uint32_t packed_second = static_cast<std::uint32_t>(fpu::to_int(second));  // cvt.w.s / mfc1
            const std::uint64_t packed =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(packed_first))) |
                (static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(packed_second)))
                 << 16U);
            m.write_u64_le(strip + 8U, packed);                                            // sd $t2,8($t1)
            strip += kEntryBytes;
            retired += kEntryCost;
        }
        retired += kInnerCheckCost * (entries + 1U);
        retired += kNextLayerCost;
        buffer_offset = next_offset;
    }
    retired += kOuterCheckCost;
    retired += kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002284b0, 4128U, 0x3a4352cf48c7d6f6ULL, "BuildFiveLayerStrip", kCeiling, build_five_layer_strip,
         EngineReturn::Nothing)
