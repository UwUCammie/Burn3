// 0x002b8920 (a0 = contact object P): ConvexContactPushApart. P holds two convex shapes, A = u32[P+0] and
// B = u32[P+240], and for each a world matrix and its inverse (four 16-byte rows; rotation rows 0..2, translation
// row 3): world A at P+48, inverse A at P+112, world B at P+288, inverse B at P+352. A shape descriptor has the
// plane array at +4 (normal xyz, offset w, 16 bytes each), the point array at +8, the point count (u8) at +24 and
// the plane count (u8) at +25; +0 is passed on to ClipRayAgainstPlanes.
//
// The function
//   1. composes the two shape-to-shape matrices (A into B's space, B into A's space) and transforms both point
//      sets with them into frame arrays (the callees read these by pointer, so they live in guest memory);
//   2. clips A's edges (ClipEdgesAgainstPlane 0x002b87e0) with the normalised direction of one matrix
//      translation and keeps the clipped points inside B's planes (ClipSegmentsAndDedupe 0x002b7a00), then does
//      the same the other way round; fewer than two points in all means no contact (returns 0);
//   3. moves the points to world space, takes the centre of their bounding box and puts it into each shape's
//      space;
//   4. finds, per shape, the plane nearest to that centre (first minimum wins) and rotates its normal to world
//      space; the contact normal is the normalised difference of the two normals (a difference below 2^-32 in
//      squared length means no contact, returns 0) and goes with the world centre to P+496 and P+512;
//   5. ClipRayAgainstPlanes (0x002b7bf0) finds the plane each shape is hit through, the penetration depth behind
//      that plane is clamped at 0 and negated, and both translation rows (P+96, P+336) are pushed along the
//      plane normals; P+484 = 1 and the function returns 1.
//
// All vector maths is the VU0 macro sequence of the listing on raw lane words (guest::execute_vu0_macro_*): xyz
// destination masks keep the destination's old w lane, which matters where a whole quad is stored. The VU0
// registers, ACC, MAC and STATUS the original leaves behind are temporaries and are not written back; the frame
// words the callees do not read (compose temporaries, byte-copy buffers, the bounding box) are not stored either.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002b87e0)
N3_CALLEE(002b7a00)
N3_CALLEE(002b7bf0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Contact object P.
constexpr GuestAddress kShapeA = 0U;
constexpr GuestAddress kShapeB = 240U;
constexpr GuestAddress kWorldA = 48U;
constexpr GuestAddress kInverseA = 112U;
constexpr GuestAddress kWorldB = 288U;
constexpr GuestAddress kInverseB = 352U;
constexpr GuestAddress kTranslationA = 96U;   // world A row 3: pushed by the result
constexpr GuestAddress kTranslationB = 336U;  // world B row 3: pushed by the result
constexpr GuestAddress kContactFlag = 484U;
constexpr GuestAddress kContactNormal = 496U;
constexpr GuestAddress kContactCentre = 512U;
// Shape descriptor.
constexpr GuestAddress kShapeIdentity = 0U;
constexpr GuestAddress kShapePlanes = 4U;
constexpr GuestAddress kShapePoints = 8U;
constexpr GuestAddress kShapePointCount = 24U;
constexpr GuestAddress kShapePlaneCount = 25U;

// The original's frame: entry sp - 5552. These words are read by the callees through pointers.
constexpr std::uint32_t kFrameBytes = 5552U;
constexpr GuestAddress kAxis = 1712U;         // normalised direction handed to ClipEdgesAgainstPlane
constexpr GuestAddress kOriginAInB = 1744U;   // A's translation seen from B (a_to_b row 3)
constexpr GuestAddress kOriginBInA = 1728U;   // B's translation seen from A (b_to_a row 3)
constexpr GuestAddress kCentreInA = 1600U;
constexpr GuestAddress kCentreInB = 1584U;
constexpr GuestAddress kPointList = 1888U;    // up to 64 quads: clipped points, B's side first
constexpr std::uint32_t kPointListCapacity = 64U;
constexpr GuestAddress kClipped = 2912U;      // ClipEdgesAgainstPlane's output
constexpr GuestAddress kPointsBInA = 4832U;   // B's points in A's space; the arrays overlap and run past the entry sp
constexpr GuestAddress kPointsAInB = 5184U;   // A's points in B's space
constexpr GuestAddress kClippedCount = 5548U;
constexpr GuestAddress kHitPlaneA = 5544U;    // ClipRayAgainstPlanes' plane index for A
constexpr GuestAddress kHitPlaneB = 5540U;

constexpr std::uint32_t kHalfBits = 0x3f000000U;      // 0.5f
constexpr std::uint32_t kOneBits = 0x3f800000U;       // 1.0f, VF0.w
constexpr std::uint32_t kMinimumSquaredLength = 0x2f800000U;  // 2^-32
// Calls retiring more than this are left to the original (the dry run returns 0 above it). A contact that clips two
// 28-plane hulls retires 40000 to 50000 instructions, most of them in the callees, so the ceiling is higher than
// the usual 20000.
constexpr std::uint32_t kCeiling = 100000U;
// The callees stop at their own ceiling of 20000 and report the count so far: anything above it is a cut-off run.
constexpr std::uint32_t kCalleeCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 531U;         // 0x2b8920..0x2b8c28: both compositions and the byte copies (a
                                                   // fixed 8 passes each: t2 = 8 counted down by bgtz) up to the test
constexpr std::uint32_t kLoopHeadCost = 5U;        // 0x2b8c2c / 0x2b8c8c: three row loads and the jump to the test
constexpr std::uint32_t kLoopPassCost = 14U;       // 11 for the point, 3 for the loop test
constexpr std::uint32_t kLoopEndCost = 3U;         // the failing loop test
constexpr std::uint32_t kShapeBHeadCost = 5U;      // 0x2b8c78: B's point count and its test
constexpr std::uint32_t kClipBlockCost = 64U;      // 0x2b8cd8..0x2b8dd4: both axes and the four calls (callees extra)
constexpr std::uint32_t kRejectCost = 2U + 9U;     // beq/daddu to the epilogue, then the epilogue
constexpr std::uint32_t kEpilogueCost = 9U;
constexpr std::uint32_t kBSideNoneCost = 2U;       // count1 == 0
constexpr std::uint32_t kBSideHeadCost = 7U;       // count1 != 0: 2 + 2 + the failing test
constexpr std::uint32_t kBSidePassCost = 21U;      // 18 + 3
constexpr std::uint32_t kSplitCost = 3U;           // 0x2b8e3c: subu, beq, nop
constexpr std::uint32_t kASideHeadCost = 8U;       // 5 + the failing test
constexpr std::uint32_t kASidePassCost = 20U;      // 17 + 3
constexpr std::uint32_t kBoundsHeadCost = 8U;      // 5 + the failing test
constexpr std::uint32_t kBoundsPassCost = 14U;     // 11 + 3
constexpr std::uint32_t kSearchACost = 55U;        // 0x2b8ef8..0x2b8fd0: centre, both centre transforms, A plane 0
constexpr std::uint32_t kSearchBCost = 36U;        // 0x2b9044..0x2b90d0: A's normal, B plane 0
constexpr std::uint32_t kSearchStartCost = 3U;     // enters the plane loop (two or more planes)
constexpr std::uint32_t kSearchTestCost = 4U;      // loop test, once per plane
constexpr std::uint32_t kSearchPassCost = 19U;     // 17 + the counter step, for planes 1..n-1
constexpr std::uint32_t kSearchImprovedCost = 2U;  // mov.s and daddu when a plane is nearer
constexpr std::uint32_t kNormalCost = 30U;         // 0x2b9144..0x2b91b8: B's normal, the difference, its length
constexpr std::uint32_t kContactBlockCost = 78U;   // 0x2b91cc..0x2b9300: normalise, two stores, both ray calls
constexpr std::uint32_t kClampACost = 1U;          // mov.s when A's depth is negative
constexpr std::uint32_t kMidCost = 5U;             // 0x2b9308..0x2b9318
constexpr std::uint32_t kClampBCost = 1U;
constexpr std::uint32_t kPushCost = 46U;           // 0x2b9320..0x2b93d4

// --- VU0 macro sequences on lane words ------------------------------------------------------------------------

constexpr Gpr128 kOneX{{kOneBits, 0U, 0U, 0U}};

struct Mat {
    Gpr128 row[4];
};

template <class Memory>
Mat load_mat(const Memory& m, const GuestAddress base) {
    return Mat{{n3::load_quad_any(m, base), n3::load_quad_any(m, base + 16U), n3::load_quad_any(m, base + 32U),
                n3::load_quad_any(m, base + 48U)}};
}

// ACC.xyz = row * v.x (VMULAx.xyz), then ACC.xyz += row * v.lane (VMADDAy/z.xyz).
Gpr128 accumulate_first(const Gpr128& row, const Gpr128& v) {
    return guest::execute_vu0_macro_broadcast_multiply({}, row, v, n3::kMaskXyz, 0U).value;
}
Gpr128 accumulate_next(const Gpr128& acc, const Gpr128& row, const Gpr128& v, const std::size_t lane) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(acc, acc, row, v, n3::kMaskXyz, lane, false).value;
}

