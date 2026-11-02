// 0x00149180 PlaceRouteSplinePoints (a0 = vehicle, a1 = edge array (u32 entries, the low 16 bits name the edge),
// a2 = strip object, a3 = float[6] fractions, t0/t1/t2 = window output quads (may be 0), t3 = "ahead" output quad
// (may be 0)). Returns nothing.
//
// The vehicle's six route control points (16-byte quads at vehicle + 0, or at vehicle + 96 when the byte at
// vehicle + 336 is set) are put at fraction[i] of the length of strip edge edge[i]: StripEdgeLength (0x00147c10)
// gives the horizontal length, the edge's end vertices (u16 pair in the strip's edge table, 16-byte vertices at
// u32[0x0064d354]) give the direction. A degenerate edge (from == to) uses its vertex as is; otherwise the point is
// from + normalise(to - from) * (fraction * length), xyz only (w keeps whatever VF4.w held, as the listing does).
// Each point is then passed through NeighbourQuadEntry (0x00149c90) with the edge as its quad index, which copies it
// onto itself when it lies inside the quad.
//
// Afterwards t = clamp(float at [vehicle + 340] + 7120, 0, 1) gives the cubic basis b = (t^3, t^2, t, 1). The
// constant matrices at 0x004b5e70.. ("ahead") and 0x004b5eb0.. ("window") turn b into weights, and the weights mix
// control rows into up to four output quads: t3 = rows 1..4 by the ahead weights, t0/t1/t2 = rows 0..3 / 1..4 /
// 2..5 by the window weights (xyz only, the weight's w picks the last row). Finally, on an open strip whose second
// edge entry names one of the last three quads, later windows are overwritten by earlier ones.
//
// VU0 registers are used exactly as the listing uses them (the helpers are the interpreter's own), because the
// control points' w lane and the end state of VF1..VF5 come from them. The frame words the original spills are not
// reproduced.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00147c10)
N3_CALLEE(00149c90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 912U;
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kPoints = 6U;                    // control points, a loop of exactly six passes
constexpr GuestAddress kVertices = 0x0064d354U;          // u32: the 16-byte vertex table
constexpr GuestAddress kAheadMatrix = 0x004b5e70U;       // four rows of 16 bytes
constexpr GuestAddress kWindowMatrix = 0x004b5eb0U;      // four rows of 16 bytes

// Vehicle, strip object and strip layouts.
constexpr GuestAddress kVehicleSecondSet = 336U;         // byte: control points live at +96
constexpr GuestAddress kSecondSetOffset = 96U;
constexpr GuestAddress kVehicleWorld = 340U;             // the object holding the route progress
constexpr GuestAddress kWorldProgress = 7120U;           // float
constexpr GuestAddress kObjectStrip = 4U;                // strip object + 4: the strip
constexpr GuestAddress kStripEdgeTable = 0U;             // strip + 0: u16 pair per edge
constexpr GuestAddress kStripQuadCount = 12U;            // strip + 12: u16
constexpr GuestAddress kStripClosed = 14U;               // strip + 14: byte
constexpr GuestAddress kEdgeSecond = 4U;                 // edge array + 4: the second entry, a whole word

constexpr std::uint32_t kOneBits = 0x3f800000U;

// Retired instruction counts of the listing's blocks, delay slots included (brief.py).
constexpr std::uint32_t kHeadCost = 22U;                 // 0x149180..0x1491d4
constexpr std::uint32_t kFirstSetCost = 1U + 1U;         // byte clear: 0x1491e0 and 0x1491e4
constexpr std::uint32_t kSecondSetCost = 2U + 1U;        // byte set: 0x1491d8..0x1491dc and 0x1491e4
constexpr std::uint32_t kPassHeadCost = 18U;             // 0x1491e8..0x14922c, up to the length call's delay slot
constexpr std::uint32_t kPassPlacedCost = 30U;           // 0x149230..0x1492a4: direction * distance + from
constexpr std::uint32_t kPassCopiedCost = 8U;            // 0x1492a8..0x1492c4: from == to
constexpr std::uint32_t kPassTailCost = 10U;             // 0x1492c8..0x1492ec: the NeighbourQuadEntry call, loop test
constexpr std::uint32_t kBasisCost = 22U;                // 0x1492f0..0x149344
constexpr std::uint32_t kAheadCost = 29U;                // 0x149348..0x1493b8
constexpr std::uint32_t kWeightsCost = 19U;              // 0x1493bc..0x149404
constexpr std::uint32_t kWindowCost = 13U;               // each window's mix (0x149408, 0x149444, 0x149480)
constexpr std::uint32_t kWindowTestCost = 2U;            // each window's beq and delay slot
constexpr std::uint32_t kStripTestCost = 4U;             // 0x1494b4..0x1494c0: the closed flag
constexpr std::uint32_t kEndTestsCost = 5U;              // 0x1494c4..0x1494d4: the n - 3 test
constexpr std::uint32_t kSecondTestCost = 3U;            // 0x1494d8..0x1494e0: the n - 2 test
constexpr std::uint32_t kThirdTestCost = 3U;             // 0x1494e4..0x1494ec: the n - 1 test
constexpr std::uint32_t kEpilogueCost = 13U;             // 0x149584..0x1495b4


template <class Memory>
void load_vf(guest::R5900State& cpu, Memory& m, const unsigned reg, const GuestAddress at) {
    cpu.write_vu0_vector(reg, n3::load_quad_any(m, at));
}

// VF2 = b = (t^3, t^2, t, 1) from t = clamp(progress, 0, 1): max.s with +0.0, then min.s with 1.0. The lanes of
// VF1/VF2 the sequence does not write are overwritten later (VF2 lane by lane here, VF1 by the matrix loads), so
// only the lanes it reads are set up.
void build_basis(guest::R5900State& cpu, const std::uint32_t progress_bits) {
    cpu.write_fpr_bits(0U, 0U);                                                             // mtc1 zero,f0
    cpu.write_fpr_bits(2U, kOneBits);                                                       // mtc1 a0,f2
    cpu.write_vu0_vector(1U, Gpr128{{kOneBits, 0U, 0U, 0U}});                              // qmtc2 a0,vf1 (a0 = 1.0)
    cpu.write_fpr_bits(1U, progress_bits);                                                  // lwc1 f1,7120(v1)
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, 1U, 0U);                                       // vmulx.w vf2,vf0,vf1x
    ee_cop1::max_s(cpu, 0U, 1U, 0U);                                                        // max.s f0,f1,f0
    ee_cop1::min_s(cpu, 0U, 0U, 2U);                                                        // min.s f0,f0,f2
    const std::uint32_t t_bits = cpu.read_fpr_bits(0U);
    cpu.write_vu0_vector(1U, Gpr128{{t_bits, (t_bits >> 31U) != 0U ? ~0U : 0U, 0U, 0U}});  // qmtc2 v1,vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 2U, 0U, false);                                // vaddx.z vf2,vf0,vf1x
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, 2U, 2U);                                       // vmulz.z vf1,vf2,vf2z
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 4U, 2U, false);                                // vaddz.y vf2,vf0,vf1z
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, 4U, 2U);                                       // vmulz.y vf1,vf2,vf2z
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 8U, 1U, false);                                // vaddy.x vf2,vf0,vf1y
}

