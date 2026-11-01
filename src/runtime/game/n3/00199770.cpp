// 0x00199770 (a0 = world object W): ComputeVertexBoundsAndCenter. A leaf that returns nothing. W holds the vertex
// array pointer P (u32 at +0x12acf4), the record count N (signed, u32 at +0x12acf8) and the record array pointer R
// (u32 at +0x12ace8). Record r is 16 bytes at R + 16r: a pointer Q (u32 at +0) to pairs of u16 values (4 bytes a
// pair) and the value count n (u16 at +12), so n + 1 pairs. M is the largest u16 over all the pairs (0 when there
// are none), and the vertex quads P[0..M] are walked. The box MIN (quad at W+0x143820) and MAX (quad at W+0x143830)
// start as P[0]. For each later vertex, a lane of MAX takes the vertex's lane when that lane is not <= MAX's lane,
// and a lane of MIN takes it when it is < MIN's lane; the new lane is 0 + v on the VU0 add (so -0 becomes +0).
// The centre quad at W+0x143810 is (MIN + MAX) * 0.5 on
// xyz with w = MAX.w. When N <= 0 the box is not walked and the centre is made from MIN and MAX as they stand in
// memory. VF1..VF3, MAC and STATUS finish as the original leaves them; FCC holds the last compare's result.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kVertexField = 0x0012acf4U;   // lui 0x13, lw -21260: P
constexpr GuestAddress kCountField = 0x0012acf8U;    // lui 0x13, lw -21256: N (signed)
constexpr GuestAddress kRecordField = 0x0012ace8U;   // lui 0x13, lw -21272: R
constexpr GuestAddress kCountOffset = 12U;           // record: u16 n at +12 (the record has n + 1 pairs)
constexpr GuestAddress kMinimum = 0x00143820U;       // lui 0x14, ori 0x3820: MIN quad
constexpr GuestAddress kMaximum = 0x00143830U;       // ori 0x3830: MAX quad
constexpr GuestAddress kCentre = 0x00143810U;        // sqc2 at 14352 from 0x140000: centre quad
constexpr GuestAddress kFrameBytes = 432U;           // addiu sp,sp,-432: the function's frame
constexpr std::uint32_t kHalfBits = 0x3f000000U;     // lui a2,0x3f00: 0.5, the centre's multiplier
constexpr std::uint8_t kMaskX = 8U;                  // VU0 destination mask of the x lane
constexpr std::uint8_t kMaskY = 4U;                  // ... of the y lane
constexpr std::uint8_t kMaskZ = 2U;                  // ... of the z lane

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count. The captured calls retire about 506,000.
constexpr std::uint32_t kCeiling = 2000000U;

// Retired instruction counts of the listing's blocks, delay slots included. Bracketed: the block addresses.
constexpr std::uint32_t kEntryCost = 8U;             // [0x199770..0x19978c] frame, N, blez and its delay slot
constexpr std::uint32_t kTailCost = 19U;             // [0x199968..0x1999a0] 15: centre; [0x199a04..0x199a10] 4: return
constexpr std::uint32_t kRecordsEntryCost = 3U;      // [0x199790..0x199798] M = 0, r = 0, jump to the record test
constexpr std::uint32_t kRecordTestCost = 3U;        // [0x199808..0x199810] r < N: once per record, once at the end
constexpr std::uint32_t kRecordHeadCost = 9U;        // [0x19979c..0x1997bc] R, Q, n + 1, jump to the pair test
constexpr std::uint32_t kRecordStepCost = 1U;        // [0x199804] r++
constexpr std::uint32_t kPairTestCost = 3U;          // [0x1997f8..0x199800] k < n + 1: once per pair, once at the end
constexpr std::uint32_t kFirstCost = 7U;             // [0x1997c0..0x1997d8] first u16, M < first, branch and delay slot
constexpr std::uint32_t kSecondCost = 4U;            // [0x1997e0..0x1997ec] second u16, M < second, branch and delay slot
constexpr std::uint32_t kRaiseCost = 1U;             // [0x1997dc], [0x1997f0]: M = the value, only when it is above M
constexpr std::uint32_t kPairStepCost = 1U;          // [0x1997f4] k++
constexpr std::uint32_t kBoxCost = 26U;              // [0x199814..0x199878] P, P[0] to MIN and MAX (see walk_bounds)
constexpr std::uint32_t kVertexTestCost = 3U;        // [0x19995c..0x199964] i <= M: once per vertex, once at the end
constexpr std::uint32_t kVertexCost = 56U;           // [0x19987c..0x199958] one vertex's compares, without the updates
constexpr std::uint32_t kLaneCost = 4U;              // one lane update: [0x1999a4..0x199a00], lqc2, vadd, beq, sqc2