// xyz = v.x * r0 + v.y * r1 + v.z * r2 (vmulax / vmadday / vmaddz.xyz); w is `w_from`'s, the register the final
// VMADDZ writes into.
Gpr128 rotate(const Mat& mat, const Gpr128& v, const Gpr128& w_from) {
    Gpr128 acc = accumulate_first(mat.row[0], v);
    acc = accumulate_next(acc, mat.row[1], v, 1U);
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(w_from, acc, mat.row[2], v, n3::kMaskXyz, 2U, false)
        .value;
}

// The same plus the translation row (vmaddaz, then vmaddw.xyz with VF0.w = 1.0).
Gpr128 transform(const Mat& mat, const Gpr128& v, const Gpr128& w_from) {
    Gpr128 acc = accumulate_first(mat.row[0], v);
    acc = accumulate_next(acc, mat.row[1], v, 1U);
    acc = accumulate_next(acc, mat.row[2], v, 2U);
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(w_from, acc, mat.row[3], n3::kVf0, n3::kMaskXyz, 3U,
                                                                   false)
        .value;
}

// x*x' + y*y' + 1.0*z*z': vmul.xyz, vadday.x, vmaddz.x against a register whose x lane is 1.0. Returns the x lane.
std::uint32_t dot3(const Gpr128& a, const Gpr128& b) {
    const Gpr128 product = guest::execute_vu0_macro_vector_multiply({}, a, b, n3::kMaskXyz).value;
    const Gpr128 acc = guest::execute_vu0_macro_broadcast_add_subtract({}, product, product, 8U, 1U, false).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(kOneX, acc, kOneX, product, 8U, 2U, false)
        .value.words[0];
}

