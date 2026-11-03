// 0x001d1670 (a0 = car C): SetAimedSpeed. AimWithinAngle (0x001cb780, limit 30 degrees) decides first: when the car is
// not aimed, the function returns 0 and changes nothing. When it is, the direction is the xyz of the velocity quad at
// C+176 made unit length on VU0 (rsqrt through Q), or the transform's forward row (the quad at u32[C+516] + 32) when
// the squared length is below 2^-32. That direction, its xyz scaled by the float at +9160 of the record at u32[C+5480],
// is the motion delta AddMotionDeltaAndNormalise (0x001beee0) reads from sp + 160: it sets the velocity to the delta
// and its length. The engine rate at C+5276 becomes 0.5 * (the floats at C+5260 and C+5264), then PickGear (0x001c6f30)
// picks the gear for the wheel radius at C+2544 and the velocity's w lane. PickGear overwrites the engine rate, the gear
// and four gearbox words are stored, and the gear is returned.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>

N3_CALLEE(001cb780)
N3_CALLEE(001beee0)
N3_CALLEE(001c6f30)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Car record offsets.
constexpr GuestAddress kRecordPointer = 5480U;       // u32: the record whose float at +9160 scales the direction
constexpr GuestAddress kRecordScale = 9160U;         // float: the scale on the direction (lwc1 f2, in the record)
constexpr GuestAddress kVelocity = 176U;             // quad: velocity xyz, w = speed
constexpr GuestAddress kTransform = 516U;            // u32: pointer to the transform (rows of 16 bytes)
constexpr GuestAddress kForwardRow = 32U;            // quad: the transform's forward row
constexpr GuestAddress kSeedRateFirst = 5260U;       // float: averaged into the engine rate before the gear pick
constexpr GuestAddress kSeedRateSecond = 5264U;      // float: likewise
constexpr GuestAddress kWheelRadius = 2544U;         // float: the radius for the gear pick
constexpr GuestAddress kGearbox = 5192U;
constexpr GuestAddress kGearCount = 5324U;           // gearbox + 132: the signed gear count
constexpr GuestAddress kEngineRate = 5276U;          // gearbox + 84 (PickGear writes it too)
constexpr GuestAddress kGear = 5320U;                // gearbox + 128
constexpr GuestAddress kResetFirst = 5328U;          // gearbox + 136: -1.0
constexpr GuestAddress kResetSecond = 5332U;         // gearbox + 140: 1.0
constexpr GuestAddress kResetThird = 5280U;          // gearbox + 88: -1.0
constexpr GuestAddress kResetFourth = 5284U;         // gearbox + 92: 0

// The fixed-offset words the aimed path reads or writes, checked before anything is stored.
constexpr std::array<GuestAddress, 10> kObjectWords{kSeedRateFirst, kSeedRateSecond, kWheelRadius, kEngineRate,
                                                    kGear,          kResetFirst,     kResetSecond,  kResetThird,
                                                    kResetFourth,   kGearCount};

// VU0 destination mask for x alone (the four-bit field: x is the high bit).
constexpr std::uint8_t kMaskX = 8U;

constexpr std::uint32_t kFrameBytes = 176U;          // addiu $sp, $sp, -176
constexpr GuestAddress kDeltaSlot = 160U;            // sqc2 vf1, 160(sp): the motion delta, read by the callee
constexpr std::uint32_t kAimLimit = 0x41f00000U;     // 30.0
constexpr std::uint32_t kTinySquared = 0x2f800000U;  // 2^-32
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;

// Retired instruction counts from the listing, delay slots included. Each callee's count is added where it is called.
constexpr std::uint32_t kEntryCost = 10U;      // 001d1670..001d1694: the aim call, its delay slot and the beq
constexpr std::uint32_t kLengthCost = 17U;     // 001d1698..001d16d8: the squared length and its test
constexpr std::uint32_t kNearZeroCost = 4U;    // 001d16dc..001d16e8: the transform's forward row
constexpr std::uint32_t kNormaliseCost = 13U;  // 001d16ec..001d171c: the unit direction
constexpr std::uint32_t kJoinCost = 29U;       // 001d1720..001d1790: the delta, the two calls and the stores
constexpr std::uint32_t kEpilogueCost = 4U;    // 001d1794..001d17a0

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The access rules of the instructions the function uses (as set_car_speed_exact in 001bed00.cpp). A load or store that
// would fault is checked before anything is stored: the dry run declines and the original runs the call and faults on
// its own. Words need natural alignment; lqc2 and sqc2 need 16-byte alignment; lq and sq ignore the low four bits.
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

