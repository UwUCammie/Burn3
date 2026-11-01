// 0x001586d0 (a0 = object A, a1 = object B): CarOverlapsOrientedBox.
//
// Answers "does A's oriented box overlap B's box" in the ground plane. Nothing
// outside the function's own stack frame is written; the shot is 1 (overlap) or
// 0, and every path is a fixed run of straight-line code.
//
// Records (raw R5900 float words; only a quad's x = word 0 and z = word 2 are
// ever used):
//   A + 200 -> frame F : axes at F+0 and F+32, position quad at F+48,
//              F + 64 -> extent record E with opposite corners E+3712, E+3728.
//   B + 112, B + 144   : B's two axes, B + 160 : B's position quad,
//   B + 176 -> B's extent record (the same corner layout).
// A footprint is built from those corners: half extent = (corner3712 -
// corner3728)/2 per axis, centre = corner3728 + half extent, and each axis
// direction scaled by its half extent (its "corner vector"). The footprint's
// inner radius is the length of the two half extents; the game's accept radius
// is the float at $gp-29156.
//
// Paths (block counts from brief.py; the longest, 441, is the declared cost):
//   31   vertical drop above +4.0
//   39   vertical drop below -4.0
//   241  centre distance beyond 2*radius + both inner radii, or not a stagger
//   350..441 the four axes are signed away from B, then the squared distance
//        from a shifted centre to the four axis segments is minimised and
//        compared with radius^2.
//
// The orbit of the check is the returned value: the original's intermediate
// vectors live in a 1408-byte frame that is dead on return, so this rewrite
// keeps them in locals and only reproduces the float arithmetic itself.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

namespace fpu = n3::fpu;
using n3::GuestAddress;

// Raw R5900 float words the listing builds with lui/mtc1 (or mtc1 $zero).
constexpr std::uint32_t kZero = 0x00000000U;       // +0.0
constexpr std::uint32_t kHalf = 0x3f000000U;       // 0.5
constexpr std::uint32_t kMinusOne = 0xbf800000U;   // -1.0
constexpr std::uint32_t kAbove = 0x40800000U;      // +4.0
constexpr std::uint32_t kBelow = 0xc0800000U;      // -4.0

// Object A: the car-sized box, reached through the frame record at A+200.
constexpr GuestAddress kFramePointer = 200U;
constexpr GuestAddress kFrameAxisA = 0U;
constexpr GuestAddress kFrameAxisB = 32U;
constexpr GuestAddress kFramePosition = 48U;
constexpr GuestAddress kFrameExtentPointer = 64U;

// Object B: the box it is tested against.
constexpr GuestAddress kBAxisA = 112U;
constexpr GuestAddress kBAxisB = 144U;
constexpr GuestAddress kBPosition = 160U;
constexpr GuestAddress kBExtentPointer = 176U;

// An extent record keeps two opposite corners of its box.
constexpr GuestAddress kCornerHigh = 3712U;
constexpr GuestAddress kCornerLow = 3728U;

// The footprint radius the game compares against, at $gp - 29156.
constexpr std::uint32_t kRadiusFromGlobalPointer = 29156U;