// Weights of the "ahead" matrix: VF4 = b on entry, VF5 = weights (all four lanes) on exit.
template <class Memory>
void ahead_weights(guest::R5900State& cpu, Memory& m) {
    load_vf(cpu, m, 2U, kAheadMatrix + 48U);
    load_vf(cpu, m, 1U, kAheadMatrix + 0U);
    ee_cop2::vmul_broadcast(cpu, 5U, 2U, 4U, 15U, 3U);                                      // vmulw.xyzw vf5,vf2,vf4w
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 4U, 15U, 0U);                                      // vmulx.xyzw vf3,vf1,vf4x
    load_vf(cpu, m, 1U, kAheadMatrix + 32U);
    load_vf(cpu, m, 2U, kAheadMatrix + 16U);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 4U, 15U, 2U);                                      // vmulz.xyzw vf1,vf1,vf4z
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 4U, 15U, 1U);                                      // vmuly.xyzw vf2,vf2,vf4y
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 15U, false);                                      // vadd.xyzw vf2,vf3,vf2
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 15U, false);                                      // vadd.xyzw vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 5U, 1U, 5U, 15U, false);                                      // vadd.xyzw vf5,vf1,vf5
}

// The output quad "ahead": control rows 1..4 by the ahead weights in VF5 (xyz). VF1 = the point.
template <class Memory>
void mix_ahead(guest::R5900State& cpu, Memory& m, const GuestAddress control) {
    load_vf(cpu, m, 2U, control + 64U);
    load_vf(cpu, m, 1U, control + 16U);
    ee_cop2::vmul_broadcast(cpu, 4U, 2U, 5U, 14U, 3U);                                      // vmulw.xyz vf4,vf2,vf5w
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 5U, 14U, 0U);                                      // vmulx.xyz vf3,vf1,vf5x
    load_vf(cpu, m, 2U, control + 32U);
    load_vf(cpu, m, 1U, control + 48U);
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 5U, 14U, 1U);                                      // vmuly.xyz vf2,vf2,vf5y
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, false);                                      // vadd.xyz vf2,vf3,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 5U, 14U, 2U);                                      // vmulz.xyz vf1,vf1,vf5z
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);                                      // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 14U, false);                                      // vadd.xyz vf1,vf1,vf4
}

