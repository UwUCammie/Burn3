// 0x00334010 (a0 = body, f12 = step): StepRigidBody. One physics step of a rigid body record S with time step dt.
//
//   1. A skip byte (+1232) set: clear it and do nothing else this frame. A body flagged inactive (signed byte
//      +534 == -1) is left alone too.
//   2. Quadratic drag: force.xyz += direction.xyz * (-1 * speed^2), speed being the w lane of the velocity quad.
//   3. Spin handling before integration, by mode byte (+698):
//        mode != 0: spin.xyz *= the global spin damping factor;
//        mode == 0 and the contact flag (+530) set: A = |pose axis x . body axis (+368)|. When the speed and A are
//          both under the rest threshold the spin is reset to the constant quad at 0x004c9cd0, and when A is under
//          the threshold the spin loses its components along the pose's second and then first axis.
//   4. IntegrateRigidBody (0x001ac470) steps the body.
//   5. Mode 1: the pose translation is moved back by the pose axes times the offset quad at +608, the pose is copied
//      to the four matrix quads at +112 (the matrix cache) and replaced there by its inverse: the transposed 3x3 and
//      the translation -(rows . translation).
//   6. The timer (+692) advances by dt.
//
// Every vector operation runs on the VU0 macro kernels in the original's order, so the MAC/STATUS flags it publishes
// match; the dot products are the original's x + y then + z accumulator sequence with a 1.0 in lane x.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001ac470)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Quad = guest::Gpr128;

// Fields of the body.
constexpr GuestAddress kInverseRow0 = 112U;   // matrix cache quads: rows 0..2 and the translation
constexpr GuestAddress kInverseRow1 = 128U;
constexpr GuestAddress kInverseRow2 = 144U;
constexpr GuestAddress kInverseOrigin = 160U;
constexpr GuestAddress kVelocity = 176U;      // w lane (+188) is the speed
constexpr GuestAddress kSpeed = 188U;
constexpr GuestAddress kDirection = 192U;
constexpr GuestAddress kSpin = 224U;
constexpr GuestAddress kForce = 240U;
constexpr GuestAddress kBodyAxis = 368U;
constexpr GuestAddress kPoseOffset = 516U;    // pointer to the pose: quads +0, +16, +32 axes, +48 translation
constexpr GuestAddress kContactFlag = 530U;
constexpr GuestAddress kStateByte = 534U;     // signed; -1 = inactive
constexpr GuestAddress kOffsetQuad = 608U;
constexpr GuestAddress kTimer = 692U;
constexpr GuestAddress kMode = 698U;
constexpr GuestAddress kSkip = 1232U;

constexpr GuestAddress kPoseAxis1 = 16U;
constexpr GuestAddress kPoseAxis2 = 32U;
constexpr GuestAddress kPoseOrigin = 48U;

// Globals.
constexpr GuestAddress kRestSpin = 0x004c9cd0U;       // quad: the spin when the body is at rest
constexpr GuestAddress kRestThreshold = 30796U;        // lwc1 at -30796($gp)
constexpr GuestAddress kSpinDamping = 31108U;          // lw at -31108($gp)

constexpr std::uint32_t kMinusOne = 0xbf800000U;

// VU0 destination masks and lanes.
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXyz = 14U;
constexpr std::size_t kLaneX = 0U;
constexpr std::size_t kLaneY = 1U;
constexpr std::size_t kLaneZ = 2U;
constexpr std::size_t kLaneW = 3U;

constexpr GuestAddress kFrameBytes = 560U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts, delay slots included (brief.py block table).
constexpr std::uint32_t kSkipPath = 15U;             // skip byte set
constexpr std::uint32_t kInactivePath = 17U;         // state byte -1
constexpr std::uint32_t kDragPrefix = 8U + 4U + 19U; // checks and the drag force
constexpr std::uint32_t kDampedSpin = 7U;            // mode != 0
constexpr std::uint32_t kNoContact = 3U;             // mode == 0, flag clear
constexpr std::uint32_t kContactTests = 3U + 16U;    // mode == 0, flag set: flag test and the dot product
constexpr std::uint32_t kRestSpeedTest = 3U;         // speed under threshold: second compare
constexpr std::uint32_t kRestReset = 3U;             // spin reset to the rest quad
constexpr std::uint32_t kContactRecheck = 3U + 4U;   // flag re-test and the second compare
constexpr std::uint32_t kProjection = 35U;           // spin projected off the pose axes
constexpr std::uint32_t kStep = 7U;                  // from the join: the call and the mode test
constexpr std::uint32_t kInverse = 50U;              // mode 1: pose update
constexpr std::uint32_t kTail = 4U + 5U;             // timer and return

