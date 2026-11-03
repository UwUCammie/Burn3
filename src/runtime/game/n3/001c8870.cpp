// 0x001c8870 (a0 = car, a1 = wheel index): UpdateWheelContactFrame. The wheel record W = car + 192 * index + 3296 holds
// the contact state of one wheel (or body joint); W + 184 points at the 4x4 frame (four quad rows, the origin in row 3)
// of the part that wheel steers, and the chassis record (car + 3264) leads through its table (word + 64) to a joint
// byte [table + index + 2780] that picks one of three axes: 0, 1 or any other value (2).
//
//  1. Contact forces. The joint's two anchor quads (table + 32 * index + 3744 / 3760) give a mid point that goes
//     through the frame into world space; with the spring lever (a constant quad times the float at W + 180) rotated
//     by the car's rotation (car + 112 rows) and the car's second frame (car + 516), three torque terms (cross
//     products of the anchor offsets with the lever, the car's velocity quad at car + 208 / 176 and the spring
//     length) accumulate onto the quad at W + 112, and the car-rotated velocity goes to W + 96.
//  2. W + 112 keeps only the component of the chosen axis (the other two lanes are cleared), W + 144 is scaled by a
//     global and IntegrateBodyOrientation (0x001cfe40) integrates the record. The float at W + 176 is split into its
//     positive part (limit above) and its negative part (limit below).
//  3. Joint angle. The chosen frame row is projected on a plane, normalised and compared with a reference direction
//     (three variants: constant references for joint 0 and 1, the frame's own rows for the others); acosf, the sign
//     of a cross product against the hinge axis and a wrap to (-180, 180] give a signed angle in degrees. Outside
//     [limit below, limit above] the frame is rotated about the hinge axis by the excess (a sine/cosine polynomial
//     and Rodrigues' rotation matrix; the product of each frame row with it).
//  4. Re-orthonormalisation. The row of the chosen axis (as read before the integration) stays, the other two rows
//     are rebuilt from cross products, the origin row is restored, and the frame is stored. After a rotation the
//     record's W + 144 quad is scaled again, and when byte car + 533 is 1 the game plays a sound/effect through the
//     non-native routine 0x0027d240, which this native leaves to the original (decline).
//
// The 2290+60 instruction listing has dead VU0 work (row length checks after each rotation), stack copies and the
// 261-266 instruction rotation body in six variants; the native keeps only what reaches memory and counts the
// retired instructions per path.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

N3_CALLEE(001cfe40)
N3_CALLEE(0011e328)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;
using V = Gpr128;

// --- Record layout ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kWheelStride = 192U;
constexpr std::uint32_t kWheelBase = 3296U;       // wheel record = car + 3296 + 192 * index
constexpr std::uint32_t kCarChassis = 3264U;      // car + 3264: chassis record pointer
constexpr std::uint32_t kChassisTable = 64U;      // chassis + 64: joint table pointer
constexpr std::uint32_t kJointAxisByte = 2780U;   // table + index + 2780: axis selector
constexpr std::uint32_t kJointAnchors = 3744U;    // table + 32 * index + 3744 / 3760: anchor quads
constexpr std::uint32_t kCarRotation = 112U;      // car + 112, 128, 144: rotation rows
constexpr std::uint32_t kCarFrame2 = 516U;        // car + 516: second frame pointer
constexpr std::uint32_t kCarVelocity = 176U;      // car + 176 and car + 208: quads added to the torque
constexpr std::uint32_t kCarVelocityCross = 208U;
constexpr std::uint32_t kCarFlag = 533U;          // signed byte: 1 plays the effect

constexpr std::uint32_t kWheelForce = 96U;        // W + 96: car-rotated velocity (quad)
constexpr std::uint32_t kWheelTorque = 112U;      // W + 112: accumulated torque (quad)
constexpr std::uint32_t kWheelSpin = 144U;        // W + 144: quad scaled by globals
constexpr std::uint32_t kWheelSpring = 180U;      // W + 180: spring float
constexpr std::uint32_t kWheelLimit = 176U;       // W + 176: signed limit float
constexpr std::uint32_t kWheelFrame = 184U;       // W + 184: frame pointer