// Retired-instruction counts of the original's straight runs (brief.py).
constexpr std::uint32_t kEntryBlock = 20U;         // 0x1586d0..0x15871c: prologue, +4.0 gate
constexpr std::uint32_t kLowerGateBlock = 8U;      // 0x158720..0x15873c: -4.0 gate
constexpr std::uint32_t kBoxBlock = 202U;          // 0x158740..0x158a64: both boxes, broad phase
constexpr std::uint32_t kZeroReturn = 11U;         // 0x158cfc/0x158cf0 path plus the epilogue
constexpr std::uint32_t kFirstAxisTest = 6U;       // 0x158a68..0x158a7c
constexpr std::uint32_t kAxisTest = 10U;           // 0x158a80/a8/d0 with their delay slots
constexpr std::uint32_t kFirstAxisFlip = 8U;       // 0x158d0c..0x158d28
constexpr std::uint32_t kAxisFlip = 10U;           // 0x158d2c/d54/d7c..
constexpr std::uint32_t kSearchPointBlock = 46U;   // 0x158af8..0x158bac
constexpr std::uint32_t kProjection = 26U;         // 0x158bc8, 0x158da4
constexpr std::uint32_t kProjectionReload = 28U;   // 0x158c68, 0x158e14: reload the axis first
constexpr std::uint32_t kFarAxisTest = 6U;         // 0x158bb0..0x158bc4
constexpr std::uint32_t kNearAxesBlock = 7U;       // 0x158c34..0x158c4c
constexpr std::uint32_t kNearAxisTest = 6U;        // 0x158c50..0x158c64
constexpr std::uint32_t kBestFromFarAxis = 1U;     // 0x158c30
constexpr std::uint32_t kBestFromNearAxis = 2U;    // 0x158e0c..0x158e10
constexpr std::uint32_t kResultBlock = 5U;         // 0x158cdc..0x158cec
constexpr std::uint32_t kReturnOne = 9U;           // lq $s6 plus the epilogue
constexpr std::uint32_t kReturnZero = 11U;         // daddu, beq, its delay slot, epilogue

// A 2D vector in the plane the test works in: the x and z of a guest quadword.
struct PlaneVector {
    std::uint32_t x;
    std::uint32_t z;
};

// x (word 0) and z (word 2) of the quadword at `address`.
template <class Memory>
PlaneVector read_plane(const Memory& memory, const GuestAddress address) {
    return {memory.read_u32_le(address), memory.read_u32_le(address + 8U)};
}

// y (word 1) of the quadword at `address`.
template <class Memory>
std::uint32_t read_height(const Memory& memory, const GuestAddress address) {
    return memory.read_u32_le(address + 4U);
}

// One box of the test: its two axis vectors scaled by their half extents, the
// centre of the footprint and the length of the half extents (the inner radius).
struct OrientedBox {
    PlaneVector axis_a;
    PlaneVector axis_b;
    PlaneVector centre;
    std::uint32_t inner_radius;
};

// Rebuild a footprint from the two corners of its extent record. The corner
// difference spans the box, so half of it is the half extent and the corner at
// +3728 plus a half extent is the centre.
OrientedBox read_box(const PlaneVector position, const PlaneVector axis_a, const PlaneVector axis_b,
                     const PlaneVector corner_high, const PlaneVector corner_low) {
    const std::uint32_t half_a = fpu::mul(fpu::sub(corner_high.x, corner_low.x), kHalf);
    const std::uint32_t half_b = fpu::mul(fpu::sub(corner_high.z, corner_low.z), kHalf);
    const std::uint32_t centre_a = fpu::add(corner_low.x, half_a);
    const std::uint32_t centre_b = fpu::add(corner_low.z, half_b);
    const OrientedBox box{
        {fpu::mul(axis_a.x, half_a), fpu::mul(axis_a.z, half_a)},
        {fpu::mul(axis_b.x, half_b), fpu::mul(axis_b.z, half_b)},
        {fpu::add(fpu::add(position.x, fpu::mul(axis_a.x, centre_a)), fpu::mul(axis_b.x, centre_b)),
         fpu::add(fpu::add(position.z, fpu::mul(axis_a.z, centre_a)), fpu::mul(axis_b.z, centre_b))},
        fpu::sqrt(fpu::madd(fpu::mul(half_b, half_b), half_a, half_a)),
    };
    return box;
}

// ADDA.S seeded with +0: the original starts several accumulation chains by
// adding a fresh product to zero, which also clears a negative zero.
std::uint32_t plus_zero(const std::uint32_t value) { return fpu::add(kZero, value); }

// MULA.S a.z, b.z ; MADD.S acc, a.x, b.x (the original's 2D dot product).
std::uint32_t dot_product(const PlaneVector a, const PlaneVector b) {
    return fpu::madd(fpu::mul(a.z, b.z), a.x, b.x);
}

