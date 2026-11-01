// 0x001bed00 (a0 = car C, f12 = speed, f3 = the rate PickGear keeps when it tests nothing): SetCarSpeed.
// The forward row of the car's transform (the quad at transform + 32) scaled by the speed is the velocity: the
// quad at C+176, whose xyz is each lane added to zero (a negative zero turns positive) and whose w lane is the
// velocity's length. A speed whose exponent field is at most 0x6f (below 2^-15) is flushed to zero first. When the
// velocity's squared length is below 2^-32 the length is zero and the heading (the quad at C+192) is the forward row; else the
// heading is the velocity divided by its length (Q = 1/sqrt, a VU0 multiply) with the forward row's w, and the
// length is sqrt on the FPU. Each wheel record (192 bytes apart, the count is the signed byte at C+4457) gets its
// angular speed at +2172: the length over the radius at +2160. Then the gearbox at C+5192 is picked with PickGear
// (f12 = the radius at C+2544, f13 = the length) and four gearbox words are set.
//
// Retired count: (zero length ? 81 : 98) + (speed flushed ? 1 : 0) + 12 * max(0, wheels) + PickGear's count.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>

N3_CALLEE(001c6f30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

// Car record offsets.
constexpr GuestAddress kTransform = 516U;            // u32: pointer to the transform (rows of 16 bytes)
constexpr GuestAddress kForwardRow = 32U;            // quad: the transform's forward row
constexpr GuestAddress kVelocity = 176U;             // quad: velocity xyz, w = speed
constexpr GuestAddress kHeading = 192U;              // quad: heading xyz, w = the forward row's w
constexpr GuestAddress kSeedRateFirst = 5260U;       // float: averaged into the engine rate before the gear pick
constexpr GuestAddress kSeedRateSecond = 5264U;      // float: likewise
constexpr GuestAddress kWheelRadiusForGears = 2544U; // float: the third wheel's radius
constexpr GuestAddress kWheelCount = 4457U;          // s8
constexpr GuestAddress kWheelStride = 192U;
constexpr GuestAddress kWheelRadius = 2160U;         // wheel record: radius
constexpr GuestAddress kWheelSpin = 2172U;           // wheel record: angular speed
constexpr GuestAddress kGearbox = 5192U;
constexpr GuestAddress kEngineRate = 5276U;          // gearbox + 84 (PickGear writes it too)
constexpr GuestAddress kGear = 5320U;                // gearbox + 128
constexpr GuestAddress kResetFirst = 5328U;          // gearbox + 136: -1.0
constexpr GuestAddress kResetSecond = 5332U;         // gearbox + 140: 1.0
constexpr GuestAddress kResetThird = 5280U;          // gearbox + 88: -1.0
constexpr GuestAddress kResetFourth = 5284U;         // gearbox + 92: 0

// Every word the function reads or writes on both paths (the tail is shared), checked before any store.
constexpr std::array<GuestAddress, 9> kTailWords{kWheelRadiusForGears, kSeedRateFirst, kSeedRateSecond,
                                                 kEngineRate,          kGear,          kResetFirst,
                                                 kResetSecond,         kResetThird,    kResetFourth};

// VU0 destination masks (x is the high bit of the four-bit field).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskW = 1U;
constexpr std::uint8_t kMaskXyz = 14U;

// Float bit patterns and the flush test: (speed bits & 0x7f800000) below 0x37800001 (exponent field at most 0x6f)
// flushes the speed to zero, and a squared length below 2^-32 (0x2f800000) is a zero length.
constexpr std::uint32_t kFlushExponent = 0x7f800000U;
constexpr std::uint32_t kFlushBelow = 0x37800001U;
constexpr std::uint32_t kTinySquared = 0x2f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr unsigned kFrameBytes = 240U;               // addiu $sp, $sp, -240

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 11U;            // 001bed00..001bed28
constexpr std::uint32_t kFlushCost = 1U;             // 001bed2c, only when the speed is flushed
constexpr std::uint32_t kSpeedCost = 24U;            // 001bed30..001bed8c
constexpr std::uint32_t kZeroLengthCost = 11U;       // 001bed90..001bedb8
constexpr std::uint32_t kLengthCost = 28U;           // 001bedbc..001bee28
constexpr std::uint32_t kJoinCost = 6U;              // 001bee2c..001bee40
constexpr std::uint32_t kLoopTestCost = 4U;          // 001bee64..001bee70: once, then after each wheel
constexpr std::uint32_t kWheelCost = 8U;             // 001bee44..001bee60, per wheel
constexpr std::uint32_t kTailCost = 25U;             // 001bee74..001beed4, the jal and its delay slot included

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The access rules of the instructions the function uses. A load or store that would fault is checked before the
// call takes the native path: the dry run declines, and the original runs the call and faults on its own. Words
// need natural alignment; lqc2 and sqc2 need 16-byte alignment; lq and sq ignore the low four address bits.
template <class Memory>
bool word_at(const Memory& memory, const GuestAddress address) {
    return (address & 3U) == 0U && memory.contains(address, 4U);
}
template <class Memory>
bool quad_at(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}
template <class Memory>
bool masked_quad_at(const Memory& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}

// The upper word a 32-bit value leaves in a GPR (mfc1 and lui sign-extend it).
constexpr std::uint32_t sign_word(const std::uint32_t value) noexcept {
    return (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Context>
n3::Run set_car_speed_exact(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& memory = call.memory();
    const GuestAddress car = call.arg(0);

    // Accesses both paths make, checked before anything is stored.
    if (!word_at(memory, car + kTransform)) {
        return call.decline();
    }
    const GuestAddress transform = memory.read_u32_le(car + kTransform);
    if (!quad_at(memory, transform + kForwardRow) || !quad_at(memory, car + kVelocity) ||
        !masked_quad_at(memory, car + kHeading) || !memory.contains(car + kWheelCount, 1U)) {
        return call.decline();
    }
    for (const GuestAddress offset : kTailWords) {
        if (!word_at(memory, car + offset)) {
            return call.decline();
        }
    }
    const auto wheel_total = static_cast<std::int32_t>(static_cast<std::int8_t>(memory.read_u8(car + kWheelCount)));
    for (std::int32_t wheel = 0; wheel < wheel_total; ++wheel) {
        const GuestAddress record = car + static_cast<GuestAddress>(wheel) * kWheelStride;
        if (!word_at(memory, record + kWheelRadius) || !word_at(memory, record + kWheelSpin)) {
            return call.decline();
        }
    }

    // The speed in f12, flushed to zero below 2^-15. mfc1 $v1, $f12 writes its word and sign into v1 and leaves
    // the upper doubleword as the caller had it.
    const std::uint32_t raw_speed = cpu.read_fpr_bits(12U);
    const bool flushed = (raw_speed & kFlushExponent) < kFlushBelow;
    const std::uint32_t speed = flushed ? 0U : raw_speed;
    Gpr128 v1 = cpu.read_gpr(3U);
    v1.words[0] = speed;
    v1.words[1] = sign_word(speed);

    // VU0, in the original's order. VF4.x is the speed; v0 and v1 are the GPR halves of the qmfc2/qmtc2 moves.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                   // vaddw.x vf1, vf0, vf0w
    cpu.write_vu0_vector(4U, v1);                                                  // qmtc2 v1, vf4
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, transform + kForwardRow));  // lqc2 vf2, 32(v0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, car + kVelocity));          // lqc2 vf3, 176(s0)
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 4U, kMaskXyz, 0U);                        // vmulx.xyz vf2, vf2, vf4x
    v1 = cpu.read_vu0_vector(2U);                                                  // qmfc2 v1, vf2
    Gpr128 v0 = cpu.read_vu0_vector(1U);                                           // qmfc2 v0, vf1
    cpu.write_vu0_vector(1U, v1);                                                  // qmtc2 v1, vf1
    ee_cop2::vadd_vector(cpu, 3U, 0U, 1U, kMaskXyz, false);                        // vadd.xyz vf3, vf0, vf1
    ee_cop2::vmul(cpu, 2U, 1U, 1U, kMaskXyz);                                      // vmul.xyz vf2, vf1, vf1
    n3::store_quad_any(memory, car + kVelocity, cpu.read_vu0_vector(3U));          // sqc2 vf3, 176(s0)
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                             // vadday.x acc, vf2, vf2y
    cpu.write_vu0_vector(1U, v0);                                                  // qmtc2 v0, vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);    // vmaddz.x vf1, vf1, vf2z
    v0 = cpu.read_vu0_vector(1U);                                                  // qmfc2 v0, vf1
    const std::uint32_t squared = v0.words[0];                                     // the squared length (vf1.x)

    std::uint32_t retired = kEntryCost + (flushed ? kFlushCost : 0U) + kSpeedCost;

    if (fpu::lt(squared, kTinySquared)) {
        // 001bed90..001bedb8: the velocity keeps its xyz with a zero w, and the heading becomes the forward row.
        cpu.write_vu0_vector(1U, n3::load_quad_any(memory, car + kVelocity));    // lqc2 vf1, 176(s0)
        v0.words[0] = 0U;                                                        // daddu v0, zero, zero
        v0.words[1] = 0U;
        cpu.write_vu0_vector(2U, v0);                                            // qmtc2 v0, vf2
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskW, 3U, true);             // vsubw.w vf1, vf0, vf0w
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kMaskW, 0U, false);            // vaddx.w vf1, vf1, vf2x
        n3::store_quad_any(memory, car + kVelocity, cpu.read_vu0_vector(1U));   // sqc2 vf1, 176(s0)
        n3::store_quad_any(memory, car + kHeading,                               // lw/lq v0 = the forward row
                           n3::load_quad_any(memory, transform + kForwardRow));  // sq v0, 192(s0)
        retired += kZeroLengthCost;
    } else {
        // 001bedbc..001bee28: the heading is the normalised velocity; the w lane takes the length (sqrt on the FPU).
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);            // vaddw.x vf1, vf0, vf0w
        if (!quad_at(memory, car + kHeading)) {                                  // lqc2 vf1, 192(s0), below
            return call.decline();
        }
        n3::store_quad_any(memory, car + kHeading, v1);                          // sq v1, 192(s0)
        v0 = cpu.read_vu0_vector(1U);                                            // qmfc2 v0, vf1
        cpu.write_vu0_vector(1U, n3::load_quad_any(memory, car + kHeading));     // lqc2 vf1, 192(s0)
        ee_cop2::vmul(cpu, 2U, 1U, 1U, kMaskXyz);                                // vmul.xyz vf2, vf1, vf1
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                       // vadday.x acc, vf2, vf2y
        cpu.write_vu0_vector(1U, v0);                                            // qmtc2 v0, vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1, vf1, vf2z
        v1 = cpu.read_vu0_vector(1U);                                            // qmfc2 v1, vf1
        v0 = n3::load_quad_any(memory, car + kHeading);                          // lq v0, 192(s0)
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                // vrsqrt q, vf0w, vf1x
        const std::uint32_t root = fpu::sqrt(v1.words[0]);                       // mtc1, sqrt.s, mfc1
        v1.words[0] = root;
        v1.words[1] = sign_word(root);
        cpu.write_vu0_vector(1U, v0);                                            // qmtc2 v0, vf1
        ee_cop2::vmul_control(cpu, 1U, 1U, kMaskXyz, ee_cop2::kControlQ);        // vmulq.xyz vf1, vf1, q
        v0 = cpu.read_vu0_vector(1U);                                            // qmfc2 v0, vf1
        n3::store_quad_any(memory, car + kHeading, v0);                          // sq v0, 192(s0)
        cpu.write_vu0_vector(1U, n3::load_quad_any(memory, car + kVelocity));    // lqc2 vf1, 176(s0)
        cpu.write_vu0_vector(2U, v1);                                            // qmtc2 v1, vf2
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskW, 3U, true);             // vsubw.w vf1, vf0, vf0w
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, kMaskW, 0U, false);            // vaddx.w vf1, vf1, vf2x
        n3::store_quad_any(memory, car + kVelocity, cpu.read_vu0_vector(1U));   // sqc2 vf1, 176(s0)
        retired += kLengthCost;
    }
    retired += kJoinCost;

    // 001bee2c..001bee40, then the wheels (001bee44..001bee70): the length is the velocity's w lane as memory holds
    // it, and each wheel's angular speed is the length over its radius.
    const std::uint32_t length = n3::load_quad_any(memory, car + kVelocity).words[3];
    const auto wheels = static_cast<std::int32_t>(static_cast<std::int8_t>(memory.read_u8(car + kWheelCount)));
    retired += kLoopTestCost;
    for (std::int32_t wheel = 0; wheel < wheels; ++wheel) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const GuestAddress record = car + static_cast<GuestAddress>(wheel) * kWheelStride;
        if (!word_at(memory, record + kWheelRadius) || !word_at(memory, record + kWheelSpin)) {
            return call.decline();
        }
        const std::uint32_t radius = memory.read_u32_le(record + kWheelRadius);
        memory.write_u32_le(record + kWheelSpin, fpu::div(length, radius));
        retired += kWheelCost + kLoopTestCost;
    }

    // 001bee74..001bee90: the engine rate starts at 0.5 * (the two seed floats); the gear pick overwrites it.
    const std::uint32_t seed_first = memory.read_u32_le(car + kSeedRateFirst);
    const std::uint32_t seed_second = memory.read_u32_le(car + kSeedRateSecond);
    memory.write_u32_le(car + kEngineRate, fpu::mul(kHalf, fpu::add(seed_first, seed_second)));

    // 001bee94..001beea8: PickGear with a0 = the gearbox, f12 = the radius for gears and f13 = the length.
    const std::uint32_t radius_for_gears = memory.read_u32_le(car + kWheelRadiusForGears);
    call.set_arg(0, car + kGearbox);
    call.set_float_arg(0, radius_for_gears);
    call.set_float_arg(1, length);
    const n3::Run pick = call.invoke([](auto& machine) { return n3::fn_001c6f30(machine); }, kFrameBytes);
    if (pick.declined) {
        return call.decline();
    }

    // 001beeac..001beec4: the gear PickGear returned, then the reset words.
    memory.write_u32_le(car + kGear, call.result());
    memory.write_u32_le(car + kResetFirst, kMinusOne);
    memory.write_u32_le(car + kResetSecond, kOne);
    memory.write_u32_le(car + kResetThird, kMinusOne);
    memory.write_u32_le(car + kResetFourth, 0U);

    retired += kTailCost + pick.retired;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001bed00, 472U, 0x60f5bd096e8b9384ULL, "SetCarSpeed", kCeiling, set_car_speed_exact, EngineReturn::Nothing)