constexpr GuestAddress kLeverQuad = 0x004c9cc0U;  // spring lever direction (lui 0x4d, -25408)
constexpr GuestAddress kHingeAxis1 = 0x004b7db0U; // hinge axis for joint 1 (32176)
constexpr GuestAddress kHingeAxis0 = 0x004b7da0U; // hinge axis for joint 0 (32160)
constexpr GuestAddress kReference = 0x004b7dc0U;  // reference direction for joints 0 and 1 (32192)

// $gp-relative float globals (the offsets are signed).
constexpr std::uint32_t kGpRadians = static_cast<std::uint32_t>(-30992);     // degrees -> radians
constexpr std::uint32_t kGpDegrees = static_cast<std::uint32_t>(-32724);     // radians -> degrees
constexpr std::uint32_t kGpSpringScale = static_cast<std::uint32_t>(-28548);
constexpr std::uint32_t kGpLeverScale = static_cast<std::uint32_t>(-28552);
constexpr std::uint32_t kGpSpinScale = static_cast<std::uint32_t>(-28544);
constexpr std::uint32_t kGpSpinRescale = static_cast<std::uint32_t>(-28556);

constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kTinyLength = 0x2f800000U;   // squared lengths below this skip the joint
constexpr std::uint32_t kHalfTurn = 0x43340000U;     // 180.0
constexpr std::uint32_t kFullTurn = 0x43b40000U;     // 360.0
constexpr GuestAddress kEffectContext = 0x01e78510U; // a0 of the 0x0027d240 call (declined)

// --- Retired instruction counts (listing blocks, delay slots included) ---------------------------------------------

constexpr std::uint32_t kEntryCost = 160U;           // 0x1c8870-0x1c8aec: entry, forces, torque
constexpr std::uint32_t kAxis0Cost = 11U;            // 0x1c9998: joint 0 clears two lanes
constexpr std::uint32_t kAxisTestCost = 3U;          // 0x1c8af0: joint 1 test
constexpr std::uint32_t kAxis1Cost = 11U;            // 0x1c8afc
constexpr std::uint32_t kAxis2Cost = 10U;            // 0x1c8b28
constexpr std::uint32_t kIntegrateCost = 14U;        // 0x1c8b50 up to the limit split, jal included (callee extra)
constexpr std::uint32_t kNegativeLimitCost = 2U;     // 0x1c8b88: limit below zero
constexpr std::uint32_t kSplitCost = 5U;             // 0x1c8b90: frame row 3 saved, joint 0 test
constexpr std::uint32_t kJoint1TestCost = 3U;        // 0x1c8ba4
constexpr std::uint32_t kArmLengthCost = 19U;        // arms 0, 1, 2: row projection and length test
constexpr std::uint32_t kArm01AngleCost = 81U;       // arms 0, 1: normalise, dot, acosf (jal included)
constexpr std::uint32_t kArm2AngleCost = 76U;        // arm 2
constexpr std::uint32_t kSignFlipCost = 4U;          // 360 - angle
constexpr std::uint32_t kWrapTestCost = 6U;          // angle <= 180 test
constexpr std::uint32_t kWrapCost = 4U;              // angle - 360
constexpr std::uint32_t kUpperTestCost = 3U;         // angle <= upper limit
constexpr std::uint32_t kLowerTestCost = 3U;         // angle < lower limit
constexpr std::uint32_t kBodyDirectCost = 266U;      // rotation body that jumps to 0x1c979c itself
constexpr std::uint32_t kBodyCost = 266U;            // the other 266 instruction bodies (plus kJoinCost)
constexpr std::uint32_t kBodyE = 261U;               // arm 2, lower
constexpr std::uint32_t kBodyF = 263U;               // arm 2, upper
constexpr std::uint32_t kJoinCost = 1U;              // 0x1c9798: saved origin row reload
constexpr std::uint32_t kTailCost = 3U;              // 0x1c979c: origin row restored, joint 0 test
constexpr std::uint32_t kTailJoint1Test = 3U;        // 0x1c97a8
constexpr std::uint32_t kRebuild0Cost = 49U;         // 0x1cac38 (joint 0)
constexpr std::uint32_t kRebuild1Cost = 49U;         // 0x1c97b4 (joint 1)
constexpr std::uint32_t kRebuild2Cost = 48U;         // 0x1c9878 (others)
constexpr std::uint32_t kRotatedTestCost = 2U;       // 0x1c9938
constexpr std::uint32_t kRescaleCost = 12U;          // 0x1c9940: rescale W + 144, effect flag test
constexpr std::uint32_t kReturnCost = 11U;           // 0x1cacfc: epilogue
constexpr std::uint32_t kFrameBytes = 7872U;       // the original's stack frame (callees see $sp lowered by it)
constexpr std::uint32_t kCeiling = 20000U;