// The same chain with an ADDA.S +0 seed instead of MULA.S.
std::uint32_t dot_product_from_zero(const PlaneVector a, const PlaneVector b) {
    return fpu::madd(plus_zero(fpu::mul(a.z, b.z)), a.x, b.x);
}

// The original negates a corner vector by multiplying both components by -1.0.
PlaneVector negated(const PlaneVector value) {
    return {fpu::mul(value.x, kMinusOne), fpu::mul(value.z, kMinusOne)};
}

// Squared distance from `point` to the closest point of the segment
// [origin, axis]: project the point onto the axis line (s = dot / |axis|^2) and
// measure the residual.
std::uint32_t segment_distance(const PlaneVector point, const PlaneVector axis,
                               const std::uint32_t axis_dot_point) {
    const std::uint32_t length_sq = fpu::madd(plus_zero(fpu::mul(axis.z, axis.z)), axis.x, axis.x);
    const std::uint32_t scale = fpu::div(axis_dot_point, length_sq);
    const std::uint32_t dx = fpu::sub(point.x, fpu::mul(axis.x, scale));
    const std::uint32_t dz = fpu::sub(point.z, fpu::mul(axis.z, scale));
    return fpu::madd(plus_zero(fpu::mul(dz, dz)), dx, dx);
}