// What the walk leaves behind. Nothing is written by it: the wrapper stores MIN and MAX when walked, and the VU0
// tail reads them. mac_flags is the OR of the lane updates' MAC flags, which publish_vu0_macro_fmac_result turns
// into their STATUS sticky bits. The updates' VF1/VF2 writes are overwritten by the tail, so nothing else remains.
struct Bounds {
    std::uint32_t retired{};
    bool walked{};                    // N > 0: MIN and MAX were rebuilt from the vertices
    bool compared{};                  // M >= 1: the walk made compares, so FCC holds the last one
    bool last_compare{};              // the last compare: MIN.z against vertex M's z (before its update)
    std::uint16_t mac_flags{};
    GuestAddress vertices{};          // P (walked only)
    std::uint32_t vertex_count{};     // M + 1 (walked only)
    Gpr128 minimum{};
    Gpr128 maximum{};
};

// VADDx/VADDy/VADDz of a box quad: the masked lane becomes 0 + v's lane (the VU0 add), the others keep the box's
// lanes, which is the quad the original's lqc2/sqc2 pair leaves in memory.
void lane_update(Gpr128& box, const Gpr128& v, const std::uint8_t mask, const std::size_t lane, Bounds& out) {
    const auto result = guest::execute_vu0_macro_broadcast_add_subtract(box, n3::kVf0, v, mask, lane, false);
    box = result.value;
    out.mac_flags = static_cast<std::uint16_t>(out.mac_flags | result.mac_flags);
    out.retired += kLaneCost;
}

// The walk on guest memory (the implementation) or on a DryMemory (call_cost). It never writes: MIN and MAX are
// kept in locals, which is equivalent because the walk reads the box only through them (call_cost declines the
// calls whose vertices could touch it). The two stores of P[0] are not reproduced: the wrapper stores MIN and MAX
// whole after the walk, which overwrites them. Above kCeiling the walk returns as soon as the count passes it.
template <class Memory>
Bounds walk_bounds(const Memory& m, const GuestAddress world) {
    Bounds out{};
    out.retired = kEntryCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(world + kCountField));
    if (count <= 0) {
        out.retired += kTailCost;
        return out;
    }
    out.retired += kRecordsEntryCost;
    std::uint32_t largest = 0U;
    for (std::int32_t record = 0; record < count; ++record) {
        out.retired += kRecordTestCost;
        if (out.retired > kCeiling) {
            return out;
        }
        out.retired += kRecordHeadCost;
        // The record array pointer is re-read per record, as the original does (nothing in this walk writes it).
        const GuestAddress entry = m.read_u32_le(world + kRecordField) + 16U * static_cast<std::uint32_t>(record);
        const GuestAddress pairs = m.read_u32_le(entry);
        const std::uint32_t pair_count = static_cast<std::uint32_t>(m.read_u16_le(entry + kCountOffset)) + 1U;
        for (std::uint32_t k = 0U; k < pair_count; ++k) {
            out.retired += kPairTestCost;
            if (out.retired > kCeiling) {
                return out;
            }
            const GuestAddress pair = pairs + 4U * k;
            const std::uint32_t first = m.read_u16_le(pair);
            out.retired += kFirstCost;
            if (largest < first) {
                largest = first;
                out.retired += kRaiseCost;
            }
            const std::uint32_t second = m.read_u16_le(pair + 2U);
            out.retired += kSecondCost;
            if (largest < second) {
                largest = second;
                out.retired += kRaiseCost;
            }
            out.retired += kPairStepCost;
        }
        out.retired += kPairTestCost;
        out.retired += kRecordStepCost;
    }
    out.retired += kRecordTestCost;
    out.retired += kBoxCost;

    // Box walk over P[0..M]. P is read once: the original re-reads its pointer per vertex, but nothing in the walk
    // writes W+0x12acf4, so the value does not change.
    const GuestAddress vertices = m.read_u32_le(world + kVertexField);
    const Gpr128 first_vertex = n3::load_quad_any(m, vertices);
    Gpr128 minimum = first_vertex;
    Gpr128 maximum = first_vertex;
    bool last_compare = false;
    for (std::uint32_t i = 1U;; ++i) {
        out.retired += kVertexTestCost;
        if (out.retired > kCeiling) {
            return out;
        }
        if (largest < i) {
            break;
        }
        out.retired += kVertexCost;
        const Gpr128 v = n3::load_quad_any(m, vertices + 16U * i);
        // Each compare reads the lane as it now stands; the lanes are independent, so the locals give the same
        // compares as the original's memory. The z compare of MIN is the last compare of the vertex, and it is
        // taken before its update.
        if (!fpu::le(v.words[0], maximum.words[0])) {
            lane_update(maximum, v, kMaskX, 0U, out);
        }
        if (!fpu::le(v.words[1], maximum.words[1])) {
            lane_update(maximum, v, kMaskY, 1U, out);
        }
        if (!fpu::le(v.words[2], maximum.words[2])) {
            lane_update(maximum, v, kMaskZ, 2U, out);
        }
        if (fpu::lt(v.words[0], minimum.words[0])) {
            lane_update(minimum, v, kMaskX, 0U, out);
        }
        if (fpu::lt(v.words[1], minimum.words[1])) {
            lane_update(minimum, v, kMaskY, 1U, out);
        }
        last_compare = fpu::lt(v.words[2], minimum.words[2]);
        if (last_compare) {
            lane_update(minimum, v, kMaskZ, 2U, out);
        }
    }
    out.retired += kTailCost;
    out.walked = true;
    out.compared = largest >= 1U;
    out.last_compare = last_compare;
    out.vertices = vertices;
    out.vertex_count = largest + 1U;
    out.minimum = minimum;
    out.maximum = maximum;
    return out;
}