// --- VU0 FMAC kernels on values (the original's VF registers are dead after the call) ----------------------------------

constexpr V kZero{};
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXy = kX | kY;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

namespace vu {

inline V scalar(const std::uint32_t bits) { return ee_cop2::scalar_vector(bits); }
inline V splat(const std::uint32_t bits) { return V{{bits, bits, bits, bits}}; }

// VADD / VSUB: dest lanes in `mask` = left +/- right, the others from `keep`.
inline V add(const V& keep, const V& left, const V& right, const std::uint8_t mask) {
    return guest::execute_vu0_macro_vector_add_subtract(keep, left, right, mask, false).value;
}
inline V sub(const V& keep, const V& left, const V& right, const std::uint8_t mask) {
    return guest::execute_vu0_macro_vector_add_subtract(keep, left, right, mask, true).value;
}
// VADDx..w / VSUBx..w: right operand broadcast from `lane`.
inline V add_lane(const V& keep, const V& left, const V& right, const std::size_t lane, const std::uint8_t mask) {
    return guest::execute_vu0_macro_broadcast_add_subtract(keep, left, right, mask, lane, false).value;
}
inline V sub_lane(const V& keep, const V& left, const V& right, const std::size_t lane, const std::uint8_t mask) {
    return guest::execute_vu0_macro_broadcast_add_subtract(keep, left, right, mask, lane, true).value;
}
// VMUL / VMULx..w (also the ACC forms with keep = the accumulator).
inline V mul(const V& keep, const V& left, const V& right, const std::uint8_t mask) {
    return guest::execute_vu0_macro_vector_multiply(keep, left, right, mask).value;
}
inline V mul_lane(const V& keep, const V& left, const V& right, const std::size_t lane, const std::uint8_t mask) {
    return guest::execute_vu0_macro_broadcast_multiply(keep, left, right, mask, lane).value;
}
// VMADD / VMADDx..w / VMSUB..: ACC +/- left * right (result into `keep`'s other lanes; for ACC forms keep = acc).
inline V madd(const V& keep, const V& acc, const V& left, const V& right, const std::uint8_t mask,
              const bool subtract = false) {
    return guest::execute_vu0_macro_vector_multiply_accumulate(keep, acc, left, right, mask, subtract).value;
}
inline V madd_lane(const V& keep, const V& acc, const V& left, const V& right, const std::size_t lane,
                   const std::uint8_t mask, const bool subtract = false) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(keep, acc, left, right, mask, lane, subtract).value;
}
inline V abs(const V& value) {
    return V{{value.words[0] & 0x7fffffffU, value.words[1] & 0x7fffffffU, value.words[2] & 0x7fffffffU,
              value.words[3] & 0x7fffffffU}};
}

// Cross product left x right as VOPMULA / VOPMSUB give it (xyz; the w lane stays that of `keep`).
inline V cross(const V& keep, const V& left, const V& right) {
    const V acc = guest::execute_vu0_macro_outer_product_multiply(kZero, left, right).value;
    return guest::execute_vu0_macro_outer_product_subtract(keep, acc, right, left).value;
}

// Scalar lane operations (one FMAC op on bit patterns).
inline std::uint32_t fadd(const std::uint32_t a, const std::uint32_t b) { return add(kZero, scalar(a), scalar(b), kX).words[0]; }
inline std::uint32_t fsub(const std::uint32_t a, const std::uint32_t b) { return sub(kZero, scalar(a), scalar(b), kX).words[0]; }
inline std::uint32_t fmul(const std::uint32_t a, const std::uint32_t b) { return mul(kZero, scalar(a), scalar(b), kX).words[0]; }