template <class Context>
n3::Run car_overlaps_oriented_box(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object_a = call.arg(0U);
    const GuestAddress object_b = call.arg(1U);

    // Height gate: the two objects must not be more than 4 units apart.
    const GuestAddress frame = m.read_u32_le(object_a + kFramePointer);
    const std::uint32_t drop =
        fpu::sub(read_height(m, object_b + kBPosition), read_height(m, frame + kFramePosition));
    if (!fpu::le(drop, kAbove)) {
        call.return_value(0U);
        return call.finish(kEntryBlock + kZeroReturn); // 31
    }
    if (fpu::lt(drop, kBelow)) {
        call.return_value(0U);
        return call.finish(kEntryBlock + kLowerGateBlock + kZeroReturn); // 39
    }

    // Both footprints in the ground plane.
    const GuestAddress extent_a = m.read_u32_le(frame + kFrameExtentPointer);
    OrientedBox a = read_box(read_plane(m, frame + kFramePosition), read_plane(m, frame + kFrameAxisA),
                             read_plane(m, frame + kFrameAxisB),
                             read_plane(m, extent_a + kCornerHigh),
                             read_plane(m, extent_a + kCornerLow));
    const GuestAddress extent_b = m.read_u32_le(object_b + kBExtentPointer);
    OrientedBox b = read_box(read_plane(m, object_b + kBPosition), read_plane(m, object_b + kBAxisA),
                             read_plane(m, object_b + kBAxisB),
                             read_plane(m, extent_b + kCornerHigh),
                             read_plane(m, extent_b + kCornerLow));

    const std::uint32_t radius = m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) -
                                               kRadiusFromGlobalPointer);
    std::uint32_t retired = kEntryBlock + kLowerGateBlock + kBoxBlock; // 230

    // Broad phase: the centres must be closer than the two inner radii plus one
    // accept radius, which leaves room for the boxes' own extents.
    const PlaneVector centre_offset{fpu::sub(b.centre.x, a.centre.x), fpu::sub(b.centre.z, a.centre.z)};
    const std::uint32_t reach = fpu::add(fpu::add(radius, a.inner_radius), b.inner_radius);
    const std::uint32_t centre_distance = fpu::sqrt(
        fpu::madd(fpu::mul(centre_offset.z, centre_offset.z), centre_offset.x, centre_offset.x));
    if (!fpu::le(centre_distance, reach)) {
        call.return_value(0U);
        return call.finish(retired + kZeroReturn); // 241
    }

    // Sign every corner vector so it points away from B: A's two axes flip when
    // they still point towards B, B's two flip when they do not point away.
    retired += kFirstAxisTest;
    if (fpu::lt(dot_product_from_zero(centre_offset, a.axis_a), kZero)) {
        a.axis_a = negated(a.axis_a);
        retired += kFirstAxisFlip;
    }
    retired += kAxisTest;
    if (fpu::lt(dot_product(centre_offset, a.axis_b), kZero)) {
        a.axis_b = negated(a.axis_b);
        retired += kAxisFlip;
    }
    retired += kAxisTest;
    if (!fpu::lt(dot_product(centre_offset, b.axis_a), kZero)) {
        b.axis_a = negated(b.axis_a);
        retired += kAxisFlip;
    }
    retired += kAxisTest;
    if (!fpu::lt(dot_product(centre_offset, b.axis_b), kZero)) {
        b.axis_b = negated(b.axis_b);
        retired += kAxisFlip;
    }

    // The search point: the centre offset shifted by B's corner vector sum minus
    // A's, i.e. the offset between the two far corners.
    retired += kSearchPointBlock;
    const PlaneVector search{
        fpu::add(centre_offset.x,
                 fpu::sub(fpu::add(b.axis_a.x, b.axis_b.x), fpu::add(a.axis_a.x, a.axis_b.x))),
        fpu::add(centre_offset.z,
                 fpu::sub(fpu::add(b.axis_a.z, b.axis_b.z), fpu::add(a.axis_a.z, a.axis_b.z)))};
    std::uint32_t closest = fpu::madd(fpu::mul(search.z, search.z), search.x, search.x);

    // B's second corner: keep it when the search point is on its side, else fall
    // back to B's first corner if the point is on that side.
    const std::uint32_t dot_b_b = dot_product(search, b.axis_b);
    if (!fpu::lt(dot_b_b, kZero)) {
        retired += kProjection;
        const std::uint32_t distance = segment_distance(search, b.axis_b, dot_b_b);
        if (fpu::lt(distance, closest)) {
            closest = distance;
            retired += kBestFromNearAxis;
        }
    } else {
        retired += kFarAxisTest;
        const std::uint32_t dot_b_a = dot_product_from_zero(search, b.axis_a);
        if (!fpu::lt(dot_b_a, kZero)) {
            retired += kProjection;
            const std::uint32_t distance = segment_distance(search, b.axis_a, dot_b_a);
            if (fpu::lt(distance, closest)) {
                closest = distance;
                retired += kBestFromFarAxis;
            }
        }
    }

    // A's two corners take part only when the search point is on their inward
    // side, measured with the second corner first.
    retired += kNearAxesBlock;
    const std::uint32_t dot_a_b = dot_product(search, a.axis_b);
    if (fpu::lt(dot_a_b, kZero)) {
        retired += kProjectionReload;
        const std::uint32_t distance = segment_distance(search, a.axis_b, dot_a_b);
        if (fpu::lt(distance, closest)) {
            closest = distance;
            retired += kBestFromNearAxis;
        }
    } else {
        retired += kNearAxisTest;
        const std::uint32_t dot_a_a = dot_product_from_zero(search, a.axis_a);
        if (fpu::lt(dot_a_a, kZero)) {
            retired += kProjectionReload;
            const std::uint32_t distance = segment_distance(search, a.axis_a, dot_a_a);
            if (fpu::lt(distance, closest)) {
                closest = distance;
                retired += kBestFromFarAxis;
            }
        }
    }

    // Overlap when the nearest corner segment is inside the accept radius.
    retired += kResultBlock;
    if (fpu::lt(closest, fpu::mul(radius, radius))) {
        call.return_value(1U);
        return call.finish(retired + kReturnOne);
    }
    call.return_value(0U);
    return call.finish(retired + kReturnZero);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001586d0, 2016U, 0x4181eaa1adf335c0ULL, "CarOverlapsOrientedBox", 441U,
               car_overlaps_oriented_box, EngineReturn::Integer)