// Weights of the window matrix: b is in VF2's source (the stored basis); returns the weights (VF1 on exit).
template <class Memory>
Gpr128 window_weights(guest::R5900State& cpu, Memory& m, const Gpr128& basis) {
    load_vf(cpu, m, 2U, kWindowMatrix + 48U);
    cpu.write_vu0_vector(4U, basis);                                                        // lqc2 vf4,896(sp)
    load_vf(cpu, m, 1U, kWindowMatrix + 0U);
    ee_cop2::vmul_broadcast(cpu, 5U, 2U, 4U, 15U, 3U);                                      // vmulw.xyzw vf5,vf2,vf4w
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 4U, 15U, 0U);                                      // vmulx.xyzw vf3,vf1,vf4x
    load_vf(cpu, m, 2U, kWindowMatrix + 16U);
    load_vf(cpu, m, 1U, kWindowMatrix + 32U);
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 4U, 15U, 1U);                                      // vmuly.xyzw vf2,vf2,vf4y
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 15U, false);                                      // vadd.xyzw vf2,vf3,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 4U, 15U, 2U);                                      // vmulz.xyzw vf1,vf1,vf4z
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 15U, false);                                      // vadd.xyzw vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 5U, 15U, false);                                      // vadd.xyzw vf1,vf1,vf5
    return cpu.read_vu0_vector(1U);
}

// Window 0: control rows 0..3 by the window weights (loaded into VF4).
template <class Memory>
void mix_window_first(guest::R5900State& cpu, Memory& m, const GuestAddress control, const Gpr128& weights) {
    load_vf(cpu, m, 2U, control + 48U);
    cpu.write_vu0_vector(4U, weights);                                                      // qmtc2 v1,vf4
    load_vf(cpu, m, 1U, control + 0U);
    ee_cop2::vmul_broadcast(cpu, 5U, 2U, 4U, 14U, 3U);                                      // vmulw.xyz vf5,vf2,vf4w
    ee_cop2::vmul_broadcast(cpu, 3U, 1U, 4U, 14U, 0U);                                      // vmulx.xyz vf3,vf1,vf4x
    load_vf(cpu, m, 2U, control + 16U);
    load_vf(cpu, m, 1U, control + 32U);
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 4U, 14U, 1U);                                      // vmuly.xyz vf2,vf2,vf4y
    ee_cop2::vadd_vector(cpu, 2U, 3U, 2U, 14U, false);                                      // vadd.xyz vf2,vf3,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 4U, 14U, 2U);                                      // vmulz.xyz vf1,vf1,vf4z
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);                                      // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 5U, 14U, false);                                      // vadd.xyz vf1,vf1,vf5
}

// Windows 1 and 2: four control rows from `rows` by the window weights (loaded into VF3).
template <class Memory>
void mix_window(guest::R5900State& cpu, Memory& m, const GuestAddress rows, const Gpr128& weights) {
    cpu.write_vu0_vector(3U, weights);                                                      // lqc2 vf3,880(sp)
    load_vf(cpu, m, 2U, rows + 0U);
    load_vf(cpu, m, 1U, rows + 16U);
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, 14U, 0U);                                      // vmulx.xyz vf2,vf2,vf3x
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 14U, 1U);                                      // vmuly.xyz vf1,vf1,vf3y
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 14U, false);                                      // vadd.xyz vf2,vf2,vf1
    load_vf(cpu, m, 1U, rows + 32U);
    load_vf(cpu, m, 4U, rows + 48U);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 14U, 2U);                                      // vmulz.xyz vf1,vf1,vf3z
    ee_cop2::vmul_broadcast(cpu, 4U, 4U, 3U, 14U, 3U);                                      // vmulw.xyz vf4,vf4,vf3w
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);                                      // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 14U, false);                                      // vadd.xyz vf1,vf1,vf4
}