// x*x + y*y + z*z the way the original builds it: VMUL, then VADDAy.x (x + y) and VMADDz.x (+ 1.0 * z).
inline std::uint32_t dot3(const V& a, const V& b) {
    const V products = mul(kZero, a, b, kXyz);
    const V partial = add_lane(kZero, products, products, 1U, kX);
    const V one = add_lane(kZero, n3::kVf0, n3::kVf0, 3U, kX);
    return madd_lane(one, partial, one, products, 2U, kX).words[0];
}

// VRSQRT Q, vf0w, v.x and the VMULq.xyz that scales v by it (the w lane is kept).
inline std::uint32_t rsqrt_of(const std::uint32_t sum_of_squares) {
    return ee_cop2::reciprocal_square_root_bits(kOne, sum_of_squares);
}
inline V scale_by_q(const V& v, const std::uint32_t q) { return mul_lane(v, v, scalar(q), 0U, kXyz); }
inline V normalize(const V& v) { return scale_by_q(v, rsqrt_of(dot3(v, v))); }

} // namespace vu

// --- The frame rotation (sine/cosine polynomial, Rodrigues matrix, frame product) ------------------------------------

// Lanes x and y of the VU polynomial: sin(theta) and cos(theta) as the original's range reduction and polynomial give
// them (x holds the argument theta - pi/2, y theta itself).
V sine_cosine(const std::uint32_t theta) {
    constexpr std::uint8_t lanes = kXy;
    const V ones = vu::splat(kOne);
    const std::uint32_t seed_x = vu::fadd(0U, theta);                  // VADDx.x vf2, vf0, theta
    const V seed{{seed_x, vu::fadd(0U, seed_x), 0U, 0U}};              // VADDx.y copies it to y
    const V argument = vu::sub_lane(seed, seed, vu::scalar(0x3fc90fdbU), 0U, kX);   // x: theta - pi/2
    const V magnitude = vu::abs(argument);
    V acc = vu::mul_lane(kZero, magnitude, vu::scalar(0xbe22f983U), 0U, lanes);               // |a| * -1/(2 pi)
    acc = vu::madd_lane(acc, acc, ones, vu::scalar(0x4b400000U), 0U, lanes, true);            // - 1.5 * 2^23
    acc = vu::madd_lane(acc, acc, ones, vu::scalar(0x4b400000U), 0U, lanes);                  // + 1.5 * 2^23
    acc = vu::madd_lane(acc, acc, magnitude, vu::scalar(0xbe22f983U), 0U, lanes, true);       // - |a| * -1/(2 pi)
    V folded = vu::madd_lane(ones, acc, ones, vu::scalar(kHalf), 0U, lanes, true);            // - 0.5
    folded = vu::abs(folded);
    folded = vu::sub_lane(folded, folded, vu::scalar(0x3e800000U), 0U, lanes);                // - 0.25
    const V p2 = vu::mul(kZero, folded, folded, lanes);
    const V p4 = vu::mul(kZero, p2, p2, lanes);
    const V p8 = vu::mul(kZero, p4, p4, lanes);
    const V c3 = vu::mul_lane(kZero, folded, vu::scalar(0xc2992661U), 0U, lanes);
    const V c3p2 = vu::mul(kZero, c3, p2, lanes);
    const V c1 = vu::mul_lane(kZero, folded, vu::scalar(0xc2255de0U), 0U, lanes);
    const V c2 = vu::mul_lane(kZero, folded, vu::scalar(0x42a33457U), 0U, lanes);
    const V c4 = vu::mul_lane(kZero, folded, vu::scalar(0x421ed7b7U), 0U, lanes);
    V poly = vu::mul(kZero, c1, p2, lanes);
    poly = vu::madd(poly, poly, c3p2, p4, lanes);
    poly = vu::madd(poly, poly, c2, p4, lanes);
    poly = vu::madd_lane(poly, poly, folded, vu::scalar(0x40c90fdaU), 0U, lanes);
    return vu::madd(c4, poly, c4, p8, lanes);
}

// The three rows of the rotation about unit axis `n` by `theta` radians, in row-vector form.
struct Rotation {
    V row[3];
};