// The upper word a 32-bit value leaves in a GPR (mfc1 sign-extends it; the upper doubleword is kept).
constexpr std::uint32_t sign_word(const std::uint32_t value) noexcept {
    return (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

template <class Context>
n3::Run set_aimed_speed(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress delta_slot = call.stack_pointer() - kFrameBytes + kDeltaSlot;

    // jal AimWithinAngle(C, 1, 30.0): a0 still holds the car, a1 = 1, f12 = 30.0.
    call.set_arg(1, 1U);
    call.set_float_arg(0, kAimLimit);
    const n3::Run aim = call.invoke([](auto& machine) { return n3::fn_001cb780(machine); }, kFrameBytes);
    if (aim.declined) {
        return call.decline();
    }
    if (call.result() == 0U) {
        call.return_value(0U);
        return call.finish(kEntryCost + kEpilogueCost + aim.retired);
    }

    // Accesses the aimed path makes, checked before anything is stored.
    if (!word_at(memory, object + kRecordPointer) || !quad_at(memory, object + kVelocity) ||
        !quad_at(memory, delta_slot)) {
        return call.decline();
    }
    const GuestAddress record = memory.read_u32_le(object + kRecordPointer);
    if (!word_at(memory, record + kRecordScale)) {
        return call.decline();
    }
    for (const GuestAddress offset : kObjectWords) {
        if (!word_at(memory, object + offset)) {
            return call.decline();
        }
    }
    // PickGear keeps the caller's f3 when the gear count is one or less. The original acosf (0x00119c20, reached through
    // AimWithinAngle) leaves a scratch value in f3 that no native reproduces, so such a call is left to the original.
    if (static_cast<std::int32_t>(memory.read_u32_le(object + kGearCount)) <= 1) {
        return call.decline();
    }

    // The VU0 steps in the original's order: vf1.x = 1.0, then vf3 = the velocity, vf2 = the squares of its xyz,
    // ACC.x = x*x + y*y, and vf1.x = the squared length. The record's scale is read between the VU0 moves, as the
    // original reads it.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                                 // vaddw.x vf1,vf0,vf0w
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, object + kVelocity));                     // lqc2 vf3,176(s0)
    const std::uint32_t scale = memory.read_u32_le(record + kRecordScale);                       // lwc1 f2,9160(v0)
    ee_cop2::vmul(cpu, 2U, 3U, 3U, n3::kMaskXyz);                                                // vmul.xyz vf2,vf3,vf3
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                                           // vadday.x acc,vf2,vf2y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);                  // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t squared = cpu.read_vu0_vector(1U).words[0];

    // The direction. The original's second pass over the squares (after the length test) leaves the same vf1.x and ACC,
    // so it is not repeated. Below 2^-32 the forward row of the transform is the direction; else the unit xyz.
    Gpr128 direction{};
    std::uint32_t retired = kEntryCost + kLengthCost + kJoinCost + kEpilogueCost;
    if (fpu::lt(squared, kTinySquared)) {
        if (!word_at(memory, object + kTransform)) {
            return call.decline();
        }
        const GuestAddress transform = memory.read_u32_le(object + kTransform);
        if (!masked_quad_at(memory, transform + kForwardRow)) {
            return call.decline();
        }
        direction = n3::load_quad_any(memory, transform + kForwardRow);
        retired += kNearZeroCost;
    } else {
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                                // vrsqrt q,vf0w,vf1x
        ee_cop2::vmul_control(cpu, 3U, 3U, n3::kMaskXyz, ee_cop2::kControlQ);                    // vmulq.xyz vf3,vf3,q
        direction = cpu.read_vu0_vector(3U);
        retired += kNormaliseCost;
    }

    // The delta: the direction's xyz times the scale. mfc1 sets the lower doubleword of the GPR to the scale and keeps
    // the upper one (the direction's z and w), so vf2 = (scale, its sign word, direction.z, direction.w).
    Gpr128 scaled = direction;
    scaled.words[0] = scale;
    scaled.words[1] = sign_word(scale);
    cpu.write_vu0_vector(2U, scaled);                                                            // qmtc2 v0,vf2
    cpu.write_vu0_vector(1U, direction);                                                         // lqc2 vf1,32(sp)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);                                  // vmulx.xyz vf1,vf1,vf2x
    n3::store_quad_any(memory, delta_slot, cpu.read_vu0_vector(1U));                             // sqc2 vf1,160(sp)

    // jal AddMotionDeltaAndNormalise(C, sp + 160).
    call.set_arg(1, delta_slot);
    const n3::Run motion = call.invoke([](auto& machine) { return n3::fn_001beee0(machine); }, kFrameBytes);
    if (motion.declined) {
        return call.decline();
    }

    // The engine rate is 0.5 * (the seed floats). It is stored before PickGear, which writes the same word.
    const std::uint32_t seed_first = memory.read_u32_le(object + kSeedRateFirst);
    const std::uint32_t seed_second = memory.read_u32_le(object + kSeedRateSecond);
    memory.write_u32_le(object + kEngineRate, fpu::mul(kHalf, fpu::add(seed_first, seed_second)));

    // jal PickGear(C + 5192, f12 = the wheel radius, f13 = the velocity's w lane as the callee left it).
    const std::uint32_t speed = n3::load_quad_any(memory, object + kVelocity).words[3];
    const std::uint32_t radius = memory.read_u32_le(object + kWheelRadius);
    call.set_arg(0, object + kGearbox);
    call.set_float_arg(0, radius);
    call.set_float_arg(1, speed);
    const n3::Run pick = call.invoke([](auto& machine) { return n3::fn_001c6f30(machine); }, kFrameBytes);
    if (pick.declined) {
        return call.decline();
    }

    // The gear PickGear returned, then the reset words.
    const std::uint32_t gear = call.result();
    memory.write_u32_le(object + kGear, gear);
    memory.write_u32_le(object + kResetFirst, kMinusOne);
    memory.write_u32_le(object + kResetSecond, kOne);
    memory.write_u32_le(object + kResetThird, kMinusOne);
    memory.write_u32_le(object + kResetFourth, 0U);
    call.return_value(gear);
    return call.finish(retired + aim.retired + motion.retired + pick.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d1670, 308U, 0xaa86dc3047779813ULL, "SetAimedSpeed", kCeiling, set_aimed_speed, EngineReturn::Integer)