// `if (to && from) *to = *from`, 2 instructions for the copy; returns the count of the tests and the copy.
template <class Memory>
std::uint32_t copy_if_both(Memory& m, const GuestAddress to, const GuestAddress from, const std::uint32_t copy_cost) {
    if (to == 0U) return kWindowTestCost;
    if (from == 0U) return kWindowTestCost + kWindowTestCost;
    n3::store_quad_any(m, to, n3::load_quad_any(m, from));
    return kWindowTestCost + kWindowTestCost + copy_cost;
}

// The end-of-strip fixups after the windows; returns their retired count (0x1494b4 up to, not including, the epilogue).
template <class Memory>
std::uint32_t strip_end_fixups(Memory& m, const GuestAddress edges, const GuestAddress strip_object,
                               const GuestAddress window0, const GuestAddress window1, const GuestAddress window2) {
    const GuestAddress strip = m.read_u32_le(strip_object + kObjectStrip);
    if (m.read_u8(strip + kStripClosed) != 0U) return kStripTestCost;
    const std::uint32_t quads = m.read_u16_le(strip + kStripQuadCount);
    const std::uint32_t second = m.read_u32_le(edges + kEdgeSecond);
    std::uint32_t cost = kStripTestCost + kEndTestsCost;
    if (second == quads - 3U) {
        // The last window takes the middle one, else the first: 0x149524..0x149550.
        cost += kWindowTestCost;
        if (window2 == 0U) return cost;
        cost += kWindowTestCost;
        if (window1 != 0U) {
            n3::store_quad_any(m, window2, n3::load_quad_any(m, window1));
            return cost + 3U;
        }
        cost += kWindowTestCost;
        if (window0 == 0U) return cost;
        n3::store_quad_any(m, window2, n3::load_quad_any(m, window0));
        return cost + 3U;
    }
    cost += kSecondTestCost;
    const bool second_last = second == quads - 2U;
    if (!second_last) {
        cost += kThirdTestCost;
        if (second != quads - 1U) return cost;
    }
    // The last two quads: windows 2 and 1 both take window 0 (0x149554.. for n - 2, 0x1494f0.. for n - 1).
    cost += copy_if_both(m, window2, window0, 2U);
    // The second copy's own tests: its instruction cost is 3 on the n - 1 path, 2 on the n - 2 path.
    cost += copy_if_both(m, window1, window0, second_last ? 2U : 3U);
    return cost;
}

