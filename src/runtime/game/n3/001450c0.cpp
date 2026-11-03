// 0x001450c0 UpdateSteeringAngle: the per-frame steering/heading angle estimate of a vehicle object (a0 = obj).
//
// Fields of the vehicle object (obj):  +504 mirror flag, +1952 car, +1968 heading axis (quad), +2496 angle (degrees),
// +2504 turn-rate divisor, +2508 previous angle, +2516 swing delta. Fields of the car's driver record (car +9264):
// +176 velocity quad (w = speed), +192 aim direction, +208 target turn quad (word 1 = target rate), +516 pose, +5036
// speed threshold, +5412 mode, +5456 "linked" byte. The pose holds the car's axes: +0 up, +32 forward.
//
// Two ways to get the angle:
//
//  * Driver aim (the record's linked byte is set, and the car is slow enough or in mode 1..2): the angle between the
//    heading axis and the aim direction (forward, or aim + forward unless obj+504 == 1), signed by the side it lies on,
//    divided by the turn rate. Its sign is compared with the target rate and a swing proportional to the difference,
//    the car's speed squared and one of two gains is written (clamped to +-90 degrees); the previous angle is cleared.
//  * No driver (everything else): the angle between the heading axis and a blend of the car's forward axis and the
//    aim direction (global blend factor), in degrees, negated when the heading leans away from the car's up axis, then
//    filtered against the previous angle with a four-threshold hysteresis; the result becomes the previous angle too.
//
// Both ways end in the same instruction count as the original (counts per path below, from the listing's blocks).
// The VU0 sequences are evaluated on values: dot product ((x + y) + 1.0 * z, as the VU orders it), normalise
// (rsqrt of the squared length, multiplied in) and the two-step cross product. The MAC/STATUS/Q publication of the
// original is not reproduced: nothing reads it again, and the VF scratch registers are dead after the return.
// acosf (0x0011e328) is a native callee; GetLinkedRecordByte (0x001419b0) is a six/seven instruction leaf and is
// inlined with its exact count.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0011e328)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using Vec = guest::Gpr128;
namespace fp = n3::fpu;

// Object and record layout.
constexpr GuestAddress kMirrorFlag = 504U;
constexpr GuestAddress kCar = 1952U;
constexpr GuestAddress kHeading = 1968U;
constexpr GuestAddress kAngle = 2496U;
constexpr GuestAddress kTurnDivisor = 2504U;
constexpr GuestAddress kPreviousAngle = 2508U;
constexpr GuestAddress kSwingDelta = 2516U;
constexpr GuestAddress kDriverRecord = 9264U;
constexpr GuestAddress kVelocity = 176U;
constexpr GuestAddress kAim = 192U;
constexpr GuestAddress kTurnTarget = 208U;
constexpr GuestAddress kPosePointer = 516U;
constexpr GuestAddress kSpeedThreshold = 5036U;
constexpr GuestAddress kMode = 5412U;
constexpr GuestAddress kLinkedByte = 5456U;
constexpr GuestAddress kForwardAxis = 32U;
constexpr GuestAddress kBlendFactor = 0x01e754f4U;  // lui 0x01e7 + 21748

// $gp-relative float constants.
constexpr std::int32_t kGpRadiansToDegrees = -32724;
constexpr std::int32_t kGpSpeedGate = -30980;
constexpr std::int32_t kGpBand0 = -30660;
constexpr std::int32_t kGpBand1 = -30656;
constexpr std::int32_t kGpBand2 = -30652;
constexpr std::int32_t kGpBand3 = -30648;
constexpr std::int32_t kGpSwingGainA = -29244;
constexpr std::int32_t kGpSwingGainB = -29240;
constexpr std::int32_t kGpOppositeSignScale = -29236;

// Float bit patterns.
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr std::uint32_t kNinety = 0x42b40000U;
constexpr std::uint32_t kMinusNinety = 0xc2b40000U;
constexpr std::uint32_t kTinyLengthSquared = 0x2f800000U;  // 2^-32
constexpr std::uint32_t kSignBit = 0x80000000U;

