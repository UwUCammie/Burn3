// 0x00255ee0 SpawnCarTrailAtBoxFace (a0 = world W, a1 = car O, a2 = distance curve of 4 floats): decides whether car O
// leaves a ballistic trail, from where and in which direction, then calls SpawnBallisticTrailSamples (0x0025d930).
//
// The car's flag table entry (0x004bc650 + 16 * byte O+400, byte 13) enables the trail. The camera distance d from the
// camera matrix M (O+516) selects a rate from the curve (0 up to curve[0], curve[3] * scale from curve[1], a linear
// blend between), scale being the float at W+0x1f84c; a rate of zero or less ends the call. The six face normals at
// W+0x1f7a0 (16 bytes apart) are dotted with the car position in world space (rows O+112..160); the face whose dot
// is positive and whose distance to the box half-extent |T1 / T2 component| is smallest (below 1000.0) wins, and picks
// two corners of the box between T1 (O+480) and T2 (O+464). Both go to world space with M, are pulled towards the
// camera by the float at gp-30832, and the span between them scaled by the fourth power of the up vector's alignment
// with the face gives the direction D; the end D points along (dot D, up > 0) is the spawn point. Mode 1 (signed byte
// O+533) may push the point a camera unit towards the camera when it is in front of the car (record flag 0x20 in the
// table at 0x005179c0); modes 1..3 also set the second float argument from the height above a second object.
//
// The three quads the spawner reads (point, direction, the plane vector X+B) are written at frame + 2800, 2832 and
// 2880 of the original's 2960-byte frame, as the callee loads them there. Vector arithmetic goes through the VU0
// macro primitives in the original's order (VU rounding, flags, the outer product pair, the square root); the w lanes
// the original never wrote (frame quads 2848 and 2864) are the frame's old words and are carried through to the
// spawner's quads.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>
#include <cstdint>

N3_CALLEE(0025d930)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Guest layout.
constexpr GuestAddress kFlagTable = 0x004bc650U;       // 16-byte entries indexed by byte O+400, byte 13 = enabled
constexpr GuestAddress kFlagEnabled = 13U;
constexpr GuestAddress kRecordTable = 0x005179c0U;     // 2992-byte records, 64-bit flags first
constexpr std::uint32_t kRecordBytes = 2992U;
constexpr std::uint64_t kRecordForwardFlag = 0x20U;
constexpr GuestAddress kFaceNormals = 0x0001f7a0U;     // W: six normals, 16 bytes apart
constexpr GuestAddress kScaleOffset = 0x0001f84cU;     // W: float scale of the rate
constexpr std::uint32_t kPullOffset = static_cast<std::uint32_t>(-30832);  // gp-relative float
constexpr std::uint32_t kFrameBytes = 2960U;

// Car (O) fields.
constexpr GuestAddress kCarCameraMatrix = 516U;
constexpr GuestAddress kCarPosition = 352U;
constexpr GuestAddress kCarUp = 368U;
constexpr GuestAddress kCarCross = 208U;               // A
constexpr GuestAddress kCarOffset = 176U;              // B
constexpr GuestAddress kCarRows = 112U;                // four rows, 16 bytes apart
constexpr GuestAddress kCarBoxFar = 480U;              // T1
constexpr GuestAddress kCarBoxNear = 464U;             // T2
constexpr GuestAddress kCarTableIndex = 400U;
constexpr GuestAddress kCarSpawnerByte = 528U;
constexpr GuestAddress kCarMode = 533U;
constexpr GuestAddress kCarObjectA = 3264U;
constexpr GuestAddress kCarObjectB = 3268U;
constexpr GuestAddress kCarRecordOwner = 5504U;
constexpr GuestAddress kOwnerRecordIndex = 10176U;
constexpr GuestAddress kObjectAQuad = 48U;
constexpr GuestAddress kObjectBQuad = 1168U;

// Offsets of the quads in the original's frame (the spawner reads the first three).
constexpr GuestAddress kFramePoint = 2800U;
constexpr GuestAddress kFrameDirection = 2832U;
constexpr GuestAddress kFrameStaleB = 2848U;
constexpr GuestAddress kFrameStaleA = 2864U;
constexpr GuestAddress kFramePlane = 2880U;

constexpr std::uint32_t kZeroBits = 0U;
constexpr std::uint32_t kInitialBest = 0x447a0000U;    // 1000.0
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;

constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;
constexpr std::uint8_t kMaskXyz = n3::kMaskXyz;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 47U;          // 0x255ee0..0x255f98: frame, plane vector, distance, flag test
constexpr std::uint32_t kDisabledCost = 3U;        // 0x256678, 0x25667c: restore and return
constexpr std::uint32_t kCurveNearCost = 9U;       // d <= curve[0]
constexpr std::uint32_t kCurveFarCost = 13U;       // d >= curve[1]
constexpr std::uint32_t kCurveBlendCost = 20U;
constexpr std::uint32_t kRateTestCost = 5U;        // 0x255fec..0x255ffc
constexpr std::uint32_t kModeTestCost = 4U;        // 0x256000..0x25600c
constexpr std::uint32_t kModeSecondCost = 3U;      // 0x256010..0x256018
constexpr std::uint32_t kModeThirdCost = 3U;       // 0x25601c..0x256024
constexpr std::uint32_t kHeightCost = 11U;         // 0x25664c..0x256674
constexpr std::uint32_t kFaceSetupCost = 123U;     // 0x256028..0x256210: world position and six dots and bounds
constexpr std::uint32_t kPassNonPositive = 11U;    // 0x256214..0x25625c, dot <= 0
constexpr std::uint32_t kPassNotBetter = 17U;
constexpr std::uint32_t kPassBetter = 19U;
constexpr std::uint32_t kAlignmentCost = 28U + 7U; // 0x256260..0x2562e8: alignment dot and the jump table
constexpr std::uint32_t kWorldCost = 62U;          // 0x256450..0x256544
constexpr std::uint32_t kPointBackCost = 3U;       // D reversed, point = B'
constexpr std::uint32_t kPointFrontCost = 2U;      // point = A'
constexpr std::uint32_t kModeOneTestCost = 4U;     // 0x25655c..0x256568
constexpr std::uint32_t kRecordLookupCost = 12U;   // 0x25656c..0x256598
constexpr std::uint32_t kRecordTransformCost = 20U;
constexpr std::uint32_t kPullCost = 8U;
constexpr std::uint32_t kPointCopyCost = 2U;       // 0x25660c (or 0x256614 after the pull)
constexpr std::uint32_t kPointStoreCost = 2U;      // 0x256614..0x256618
constexpr std::uint32_t kCallCost = 6U;            // 0x25661c..0x256630 with the jal and its delay slot
constexpr std::uint32_t kReturnCost = 2U;          // 0x25667c
constexpr std::uint32_t kCeiling = 20000U;

template <class Memory>
Gpr128 quad(const Memory& m, const GuestAddress address) {
    return n3::load_quad_any(m, address);
}

// VU0 lane-copy through the adder: vaddx.x / vaddy.y / vaddz.z fd, vf0, ft (lane = the mask's own lane).
void add_lane(guest::R5900State& cpu, const unsigned fd, const unsigned ft, const std::uint8_t mask,
              const std::size_t lane) {
    ee_cop2::vadd_broadcast(cpu, fd, 0U, ft, mask, lane, false);
}

// vaddw.x vf1, vf0, vf0w: the 1.0 the dot products multiply their z lane by.
void one_in_x(guest::R5900State& cpu) { ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false); }

// dot(a.xyz, b.xyz) in the x lane of VF1: vmul.xyz, vadday.x, vmaddz.x with 1.0 in vf1.x. The first dot of the
// call (one_first false) multiplies before it makes the 1.0, the others after.
std::uint32_t dot_xyz(guest::R5900State& cpu, const Gpr128& a, const Gpr128& b, const bool one_first) {
    cpu.write_vu0_vector(3U, a);
    cpu.write_vu0_vector(2U, b);
    if (one_first) {
        one_in_x(cpu);
    }
    ee_cop2::vmul(cpu, 2U, 3U, 2U, kMaskXyz);
    if (!one_first) {
        one_in_x(cpu);
    }
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    return cpu.read_vu0_vector(1U).words[0];
}