// A VU0 register holding a 32-bit value in lane x (qmtc2 of a GPR; the broadcast operations read only that lane).
Quad scalar(const std::uint32_t bits) {
    Quad value{};
    value.words[0] = bits;
    return value;
}

// The VU0 macro kernels with the accumulator, in the original's instruction order. Each FMAC publishes MAC/STATUS.
struct Vu0 {
    guest::R5900State& cpu;
    Quad acc;

    Quad publish(const guest::Vu0MacroFmacResult& result) const {
        guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
        return result.value;
    }
    // vadd / vsub: dest = left +/- right on the masked lanes.
    Quad add(const Quad& dest, const Quad& left, const Quad& right, const std::uint8_t mask, const bool sub = false) const {
        return publish(guest::execute_vu0_macro_vector_add_subtract(dest, left, right, mask, sub));
    }
    // vaddx..w: dest = left + right.lane.
    Quad add_lane(const Quad& dest, const Quad& left, const Quad& right, const std::uint8_t mask,
                  const std::size_t lane) const {
        return publish(guest::execute_vu0_macro_broadcast_add_subtract(dest, left, right, mask, lane, false));
    }
    Quad mul(const Quad& dest, const Quad& left, const Quad& right, const std::uint8_t mask) const {
        return publish(guest::execute_vu0_macro_vector_multiply(dest, left, right, mask));
    }
    Quad mul_lane(const Quad& dest, const Quad& left, const Quad& right, const std::uint8_t mask,
                  const std::size_t lane) const {
        return publish(guest::execute_vu0_macro_broadcast_multiply(dest, left, right, mask, lane));
    }
    // vadday.x: acc.x = left.x + right.y.
    void adda_lane(const Quad& left, const Quad& right, const std::uint8_t mask, const std::size_t lane) {
        acc = publish(guest::execute_vu0_macro_broadcast_add_subtract(acc, left, right, mask, lane, false));
    }
    // vmulax: acc = left * right.lane.
    void mula_lane(const Quad& left, const Quad& right, const std::uint8_t mask, const std::size_t lane) {
        acc = publish(guest::execute_vu0_macro_broadcast_multiply(acc, left, right, mask, lane));
    }
    // vmadday: acc += left * right.lane.
    void madda_lane(const Quad& left, const Quad& right, const std::uint8_t mask, const std::size_t lane) {
        acc = publish(guest::execute_vu0_macro_broadcast_multiply_accumulate(acc, acc, left, right, mask, lane, false));
    }
    // vmaddz: dest = acc + left * right.lane.
    Quad madd_lane(const Quad& dest, const Quad& left, const Quad& right, const std::uint8_t mask,
                   const std::size_t lane) const {
        return publish(guest::execute_vu0_macro_broadcast_multiply_accumulate(dest, acc, left, right, mask, lane, false));
    }

    // x*x' + y*y' + z*z' for two quads, as the original computes it: the product, acc.x = x + y, then acc.x + 1.0 * z
    // (`unit` carries the 1.0 in lane x). `dest` is the register the product lands in (only its w lane survives).
    std::uint32_t dot3(const Quad& dest, const Quad& left, const Quad& right, const Quad& unit) {
        const Quad product = mul(dest, left, right, kXyz);
        adda_lane(product, product, kX, kLaneY);
        return madd_lane(unit, unit, product, kX, kLaneZ).words[0];
    }
};

// v -= axis * (axis . v), the part of the spin along one pose axis removed (xyz only).
Quad remove_along(Vu0& vu, const Quad& spin, const Quad& axis, const Quad& unit) {
    const std::uint32_t along = vu.dot3(spin, axis, spin, unit);
    const Quad part = vu.mul_lane(axis, axis, scalar(along), kXyz, kLaneX);
    return vu.add(spin, spin, part, kXyz, true);
}