constexpr std::uint32_t kFrameBytes = 912U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks (delay slots included, callees excluded).
constexpr std::uint32_t kEntry = 9U;                 // 0x001450c0-0x001450e0
constexpr std::uint32_t kLeafNullRecord = 6U;        // GetLinkedRecordByte, record pointer 0
constexpr std::uint32_t kLeafRecord = 7U;            // GetLinkedRecordByte, record present
constexpr std::uint32_t kGate = 12U;                 // 0x001450e4-0x00145110
constexpr std::uint32_t kGateMode = 5U;              // 0x00145114-0x00145124
constexpr std::uint32_t kAimMirrored = 7U + 2U;      // 0x00145128 + 0x0014514c
constexpr std::uint32_t kAimSum = 7U + 2U + 3U;      // 0x00145128 + 0x00145144 + 0x00145154
constexpr std::uint32_t kDirectionTiny = 15U + 3U;   // 0x00145160 + 0x0014519c: forward axis used as it is
constexpr std::uint32_t kDirectionNormalised = 15U + 15U;  // 0x00145160 + 0x001451a8
constexpr std::uint32_t kAngleBlock = 42U;           // 0x001451e4-0x00145288 (the acosf call included, not its body)
constexpr std::uint32_t kSideRepair = 5U;            // 0x0014528c
constexpr std::uint32_t kSideBlock = 33U;            // 0x001452a0-0x00145320
constexpr std::uint32_t kSideNegate = 1U;            // 0x00145324
constexpr std::uint32_t kRateBlock = 27U;            // 0x00145328-0x00145390
constexpr std::uint32_t kRateOppositeSign = 2U;      // 0x00145394
constexpr std::uint32_t kDeltaBlock = 7U;            // 0x00145508
constexpr std::uint32_t kSwingNonPositiveLow = 3U + 16U;   // target rate <= 0, turn <= rate
constexpr std::uint32_t kSwingNonPositiveHigh = 3U + 17U;  // target rate <= 0, turn > rate
constexpr std::uint32_t kSwingPositive = 2U + 3U + 17U;    // target rate > 0 (either compare result)
constexpr std::uint32_t kClampHigh = 7U;             // 0x001454c0
constexpr std::uint32_t kClampHighStore = 2U;        // 0x0014552c
constexpr std::uint32_t kClampLow = 7U;              // 0x001454dc
constexpr std::uint32_t kClampLowStore = 1U;         // 0x001454f8
constexpr std::uint32_t kClampTail = 1U + 2U + 3U;   // 0x001454fc, 0x00145500, 0x001456f4
constexpr std::uint32_t kBlendBlock = 67U;           // 0x00145534-0x0014563c (the acosf call included)
constexpr std::uint32_t kBlendNegate = 4U;           // 0x001456cc
constexpr std::uint32_t kHysteresisEntry = 7U + 4U;  // 0x00145640 + (0x00145690 | 0x0014565c)
constexpr std::uint32_t kHysteresisPull = 3U;        // 0x001456e8 | 0x001456dc
constexpr std::uint32_t kHysteresisRelaxCheck = 4U;  // 0x001456a0 | 0x0014566c
constexpr std::uint32_t kHysteresisRelaxLow = 4U;    // 0x001456b0
constexpr std::uint32_t kHysteresisRelaxHigh = 5U;   // 0x0014567c
constexpr std::uint32_t kHysteresisTail = 3U + 2U + 3U;  // 0x001456c0, 0x00145500, 0x001456f4

// --- VU0 macro sequences on values -------------------------------------------------------------------------------

constexpr std::uint8_t kMaskX = 8U;

// vmul.xyz; vadday.x; vmaddz.x with vf1.x = 1.0: x*x' + y*y' accumulated first, then 1.0 * z added.
std::uint32_t dot3(const Vec& left, const Vec& right) {
    const Vec product = guest::execute_vu0_macro_vector_multiply(Vec{}, left, right, n3::kMaskXyz).value;
    const Vec accumulator =
        guest::execute_vu0_macro_broadcast_add_subtract(Vec{}, product, product, kMaskX, 1U, false).value;
    constexpr Vec one{{kOne, 0U, 0U, 0U}};
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(Vec{}, accumulator, one, product, kMaskX, 2U, false)
        .value.words[0];
}

// vrsqrt Q = 1 / sqrt(length squared); vmulq.xyz (w stays that of the vector).
Vec scale_to_unit(const Vec& vector, const std::uint32_t length_squared) {
    const std::uint32_t q = guest::ee_cop2::reciprocal_square_root_bits(kOne, length_squared);
    return guest::execute_vu0_macro_broadcast_multiply(vector, vector, guest::ee_cop2::scalar_vector(q),
                                                       n3::kMaskXyz, 0U)
        .value;
}