template <class Context>
n3::Run place_route_spline_points(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const std::uint64_t vehicle64 = call.arg64(0);
    const GuestAddress vehicle = call.arg(0);
    const GuestAddress edges = call.arg(1);
    const std::uint64_t strip_object64 = call.arg64(2);
    const GuestAddress strip_object = call.arg(2);
    const GuestAddress fractions = call.arg(3);
    const GuestAddress window0 = call.arg(4);
    const GuestAddress window1 = call.arg(5);
    const GuestAddress window2 = call.arg(6);
    const GuestAddress ahead = call.arg(7);

    std::uint32_t retired = kHeadCost;
    GuestAddress control = vehicle;
    if (m.read_u8(vehicle + kVehicleSecondSet) != 0U) {
        control += kSecondSetOffset;
        retired += kSecondSetCost;
    } else {
        retired += kFirstSetCost;
    }

    // Exactly six passes, whatever the data: the loop bound is a constant, so no ceiling test is needed.
    for (std::uint32_t i = 0U; i < kPoints; ++i) {
        const std::uint32_t edge = m.read_u16_le(edges + 4U * i);
        const std::uint32_t fraction = m.read_u32_le(fractions + 4U * i);
        call.set_arg64(0, strip_object64);
        call.set_arg64(1, edge);
        const n3::Run length = call.invoke([](auto& machine) { return n3::fn_00147c10(machine); }, kFrameBytes);
        if (length.declined) return call.decline();
        const std::uint32_t distance = fpu::mul(fraction, call.float_result());

        const GuestAddress strip = m.read_u32_le(strip_object + kObjectStrip);
        const GuestAddress ends = m.read_u32_le(strip + kStripEdgeTable) + edge * 4U;
        const std::uint32_t from = m.read_u16_le(ends);
        const std::uint32_t to = m.read_u16_le(ends + 2U);
        const GuestAddress vertices = m.read_u32_le(kVertices);
        const GuestAddress slot = control + 16U * i;
        retired += kPassHeadCost + length.retired + kPassTailCost;
        if (from == to) {
            n3::store_quad_any(m, slot, n3::load_quad_any(m, vertices + (to << 4U)));
            retired += kPassCopiedCost;
        } else {
            // dir = normalise(vertex[to] - vertex[from]) by the VU0 reciprocal square root; point = dir * distance
            // + from (xyz). The listing parks VF1.x = 1.0 across the loads, so VF1's other lanes survive.
            ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                        // vaddw.x vf1,vf0,vf0w
            const Gpr128 saved = cpu.read_vu0_vector(1U);                                   // qmfc2 a1,vf1
            load_vf(cpu, m, 3U, vertices + (from << 4U));
            load_vf(cpu, m, 1U, vertices + (to << 4U));
            ee_cop2::vadd_vector(cpu, 4U, 1U, 3U, 14U, true);                               // vsub.xyz vf4,vf1,vf3
            ee_cop2::vmul(cpu, 2U, 4U, 4U, 14U);                                            // vmul.xyz vf2,vf4,vf4
            ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                                  // vadday.x acc,vf2,vf2y
            cpu.write_vu0_vector(1U, saved);                                                // qmtc2 a1,vf1
            ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);         // vmaddz.x vf1,vf1,vf2z
            ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                       // vrsqrt q,vf0w,vf1x
            ee_cop2::vmul_control(cpu, 4U, 4U, 14U, ee_cop2::kControlQ);                    // vmulq.xyz vf4,vf4,q
            cpu.write_vu0_vector(1U, cpu.read_vu0_vector(4U));                              // qmfc2 v0,vf4; qmtc2 v0,vf1
            // qmtc2 v1,vf2: only lane x is read, and VF2 is overwritten before it is read again.
            cpu.write_vu0_vector(2U, Gpr128{{distance, (distance >> 31U) != 0U ? ~0U : 0U, 0U, 0U}});
            ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U);                              // vmulx.xyz vf1,vf1,vf2x
            ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, 14U, false);                              // vadd.xyz vf1,vf1,vf3
            n3::store_quad_any(m, slot, cpu.read_vu0_vector(1U));                           // sqc2 vf1,0(v0)
            retired += kPassPlacedCost;
        }

        // NeighbourQuadEntry(vehicle, slot, strip object, edge, slot): copies the point onto itself when it is inside.
        call.set_arg64(0, vehicle64);
        call.set_arg(1, slot);
        call.set_arg64(2, strip_object64);
        call.set_arg64(3, edge);
        call.set_arg(4, slot);
        const n3::Run neighbour = call.invoke([](auto& machine) { return n3::fn_00149c90(machine); }, kFrameBytes);
        if (neighbour.declined) return call.decline();
        retired += neighbour.retired;
    }

    // The basis b = (t^3, t^2, t, 1) from the vehicle's route progress.
    const GuestAddress world = m.read_u32_le(vehicle + kVehicleWorld);
    build_basis(cpu, m.read_u32_le(world + kWorldProgress));
    const Gpr128 basis = cpu.read_vu0_vector(2U);                                           // sqc2 vf2,896(sp)
    retired += kBasisCost;
    if (ahead != 0U) {
        cpu.write_vu0_vector(4U, basis);                                                    // qmtc2 a0,vf4
        ahead_weights(cpu, m);
        mix_ahead(cpu, m, control);
        n3::store_quad_any(m, ahead, cpu.read_vu0_vector(1U));                              // sqc2 vf1,0(t3)
        retired += kAheadCost;
    }
    const Gpr128 weights = window_weights(cpu, m, basis);                                   // sqc2 vf1,880(sp)
    retired += kWeightsCost;
    if (window0 != 0U) {
        mix_window_first(cpu, m, control, weights);
        n3::store_quad_any(m, window0, cpu.read_vu0_vector(1U));                            // sqc2 vf1,0(s4)
        retired += kWindowCost;
    }
    retired += kWindowTestCost;
    if (window1 != 0U) {
        mix_window(cpu, m, control + 16U, weights);
        n3::store_quad_any(m, window1, cpu.read_vu0_vector(1U));                            // sqc2 vf1,0(s3)
        retired += kWindowCost;
    }
    retired += kWindowTestCost;
    if (window2 != 0U) {
        mix_window(cpu, m, control + 32U, weights);
        n3::store_quad_any(m, window2, cpu.read_vu0_vector(1U));                            // sqc2 vf1,0(s2)
        retired += kWindowCost;
    }
    retired += strip_end_fixups(m, edges, strip_object, window0, window1, window2) + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00149180, 1080U, 0x96ebc2aa20070d02ULL, "PlaceRouteSplinePoints", kCeiling, place_route_spline_points,
         EngineReturn::Nothing)
