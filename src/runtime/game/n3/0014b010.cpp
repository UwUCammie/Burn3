// 0x0014b010 RoadLookAheadPoint (a0 = car, a1 = segment (low 16 bits), a3 = mode, t0 = point out (aligned quad),
// t1 = byte out, t2 = halfword out; returns nothing). Looks a short way ahead on the car's road: from `segment` it
// keeps stepping to the neighbouring road segment (in the direction the mode selects: mode 1 forwards, anything
// else backwards) while RouteOpenAheadScan / SegmentRangeFlags report the 8 quads beyond it as not blocked (flag
// bit 4 stops the walk). It writes the road id byte and the segment it ended on, then the world point
//
//     vertex[first vertex of the START segment] + normalize(edge direction of the END segment) * (t * reach)
//
// with t = clamp01(car.f664 / G) and reach = f32[car.object + 9268]. The edge direction is vertex[b] - vertex[a] of
// the end segment's two index entries (a, b swapped by the mode); a zero-length edge borrows the neighbouring
// segment's entries, and an edge too short to normalise gives a zero offset.
//
// Records (a road path R is a 0x52268c + 8 * id entry; R + 4 holds the road's strip T):
//   T[0]  -> s16 vertex pair per segment (4 bytes each);  T[8] -> 10-byte segment records:
//   byte +4 / +5 = neighbour road id (backward / forward slot), u16 +6 / +8 = neighbour segment.
//   0x522690 + 8 * id -> strip of road id (u16 at +12 = quad count, for the ring wrap of the quad range).
//
// The vector part runs on the VU0 registers exactly as the original does (vsub, squared length by vmul + vadday +
// vmaddz, vrsqrt, vmulq, vmulx, vadd), because its rounding and flag behaviour is what the game saw.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

N3_CALLEE(00148ca0)
N3_CALLEE(0014a560)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kRoadTable = 0x00522690U;     // strip of road id at +8 * id
constexpr GuestAddress kRoadPaths = 0x0052268cU;     // road path of road id at +8 * id (its +4 is the table entry)
constexpr GuestAddress kVertices = 0x0064d354U;      // pointer to the 16-byte vertex array
constexpr GuestAddress kScaleDivisor = 0x01e7557cU;  // G
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskXyz = 14U;
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint32_t kTinyLengthBits = 0x2f800000U;  // 2^-32
constexpr std::uint32_t kStopFlag = 4U;
constexpr std::uint32_t kLookAheadQuads = 8U;
constexpr std::uint32_t kFrameBytes = 320U;
constexpr std::uint32_t kCeiling = 20000U;

// Record field offsets.
constexpr GuestAddress kCarRoad = 468U;
constexpr GuestAddress kCarObject = 4U;
constexpr GuestAddress kCarFactor = 664U;
constexpr GuestAddress kObjectReach = 9268U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 19U;          // 0x14b010..0x14b058
constexpr std::uint32_t kForwardCost = 10U;       // 0x14b05c..0x14b080 (mode 1)
constexpr std::uint32_t kBackwardCost = 8U;       // 0x14b084..0x14b0a0
constexpr std::uint32_t kStartCost = 19U;         // 0x14b0a4..0x14b0ec
constexpr std::uint32_t kStartWrapCost = 9U;      // 0x14b42c..0x14b44c: quad range wrapped
constexpr std::uint32_t kStepCost = 35U;          // 0x14b144..0x14b1cc
constexpr std::uint32_t kStepWrapCost = 7U;       // 0x14b450..0x14b468
constexpr std::uint32_t kScanCallCost = 6U;       // jal RouteOpenAheadScan, the test of its result
constexpr std::uint32_t kClosedCost = 1U;         // not open: flags = 4
constexpr std::uint32_t kFlagsCallCost = 12U;     // open: set up and call SegmentRangeFlags
constexpr std::uint32_t kAfterStartCost = 2U;     // 0x14b13c
constexpr std::uint32_t kLoopTestCost = 4U;       // 0x14b23c
constexpr std::uint32_t kStepTailCost = 3U;       // 0x14b21c
constexpr std::uint32_t kCommitCost = 5U;         // 0x14b228
constexpr std::uint32_t kExitCost = 16U;          // 0x14b24c
constexpr std::uint32_t kDegenerateTestCost = 2U; // 0x14b28c
constexpr std::uint32_t kDegenerateFirstCost = 3U + 6U;   // segment 0 -> 1: 0x14b46c, 0x14b29c
constexpr std::uint32_t kDegenerateOtherCost = 2U + 6U;   // otherwise: 0x14b294, 0x14b29c
constexpr std::uint32_t kFactorCost = 15U;        // 0x14b2b4
constexpr std::uint32_t kEdgeForwardCost = 15U;   // 0x14b2f0
constexpr std::uint32_t kEdgeBackwardCost = 14U;  // 0x14b32c
constexpr std::uint32_t kLengthCost = 15U;        // 0x14b364
constexpr std::uint32_t kTinyCost = 3U;           // 0x14b3a0
constexpr std::uint32_t kNormaliseCost = 23U;     // 0x14b3ac
constexpr std::uint32_t kPointCost = 9U;          // 0x14b408
constexpr std::uint32_t kReturnCost = 12U;        // 0x14b478