Vec normalise(const Vec& vector) {
    return scale_to_unit(vector, dot3(vector, vector));
}

// vopmula.xyz; vopmsub.xyz into the second operand: xyz of a x b, w of b.
Vec cross(const Vec& a, const Vec& b) {
    const Vec accumulator = guest::execute_vu0_macro_outer_product_multiply(Vec{}, a, b).value;
    return guest::execute_vu0_macro_outer_product_subtract(b, accumulator, b, a).value;
}

// MAX.S / MIN.S on raw words, ordered as guest::ee_cop1::max_s / min_s order them.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(l < 0 && r < 0 ? std::min(l, r) : std::max(l, r));
}
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(l < 0 && r < 0 ? std::max(l, r) : std::min(l, r));
}

// acosf's argument: the dot product limited to [-1, 1].
std::uint32_t clamp_unit(const std::uint32_t value) {
    return min_bits(max_bits(value, kMinusOne), kOne);
}

// --- the function ------------------------------------------------------------------------------------------------

// acosf through the native callee; false when the callee declined.
template <class Call>
bool arc_cosine(Call& call, const std::uint32_t argument, std::uint32_t& result, std::uint32_t& retired) {
    call.set_float_arg(0U, argument);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_0011e328(m); }, kFrameBytes);
    if (callee.declined) {
        return false;
    }
    result = call.float_result();
    retired += callee.retired;
    return true;
}