Rotation rotation_about(const V& n, const std::uint32_t theta) {
    using vu::fadd;
    using vu::fmul;
    using vu::fsub;
    const V sc = sine_cosine(theta);
    const std::uint32_t sin_t = sc.words[0];
    const std::uint32_t one_minus_cos = fadd(0U, fsub(kOne, sc.words[1]));
    std::uint32_t t[3];       // (1 - n_k^2) * (1 - cos)
    std::uint32_t s[3];       // n_k * sin
    for (unsigned k = 0U; k < 3U; ++k) {
        const std::uint32_t square = fmul(n.words[k], n.words[k]);
        t[k] = fmul(fadd(fsub(0U, square), kOne), one_minus_cos);
        s[k] = fmul(n.words[k], sin_t);
    }
    // p = (ny nz, nz nx, nx ny) * (1 - cos)
    const std::uint32_t p[3] = {fmul(fadd(0U, fmul(n.words[1], n.words[2])), one_minus_cos),
                                fmul(fadd(0U, fmul(n.words[2], n.words[0])), one_minus_cos),
                                fmul(fadd(0U, fmul(n.words[0], n.words[1])), one_minus_cos)};
    Rotation r;
    r.row[0] = V{{fadd(0U, fsub(kOne, t[0])), fadd(0U, fadd(p[2], s[2])), fadd(0U, fsub(p[1], s[1])), 0U}};
    r.row[1] = V{{fadd(0U, fsub(p[2], s[2])), fadd(0U, fsub(kOne, t[1])), fadd(0U, fadd(p[0], s[0])), 0U}};
    r.row[2] = V{{fadd(0U, fadd(p[1], s[1])), fadd(0U, fsub(p[0], s[0])), fadd(0U, fsub(kOne, t[2])), 0U}};
    return r;
}

// frame row * rotation: (R0 * x + R1 * y) + R2 * z, the row's w lane kept.
V rotate_row(const Rotation& r, const V& row) {
    V acc = vu::mul_lane(kZero, r.row[0], row, 0U, kXyz);
    acc = vu::madd_lane(acc, acc, r.row[1], row, 1U, kXyz);
    return vu::madd_lane(row, acc, r.row[2], row, 2U, kXyz);
}

// --- Frames and transforms --------------------------------------------------------------------------------------------

struct Frame {
    V row[4];     // x, y, z axes and the origin (row 3), each a quad whose w lane rides along
};

template <class Memory>
Frame load_frame(const Memory& m, const GuestAddress base) {
    return Frame{{n3::load_quad_any(m, base), n3::load_quad_any(m, base + 16U), n3::load_quad_any(m, base + 32U),
                  n3::load_quad_any(m, base + 48U)}};
}

// VMULAx / VMADDAy / VMADDz: (r0 * p.x + r1 * p.y) + r2 * p.z on xyz, the w lane of r2 kept.
V transform_vector(const V& r0, const V& r1, const V& r2, const V& p) {
    V acc = vu::mul_lane(kZero, r0, p, 0U, kXyz);
    acc = vu::madd_lane(acc, acc, r1, p, 1U, kXyz);
    return vu::madd_lane(r2, acc, r2, p, 2U, kXyz);
}

// The same plus the origin row (VMADDAz, then VMADDw with vf0.w = 1.0): the point p in the frame's parent space.
V transform_point(const Frame& frame, const V& p) {
    V acc = vu::mul_lane(kZero, frame.row[0], p, 0U, kXyz);
    acc = vu::madd_lane(acc, acc, frame.row[1], p, 1U, kXyz);
    acc = vu::madd_lane(acc, acc, frame.row[2], p, 2U, kXyz);
    return vu::madd_lane(frame.row[3], acc, frame.row[3], n3::kVf0, 3U, kXyz);
}

// Which rotation body the joint angle selects (the listing has one 261-266 instruction copy of each).
enum class Body { kNone, kA, kB, kC, kD, kE, kF };

// Instructions from the end of the angle tests to the label 0x1c979c, the join included.
constexpr std::uint32_t body_cost(const Body body) {
    switch (body) {
    case Body::kA: return kBodyDirectCost;                    // ends by jumping to 0x1c979c itself
    case Body::kB:
    case Body::kC:
    case Body::kD: return kBodyCost + kJoinCost;
    case Body::kE: return kBodyE + kJoinCost;
    case Body::kF: return kBodyF + kJoinCost;
    default: return kJoinCost;
    }
}