// v * (r0, r1, r2) [+ r3]: vmulax / vmadday / vmaddaz, then vmaddw (translate) or vmaddz; xyz only, the w lane
// of the result is the one of `w_from`.
Gpr128 transform(guest::R5900State& cpu, const Gpr128& v, const std::array<Gpr128, 4>& rows, const bool translate,
                 const Gpr128& w_from) {
    cpu.write_vu0_vector(2U, v);
    cpu.write_vu0_vector(3U, w_from);
    cpu.write_vu0_vector(1U, rows[0]);
    ee_cop2::vmula_broadcast(cpu, 1U, 2U, kMaskXyz, false);
    cpu.write_vu0_vector(1U, rows[1]);
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kMaskXyz, 1U, false, true, false);
    cpu.write_vu0_vector(1U, rows[2]);
    if (translate) {
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kMaskXyz, 2U, false, true, false);
        cpu.write_vu0_vector(1U, rows[3]);
        ee_cop2::vmadd_broadcast(cpu, 3U, 1U, 0U, kMaskXyz, 3U, false, false, false);
    } else {
        ee_cop2::vmadd_broadcast(cpu, 3U, 1U, 2U, kMaskXyz, 2U, false, false, false);
    }
    return cpu.read_vu0_vector(3U);
}

// The two corner quads (frame 2864 = A, 2848 = B) of the box face `face`, built from T1, T2 and the world position.
struct Corners {
    Gpr128 a;
    Gpr128 b;
};

Corners box_corners(guest::R5900State& cpu, const std::uint32_t face, const Gpr128& stale_a, const Gpr128& stale_b,
                    const Gpr128& t1, const Gpr128& t2, const Gpr128& pw) {
    cpu.write_vu0_vector(5U, stale_a);
    cpu.write_vu0_vector(2U, stale_b);
    cpu.write_vu0_vector(3U, pw);
    switch (face) {
    case 0:  // A = (T1.x, Pw.y, T1.z), B = (T1.x, Pw.y, T2.z)
        cpu.write_vu0_vector(4U, t1);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 2U, 4U, kMaskX, 0U);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 4U, kMaskZ, 2U);
        cpu.write_vu0_vector(1U, t2);
        add_lane(cpu, 2U, 3U, kMaskY, 1U);
        add_lane(cpu, 2U, 1U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(2U)};
    case 1:  // A = (T2.x, Pw.y, T1.z), B = (T2.x, Pw.y, T2.z)
        cpu.write_vu0_vector(4U, t2);
        cpu.write_vu0_vector(1U, stale_b);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 1U, 4U, kMaskX, 0U);
        add_lane(cpu, 1U, 3U, kMaskY, 1U);
        add_lane(cpu, 1U, 4U, kMaskZ, 2U);
        cpu.write_vu0_vector(2U, t1);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 2U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(1U)};
    case 2:  // A = (Pw.x, T2.y, T1.z), B = (Pw.x, T2.y, T2.z)
        cpu.write_vu0_vector(4U, pw);
        cpu.write_vu0_vector(1U, stale_b);
        cpu.write_vu0_vector(3U, t2);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 1U, 4U, kMaskX, 0U);
        add_lane(cpu, 1U, 3U, kMaskY, 1U);
        add_lane(cpu, 1U, 3U, kMaskZ, 2U);
        cpu.write_vu0_vector(2U, t1);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 2U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(1U)};
    case 3:  // A = (Pw.x, T1.y, T1.z), B = (Pw.x, T1.y, T2.z)
        cpu.write_vu0_vector(4U, pw);
        cpu.write_vu0_vector(3U, t1);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 2U, 4U, kMaskX, 0U);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 3U, kMaskZ, 2U);
        cpu.write_vu0_vector(1U, t2);
        add_lane(cpu, 2U, 3U, kMaskY, 1U);
        add_lane(cpu, 2U, 1U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(2U)};
    case 4:  // A = (T1.x, Pw.y, T2.z), B = (T2.x, Pw.y, T2.z)
        cpu.write_vu0_vector(2U, t2);
        cpu.write_vu0_vector(1U, stale_b);
        cpu.write_vu0_vector(4U, t1);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 1U, 2U, kMaskX, 0U);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 1U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 2U, kMaskZ, 2U);
        add_lane(cpu, 1U, 2U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(1U)};
    default:  // 5: A = (T1.x, Pw.y, T1.z), B = (T2.x, Pw.y, T1.z)
        cpu.write_vu0_vector(4U, t1);
        cpu.write_vu0_vector(1U, t2);
        add_lane(cpu, 5U, 4U, kMaskX, 0U);
        add_lane(cpu, 2U, 1U, kMaskX, 0U);
        add_lane(cpu, 5U, 3U, kMaskY, 1U);
        add_lane(cpu, 2U, 3U, kMaskY, 1U);
        add_lane(cpu, 5U, 4U, kMaskZ, 2U);
        add_lane(cpu, 2U, 4U, kMaskZ, 2U);
        return {cpu.read_vu0_vector(5U), cpu.read_vu0_vector(2U)};
    }
}

