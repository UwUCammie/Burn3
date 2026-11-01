// 0x001beaa0 (a0 = rigid body, a1 = quad A (a world point), a2 = quad B (a force)): ApplyForceAtPoint, returns nothing.
// B is added to the force accumulator at body+272 (xyz; the w lane is kept). C is the quad at (*(body+516)) + 48, and
// the cross product (A - C) x B is formed with the VU0 outer product pair. The torque (A - C') x B is also added into
// the accumulator at body+288 (xyz) only when the mode at body+5412 is neither 1 nor 2, and either |y| is at most the
// float at $gp-28584 or y has the opposite sign to the y lane of the first cross product. y is lane 1 of the quad at
// body+208. C' is read again through the pointer after the force store.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kCentreBlock = 516U;        // lw: pointer to the centre-of-mass block
constexpr GuestAddress kCentreOfMass = 48U;        // quad at that pointer + 48
constexpr GuestAddress kLeverQuad = 208U;          // quad at body + 208; lane 1 is y
constexpr GuestAddress kModeWord = 5412U;          // lw: 1 or 2 means force only
constexpr GuestAddress kForceQuad = 272U;          // force accumulator
constexpr GuestAddress kTorqueQuad = 288U;         // torque accumulator
constexpr std::int32_t kThresholdOffset = -28584;  // lwc1 at $gp + offset: the threshold float
constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kOneBits = 0x3f800000U;    // +1.0f: the sign test compares +1.0f and -1.0f

// Retired instruction counts of the four paths, delay slots included.
constexpr std::uint32_t kModeCost = 35U;           // mode 1 or 2: force only
constexpr std::uint32_t kSmallCost = 49U;          // |y| at most the threshold: force and torque
constexpr std::uint32_t kSameSignCost = 60U;       // |y| above it, same sign: force only
constexpr std::uint32_t kOppositeSignCost = 69U;   // |y| above it, opposite sign: force and torque

template <class Context>
n3::Run apply_force_at_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress body = call.arg(0);

    // The listing's loads: A, B, the centre-of-mass quad C (vf1), and the lever quad at body + 208.
    const guest::Gpr128 point = n3::load_quad_any(m, call.arg(1));
    const guest::Gpr128 force = n3::load_quad_any(m, call.arg(2));
    const GuestAddress centre_block = m.read_u32_le(body + kCentreBlock);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, centre_block + kCentreOfMass));
    const guest::Gpr128 lever = n3::load_quad_any(m, body + kLeverQuad);

    // r = A - C in vf1 (vsub.xyz). cross = r x B: vopmula.xyz ACC, vf2 (r), vf1 (B), then vopmsub.xyz
    // vf1, vf1 (B), vf2 (r) leaves the cross product in vf1.
    cpu.write_vu0_vector(2U, point);
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, force);
    ee_cop2::vopmula(cpu, 2U, 1U);
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);
    const guest::Gpr128 cross = cpu.read_vu0_vector(1U);

    // The torque is skipped (force only) when the mode is 1 or 2, or when |y| is above the threshold and y has the
    // same sign as the cross product's y lane. retired is the exact retired count of the path taken.
    const std::uint32_t y = lever.words[1];
    const std::uint32_t mode = m.read_u32_le(body + kModeWord);
    bool torque = false;
    std::uint32_t retired = kModeCost;
    if (mode - 1U >= 2U) {
        const GuestAddress threshold_address =
            cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kThresholdOffset);
        const std::uint32_t threshold = m.read_u32_le(threshold_address);
        if (fpu::le(fpu::abs(y), threshold)) {
            torque = true;
            retired = kSmallCost;
        } else {
            const bool same_sign = fpu::eq((y & kSignBit) | kOneBits, (cross.words[1] & kSignBit) | kOneBits);
            torque = !same_sign;
            retired = same_sign ? kSameSignCost : kOppositeSignCost;
        }
    }

    if (!torque) {
        // Force only: vf1 = B, vf2 = F + B (vadd.xyz), stored back to body + 272.
        cpu.write_vu0_vector(1U, force);
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, body + kForceQuad));
        ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);
        n3::store_quad_any(m, body + kForceQuad, cpu.read_vu0_vector(2U));
        return call.finish(retired);
    }

    // Force and torque: vf3 = B, vf1 = F + B (vadd.xyz) stored to body + 272 first. Then C' is read through the
    // pointer again, vf1 = A - C', the cross product (A - C') x B is formed with vf3 = B, and vf2 = T + cross
    // (vadd.xyz) is stored to body + 288.
    cpu.write_vu0_vector(3U, force);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, body + kForceQuad));
    cpu.write_vu0_vector(2U, point);
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, false);
    n3::store_quad_any(m, body + kForceQuad, cpu.read_vu0_vector(1U));
    const GuestAddress centre_block_again = m.read_u32_le(body + kCentreBlock);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, centre_block_again + kCentreOfMass));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);
    ee_cop2::vopmula(cpu, 1U, 3U);
    ee_cop2::vopmsub(cpu, 1U, 3U, 1U);
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, body + kTorqueQuad));
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, n3::kMaskXyz, false);
    n3::store_quad_any(m, body + kTorqueQuad, cpu.read_vu0_vector(2U));
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_001beaa0{0x001beaa0U, 300U, 0x645c81cd8b075602ULL, "ApplyForceAtPoint",
                                                  70U, n3::live<&apply_force_at_point<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001beaa0, apply_force_at_point)