// The three vectors an angle test is made of, and the hinge direction its sign is taken against.
struct AngleProbe {
    V first;       // normalised vector the cross product starts from
    V second;      // normalised vector it ends at (also the keep lane of the cross)
    V sign_axis;   // direction the cross product is dotted with
};

template <class Memory>
void write_frame(Memory& m, const GuestAddress base, const V (&rows)[4]) {
    for (unsigned k = 0U; k < 4U; ++k) {
        n3::store_quad_any(m, base + 16U * k, rows[k]);
    }
}

template <class Context>
n3::Run update_wheel_contact_frame(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress car = call.arg(0U);
    const std::uint32_t index = call.arg(1U);
    const GuestAddress wheel = car + kWheelStride * index + kWheelBase;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto global = [&m, gp](const std::uint32_t offset) { return m.read_u32_le(gp + offset); };
    const auto quad = [&m](const GuestAddress address) { return n3::load_quad_any(m, address); };

    // --- 1. Contact forces and torque ------------------------------------------------------------------------------
    const GuestAddress table = m.read_u32_le(m.read_u32_le(car + kCarChassis) + kChassisTable);
    const std::uint32_t axis = m.read_u8(table + index + kJointAxisByte);
    const GuestAddress joint = table + (index << 5U);
    const GuestAddress frame_address = m.read_u32_le(wheel + kWheelFrame);
    const Frame frame = load_frame(m, frame_address);
    const V anchor_far = quad(joint + kJointAnchors + 16U);
    const V anchor_near = quad(joint + kJointAnchors);
    const V mid = vu::mul_lane(anchor_far, vu::add(anchor_far, anchor_near, anchor_far, kXyz), vu::scalar(kHalf), 0U,
                               kXyz);
    const V anchor_world = transform_point(frame, mid);
    const std::uint32_t spring = m.read_u32_le(wheel + kWheelSpring);
    const V car_x = quad(car + kCarRotation);
    const V car_y = quad(car + kCarRotation + 16U);
    const V car_z = quad(car + kCarRotation + 32U);
    const V lever_dir = quad(kLeverQuad);
    const V lever = vu::mul_lane(lever_dir, lever_dir, vu::scalar(spring), 0U, kXyz);
    const V lever_world = transform_vector(car_x, car_y, car_z, lever);
    const V anchor_offset = vu::sub(lever, anchor_world, frame.row[3], kXyz);
    V torque = quad(wheel + kWheelTorque);
    torque = vu::add(torque, torque, vu::cross(lever_world, anchor_offset, lever_world), kXyz);

    const Frame car_frame = load_frame(m, m.read_u32_le(car + kCarFrame2));
    const V in_car_frame = transform_point(car_frame, anchor_world);
    const V car_offset = vu::sub(car_frame.row[3], in_car_frame, car_frame.row[3], kXyz);
    const V velocity_cross = quad(car + kCarVelocityCross);
    V velocity = vu::cross(velocity_cross, velocity_cross, car_offset);
    velocity = vu::add(velocity, velocity, quad(car + kCarVelocity), kXyz);
    const V wheel_velocity = transform_vector(car_x, car_y, car_z, velocity);
    const V previous_force = quad(wheel + kWheelForce);
    n3::store_quad_any(m, wheel + kWheelForce, wheel_velocity);
    const V force_change = vu::sub(kZero, wheel_velocity, previous_force, kXyz);

    const std::uint32_t spring_scaled = fpu::mul(fpu::neg(spring), global(kGpSpringScale));
    const V arm_offset = vu::sub(frame.row[3], anchor_world, frame.row[3], kXyz);
    const V scaled_change = vu::mul_lane(force_change, force_change, vu::scalar(spring_scaled), 0U, kXyz);
    torque = vu::add(torque, torque, vu::cross(arm_offset, arm_offset, scaled_change), kXyz);

    const std::uint32_t speed_squared = vu::dot3(wheel_velocity, wheel_velocity);
    const std::uint32_t speed = vu::fmul(kOne, ee_cop2::square_root_bits(speed_squared & 0x7fffffffU));
    const V along = vu::mul_lane(wheel_velocity, wheel_velocity, vu::scalar(fpu::neg(speed)), 0U, kXyz);
    const V lever_arm = vu::mul_lane(along, along, vu::scalar(global(kGpLeverScale)), 0U, kXyz);
    torque = vu::add(torque, torque, vu::cross(arm_offset, arm_offset, lever_arm), kXyz);

    // --- 2. The chosen axis keeps its torque lane, W + 144 is scaled and the record integrated ---------------------
    std::uint32_t retired = kEntryCost;
    if (axis == 0U) {
        torque.words[1] = 0U;
        torque.words[2] = 0U;
        retired += kAxis0Cost;
    } else if (axis == 1U) {
        torque.words[0] = 0U;
        torque.words[2] = 0U;
        retired += kAxisTestCost + kAxis1Cost;
    } else {
        torque.words[0] = 0U;
        torque.words[1] = 0U;
        retired += kAxisTestCost + kAxis2Cost;
    }
    n3::store_quad_any(m, wheel + kWheelTorque, torque);
    // The axis row is read here, before the integration (which may rewrite the frame); the origin row after it.
    const GuestAddress axis_row_address = frame_address + 16U * (axis < 2U ? axis : 2U);
    const V axis_row = quad(axis_row_address);
    const V spin = quad(wheel + kWheelSpin);
    n3::store_quad_any(m, wheel + kWheelSpin, vu::mul_lane(spin, spin, vu::scalar(global(kGpSpinScale)), 0U, kXyz));

    call.set_arg(0U, wheel);
    const n3::Run integrated = call.invoke([](auto& machine) { return n3::fn_001cfe40(machine); }, kFrameBytes);
    if (integrated.declined) {
        return call.decline();
    }
    retired += kIntegrateCost + integrated.retired;

    // Limit float -> (upper limit, lower limit): the positive part and the negative part.
    const std::uint32_t limit = m.read_u32_le(wheel + kWheelLimit);
    std::uint32_t upper = limit;
    std::uint32_t lower = 0U;
    if (fpu::lt(limit, 0U)) {
        upper = 0U;
        lower = limit;
        retired += kNegativeLimitCost;
    }
    retired += kSplitCost + (axis != 0U ? kJoint1TestCost : 0U);   // joints 1 and 2 pass the joint 1 test too

    // --- 3. Joint angle and the rotation of the frame ------------------------------------------------------------
    const GuestAddress frame_now = m.read_u32_le(wheel + kWheelFrame);
    Frame rows = load_frame(m, frame_now);
    const V saved_origin = rows.row[3];
    bool rotated = false;                       // the original's s2: a rotation body ran
    Body body = Body::kNone;
    const std::uint32_t degrees = global(kGpDegrees);

    // The vectors the angle is measured between. Joints 0 and 1 project frame row 2 on a plane (x or y cleared) and
    // compare it with a constant reference about a constant hinge; the others compare row 0 with its own
    // projection (y cleared) about row 2.
    AngleProbe probe{};
    bool measured = false;
    retired += kArmLengthCost;
    if (axis < 2U) {
        V projected = rows.row[2];
        projected.words[axis == 0U ? 0U : 1U] = vu::fadd(0U, 0U);
        if (!fpu::lt(vu::dot3(projected, projected), kTinyLength)) {
            probe = {vu::normalize(projected), vu::normalize(quad(kReference)),
                     quad(axis == 0U ? kHingeAxis0 : kHingeAxis1)};
            retired += kArm01AngleCost;
            measured = true;
        }
    } else {
        V projected = rows.row[0];
        projected.words[1] = vu::fadd(0U, 0U);
        if (!fpu::lt(vu::dot3(projected, projected), kTinyLength)) {
            probe = {vu::normalize(rows.row[0]), vu::normalize(projected), rows.row[2]};
            retired += kArm2AngleCost;
            measured = true;
        }
    }

    if (measured) {
        const std::uint32_t cosine = vu::dot3(probe.first, probe.second);
        const V crossed = vu::cross(probe.second, probe.first, probe.second);
        const std::uint32_t sign = vu::dot3(crossed, probe.sign_axis);
        const std::uint32_t clamped =
            ee_cop2::minimum_bits(ee_cop2::maximum_bits(cosine, vu::fsub(0U, kOne)), kOne);   // VMAX -1, VMINI 1
        call.set_float_arg(0U, clamped);
        const n3::Run arc_cosine = call.invoke([](auto& machine) { return n3::fn_0011e328(machine); }, kFrameBytes);
        if (arc_cosine.declined) {
            return call.decline();
        }
        retired += arc_cosine.retired;
        std::uint32_t angle = fpu::mul(degrees, call.float_result());
        if (fpu::lt(sign, 0U)) {
            angle = fpu::sub(kFullTurn, angle);
            retired += kSignFlipCost;
        }
        retired += kWrapTestCost;
        if (!fpu::le(angle, kHalfTurn)) {
            angle = fpu::sub(angle, kFullTurn);
            retired += kWrapCost;
        }
        // Above the upper limit, or below the lower one: rotate by the excess.
        std::uint32_t exceeded = 0U;
        retired += kUpperTestCost;
        if (!fpu::le(angle, upper)) {
            exceeded = upper;
            body = axis == 0U ? Body::kC : (axis == 1U ? Body::kD : Body::kF);
        } else {
            retired += kLowerTestCost;
            if (fpu::lt(angle, lower)) {
                exceeded = lower;
                body = axis == 0U ? Body::kB : (axis == 1U ? Body::kA : Body::kE);
            }
        }
        if (body != Body::kNone) {
            const std::uint32_t theta = fpu::mul(global(kGpRadians), fpu::sub(angle, exceeded));
            const Rotation rotation = rotation_about(vu::normalize(probe.sign_axis), theta);
            for (unsigned k = 0U; k < 3U; ++k) {
                rows.row[k] = rotate_row(rotation, rows.row[k]);
            }
            rotated = true;
        }
    }
    retired += body_cost(body);

    // --- 4. Re-orthonormalise around the chosen axis row and store the frame ---------------------------------------
    // (The rotation body also stores its rotated rows and a rotated origin row; all of them are overwritten here
    // before anything reads them, so only the final rows are stored.)
    retired += kTailCost;
    V out[4];
    out[3] = saved_origin;
    if (axis == 0U) {
        const V second = vu::normalize(vu::cross(rows.row[1], axis_row, rows.row[1]));
        out[0] = axis_row;
        out[1] = vu::cross(second, second, axis_row);
        out[2] = second;
        retired += kRebuild0Cost;
    } else if (axis == 1U) {
        const V first = vu::normalize(vu::cross(rows.row[2], axis_row, rows.row[2]));
        out[0] = first;
        out[1] = axis_row;
        out[2] = vu::cross(first, first, axis_row);
        retired += kTailJoint1Test + kRebuild1Cost;
    } else {
        const V first = vu::normalize(vu::cross(rows.row[1], rows.row[1], axis_row));
        out[0] = first;
        out[1] = vu::cross(first, axis_row, first);
        out[2] = axis_row;
        retired += kTailJoint1Test + kRebuild2Cost;
    }
    write_frame(m, frame_now, out);

    retired += kRotatedTestCost;
    if (!rotated) {
        return call.finish(retired + kReturnCost);
    }
    // After a rotation W + 144 is scaled once more, and a flagged car plays an effect through 0x0027d240.
    retired += kRescaleCost;
    const V spin_after = quad(wheel + kWheelSpin);
    n3::store_quad_any(m, wheel + kWheelSpin,
                       vu::mul_lane(spin_after, spin_after, vu::scalar(fpu::neg(global(kGpSpinRescale))), 0U, kXyz));
    if (static_cast<std::int8_t>(m.read_u8(car + kCarFlag)) != 1) {
        return call.finish(retired + kReturnCost);
    }
    // 0x0027d240 (a0 = kEffectContext, a1 = frame origin, a2 = 0, f12 = 100.0) is not native: the original runs.
    (void)kEffectContext;
    return call.decline();
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001c8870, 9400U, 0x9f5589e7d07675dfULL, "UpdateWheelContactFrame", kCeiling, update_wheel_contact_frame,
         EngineReturn::Nothing)
