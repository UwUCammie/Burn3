// 0x003d9360 (a0 = record R, a1 = quad block Q): ClampFrustumQuadEdges. Returns nothing.
// R's u32 at +0 is B, the base of 16 records of 32 bytes (their first quads are vectors). Q+0..127 are eight input
// points; Q+128, Q+144, Q+160 and Q+176 are outputs. Each output starts as a copy of a B quad (B+384 to Q+128,
// B+448 to Q+144, B+416 to Q+160, B+480 to Q+176), then gets:
//  - its z clamped against an input point: min(z, in.z - C) for Q+128 (in = Q+0) and Q+160 (in = Q+32), and
//    max(z, in.z + C) for Q+144 (in = Q+16) and Q+176 (in = Q+48); C is the float at gp-31320;
//  - its y set to the highest Y of the 16 B quads and the 8 input points (the maximum starts at the float at
//    gp-31016);
//  - its x set to the x of the edge from P0 to P1 at its z (P0 and P1 are Q+0 and Q+16 for Q+128 and Q+144, Q+32
//    and Q+48 for Q+160 and Q+176).
// The VU0 steps run on the real VU0 registers through the ee_cop2 primitives, in the listing's order, so vf1..vf5
// and the MAC/STATUS flags end as the original leaves them. The listing's stack spills are not kept: each one is
// reloaded before anything changes it, so its lane is used directly.

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

constexpr GuestAddress kMaxYOffset = static_cast<GuestAddress>(-31016);    // $gp offset: the starting maximum Y
constexpr GuestAddress kMarginOffset = static_cast<GuestAddress>(-31320);  // $gp offset: the z margin C
// Destination masks: x is the high bit, as the instructions encode them.
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;
// Both loops run a fixed 16 and 8 times: 5 + 17 * 3 + 16 * 8 (the first), 3 + 9 * 3 + 8 * 7 (the second) and 218
// after them, so every path retires the same count.
constexpr std::uint32_t kRetired = 488U;