template <class Context>
n3::Run update_steering_angle(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress obj = call.arg(0U);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const auto gp_word = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };

    m.write_u32_le(obj + kSwingDelta, 0U);
    const GuestAddress car = m.read_u32_le(obj + kCar);
    const GuestAddress record = m.read_u32_le(car + kDriverRecord);
    const Vec heading = n3::load_quad_any(m, obj + kHeading);

    std::uint32_t retired = kEntry;
    bool driver_aim = false;
    if (record == 0U) {
        retired += kLeafNullRecord;
    } else {
        retired += kLeafRecord;
        driver_aim = m.read_u8(record + kLinkedByte) != 0U;
    }

    std::uint32_t speed = 0U;
    if (driver_aim) {
        speed = n3::load_quad_any(m, record + kVelocity).words[3];
        retired += kGate;
        const bool slow = fp::le(fp::mul(gp_word(kGpSpeedGate), speed), m.read_u32_le(record + kSpeedThreshold));
        if (!slow) {
            retired += kGateMode;
            driver_aim = m.read_u32_le(record + kMode) - 1U < 2U;
        }
    }

    if (!driver_aim) {
        // No driver: the blended-direction angle with hysteresis.
        const std::uint32_t blend = m.read_u32_le(kBlendFactor);
        const std::uint32_t rest = fp::sub(kOne, blend);
        const Vec blended_car = guest::execute_vu0_macro_broadcast_multiply(
                                    Vec{}, n3::load_quad_any(m, car + kForwardAxis), guest::ee_cop2::scalar_vector(blend),
                                    n3::kMaskXyz, 0U)
                                    .value;
        const Vec blended_aim = guest::execute_vu0_macro_broadcast_multiply(
                                    Vec{}, n3::load_quad_any(m, m.read_u32_le(car + kDriverRecord) + kAim),
                                    guest::ee_cop2::scalar_vector(rest), n3::kMaskXyz, 0U)
                                    .value;
        const Vec mix = guest::execute_vu0_macro_vector_add_subtract(Vec{}, blended_aim, blended_car, n3::kMaskXyz, false)
                            .value;
        std::uint32_t acos_degrees = 0U;
        retired += kBlendBlock;
        if (!arc_cosine(call, clamp_unit(dot3(heading, normalise(mix))), acos_degrees, retired)) {
            return call.decline();
        }
        std::uint32_t angle = fp::mul(gp_word(kGpRadiansToDegrees), acos_degrees);
        if (fp::lt(dot3(n3::load_quad_any(m, car), heading), 0U)) {
            angle = fp::neg(angle);
            retired += kBlendNegate;
        }

        const std::uint32_t previous = m.read_u32_le(obj + kPreviousAngle);
        const std::uint32_t change = fp::sub(angle, previous);
        retired += kHysteresisEntry;
        if (fp::le(previous, 0U)) {
            if (!fp::le(change, gp_word(kGpBand2))) {
                angle = fp::add(gp_word(kGpBand2), previous);
                retired += kHysteresisPull;
            }
            retired += kHysteresisRelaxCheck;
            if (fp::lt(change, gp_word(kGpBand3))) {
                angle = fp::sub(previous, gp_word(kGpBand0));
                retired += kHysteresisRelaxLow;
            }
        } else {
            if (!fp::le(change, gp_word(kGpBand0))) {
                angle = fp::add(gp_word(kGpBand0), previous);
                retired += kHysteresisPull;
            }
            retired += kHysteresisRelaxCheck;
            if (fp::lt(change, gp_word(kGpBand1))) {
                angle = fp::sub(previous, gp_word(kGpBand2));
                retired += kHysteresisRelaxHigh;
            }
        }
        m.write_u32_le(obj + kAngle, angle);
        m.write_u32_le(obj + kPreviousAngle, angle);
        return call.finish(retired + kHysteresisTail);
    }

    // Driver aim.
    const GuestAddress pose = m.read_u32_le(record + kPosePointer);
    const Vec forward = n3::load_quad_any(m, pose + kForwardAxis);
    const Vec aim = n3::load_quad_any(m, record + kAim);
    Vec target = aim;
    if (m.read_u32_le(obj + kMirrorFlag) == 1U) {
        retired += kAimMirrored;
    } else {
        target = guest::execute_vu0_macro_vector_add_subtract(forward, aim, forward, n3::kMaskXyz, false).value;
        retired += kAimSum;
    }
    const std::uint32_t target_length_squared = dot3(target, target);
    Vec direction = forward;
    if (fp::lt(target_length_squared, kTinyLengthSquared)) {
        retired += kDirectionTiny;
    } else {
        direction = scale_to_unit(target, target_length_squared);
        retired += kDirectionNormalised;
    }

    std::uint32_t angle = 0U;
    retired += kAngleBlock;
    if (!arc_cosine(call, clamp_unit(dot3(heading, direction)), angle, retired)) {
        return call.decline();
    }
    // The side the heading lies on: along direction x up (direction x forward when that is degenerate).
    Vec side = cross(direction, n3::load_quad_any(m, pose));
    if (fp::lt(dot3(side, side), kTinyLengthSquared)) {
        side = cross(direction, forward);
        retired += kSideRepair;
    }
    retired += kSideBlock;
    if (fp::lt(dot3(normalise(cross(side, direction)), heading), 0U)) {
        angle = fp::neg(angle);
        retired += kSideNegate;
    }

    const std::uint32_t turn = fp::div(angle, m.read_u32_le(obj + kTurnDivisor));
    const std::uint32_t target_rate = n3::load_quad_any(m, record + kTurnTarget).words[1];
    // 1.0 when turn and target rate lean the same way, otherwise the global scale.
    const std::uint32_t side_scale = ((turn ^ target_rate) & kSignBit) == 0U ? kOne : gp_word(kGpOppositeSignScale);
    retired += kRateBlock + ((turn ^ target_rate) & kSignBit ? kRateOppositeSign : 0U) + kDeltaBlock;
    m.write_u32_le(obj + kSwingDelta, fp::sub(target_rate, turn));

    // Four copies of one swing formula; the gain depends on the target's sign and on which of the two is larger.
    const bool rate_not_positive = fp::le(target_rate, 0U);
    const bool turn_not_above = fp::le(turn, target_rate);
    const std::uint32_t gain = gp_word(rate_not_positive == turn_not_above ? kGpSwingGainA : kGpSwingGainB);
    retired += !rate_not_positive ? kSwingPositive : (turn_not_above ? kSwingNonPositiveLow : kSwingNonPositiveHigh);
    angle = fp::mul(side_scale, fp::mul(gain, fp::mul(fp::mul(fp::sub(turn, target_rate), speed), speed)));

    retired += kClampHigh;
    if (!fp::le(angle, kNinety)) {
        angle = kNinety;
        retired += kClampHighStore;
    }
    retired += kClampLow;
    if (fp::lt(angle, kMinusNinety)) {
        angle = kMinusNinety;
        retired += kClampLowStore;
    }
    m.write_u32_le(obj + kAngle, angle);
    m.write_u32_le(obj + kPreviousAngle, 0U);
    return call.finish(retired + kClampTail);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001450c0, 1600U, 0xd2f8dc415387d432ULL, "UpdateSteeringAngle", kCeiling, update_steering_angle,
         EngineReturn::Nothing)