// MAX.S / MIN.S on raw words (EE sign/magnitude ordering, as guest::ee_cop1::max_s / min_s).
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = static_cast<std::int32_t>(left);
    const auto r = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(l < 0 && r < 0 ? std::min(l, r) : std::max(l, r));
}
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = static_cast<std::int32_t>(left);
    const auto r = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(l < 0 && r < 0 ? std::max(l, r) : std::min(l, r));
}

// One neighbour step: the segment the walk would move to, and the quad range to test beyond it.
struct Hop {
    std::uint32_t road_id;     // neighbour road id (byte)
    std::uint32_t segment;     // neighbour segment (halfword)
};

template <class Context>
n3::Run road_look_ahead_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const GuestAddress car = call.arg(0);
    const GuestAddress point_out = call.arg(4);
    const GuestAddress byte_out = call.arg(5);
    const GuestAddress half_out = call.arg(6);
    const std::uint32_t start_segment = call.arg(1) & 0xffffU;
    // The walk direction tests the whole register (bne on 64 bits); the edge direction tests the stored word.
    const bool forward_walk = call.arg64(3) == 1U;
    const bool forward_edge = call.arg(3) == 1U;
    const std::uint32_t slot = forward_walk ? 0U : 1U;

    std::uint32_t retired = kHeadCost + (forward_walk ? kForwardCost : kBackwardCost) + kStartCost;

    // The recorded neighbour slots of segment `segment` of road path `path`.
    const auto record = [&](const GuestAddress path, const std::uint32_t segment) {
        return m.read_u32_le(m.read_u32_le(path + 4U) + 8U) + segment * 10U;
    };
    const auto hop_at = [&](const GuestAddress rec) {
        return Hop{m.read_u8(rec + 4U + slot), m.read_u16_le(rec + 6U + 2U * slot)};
    };

    // RouteOpenAheadScan over the 8 quads after `first` in strip `id`, then SegmentRangeFlags when it reports the
    // range open; returns the flags (4 when closed). `wrap_cost` is the extra block taken for a wrapped range.
    std::uint32_t flags = 0U;
    const auto scan = [&](const std::uint32_t id, const std::uint32_t first, const std::uint32_t wrap_cost) {
        const std::uint32_t last = (first + kLookAheadQuads) & 0xffffU;
        std::int32_t count = static_cast<std::int32_t>(last - first);
        retired += kScanCallCost;
        if (count < 0) {
            count += static_cast<std::int32_t>(m.read_u16_le(m.read_u32_le(kRoadTable + 8U * id) + 12U));
            retired += wrap_cost;
        }
        call.set_arg(0, id);
        call.set_arg(1, first);
        call.set_arg(2, static_cast<std::uint32_t>(count) & 0xffffU);
        const n3::Run open = call.invoke([](auto& mm) { return n3::fn_00148ca0(mm); }, kFrameBytes);
        if (open.declined) {
            return false;
        }
        retired += open.retired;
        if (call.result64() == 0U) {
            flags = kStopFlag;
            retired += kClosedCost;
            return true;
        }
        call.set_arg(0, car);
        call.set_arg(1, kRoadPaths + 8U * id);
        call.set_arg(2, first);
        call.set_arg(3, last);
        const n3::Run range = call.invoke([](auto& mm) { return n3::fn_0014a560(mm); }, kFrameBytes);
        if (range.declined) {
            return false;
        }
        retired += kFlagsCallCost + range.retired;
        flags = call.result();
        return true;
    };

    // The vertex pair index the point is measured from comes from the START segment, whatever the walk does.
    const GuestAddress road = m.read_u32_le(car + kCarRoad);
    const std::uint32_t point_vertex =
        m.read_u16_le(m.read_u32_le(m.read_u32_le(road + 4U)) + 4U * start_segment + 2U * slot);

    std::uint32_t id_byte = m.read_u8(road);
    std::uint32_t current = start_segment;
    GuestAddress path = road;
    {
        const Hop first = hop_at(record(path, start_segment));
        if (!scan(first.road_id, first.segment, kStartWrapCost)) {
            return call.decline();
        }
        retired += kAfterStartCost;
    }
    // The loop moves one neighbour at a time; the first hop's quads were just tested, so stepping commits the
    // road id and segment of the hop whose own look-ahead is tested next.
    for (;;) {
        retired += kLoopTestCost;
        if ((flags & kStopFlag) != 0U) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        const Hop step = hop_at(record(path, current));
        const GuestAddress next_path = kRoadPaths + 8U * step.road_id;
        const Hop beyond = hop_at(record(next_path, step.segment));
        retired += kStepCost;
        if (!scan(beyond.road_id, beyond.segment, kStepWrapCost)) {
            return call.decline();
        }
        retired += kStepTailCost;
        if ((flags & kStopFlag) != 0U) {
            break;
        }
        id_byte = step.road_id;
        current = step.segment & 0xffffU;
        path = next_path;
        retired += kCommitCost;
    }

    retired += kExitCost;
    m.write_u8(byte_out, static_cast<std::uint8_t>(id_byte));
    m.write_u16_le(half_out, static_cast<std::uint16_t>(current));

    const GuestAddress pairs = m.read_u32_le(m.read_u32_le(path + 4U));
    const auto pair_entry = [&](const std::uint32_t segment, const std::uint32_t half) {
        return static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(pairs + 4U * segment + 2U * half))));
    };
    std::uint32_t first_vertex = pair_entry(current, 0U);
    std::uint32_t second_vertex = pair_entry(current, 1U);
    if (first_vertex == second_vertex) {
        retired += kDegenerateTestCost;
        if (current == 0U) {
            current = 1U;
            retired += kDegenerateFirstCost;
        } else {
            current = (current - 1U) & 0xffffU;
            retired += kDegenerateOtherCost;
        }
        first_vertex = pair_entry(current, 0U);
        second_vertex = pair_entry(current, 1U);
    }

    // t = clamp01(car.f664 / G)
    const std::uint32_t factor =
        min_bits(max_bits(fpu::div(m.read_u32_le(car + kCarFactor), m.read_u32_le(kScaleDivisor)), 0U), kOneBits);
    retired += kFactorCost;

    // VF1 = the first vertex, VF2 = the second; the edge VF1 = VF2 - VF1 keeps the first vertex's w.
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const auto vertex = [&](const std::uint32_t index) { return vertices + (index << 4U); };
    const std::uint32_t from = forward_edge ? first_vertex : second_vertex;
    const std::uint32_t to = forward_edge ? second_vertex : first_vertex;
    retired += forward_edge ? kEdgeForwardCost : kEdgeBackwardCost;
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex(from)));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex(to)));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kMaskXyz, true);  // vsub.xyz vf1,vf2,vf1
    const Gpr128 edge = cpu.read_vu0_vector(1U);

    // Squared length in lane x: vf1.x = 1, vf3 = edge * edge, x = (vf3.x + vf3.y) + 1 * vf3.z.
    cpu.write_vu0_vector(2U, edge);
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 3U, 2U, 2U, kMaskXyz);
    ee_cop2::vadda_broadcast(cpu, 3U, 3U, kMaskX, 1U);              // vadday.x acc,vf3,vf3y
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 3U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf3z
    retired += kLengthCost;
    if (fpu::lt(cpu.read_vu0_vector(1U).words[0], kTinyLengthBits)) {
        ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, kMaskXyz, false);  // vadd.xyz vf1,vf0,vf0 (w stays the edge's)
        retired += kTinyCost;
    } else {
        // The same length again, then Q = 1 / sqrt(length) and offset = edge * Q * (t * reach).
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);
        ee_cop2::vmul(cpu, 2U, 2U, 2U, kMaskXyz);
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                          // vrsqrt q,vf0w,vf1x
        cpu.write_vu0_vector(1U, edge);
        ee_cop2::vmul_control(cpu, 1U, 1U, kMaskXyz, ee_cop2::kControlQ);  // vmulq.xyz vf1,vf1,q
        const std::uint32_t reach = m.read_u32_le(m.read_u32_le(car + kCarObject) + kObjectReach);
        Gpr128 scale{};
        scale.words[0] = fpu::mul(factor, reach);
        cpu.write_vu0_vector(2U, scale);
        ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kMaskXyz, 0U);        // vmulx.xyz vf1,vf1,vf2x
        retired += kNormaliseCost;
    }

    // point = offset + vertex[start]; the quad store is last.
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex(point_vertex)));
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kMaskXyz, false);  // vadd.xyz vf1,vf2,vf1
    n3::store_quad_any(m, point_out, cpu.read_vu0_vector(1U));
    return call.finish(retired + kPointCost + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014b010, 1176U, 0xda1fc76f4cad7c18ULL, "RoadLookAheadPoint", kCeiling, road_look_ahead_point,
         EngineReturn::Nothing)