// Signed distance of a point to a plane: dot(n, p) - w (vsubw.x).
std::uint32_t plane_distance(const Gpr128& plane, const Gpr128& point) {
    const Gpr128 dot{{dot3(plane, point), 0U, 0U, 0U}};
    return guest::execute_vu0_macro_broadcast_add_subtract(dot, dot, plane, 8U, 3U, true).value.words[0];
}

// v * (1 / |v|) on xyz, w kept: vrsqrt Q = 1.0 / sqrt(dot) and vmulq.xyz.
Gpr128 normalise(const Gpr128& v) {
    const std::uint32_t q = ee_cop2::reciprocal_square_root_bits(kOneBits, dot3(v, v));
    return guest::execute_vu0_macro_broadcast_multiply(v, v, ee_cop2::scalar_vector(q), n3::kMaskXyz, 0U).value;
}

// v * s on xyz, w kept (vmulx.xyz).
Gpr128 scale(const Gpr128& v, const std::uint32_t s) {
    return guest::execute_vu0_macro_broadcast_multiply(v, v, ee_cop2::scalar_vector(s), n3::kMaskXyz, 0U).value;
}

// a + b on xyz, w of a (vadd.xyz).
Gpr128 add_xyz(const Gpr128& a, const Gpr128& b) {
    return guest::execute_vu0_macro_vector_add_subtract(a, a, b, n3::kMaskXyz, false).value;
}

// --- shapes -----------------------------------------------------------------------------------------------------

struct Shape {
    GuestAddress address{};
    GuestAddress identity{};   // u32 at +0
    GuestAddress planes{};
    GuestAddress points{};
    std::uint32_t point_count{};   // u8
    std::uint32_t plane_count{};   // u8
};

template <class Memory>
Shape load_shape(const Memory& m, const GuestAddress address) {
    return Shape{address,
                 m.read_u32_le(address + kShapeIdentity),
                 m.read_u32_le(address + kShapePlanes),
                 m.read_u32_le(address + kShapePoints),
                 m.read_u8(address + kShapePointCount),
                 m.read_u8(address + kShapePlaneCount)};
}

