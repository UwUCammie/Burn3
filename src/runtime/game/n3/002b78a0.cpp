// 0x002b78a0 ClipSegmentsToPlane (a0 = output quads, a1 = input quads, a2 = number of input points, a3 = the plane
// quad {nx, ny, nz, w}). The input is one segment per pair of points (in[2k], in[2k+1]). A point is inside when
// dot3(plane, p) <= plane.w. Per segment the written points come in this order: the intersection point when exactly
// one end is inside, then each inside end point. An odd count also reads the point past the last one, whose flag
// decides what is written, as the original does. Returns the number of points written (v0).
//
// The distances and the intersection run through the VU0 macro ops in the original's order, so the rounding is the
// original's. The VF1-VF5 and ACC values and the MAC flags the original leaves behind are temporaries: not written back.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Quad = guest::Gpr128;

constexpr std::uint32_t kOne = 0x3f800000U;          // 1.0f: VF4.x and VF3.x
constexpr std::uint8_t kMaskX = 8U;                  // destination mask of lane x alone
constexpr GuestAddress kQuadBytes = 16U;
// Calls retiring more than this are left to the original (the dry run returns 0 above it).
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEmptyCost = 6U;             // 0x002b78a0-0x002b78ac and the return block, for a2 == 0
constexpr std::uint32_t kPrologueCost = 14U;         // 0x002b78a0-0x002b78d4 for a2 != 0 (plane, w, VF5, VF4, VF3)
constexpr std::uint32_t kEpilogueCost = 5U;          // the loop test that fails (0x002b78d8-0x002b78e0), the return
constexpr std::uint32_t kSegmentCost = 35U;          // loop test 3, body 21, c.ole.s 3, bne 2, two emit tests 2 + 2,
                                                     // loop step 2: every block with its delay slot
constexpr std::uint32_t kInsideEndCost = 1U;         // an inside end point's flag: the delay-slot daddu only
constexpr std::uint32_t kOutsideEndCost = 2U;        // an outside end point's flag: a jump and its delay slot
constexpr std::uint32_t kIntersectionCost = 17U;     // the intersection point, 0x002b797c-0x002b79bc
constexpr std::uint32_t kEmitCost = 6U;              // an inside end point written, 0x002b79c0 or 0x002b79d8

// Retired count of one segment, from its two inside flags.
constexpr std::uint32_t segment_cost(const bool in0, const bool in1) {
    return kSegmentCost + (in0 ? kInsideEndCost : kOutsideEndCost) + (in1 ? kInsideEndCost : kOutsideEndCost) +
           (in0 != in1 ? kIntersectionCost : 0U) + (in0 ? kEmitCost : 0U) + (in1 ? kEmitCost : 0U);
}

// dot3(plane, p) = (x*nx + y*ny) + 1.0*(z*nz) on the macro ops: the three products, x + y into ACC.x, then the z
// product added with VF4.x = 1.0 as the multiplier. Returns lane x.
std::uint32_t plane_distance(const Quad& plane, const Quad& point) {
    const Quad one{{kOne, 0U, 0U, 0U}};
    const Quad products = guest::execute_vu0_macro_vector_multiply(point, plane, point, n3::kMaskXyz).value;
    const Quad sum_xy =
        guest::execute_vu0_macro_broadcast_add_subtract(products, products, products, kMaskX, 1U, false).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(products, sum_xy, one, products, kMaskX, 2U, false)
        .value.words[0];
}

// p0 + t * (p1 - p0) on the macro ops in the original's order: ACC = p0 + 0, ACC = ACC - p0 * t, then ACC + p1 * t.
// Only xyz are written, so the result takes its w from p1. t is the broadcast lane of VF2.
Quad intersection_point(const Quad& p0, const Quad& p1, const std::uint32_t t) {
    const Quad t_lane{{t, 0U, 0U, 0U}};
    const Quad acc0 =
        guest::execute_vu0_macro_broadcast_add_subtract(p0, p0, n3::kVf0, n3::kMaskXyz, 0U, false).value;
    const Quad acc1 =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(acc0, acc0, p0, t_lane, n3::kMaskXyz, 0U, true).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(p1, acc1, p1, t_lane, n3::kMaskXyz, 0U, false).value;
}

template <class Memory>
void store_point(Memory& m, const GuestAddress out, const std::uint32_t index, const Quad& point) {
    n3::store_quad_any(m, out + kQuadBytes * index, point);
}

struct ClipResult {
    std::uint32_t retired{};
    std::uint32_t written{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (the dry run behind call_cost). Each
// segment reads both of its points before it writes, as the original does, so the output may be the input array.
template <class Memory>
ClipResult clip_segments(Memory& m, const GuestAddress out, const GuestAddress in, const std::uint32_t points,
                         const GuestAddress plane_address) {
    const Quad plane = n3::load_quad_any(m, plane_address);
    if (points == 0U) {
        return ClipResult{kEmptyCost, 0U};
    }
    const std::uint32_t plane_w = plane.words[3];
    ClipResult result{kPrologueCost, 0U};
    for (std::uint32_t index = 0U; index < points; index += 2U) {
        if (result.retired > kCeiling) {
            return result;
        }
        const Quad p0 = n3::load_quad_any(m, in + kQuadBytes * index);
        const Quad p1 = n3::load_quad_any(m, in + kQuadBytes * (index + 1U));
        const std::uint32_t d0 = plane_distance(plane, p0);
        const std::uint32_t d1 = plane_distance(plane, p1);
        const bool in0 = fpu::le(d0, plane_w);
        const bool in1 = fpu::le(d1, plane_w);
        result.retired += segment_cost(in0, in1);
        if (in0 != in1) {
            const std::uint32_t t = fpu::div(fpu::sub(plane_w, d0), fpu::sub(d1, d0));
            store_point(m, out, result.written, intersection_point(p0, p1, t));
            ++result.written;
        }
        if (in0) {
            store_point(m, out, result.written, p0);
            ++result.written;
        }
        if (in1) {
            store_point(m, out, result.written, p1);
            ++result.written;
        }
    }
    result.retired += kEpilogueCost;
    return result;
}

template <class Context>
n3::Run clip_segments_to_plane(Context& context) {
    auto call = n3::guest_call(context);
    const ClipResult clip = clip_segments(call.memory(), call.arg(0U), call.arg(1U), call.arg(2U), call.arg(3U));
    call.return_value(clip.written);
    return call.finish(clip.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002b78a0, 344U, 0x55c13c5786896734ULL, "ClipSegmentsToPlane", kCeiling, clip_segments_to_plane,
         EngineReturn::Integer)
