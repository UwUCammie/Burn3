// 0x003090a0 (a0 = output matrix, 64 bytes; a1 = pointer to the axis quadword; f12 = angle): BuildAxisRotation.
// Writes four rows at a0: three rotation rows built from the axis, the sine/cosine pair of the angle and the
// stale w lanes of the frame, and a last row {0, 0, 0, w}.
//
// The sine pair comes from the same VU polynomial as build_orientation in 0x0026a7e0: the angle is placed in
// the x and y lanes of a quad (z and w are whatever the dead frame holds at +16), the lanes x and z are shifted
// by a quarter turn (1.5707964), a magic-number rounding folds the angle into one period (-0.15915494, 12582912.0,
// 0.5), the triangle wave is folded with abs and 0.25, and an odd polynomial in x, x^2, x^4, x^8 gives the
// sines. The rotation rows then use (1 - cos-like), the axis scaled by the sines and the stale w lanes.
//
// The listing's VU0 registers (vf1..vf7, ACC, I, STATUS and MAC) are observable afterwards, so the arithmetic
// is carried out on the VU0 registers exactly in the listing's order; the quadwords it parks in the stack frame
// and reloads are C++ locals. Frame words it reads before writing (the dead frame below the entry sp, at +16,
// +432, +448, +464 and +480) come from guest memory: their w lanes reach the output rows.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 528U;
constexpr std::uint32_t kStaleAngleQuad = 16U;   // lqc2 vf2
constexpr std::uint32_t kStaleUnitQuad = 432U;   // lqc2 vf4
constexpr std::uint32_t kStaleRow0 = 448U;       // lqc2 vf5
constexpr std::uint32_t kStaleRow1 = 464U;
constexpr std::uint32_t kStaleRow2 = 480U;

constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kQuarterTurn = 0x3fc90fdbU;      // 1.5707964
constexpr std::uint32_t kInverseTurn = 0xbe22f983U;      // -0.15915494
constexpr std::uint32_t kRoundingMagic = 0x4b400000U;    // 12582912.0
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kCoefficient3 = 0xc2992661U;
constexpr std::uint32_t kCoefficient2 = 0xc2255de0U;
constexpr std::uint32_t kCoefficient1 = 0x42a33457U;
constexpr std::uint32_t kCoefficient0 = 0x421ed7b7U;
constexpr std::uint32_t kCoefficientI = 0x40c90fdaU;

// Masks of the VU destination lanes (x is the high bit).
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXz = 10U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::uint8_t kXyzw = 15U;

constexpr std::uint32_t kRetired = 150U;  // straight code, every instruction once