// The shape's points through `mat` into the frame array at `out` (the original's loop of 11 instructions a point):
// the point's w lane stays.
template <class Memory>
void transform_points(Memory& m, const Shape& shape, const Mat& mat, const GuestAddress out) {
    for (std::uint32_t i = 0U; i < shape.point_count; ++i) {
        const Gpr128 point = n3::load_quad_any(m, shape.points + (i << 4U));
        n3::store_quad_any(m, out + (i << 4U), transform(mat, point, point));
    }
}

struct Nearest {
    std::uint32_t index{};
    std::uint32_t improved{};   // how often a later plane was strictly nearer
};

// The plane with the smallest |distance| to the point; the first minimum wins (c.olt.s is strict).
template <class Memory>
Nearest nearest_plane(const Memory& m, const Shape& shape, const Gpr128& point) {
    std::uint32_t best = fpu::abs(plane_distance(n3::load_quad_any(m, shape.planes), point));
    Nearest nearest{};
    for (std::uint32_t i = 1U; i < shape.plane_count; ++i) {
        const std::uint32_t distance = fpu::abs(plane_distance(n3::load_quad_any(m, shape.planes + (i << 4U)), point));
        if (fpu::lt(distance, best)) {
            best = distance;
            nearest.index = i;
            ++nearest.improved;
        }
    }
    return nearest;
}

// The original's cost of a nearest-plane search after its first block (`head`).
std::uint32_t search_cost(const std::uint32_t head, const Shape& shape, const Nearest& nearest) {
    if (shape.plane_count < 2U) {
        return head;
    }
    return head + kSearchStartCost + kSearchTestCost * shape.plane_count +
           kSearchPassCost * (shape.plane_count - 1U) + kSearchImprovedCost * nearest.improved;
}

// The depth below zero counts as zero (c.olt.s against +0.0, mov.s), then it is negated (neg.s flips the sign bit).
std::uint32_t pushed_depth(const std::uint32_t depth) {
    return fpu::neg(fpu::lt(depth, 0U) ? 0U : depth);
}

// --- the callees ------------------------------------------------------------------------------------------------

// A callee that went past its ceiling stopped early, so its memory and count are not the original's: decline.
n3::Run checked_callee(const n3::Run run) {
    return run.retired > kCalleeCeiling ? n3::kDeclined : run;
}

template <class Call>
n3::Run clip_edges(Call& call, const GuestAddress frame, const Shape& shape, const GuestAddress points) {
    call.set_arg(0U, frame + kClipped);
    call.set_arg(1U, frame + kClippedCount);
    call.set_arg(2U, frame + kAxis);
    call.set_arg(3U, shape.address);
    call.set_arg(4U, points);
    return checked_callee(call.invoke([](auto& machine) { return n3::fn_002b87e0(machine); }, kFrameBytes));
}

template <class Call>
n3::Run dedupe(Call& call, const GuestAddress frame, const GuestAddress out, const std::uint32_t capacity,
               const Shape& clip_planes) {
    call.set_arg(0U, out);
    call.set_arg(1U, capacity);
    call.set_arg(2U, frame + kClipped);
    call.set_arg(3U, call.memory().read_u32_le(frame + kClippedCount));
    call.set_arg(4U, clip_planes.planes);
    call.set_arg(5U, clip_planes.plane_count);
    return checked_callee(call.invoke([](auto& machine) { return n3::fn_002b7a00(machine); }, kFrameBytes));
}

template <class Call>
n3::Run ray_hit(Call& call, const GuestAddress out, const Shape& shape, const GuestAddress origin,
                const GuestAddress direction) {
    call.set_arg(0U, out);
    call.set_arg(1U, shape.plane_count);
    call.set_arg(2U, shape.identity);
    call.set_arg(3U, shape.planes);
    call.set_arg(4U, origin);
    call.set_arg(5U, direction);
    return checked_callee(call.invoke([](auto& machine) { return n3::fn_002b7bf0(machine); }, kFrameBytes));
}