template <class Context>
n3::Run spawn_car_trail(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress world = call.arg(0);
    const GuestAddress car = call.arg(1);
    const GuestAddress curve = call.arg(2);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress camera = m.read_u32_le(car + kCarCameraMatrix);

    // Plane vector X + B with X = A x (P - camera translation), and the distance d = |X + B| (0x255ee0..0x255f90).
    const Gpr128 camera_translation = quad(m, camera + 48U);
    const Gpr128 position = quad(m, car + kCarPosition);
    const Gpr128 up = quad(m, car + kCarUp);
    cpu.write_vu0_vector(2U, position);
    cpu.write_vu0_vector(1U, camera_translation);
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kMaskXyz, true);  // vsub.xyz vf1,vf2,vf1
    cpu.write_vu0_vector(2U, quad(m, car + kCarCross));
    ee_cop2::vopmula(cpu, 2U, 1U);
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);
    cpu.write_vu0_vector(3U, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(2U, quad(m, car + kCarOffset));
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kMaskXyz, false);
    one_in_x(cpu);
    const Gpr128 plane = cpu.read_vu0_vector(3U);
    ee_cop2::vmul(cpu, 2U, 3U, 3U, kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    ee_cop2::vsqrt(cpu, 1U, 0U);
    one_in_x(cpu);
    ee_cop2::vmul_control(cpu, 1U, 1U, kMaskX, ee_cop2::kControlQ);
    const std::uint32_t distance = cpu.read_vu0_vector(1U).words[0];

    std::uint32_t retired = kEntryCost;
    const GuestAddress flags = kFlagTable + (static_cast<GuestAddress>(m.read_u8(car + kCarTableIndex)) << 4U);
    if (m.read_u8(flags + kFlagEnabled) == 0U) {
        return call.finish(retired + kDisabledCost);
    }

    // The rate: 0 near, curve[3] * scale far, a blend between (0x255f9c..0x255fe8).
    const std::uint32_t scale = m.read_u32_le(world + kScaleOffset);
    const std::uint32_t near_x = m.read_u32_le(curve);
    std::uint32_t rate;
    if (fpu::le(distance, near_x)) {
        rate = kZeroBits;
        retired += kCurveNearCost;
    } else {
        const std::uint32_t far_x = m.read_u32_le(curve + 4U);
        const std::uint32_t near_y = m.read_u32_le(curve + 8U);
        const std::uint32_t far_y = m.read_u32_le(curve + 12U);
        if (!fpu::lt(distance, far_x)) {
            rate = fpu::mul(far_y, scale);
            retired += kCurveFarCost;
        } else {
            const std::uint32_t t = fpu::div(fpu::sub(distance, near_x), fpu::sub(far_x, near_x));
            const std::uint32_t blended = fpu::madd(fpu::add(kZeroBits, near_y), fpu::sub(far_y, near_y), t);
            rate = fpu::mul(scale, blended);
            retired += kCurveBlendCost;
        }
    }
    retired += kRateTestCost;
    if (fpu::le(rate, kZeroBits)) {
        return call.finish(retired + kDisabledCost);
    }

    // Modes 1..3: the second float argument is the height of the car above its second object (0x25664c).
    const auto mode = static_cast<std::int8_t>(m.read_u8(car + kCarMode));
    std::uint32_t height = kZeroBits;
    retired += kModeTestCost;
    if (mode != 3) {
        retired += kModeSecondCost;
        if (mode != 2) {
            retired += kModeThirdCost;
        }
    }
    if (mode >= 1 && mode <= 3) {
        const Gpr128 object_a = quad(m, m.read_u32_le(car + kCarObjectA) + kObjectAQuad);
        const Gpr128 object_b = quad(m, m.read_u32_le(car + kCarObjectB) + kObjectBQuad);
        cpu.write_vu0_vector(3U, position);
        cpu.write_vu0_vector(2U, object_a);
        cpu.write_vu0_vector(1U, object_b);
        ee_cop2::vadd_broadcast(cpu, 2U, 3U, 2U, kMaskY, 1U, true);   // vsuby.y vf2,vf3,vf2y
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskY, 3U, false);  // vaddw.y vf1,vf2,vf1w
        height = cpu.read_vu0_vector(1U).words[1];
        retired += kHeightCost;
    }

    // World position and the six face dots (0x256028..0x256210).
    const Gpr128 t1 = quad(m, car + kCarBoxFar);
    const Gpr128 t2 = quad(m, car + kCarBoxNear);
    std::array<Gpr128, 4> car_rows;
    for (unsigned i = 0; i < 4U; ++i) {
        car_rows[i] = quad(m, car + kCarRows + 16U * i);
    }
    const Gpr128 pw = transform(cpu, position, car_rows, true, car_rows[3]);
    std::array<std::uint32_t, 6> dots;
    dots[1] = dot_xyz(cpu, pw, quad(m, world + kFaceNormals + 16U), false);
    dots[0] = dot_xyz(cpu, pw, quad(m, world + kFaceNormals), true);
    for (unsigned k = 2; k < 6U; ++k) {
        dots[k] = dot_xyz(cpu, pw, quad(m, world + kFaceNormals + 16U * k), true);
    }
    // Half-extents the dots are measured against: |T1.x|, |T2.x|, |T2.y|, |T1.y|, |T2.z|, |T1.z|.
    const std::array<std::uint32_t, 6> extents{fpu::abs(t1.words[0]), fpu::abs(t2.words[0]), fpu::abs(t2.words[1]),
                                               fpu::abs(t1.words[1]), fpu::abs(t2.words[2]), fpu::abs(t1.words[2])};
    retired += kFaceSetupCost;

    // The face: the nearest positive dot to its extent (within 1000.0).
    std::uint32_t best = kInitialBest;
    std::uint32_t face = 0U;
    for (std::uint32_t k = 0; k < 6U; ++k) {
        if (fpu::le(dots[k], kZeroBits)) {
            retired += kPassNonPositive;
            continue;
        }
        const std::uint32_t gap = fpu::abs(fpu::sub(dots[k], extents[k]));
        if (fpu::lt(gap, best)) {
            best = gap;
            face = k;
            retired += kPassBetter;
        } else {
            retired += kPassNotBetter;
        }
    }

    // s = -dot(U rotated by the car's 3x3, face normal) (0x256260..0x2562c0), then the jump table's 7 instructions.
    const Gpr128 rotated = transform(cpu, up, car_rows, false, car_rows[2]);
    cpu.write_vu0_vector(2U, rotated);
    cpu.write_vu0_vector(3U, quad(m, world + kFaceNormals + 16U * face));
    one_in_x(cpu);
    ee_cop2::vadd_vector(cpu, 3U, 0U, 3U, kMaskXyz, true);  // vsub.xyz vf3,vf0,vf3
    ee_cop2::vmul(cpu, 2U, 2U, 3U, kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    const std::uint32_t alignment = cpu.read_vu0_vector(1U).words[0];
    retired += kAlignmentCost;

    // The corner quads' w lanes are the frame's old words (never written before this point).
    const Corners corners = box_corners(cpu, face, quad(m, frame + kFrameStaleA), quad(m, frame + kFrameStaleB), t1,
                                        t2, pw);
    retired += (face == 5U) ? 14U : 15U;

    // Both corners to world space (0x256450..0x256544).
    std::array<Gpr128, 4> camera_rows;
    for (unsigned i = 0; i < 4U; ++i) {
        camera_rows[i] = quad(m, camera + 16U * i);
    }
    const Gpr128 a_world = transform(cpu, corners.a, camera_rows, true, corners.a);
    const Gpr128 b_world = transform(cpu, corners.b, camera_rows, true, corners.b);
    const std::uint32_t square = fpu::mul(alignment, alignment);
    const std::uint32_t weight = fpu::mul(square, square);
    cpu.write_vu0_vector(1U, b_world);
    one_in_x(cpu);
    const Gpr128 one_vector = cpu.read_vu0_vector(1U);
    const std::uint32_t pull = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kPullOffset);
    cpu.write_vu0_vector(6U, Gpr128{{weight, 0U, 0U, 0U}});
    cpu.write_vu0_vector(7U, b_world);
    cpu.write_vu0_vector(8U, a_world);
    cpu.write_vu0_vector(3U, camera_translation);
    cpu.write_vu0_vector(2U, Gpr128{{pull, 0U, 0U, 0U}});
    ee_cop2::vadd_vector(cpu, 4U, 3U, 8U, kMaskXyz, true);
    ee_cop2::vmul_broadcast(cpu, 2U, 4U, 2U, kMaskXyz, 0U);
    ee_cop2::vadd_vector(cpu, 8U, 8U, 2U, kMaskXyz, false);
    cpu.write_vu0_vector(1U, Gpr128{{pull, 0U, 0U, 0U}});
    ee_cop2::vadd_vector(cpu, 2U, 3U, 7U, kMaskXyz, true);
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 1U, kMaskXyz, 0U);
    ee_cop2::vadd_vector(cpu, 7U, 7U, 1U, kMaskXyz, false);
    const Gpr128 a_pulled = cpu.read_vu0_vector(8U);
    const Gpr128 b_pulled = cpu.read_vu0_vector(7U);
    cpu.write_vu0_vector(2U, b_pulled);
    cpu.write_vu0_vector(1U, a_pulled);
    ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, kMaskXyz, true);
    ee_cop2::vmul_broadcast(cpu, 3U, 3U, 6U, kMaskXyz, 0U);
    cpu.write_vu0_vector(5U, up);
    ee_cop2::vmul(cpu, 2U, 3U, 5U, kMaskXyz);
    Gpr128 direction = cpu.read_vu0_vector(3U);
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
    cpu.write_vu0_vector(1U, one_vector);
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
    const std::uint32_t side = cpu.read_vu0_vector(1U).words[0];
    retired += kWorldCost;
    Gpr128 point;
    if (fpu::le(side, kZeroBits)) {
        point = b_pulled;
        ee_cop2::vadd_vector(cpu, 1U, 0U, 3U, kMaskXyz, true);  // vsub.xyz vf1,vf0,vf3
        direction = cpu.read_vu0_vector(1U);
        retired += kPointBackCost;
    } else {
        point = a_pulled;
        retired += kPointFrontCost;
    }

    // Mode 1: a point in front of the car's own rows moves a camera unit towards the camera (0x25655c..0x256618).
    retired += kModeOneTestCost;
    if (mode == 1) {
        retired += kRecordLookupCost;
        const std::uint32_t slot = m.read_u32_le(m.read_u32_le(car + kCarRecordOwner) + kOwnerRecordIndex);
        const std::uint32_t offset = slot * kRecordBytes;
        const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(slot)) *
                                     static_cast<std::int64_t>(kRecordBytes);
        cpu.write_lo(0U, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(product))));
        cpu.write_hi(0U, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(product >> 32))));
        const std::uint64_t record = m.read_u64_le(kRecordTable + offset);
        bool pulled = false;
        if ((record & kRecordForwardFlag) != 0U) {
            retired += kRecordTransformCost;
            const Gpr128 q = transform(cpu, point, car_rows, true, car_rows[3]);
            if (!fpu::le(q.words[2], kZeroBits)) {
                retired += kPullCost;
                cpu.write_vu0_vector(3U, point);
                cpu.write_vu0_vector(2U, Gpr128{{kMinusOneBits, 0U, 0U, 0U}});
                cpu.write_vu0_vector(1U, camera_rows[2]);
                ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kMaskXyz, 0U);
                ee_cop2::vadd_vector(cpu, 1U, 3U, 1U, kMaskXyz, false);
                point = cpu.read_vu0_vector(1U);
                pulled = true;
            }
        }
        retired += pulled ? kPointStoreCost : kPointCopyCost + kPointStoreCost;
    }

    // The spawner reads the point, the direction and the plane vector from the original's frame.
    n3::store_quad_any(m, frame + kFramePoint, point);
    n3::store_quad_any(m, frame + kFrameDirection, direction);
    n3::store_quad_any(m, frame + kFramePlane, plane);
    call.set_arg(1, frame + kFramePoint);
    call.set_arg(2, frame + kFrameDirection);
    call.set_arg(3, frame + kFramePlane);
    call.set_arg(4, m.read_u8(car + kCarSpawnerByte));
    call.set_float_arg(0, rate);
    call.set_float_arg(1, height);
    retired += kCallCost;
    const n3::Run spawned = call.invoke([](auto& machine) { return n3::fn_0025d930(machine); }, kFrameBytes);
    if (spawned.declined) {
        return call.decline();
    }
    return call.finish(retired + spawned.retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00255ee0, 1956U, 0xe52ad249501e9b41ULL, "SpawnCarTrailAtBoxFace", kCeiling, spawn_car_trail,
         EngineReturn::Nothing)