template <class Context>
n3::Run build_axis_rotation(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress output = call.arg(0U);
    const GuestAddress axis_address = call.arg(1U);
    const std::uint32_t angle = cpu.read_fpr_bits(12U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    auto add_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, false);
    };
    auto sub_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, true);
    };
    auto mul_lane = [&](unsigned fd, unsigned fs, unsigned ft, std::uint8_t mask, std::size_t lane) {
        ee_cop2::vmul_broadcast(cpu, fd, fs, ft, mask, lane);
    };
    auto set_i = [&](std::uint32_t bits) { ee_cop2::ctc2(cpu, ee_cop2::kControlI, bits); };
    auto vf = [&](unsigned index) { return cpu.read_vu0_vector(index); };
    auto set_vf = [&](unsigned index, const Gpr128& value) { cpu.write_vu0_vector(index, value); };
    // A word moved into a vector register whose other lanes are never read (only lane x is used).
    auto scalar_quad = [](std::uint32_t bits) { return Gpr128{{bits, 0U, 0U, 0U}}; };

    const Gpr128 axis = n3::load_quad_any(m, axis_address);

    // Angle quad {angle, angle, stale z, stale w}; unit quad {1, 0, stale z, stale w}.
    set_vf(4U, n3::load_quad_any(m, frame + kStaleUnitQuad));
    set_vf(2U, n3::load_quad_any(m, frame + kStaleAngleQuad));
    set_vf(5U, scalar_quad(kOneBits));
    set_vf(1U, scalar_quad(angle));
    add_lane(2U, 0U, 1U, kX, 0U);
    add_lane(2U, 0U, 2U, kY, 0U);
    set_vf(3U, scalar_quad(0U));
    add_lane(4U, 0U, 5U, kX, 0U);
    add_lane(4U, 0U, 3U, kY, 0U);
    const Gpr128 unit = vf(4U);

    // Sine of the angle (lanes y, w) and of the angle shifted by a quarter turn (lanes x, z).
    set_i(kQuarterTurn);
    set_vf(1U, vf(2U));
    ee_cop2::vsub_i(cpu, 1U, 1U, kXz);
    ee_cop2::vabs(cpu, 1U, 1U, kXyzw);
    const Gpr128 magnitude = vf(1U);
    static_cast<void>(ee_cop2::vmax_w(cpu, 1U, 0U, 0U, kXyzw));
    const Gpr128 ones = vf(1U);
    set_i(kInverseTurn);
    set_vf(2U, magnitude);
    ee_cop2::vmula_broadcast(cpu, 2U, 0U, kXyzw, true);
    set_i(kRoundingMagic);
    set_vf(1U, ones);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, kXyzw, 0U, true, true, true);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 0U, kXyzw, 0U, true, true, false);
    set_i(kInverseTurn);
    ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 0U, kXyzw, 0U, true, true, true);
    set_i(kHalf);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyzw, 0U, true, false, true);
    ee_cop2::vabs(cpu, 1U, 1U, kXyzw);
    set_i(kQuarter);
    ee_cop2::vsub_i(cpu, 1U, 1U, kXyzw);
    set_vf(3U, vf(1U));

    // Powers of the folded angle, then the polynomial terms.
    ee_cop2::vmul(cpu, 4U, 3U, 3U, kXyzw);
    ee_cop2::vmul(cpu, 2U, 4U, 4U, kXyzw);
    ee_cop2::vmul(cpu, 1U, 2U, 2U, kXyzw);
    const Gpr128 power8 = vf(1U);
    set_i(kCoefficient3);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    ee_cop2::vmul(cpu, 1U, 1U, 4U, kXyzw);
    const Gpr128 term3 = vf(1U);
    set_i(kCoefficient2);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term2 = vf(1U);
    set_i(kCoefficient1);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term1 = vf(1U);
    set_i(kCoefficient0);
    ee_cop2::vmul_control(cpu, 1U, 3U, kXyzw, ee_cop2::kControlI);
    const Gpr128 term0 = vf(1U);
    set_vf(1U, term2);
    ee_cop2::vmula(cpu, 1U, 4U, kXyzw);
    set_vf(1U, term3);
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, kXyzw, true);
    set_i(kCoefficientI);
    set_vf(1U, term1);
    ee_cop2::vmadd(cpu, 0U, 1U, 2U, kXyzw, true);
    ee_cop2::vmadd_broadcast(cpu, 0U, 3U, 0U, kXyzw, 0U, true, true, false);
    set_vf(2U, term0);
    set_vf(1U, power8);
    ee_cop2::vmadd(cpu, 1U, 2U, 1U, kXyzw, false);
    const Gpr128 sines = vf(1U);

    // The rotation rows from the axis (vf1), the unit quad (vf4) and the sines (vf7).
    set_vf(4U, unit);
    set_vf(7U, sines);
    set_vf(1U, axis);
    sub_lane(2U, 4U, 7U, kX, 1U);
    ee_cop2::vadd_vector(cpu, 6U, 0U, 1U, kXyz, true);
    add_lane(7U, 0U, 2U, kZ, 0U);
    add_lane(6U, 6U, 4U, kXyz, 0U);
    mul_lane(3U, 1U, 7U, kXyz, 0U);
    mul_lane(6U, 6U, 7U, kXyz, 2U);

    set_vf(5U, n3::load_quad_any(m, frame + kStaleRow0));
    sub_lane(1U, 4U, 6U, kX, 0U);
    add_lane(5U, 0U, 1U, kX, 0U);
    add_lane(5U, 0U, 3U, kY, 2U);
    sub_lane(1U, 4U, 3U, kY, 1U);
    add_lane(5U, 0U, 1U, kZ, 1U);
    n3::store_quad_any(m, output, vf(5U));

    set_vf(5U, n3::load_quad_any(m, frame + kStaleRow1));
    sub_lane(2U, 4U, 3U, kY, 2U);
    add_lane(5U, 0U, 2U, kX, 1U);
    sub_lane(1U, 4U, 6U, kX, 1U);
    add_lane(5U, 0U, 1U, kY, 0U);
    add_lane(5U, 0U, 3U, kZ, 0U);
    n3::store_quad_any(m, output + 16U, vf(5U));

    set_vf(5U, n3::load_quad_any(m, frame + kStaleRow2));
    sub_lane(2U, 4U, 3U, kY, 0U);
    add_lane(5U, 0U, 3U, kX, 1U);
    sub_lane(1U, 4U, 6U, kX, 2U);
    add_lane(5U, 0U, 2U, kY, 1U);
    add_lane(5U, 0U, 1U, kZ, 0U);
    n3::store_quad_any(m, output + 32U, vf(5U));

    ee_cop2::vadd_vector(cpu, 4U, 0U, 0U, kXyz, false);
    n3::store_quad_any(m, output + 48U, vf(4U));
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003090a0, 600U, 0x99806cf924ba5ecdULL, "BuildAxisRotation", 150U,
               takedown::runtime::build_axis_rotation, EngineReturn::Nothing)