// MAX.S and MIN.S on raw words, as ee_cop1::max_s and min_s compute them: the encodings compare as signed
// integers, in reverse order when both are negative.
std::uint32_t ee_max_s(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

std::uint32_t ee_min_s(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

// One edge point (listing PART D). The x of quad `q` becomes V.x + ((P0.x - Q.x) + t), where
// t = (P1.x - P0.x) * (Q.z - P0.z) / (P1.z - P0.z). `v` is the quad the listing loads as V (the output itself, or
// a B quad). `rz` holds the z difference and `rw` the VU0 product: a3 in the first three edge points, and a0 and
// v1 in the last one, as the listing uses them.
void edge_point(guest::R5900State& cpu, auto& m, const GuestAddress q, const GuestAddress p0,
                const GuestAddress p1, const GuestAddress v, const std::size_t rz, const std::size_t rw) {
    cpu.write_vu0_vector(3, n3::load_quad_any(m, q));                                  // lqc2 $vf3, q
    cpu.write_vu0_vector(5, n3::load_quad_any(m, p0));                                 // lqc2 $vf5, p0
    cpu.write_vu0_vector(2, n3::load_quad_any(m, p1));                                 // lqc2 $vf2, p1
    ee_cop2::vadd_broadcast(cpu, 4, 3, 5, kMaskZ, 2, true);                        // vsubz.z $vf4, $vf3, $vf5z
    const std::uint32_t dz = cpu.read_vu0_vector(4).words[2];                      // stored, then lw'd as rz
    ee_cop2::vadd_broadcast(cpu, 4, 5, 3, kMaskX, 0, true);                        // vsubx.x $vf4, $vf5, $vf3x
    cpu.write_gpr_sign_extended_32(rz, dz);
    ee_cop2::vadd_broadcast(cpu, 3, 2, 5, kMaskZ, 2, true);                        // vsubz.z $vf3, $vf2, $vf5z
    const std::uint32_t edge_z = cpu.read_vu0_vector(3).words[2];                  // stored, then lwc1'd into f0
    ee_cop2::vadd_broadcast(cpu, 3, 2, 5, kMaskX, 0, true);                        // vsubx.x $vf3, $vf2, $vf5x
    cpu.write_fpr_bits(0, edge_z);
    cpu.write_vu0_vector(1, n3::load_quad_any(m, v));                                  // lqc2 $vf1, v
    cpu.write_vu0_vector(2, cpu.read_gpr(rz));                                     // qmtc2 rz, $vf2
    ee_cop2::vmul_broadcast(cpu, 2, 3, 2, kMaskX, 0);                              // vmulx.x $vf2, $vf3, $vf2x
    cpu.write_gpr(rw, cpu.read_vu0_vector(2));                                     // qmfc2 rw, $vf2
    cpu.write_fpr_bits(1, cpu.read_gpr_u32(rw));                                   // mtc1 rw, $f1
    cpu.write_vu0_vector(3, n3::load_quad_any(m, q));                                  // lqc2 $vf3, q
    cpu.write_fpr_bits(0, fpu::div(cpu.read_fpr_bits(1), cpu.read_fpr_bits(0)));   // div.s $f0, $f1, $f0
    cpu.write_gpr_sign_extended_32(rw, cpu.read_fpr_bits(0));                      // mfc1 rw, $f0
    cpu.write_vu0_vector(2, cpu.read_gpr(rw));                                     // qmtc2 rw, $vf2
    ee_cop2::vadd_broadcast(cpu, 2, 4, 2, kMaskX, 0, false);                       // vaddx.x $vf2, $vf4, $vf2x
    ee_cop2::vadd_broadcast(cpu, 1, 1, 2, kMaskX, 0, false);                       // vaddx.x $vf1, $vf1, $vf2x
    ee_cop2::vadd_broadcast(cpu, 3, 0, 1, kMaskX, 0, false);                       // vaddx.x $vf3, $vf0, $vf1x
    n3::store_quad_any(m, q, cpu.read_vu0_vector(3));                                  // sqc2 $vf3, q
}

template <class Context>
n3::Run clamp_frustum_quad_edges(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress quads = call.arg(1);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // Part A: f3 = the highest Y among the 16 B quads (B read from the record each time) and the 8 input points.
    cpu.write_fpr_bits(3, m.read_u32_le(gp + kMaxYOffset));                        // lwc1 $f3, -31016($gp)
    for (std::uint32_t i = 0U; i < 16U; ++i) {
        const GuestAddress base = m.read_u32_le(record);                           // lw $v1, 0($a0)
        cpu.write_fpr_bits(3, ee_max_s(cpu.read_fpr_bits(3), n3::load_quad_any(m, base + (i << 5U)).words[1]));
    }
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        cpu.write_fpr_bits(3, ee_max_s(cpu.read_fpr_bits(3), n3::load_quad_any(m, quads + (i << 4U)).words[1]));
    }

    // Part B: the outputs start as B quads 12..15 (stored in this order), then get their z clamps. vf2.x is C.
    cpu.write_gpr_sign_extended_32(7, m.read_u32_le(record));                      // lw $a3, 0($a0)
    cpu.write_fpr_bits(2, m.read_u32_le(gp + kMarginOffset));                      // lwc1 $f2, -31320($gp)
    cpu.write_gpr(7, n3::load_quad_any(m, cpu.read_gpr_u32(7) + 384U));               // lq $a3, 384($a3)
    cpu.write_gpr_sign_extended_32(8, cpu.read_fpr_bits(2));                       // mfc1 $t0, $f2
    cpu.write_vu0_vector(2, cpu.read_gpr(8));                                      // qmtc2 $t0, $vf2
    n3::store_quad_any(m, quads + 128U, cpu.read_gpr(7));                              // sq $a3, 128($a1)
    cpu.write_gpr_sign_extended_32(7, m.read_u32_le(record));                      // lw $a3, 0($a0)
    cpu.write_gpr(7, n3::load_quad_any(m, cpu.read_gpr_u32(7) + 416U));               // lq $a3, 416($a3)
    n3::store_quad_any(m, quads + 160U, cpu.read_gpr(7));                              // sq $a3, 160($a1)
    cpu.write_gpr_sign_extended_32(7, m.read_u32_le(record));                      // lw $a3, 0($a0)
    cpu.write_gpr(7, n3::load_quad_any(m, cpu.read_gpr_u32(7) + 448U));               // lq $a3, 448($a3)
    n3::store_quad_any(m, quads + 144U, cpu.read_gpr(7));                              // sq $a3, 144($a1)
    cpu.write_gpr_sign_extended_32(7, m.read_u32_le(record));                      // lw $a3, 0($a0)
    cpu.write_gpr(7, n3::load_quad_any(m, cpu.read_gpr_u32(7) + 480U));               // lq $a3, 480($a3)
    n3::store_quad_any(m, quads + 176U, cpu.read_gpr(7));                              // sq $a3, 176($a1)

    // Clamp of Q+128: min(copy z, Q+0 z - C).
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 0U));                         // lqc2 $vf1, 0($a1)
    cpu.write_gpr(7, n3::load_quad_any(m, quads + 128U));                              // lq $a3, 128($a1)
    ee_cop2::vadd_broadcast(cpu, 1, 1, 2, kMaskZ, 0, true);                        // vsubx.z $vf1, $vf1, $vf2x
    cpu.write_fpr_bits(1, cpu.read_gpr(7).words[2]);                               // f1: z of the copy
    cpu.write_fpr_bits(0, cpu.read_vu0_vector(1).words[2]);                        // f0: z of vf1
    cpu.write_fpr_bits(0, ee_min_s(cpu.read_fpr_bits(1), cpu.read_fpr_bits(0)));   // min.s $f0, $f1, $f0
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 128U));                       // lqc2 $vf1, 128($a1)
    cpu.write_gpr_sign_extended_32(8, cpu.read_fpr_bits(0));                       // mfc1 $t0, $f0
    cpu.write_gpr_sign_extended_32(7, cpu.read_fpr_bits(2));                       // mfc1 $a3, $f2
    cpu.write_vu0_vector(2, cpu.read_gpr(8));                                      // qmtc2 $t0, $vf2
    ee_cop2::vadd_broadcast(cpu, 1, 0, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf0, $vf2x
    n3::store_quad_any(m, quads + 128U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 128($a1)
    cpu.write_vu0_vector(2, cpu.read_gpr(7));                                      // qmtc2 $a3, $vf2

    // Clamp of Q+160: min(copy z, Q+32 z - C).
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 32U));                        // lqc2 $vf1, 32($a1)
    cpu.write_gpr(7, n3::load_quad_any(m, quads + 160U));                              // lq $a3, 160($a1)
    ee_cop2::vadd_broadcast(cpu, 1, 1, 2, kMaskZ, 0, true);                        // vsubx.z $vf1, $vf1, $vf2x
    cpu.write_fpr_bits(1, cpu.read_gpr(7).words[2]);                               // f1: z of the copy
    cpu.write_fpr_bits(0, cpu.read_vu0_vector(1).words[2]);                        // f0: z of vf1
    cpu.write_fpr_bits(0, ee_min_s(cpu.read_fpr_bits(1), cpu.read_fpr_bits(0)));   // min.s $f0, $f1, $f0
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 160U));                       // lqc2 $vf1, 160($a1)
    cpu.write_gpr_sign_extended_32(8, cpu.read_fpr_bits(0));                       // mfc1 $t0, $f0
    cpu.write_gpr_sign_extended_32(7, cpu.read_fpr_bits(2));                       // mfc1 $a3, $f2
    cpu.write_vu0_vector(2, cpu.read_gpr(8));                                      // qmtc2 $t0, $vf2
    ee_cop2::vadd_broadcast(cpu, 1, 0, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf0, $vf2x
    n3::store_quad_any(m, quads + 160U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 160($a1)
    cpu.write_vu0_vector(2, cpu.read_gpr(7));                                      // qmtc2 $a3, $vf2

    // Clamp of Q+144: max(copy z, Q+16 z + C).
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 16U));                        // lqc2 $vf1, 16($a1)
    cpu.write_gpr(7, n3::load_quad_any(m, quads + 144U));                              // lq $a3, 144($a1)
    ee_cop2::vadd_broadcast(cpu, 1, 1, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf1, $vf2x
    cpu.write_fpr_bits(1, cpu.read_gpr(7).words[2]);                               // f1: z of the copy
    cpu.write_fpr_bits(0, cpu.read_vu0_vector(1).words[2]);                        // f0: z of vf1
    cpu.write_fpr_bits(0, ee_max_s(cpu.read_fpr_bits(1), cpu.read_fpr_bits(0)));   // max.s $f0, $f1, $f0
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 144U));                       // lqc2 $vf1, 144($a1)
    cpu.write_gpr_sign_extended_32(8, cpu.read_fpr_bits(0));                       // mfc1 $t0, $f0
    cpu.write_gpr_sign_extended_32(7, cpu.read_fpr_bits(2));                       // mfc1 $a3, $f2
    cpu.write_vu0_vector(2, cpu.read_gpr(8));                                      // qmtc2 $t0, $vf2
    ee_cop2::vadd_broadcast(cpu, 1, 0, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf0, $vf2x
    n3::store_quad_any(m, quads + 144U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 144($a1)
    cpu.write_vu0_vector(2, cpu.read_gpr(7));                                      // qmtc2 $a3, $vf2

    // Clamp of Q+176: max(copy z, Q+48 z + C). Leaves a3 = the highest Y and t0..t2 = the same, for part C.
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 48U));                        // lqc2 $vf1, 48($a1)
    cpu.write_gpr(7, n3::load_quad_any(m, quads + 176U));                              // lq $a3, 176($a1)
    ee_cop2::vadd_broadcast(cpu, 1, 1, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf1, $vf2x
    cpu.write_fpr_bits(1, cpu.read_gpr(7).words[2]);                               // f1: z of the copy
    cpu.write_fpr_bits(0, cpu.read_vu0_vector(1).words[2]);                        // f0: z of vf1
    cpu.write_fpr_bits(0, ee_max_s(cpu.read_fpr_bits(1), cpu.read_fpr_bits(0)));   // max.s $f0, $f1, $f0
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 176U));                       // lqc2 $vf1, 176($a1)
    cpu.write_gpr_sign_extended_32(7, cpu.read_fpr_bits(0));                       // mfc1 $a3, $f0
    cpu.write_gpr_sign_extended_32(10, cpu.read_fpr_bits(3));                      // mfc1 $t2, $f3
    cpu.write_gpr_sign_extended_32(9, cpu.read_fpr_bits(3));                       // mfc1 $t1, $f3
    cpu.write_vu0_vector(2, cpu.read_gpr(7));                                      // qmtc2 $a3, $vf2
    cpu.write_gpr_sign_extended_32(8, cpu.read_fpr_bits(3));                       // mfc1 $t0, $f3
    ee_cop2::vadd_broadcast(cpu, 1, 0, 2, kMaskZ, 0, false);                       // vaddx.z $vf1, $vf0, $vf2x
    cpu.write_gpr_sign_extended_32(7, cpu.read_fpr_bits(3));                       // mfc1 $a3, $f3
    n3::store_quad_any(m, quads + 176U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 176($a1)

    // Part C: every output's y becomes the highest Y (vaddx.y broadcasts the value held in t2, t1, t0 and a3).
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 128U));                       // lqc2 $vf1, 128($a1)
    cpu.write_vu0_vector(5, cpu.read_gpr(10));                                     // qmtc2 $t2, $vf5
    ee_cop2::vadd_broadcast(cpu, 1, 0, 5, kMaskY, 0, false);                       // vaddx.y $vf1, $vf0, $vf5x
    n3::store_quad_any(m, quads + 128U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 128($a1)
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 144U));                       // lqc2 $vf1, 144($a1)
    cpu.write_vu0_vector(4, cpu.read_gpr(9));                                      // qmtc2 $t1, $vf4
    ee_cop2::vadd_broadcast(cpu, 1, 0, 4, kMaskY, 0, false);                       // vaddx.y $vf1, $vf0, $vf4x
    n3::store_quad_any(m, quads + 144U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 144($a1)
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 160U));                       // lqc2 $vf1, 160($a1)
    cpu.write_vu0_vector(3, cpu.read_gpr(8));                                      // qmtc2 $t0, $vf3
    ee_cop2::vadd_broadcast(cpu, 1, 0, 3, kMaskY, 0, false);                       // vaddx.y $vf1, $vf0, $vf3x
    n3::store_quad_any(m, quads + 160U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 160($a1)
    cpu.write_vu0_vector(1, n3::load_quad_any(m, quads + 176U));                       // lqc2 $vf1, 176($a1)
    cpu.write_vu0_vector(2, cpu.read_gpr(7));                                      // qmtc2 $a3, $vf2
    ee_cop2::vadd_broadcast(cpu, 1, 0, 2, kMaskY, 0, false);                       // vaddx.y $vf1, $vf0, $vf2x
    n3::store_quad_any(m, quads + 176U, cpu.read_vu0_vector(1));                       // sqc2 $vf1, 176($a1)

    // Part D: the x lanes, one edge point per output (the edge's z difference reaches the output's z first).
    edge_point(cpu, m, quads + 128U, quads + 0U, quads + 16U, quads + 128U, 7U, 7U);
    edge_point(cpu, m, quads + 144U, quads + 0U, quads + 16U, quads + 144U, 7U, 7U);
    cpu.write_gpr_sign_extended_32(7, m.read_u32_le(record));                      // lw $a3, 0($a0)
    edge_point(cpu, m, quads + 160U, quads + 32U, quads + 48U, cpu.read_gpr_u32(7) + 416U, 7U, 7U);
    cpu.write_gpr_sign_extended_32(4, m.read_u32_le(record));                      // lw $a0, 0($a0)
    edge_point(cpu, m, quads + 176U, quads + 32U, quads + 48U, cpu.read_gpr_u32(4) + 480U, 4U, 3U);

    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_003d9360{0x003d9360U, 988U, 0xe0199c6d5b6ee935ULL, "ClampFrustumQuadEdges",
                                                  kRetired, n3::live<&clamp_frustum_quad_edges<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003d9360, clamp_frustum_quad_edges)