template <class Context>
n3::Run step_rigid_body(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress body = call.arg(0);
    const std::uint32_t step = cpu.read_fpr_bits(12U);

    if (m.read_u8(body + kSkip) != 0U) {
        m.write_u8(body + kSkip, 0U);
        return call.finish(kSkipPath);
    }
    if (static_cast<std::int8_t>(m.read_u8(body + kStateByte)) == -1) {
        return call.finish(kInactivePath);
    }

    Vu0 vu{cpu, cpu.vu0_accumulator()};
    const std::uint32_t speed = m.read_u32_le(body + kSpeed);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // Drag: force.xyz += direction.xyz * (-1 * speed^2).
    const Quad direction = n3::load_quad_any(m, body + kDirection);
    Quad force = n3::load_quad_any(m, body + kForce);
    const std::uint32_t drag = fpu::mul(kMinusOne, fpu::mul(speed, speed));
    const Quad pushed = vu.mul_lane(direction, direction, scalar(drag), kXyz, kLaneX);
    force = vu.add(force, force, pushed, kXyz);
    n3::store_quad_any(m, body + kForce, force);

    std::uint32_t retired = kDragPrefix;
    if (m.read_u8(body + kMode) != 0U) {
        const Quad spin = n3::load_quad_any(m, body + kSpin);
        const Quad damped = vu.mul_lane(spin, spin, scalar(m.read_u32_le(gp - kSpinDamping)), kXyz, kLaneX);
        n3::store_quad_any(m, body + kSpin, damped);
        retired += kDampedSpin;
    } else if (m.read_u8(body + kContactFlag) == 0U) {
        retired += kNoContact;
    } else {
        // How square the body's axis is to the pose's first axis: A = |pose.axis0 . body axis|.
        const GuestAddress pose = m.read_u32_le(body + kPoseOffset);
        const Quad unit = vu.add_lane(force, n3::kVf0, n3::kVf0, kX, kLaneW);  // lane x = 1.0
        const Quad pose_axis0 = n3::load_quad_any(m, pose);
        const Quad body_axis = n3::load_quad_any(m, body + kBodyAxis);
        const std::uint32_t across = fpu::abs(vu.dot3(pose_axis0, pose_axis0, body_axis, unit));
        const std::uint32_t threshold = m.read_u32_le(gp - kRestThreshold);
        retired += kContactTests;
        if (fpu::lt(speed, threshold)) {
            retired += kRestSpeedTest;
            if (fpu::lt(across, threshold)) {
                n3::store_quad_any(m, body + kSpin, n3::load_quad_any(m, kRestSpin));
                retired += kRestReset;
            }
        }
        // The contact flag is still set (the stores above cannot reach it).
        retired += kContactRecheck;
        if (fpu::lt(across, threshold)) {
            const GuestAddress axes = m.read_u32_le(body + kPoseOffset);
            Quad spin = n3::load_quad_any(m, body + kSpin);
            const Quad axis2 = n3::load_quad_any(m, axes + kPoseAxis2);
            const Quad unit2 = vu.add_lane(unit, n3::kVf0, n3::kVf0, kX, kLaneW);
            spin = remove_along(vu, spin, axis2, unit2);
            n3::store_quad_any(m, body + kSpin, spin);
            const Quad unit3 = vu.add_lane(unit2, n3::kVf0, n3::kVf0, kX, kLaneW);
            const GuestAddress axes2 = m.read_u32_le(body + kPoseOffset);
            const Quad axis1 = n3::load_quad_any(m, axes2 + kPoseAxis1);
            spin = n3::load_quad_any(m, body + kSpin);
            spin = remove_along(vu, spin, axis1, unit3);
            n3::store_quad_any(m, body + kSpin, spin);
            retired += kProjection;
        }
    }
    cpu.set_vu0_accumulator(vu.acc);

    // Integrate.
    call.set_arg(0, body);
    call.set_float_arg(0, step);
    const n3::Run stepped = call.invoke([](auto& memory) { return n3::fn_001ac470(memory); }, kFrameBytes);
    if (stepped.declined) {
        return call.decline();
    }
    retired += kStep + stepped.retired;

    vu.acc = cpu.vu0_accumulator();
    if (m.read_u8(body + kMode) == 1U) {
        const GuestAddress pose = m.read_u32_le(body + kPoseOffset);
        const Quad offset = n3::load_quad_any(m, body + kOffsetQuad);
        const Quad axis0 = n3::load_quad_any(m, pose);
        const Quad axis1 = n3::load_quad_any(m, pose + kPoseAxis1);
        const Quad axis2 = n3::load_quad_any(m, pose + kPoseAxis2);
        vu.mula_lane(axis0, offset, kXyz, kLaneX);
        vu.madda_lane(axis1, offset, kXyz, kLaneY);
        const Quad moved = vu.madd_lane(axis2, axis2, offset, kXyz, kLaneZ);
        const Quad origin = n3::load_quad_any(m, pose + kPoseOrigin);
        n3::store_quad_any(m, pose + kPoseOrigin, vu.add(origin, origin, moved, kXyz, true));

        // Copy the pose to the matrix cache, then transpose its 3x3 in place.
        const GuestAddress copy = m.read_u32_le(body + kPoseOffset);
        n3::store_quad_any(m, body + kInverseRow0, n3::load_quad_any(m, copy));
        n3::store_quad_any(m, body + kInverseRow1, n3::load_quad_any(m, copy + kPoseAxis1));
        n3::store_quad_any(m, body + kInverseRow2, n3::load_quad_any(m, copy + kPoseAxis2));
        n3::store_quad_any(m, body + kInverseOrigin, n3::load_quad_any(m, copy + kPoseOrigin));

        const Quad row1 = n3::load_quad_any(m, body + kInverseRow1);
        Quad row0 = n3::load_quad_any(m, body + kInverseRow0);
        const Quad row2 = n3::load_quad_any(m, body + kInverseRow2);
        const Quad old_row0 = n3::load_quad_any(m, body + kInverseRow0);
        row0 = vu.add_lane(row0, n3::kVf0, row1, kY, kLaneX);
        row0 = vu.add_lane(row0, n3::kVf0, row2, kZ, kLaneX);
        n3::store_quad_any(m, body + kInverseRow0, row0);
        Quad column1 = n3::load_quad_any(m, body + kInverseRow1);
        column1 = vu.add_lane(column1, n3::kVf0, old_row0, kX, kLaneY);
        column1 = vu.add_lane(column1, n3::kVf0, row2, kZ, kLaneY);
        n3::store_quad_any(m, body + kInverseRow1, column1);
        Quad column2 = n3::load_quad_any(m, body + kInverseRow2);
        column2 = vu.add_lane(column2, n3::kVf0, old_row0, kX, kLaneZ);
        column2 = vu.add_lane(column2, n3::kVf0, row1, kY, kLaneZ);
        n3::store_quad_any(m, body + kInverseRow2, column2);

        // The inverse translation: -(row0 * t.x + row1 * t.y + row2 * t.z).
        const Quad origin_in = n3::load_quad_any(m, body + kInverseOrigin);
        const Quad first = n3::load_quad_any(m, body + kInverseRow0);
        const Quad part_z = vu.mul_lane(row1, column2, origin_in, kXyz, kLaneZ);
        const Quad part_x = vu.mul_lane(column2, first, origin_in, kXyz, kLaneX);
        Quad second = n3::load_quad_any(m, body + kInverseRow1);
        second = vu.mul_lane(second, second, origin_in, kXyz, kLaneY);
        Quad sum = vu.add(second, part_x, second, kXyz);
        sum = vu.add(sum, sum, part_z, kXyz);
        sum = vu.add(sum, n3::kVf0, sum, kXyz, true);
        n3::store_quad_any(m, body + kInverseOrigin, sum);
        retired += kInverse;
    }
    cpu.set_vu0_accumulator(vu.acc);

    // The timer.
    m.write_u32_le(body + kTimer, fpu::add(m.read_u32_le(body + kTimer), step));
    return call.finish(retired + kTail);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00334010, 692U, 0xca3acfa1c2406ecbULL, "StepRigidBody", kCeiling, step_rigid_body, EngineReturn::Nothing)