template <class Context>
n3::Run convex_contact_push_apart(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress list = frame + kPointList;
    const Shape a = load_shape(m, m.read_u32_le(object + kShapeA));
    const Shape b = load_shape(m, m.read_u32_le(object + kShapeB));
    const Mat world_a = load_mat(m, object + kWorldA);
    const Mat inverse_a = load_mat(m, object + kInverseA);
    const Mat world_b = load_mat(m, object + kWorldB);
    const Mat inverse_b = load_mat(m, object + kInverseB);

    // 1. A's matrix into B's space and B's into A's (rotation rows keep the source row's w, the translation row
    // takes the inverse's translation w).
    Mat a_to_b{};
    Mat b_to_a{};
    for (std::size_t k = 0U; k < 3U; ++k) {
        a_to_b.row[k] = rotate(inverse_b, world_a.row[k], world_a.row[k]);
        b_to_a.row[k] = rotate(inverse_a, world_b.row[k], world_b.row[k]);
    }
    a_to_b.row[3] = transform(inverse_b, world_a.row[3], inverse_b.row[3]);
    b_to_a.row[3] = transform(inverse_a, world_b.row[3], inverse_a.row[3]);
    n3::store_quad_any(m, frame + kOriginAInB, a_to_b.row[3]);
    n3::store_quad_any(m, frame + kOriginBInA, b_to_a.row[3]);

    // Both point sets into the frame arrays. B's array starts 352 bytes below A's, so from the 23rd point on they
    // overlap, and past the 23rd of A's they leave the frame: real stores, in this order.
    std::uint32_t retired = kSetupCost;
    if (a.point_count > 0U) {
        retired += kLoopHeadCost + kLoopPassCost * a.point_count + kLoopEndCost;
        transform_points(m, a, a_to_b, frame + kPointsAInB);
    }
    retired += kShapeBHeadCost;
    if (b.point_count > 0U) {
        retired += kLoopHeadCost + kLoopPassCost * b.point_count + kLoopEndCost;
        transform_points(m, b, b_to_a, frame + kPointsBInA);
    }

    // 2. A's edges clipped with the normalised direction of b_to_a's translation, kept inside B's planes; then B's
    // edges with a_to_b's, kept inside A's planes, appended to the same list.
    retired += kClipBlockCost;
    n3::store_quad_any(m, frame + kAxis, normalise(b_to_a.row[3]));
    n3::Run run = clip_edges(call, frame, a, frame + kPointsAInB);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    run = dedupe(call, frame, list, kPointListCapacity, b);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    const std::uint32_t count1 = call.result();
    n3::store_quad_any(m, frame + kAxis, normalise(a_to_b.row[3]));
    run = clip_edges(call, frame, b, frame + kPointsBInA);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    run = dedupe(call, frame, list + (count1 << 4U), kPointListCapacity - count1, a);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    const std::uint32_t total = count1 + call.result();
    if (total < 2U) {
        call.return_value(0U);
        return call.finish(retired + kRejectCost);
    }

    // 3. The list to world space (the first count1 points are in B's space, the rest in A's), its bounding box and
    // the centre. The counts are signed in the loops (slt); anything near the ceiling is left to the original.
    const std::int32_t first_count = static_cast<std::int32_t>(count1);
    const std::int32_t list_count = static_cast<std::int32_t>(total);
    const std::int32_t second_count = static_cast<std::int32_t>(total - count1);
    constexpr std::int32_t kBigCount = 2000;   // 14 or more instructions a pass: past the ceiling
    if (first_count > kBigCount || second_count > kBigCount || list_count > kBigCount) {
        return call.finish(kCeiling + 1U);
    }
    retired += count1 == 0U ? kBSideNoneCost
                            : kBSideHeadCost + kBSidePassCost * static_cast<std::uint32_t>(first_count > 0 ? first_count : 0);
    retired += kSplitCost;
    if (total != count1) {
        retired += kASideHeadCost + kASidePassCost * static_cast<std::uint32_t>(second_count > 0 ? second_count : 0);
    }
    retired += kBoundsHeadCost + kBoundsPassCost * static_cast<std::uint32_t>(list_count > 1 ? list_count - 1 : 0);
    if (retired > kCeiling) {
        return call.finish(retired);
    }
    for (std::int32_t i = 0; i < first_count; ++i) {
        const GuestAddress at = list + (static_cast<std::uint32_t>(i) << 4U);
        n3::store_quad_any(m, at, transform(world_b, n3::load_quad_any(m, at), world_b.row[3]));
    }
    const GuestAddress second = list + (count1 << 4U);
    for (std::int32_t i = 0; i < second_count; ++i) {
        const GuestAddress at = second + (static_cast<std::uint32_t>(i) << 4U);
        n3::store_quad_any(m, at, transform(world_a, n3::load_quad_any(m, at), world_a.row[3]));
    }
    Gpr128 low = n3::load_quad_any(m, list);
    Gpr128 high = low;
    for (std::int32_t i = 1; i < list_count; ++i) {
        const Gpr128 point = n3::load_quad_any(m, list + (static_cast<std::uint32_t>(i) << 4U));
        for (std::size_t lane = 0U; lane < 3U; ++lane) {
            low.words[lane] = ee_cop2::minimum_bits(low.words[lane], point.words[lane]);
            high.words[lane] = ee_cop2::maximum_bits(high.words[lane], point.words[lane]);
        }
    }
    const Gpr128 centre = scale(add_xyz(low, high), kHalfBits);
    const Gpr128 centre_a = transform(inverse_a, centre, inverse_a.row[3]);
    const Gpr128 centre_b = transform(inverse_b, centre, inverse_b.row[3]);
    n3::store_quad_any(m, frame + kCentreInA, centre_a);
    n3::store_quad_any(m, frame + kCentreInB, centre_b);

    // 4. The plane of each shape nearest to the centre, its normal in world space, and the contact normal.
    const Nearest near_a = nearest_plane(m, a, centre_a);
    retired += search_cost(kSearchACost, a, near_a);
    const Gpr128 normal_a = rotate(world_a, n3::load_quad_any(m, a.planes + (near_a.index << 4U)), world_a.row[2]);
    const Nearest near_b = nearest_plane(m, b, centre_b);
    retired += search_cost(kSearchBCost, b, near_b);
    const Gpr128 normal_b = rotate(world_b, n3::load_quad_any(m, b.planes + (near_b.index << 4U)), world_b.row[2]);
    retired += kNormalCost;
    const Gpr128 difference = guest::execute_vu0_macro_vector_add_subtract(normal_a, normal_a, normal_b, n3::kMaskXyz,
                                                                           true)
                                  .value;
    if (fpu::lt(dot3(difference, difference), kMinimumSquaredLength)) {
        call.return_value(0U);
        return call.finish(retired + kRejectCost);
    }

    // 5. Where the ray from each centre leaves its shape, and the push along that plane's normal.
    retired += kContactBlockCost;
    m.write_u128_le(object + kContactCentre, centre.words);
    m.write_u128_le(object + kContactNormal, normalise(difference).words);
    run = ray_hit(call, frame + kHitPlaneA, a, frame + kCentreInA, frame + kOriginBInA);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    run = ray_hit(call, frame + kHitPlaneB, b, frame + kCentreInB, frame + kOriginAInB);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;

    const Gpr128 hit_plane_a = n3::load_quad_any(m, a.planes + (m.read_u32_le(frame + kHitPlaneA) << 4U));
    const Gpr128 hit_plane_b = n3::load_quad_any(m, b.planes + (m.read_u32_le(frame + kHitPlaneB) << 4U));
    // offset - dot(centre, normal): vsubx.w into the plane's w lane, read back as a scalar float.
    const std::uint32_t depth_a = guest::execute_vu0_macro_broadcast_add_subtract(
                                      hit_plane_a, hit_plane_a, ee_cop2::scalar_vector(dot3(centre_a, hit_plane_a)),
                                      n3::kMaskW, 0U, true)
                                      .value.words[3];
    const std::uint32_t depth_b = guest::execute_vu0_macro_broadcast_add_subtract(
                                      hit_plane_b, hit_plane_b, ee_cop2::scalar_vector(dot3(centre_b, hit_plane_b)),
                                      n3::kMaskW, 0U, true)
                                      .value.words[3];
    retired += kMidCost + kPushCost + kEpilogueCost;
    if (fpu::lt(depth_a, 0U)) {
        retired += kClampACost;
    }
    if (fpu::lt(depth_b, 0U)) {
        retired += kClampBCost;
    }
    const Gpr128 push_a = rotate(world_a, scale(hit_plane_a, pushed_depth(depth_a)), world_a.row[2]);
    const Gpr128 push_b = rotate(world_b, scale(hit_plane_b, pushed_depth(depth_b)), world_b.row[2]);
    m.write_u128_le(object + kTranslationA, add_xyz(world_a.row[3], push_a).words);
    m.write_u128_le(object + kTranslationB, add_xyz(world_b.row[3], push_b).words);
    m.write_u32_le(object + kContactFlag, 1U);
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002b8920, 2780U, 0x5db14018d932686bULL, "ConvexContactPushApart", kCeiling, convex_contact_push_apart,
         EngineReturn::Integer)