// C.LT.S / C.LE.S set FCC (FCR31 bit 23) to their result, as guest::ee_cop1::compare_s does.
void set_condition(guest::R5900State& cpu, const bool value) {
    cpu.set_fcr31_bits(value ? cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition
                             : cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition);
}

// Whether [first, first + first_length) and [second, second + second_length) share a byte of physical RAM (the
// mask drops the KSEG and uncached-alias bits). A range that runs past the masked space counts as touching.
bool hits(const GuestAddress first, const std::uint64_t first_length, const GuestAddress second,
          const std::uint64_t second_length) {
    constexpr std::uint64_t kPhysicalLimit = 0x20000000ULL;
    const std::uint64_t a = first & 0x1fffffffU;
    const std::uint64_t b = second & 0x1fffffffU;
    if (a + first_length > kPhysicalLimit || b + second_length > kPhysicalLimit) {
        return true;
    }
    return a < b + second_length && b < a + first_length;
}

// Declines (0) every call whose walk the implementation cannot reproduce on its own: a count above the ceiling, a
// vertex range that touches MIN/MAX or the frame (the walk would read the original's own stores there), or a centre
// quad inside the frame (it would change the epilogue's restores). The original runs those calls.
template <class Context>
std::uint32_t compute_vertex_bounds_cost(const Context& context) {
    const GuestAddress world = context.cpu.read_gpr_u32(4U);
    n3::DryMemory dry{context.memory};
    const Bounds bounds = walk_bounds(dry, world);
    if (bounds.retired > kCeiling) {
        return 0U;
    }
    const GuestAddress frame = context.cpu.read_gpr_u32(29U) - kFrameBytes;
    if (hits(world + kCentre, 16U, frame, kFrameBytes)) {
        return 0U;
    }
    if (bounds.walked) {
        const GuestAddress box = world + kMinimum;
        const std::uint64_t vertex_bytes = 16U * static_cast<std::uint64_t>(bounds.vertex_count);
        if (hits(bounds.vertices, vertex_bytes, box, 32U) || hits(bounds.vertices, vertex_bytes, frame, kFrameBytes) ||
            hits(box, 32U, frame, kFrameBytes)) {
            return 0U;
        }
    }
    return bounds.retired;
}

template <class Context>
n3::Run compute_vertex_bounds(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress world = call.arg(0);
    const Bounds bounds = walk_bounds(m, world);
    if (bounds.walked) {
        // The original's last stores to MIN and MAX. They replace the P[0] stores and every lane update's store.
        n3::store_quad_any(m, world + kMinimum, bounds.minimum);
        n3::store_quad_any(m, world + kMaximum, bounds.maximum);
    }
    // The lane updates' STATUS sticky bits. publish_vu0_macro_fmac_result summarises the MAC flags by group, and the
    // summary of an OR of flags is the OR of their summaries, so one publish gives the sticky bits of all the
    // updates. The two tail publishes below then overwrite the summary and MAC, as the original's do.
    if (bounds.mac_flags != 0U) {
        guest::publish_vu0_macro_fmac_result(cpu, bounds.mac_flags);
    }
    if (bounds.compared) {
        set_condition(cpu, bounds.last_compare);
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, world + kMaximum));  // lqc2 vf1,0(MAX)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, world + kMinimum));  // lqc2 vf2,0(MIN)
    cpu.write_gpr_sign_extended_32(6U, kHalfBits);                 // lui a2,0x3f00
    cpu.write_vu0_vector(3U, cpu.read_gpr(6U));                    // qmtc2 a2,vf3
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, false);    // vadd.xyz vf1,vf2,vf1
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, n3::kMaskXyz, 0U);    // vmulx.xyz vf1,vf1,vf3x
    n3::store_quad_any(m, world + kCentre, cpu.read_vu0_vector(1U));  // sqc2 vf1,CENTRE
    return call.finish(bounds.retired);
}

} // namespace

extern constinit const EngineFunction n3_00199770{0x00199770U, 676U, 0xc161e91e447b0e40ULL,
                                                  "ComputeVertexBoundsAndCenter", kCeiling, n3::live<&compute_vertex_bounds<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, &compute_vertex_bounds_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00199770, compute_vertex_bounds, compute_vertex_bounds_cost)
