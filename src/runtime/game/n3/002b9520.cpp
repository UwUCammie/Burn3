// 0x002b9520 (a0 = contact query record Q, a1 = object A, a2 = object B, a3 = flag byte): ContactPairQuery.
// Each object fills one half of Q: half + 0 gets its shape word (object + 520), half + 48 its world matrix (the four
// quads at the pointer object + 516 names) and half + 112 its inverse matrix (object + 112..175). A's half is at Q,
// B's at Q + 240. Q + 480 gets the flag byte and Q + 484 gets 0.
//
// The positions are the world rows 3 (Q + 96 and Q + 336). The VU0 chain of the listing takes the squared distance
// of their xyz lanes. If that is below the float at $gp - 29676 the query returns 0; otherwise the narrow phase
// ConvexContactPushApart (0x002b8920) runs on Q and its result, low byte only, is returned.
//
// The copies keep the listing's order (the shape word, then the pointer, then each quad is read before it is
// stored), so a record that overlaps an object is read exactly as the original reads it. The VU0 registers the
// listing leaves behind (vf1, vf2 and ACC) are written back before the decision, and each macro-mode op publishes its
// MAC and STATUS flags in the listing's order. 0x002b8920 does not write its own VU0 leftovers back, so on the
// narrow-phase path the VU0 state stays the original's only up to that call.
//
// Counts: the first 61 instructions (the copies, the distance test and the branch with its delay slot) run on both
// paths. Returning 0 adds 5 (daddu; beq and its delay slot; jr and its delay slot): 66. The narrow phase adds 8 (jal
// and its delay slot; beq and andi; beq and ld; jr and addiu) and the callee's own count: 69 + callee.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002b8920)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;

// Destination masks: x is the high bit, so the x lane alone is 8.
constexpr std::uint8_t kMaskX = 8U;

constexpr GuestAddress kSecondHalf = 240U;      // B's half of the record
constexpr GuestAddress kFlagWord = 480U;
constexpr GuestAddress kZeroWord = 484U;
constexpr GuestAddress kObjectWorld = 516U;     // object: pointer to its world matrix
constexpr GuestAddress kObjectShape = 520U;     // object: shape pointer, copied to the half's first word
constexpr GuestAddress kObjectInverse = 112U;   // object: inverse matrix, copied as it is
constexpr GuestAddress kHalfWorld = 48U;        // half: world matrix (64 bytes)
constexpr GuestAddress kHalfInverse = 112U;     // half: inverse matrix (64 bytes)
constexpr std::uint32_t kFrameBytes = 32U;      // the original's frame, lowered for the narrow phase

// lwc1 $f0, -29676($gp): the squared-distance threshold.
constexpr GuestAddress kThresholdOffset = 0U - 29676U;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kPrefixCost = 61U;
constexpr std::uint32_t kNearCost = kPrefixCost + 5U;
constexpr std::uint32_t kFarOwnCost = kPrefixCost + 8U;

// One object's half of the record, in the listing's order: the shape word, the world pointer, then the four world
// quads (each read, then stored), then the four inverse quads (each read, then stored). Returns the last world quad,
// which is the world matrix's row 3 (the position). Nothing later in the query stores over that quad, so the value
// is the one the original's VU0 loads from the record read back.
template <class Memory>
Gpr128 copy_object_half(Memory& m, const GuestAddress half, const GuestAddress object) {
    const std::uint32_t shape = m.read_u32_le(object + kObjectShape);
    m.write_u32_le(half, shape);
    const GuestAddress world = m.read_u32_le(object + kObjectWorld);
    Gpr128 position{};
    for (GuestAddress offset = 0U; offset < 64U; offset += 16U) {
        position = n3::load_quad_any(m, world + offset);
        n3::store_quad_any(m, half + kHalfWorld + offset, position);
    }
    for (GuestAddress offset = 0U; offset < 64U; offset += 16U) {
        n3::store_quad_any(m, half + kHalfInverse + offset, n3::load_quad_any(m, object + kObjectInverse + offset));
    }
    return position;
}

// The VU0 registers the listing leaves: vf1 (x: the squared distance, y, z, w: the caller's), vf2 (the squares) and
// ACC (x: squares.x + squares.y, y, z, w: the caller's).
struct DistanceChain {
    Gpr128 vf1{};
    Gpr128 vf2{};
    Gpr128 acc{};
};

// The listing's VU0 chain on the two position rows, each macro op publishing its MAC and STATUS flags in the order
// the listing issues them: vaddw.x (vf1.x = 0 + 1.0, the caller's other lanes kept), vsub.xyz (a - b, with b's w lane),
// vmul.xyz (the squares, vf2 takes them), vadday.x (ACC.x = squares.x + squares.y) and vmaddz.x (vf1.x = ACC.x +
// 1.0 * squares.z, against the 1.0 that the restored vf1 holds in its x lane).
DistanceChain distance_chain(guest::R5900State& cpu, const Gpr128& a, const Gpr128& b) {
    const guest::Vu0MacroFmacResult one = guest::execute_vu0_macro_broadcast_add_subtract(
        cpu.read_vu0_vector(1U), n3::kVf0, n3::kVf0, kMaskX, 3U, false);
    const guest::Vu0MacroFmacResult difference = guest::execute_vu0_macro_vector_add_subtract(b, a, b, n3::kMaskXyz, true);
    const guest::Vu0MacroFmacResult squares =
        guest::execute_vu0_macro_vector_multiply(difference.value, difference.value, difference.value, n3::kMaskXyz);
    const guest::Vu0MacroFmacResult sum = guest::execute_vu0_macro_broadcast_add_subtract(
        cpu.vu0_accumulator(), squares.value, squares.value, kMaskX, 1U, false);
    const guest::Vu0MacroFmacResult result = guest::execute_vu0_macro_broadcast_multiply_accumulate(
        one.value, sum.value, one.value, squares.value, kMaskX, 2U, false);
    guest::publish_vu0_macro_fmac_result(cpu, one.mac_flags);
    guest::publish_vu0_macro_fmac_result(cpu, difference.mac_flags);
    guest::publish_vu0_macro_fmac_result(cpu, squares.mac_flags);
    guest::publish_vu0_macro_fmac_result(cpu, sum.mac_flags);
    guest::publish_vu0_macro_fmac_result(cpu, result.mac_flags);
    return DistanceChain{result.value, squares.value, sum.value};
}

template <class Context>
n3::Run contact_pair_query(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress query = call.arg(0U);
    const std::uint32_t flag = call.arg(3U) & 0xffU;

    m.write_u32_le(query + kZeroWord, 0U);
    m.write_u32_le(query + kFlagWord, flag);
    const Gpr128 position_a = copy_object_half(m, query, call.arg(1U));
    const Gpr128 position_b = copy_object_half(m, query + kSecondHalf, call.arg(2U));

    const DistanceChain chain = distance_chain(call.cpu(), position_a, position_b);
    call.cpu().write_vu0_vector(1U, chain.vf1);
    call.cpu().write_vu0_vector(2U, chain.vf2);
    call.cpu().set_vu0_accumulator(chain.acc);

    const std::uint32_t squared = chain.vf1.words[0];
    const GuestAddress threshold_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kThresholdOffset;
    const std::uint32_t threshold = m.read_u32_le(threshold_address);
    if (fpu::lt(squared, threshold)) {
        call.return_value(0U);
        return call.finish(kNearCost);
    }
    // The narrow phase takes the record in a0, which the original never changes; it reads nothing else.
    const n3::Run narrow = call.invoke([](auto& machine) { return n3::fn_002b8920(machine); }, kFrameBytes);
    if (narrow.declined) {
        return call.decline();
    }
    call.return_value(call.result() & 0xffU);
    return call.finish(kFarOwnCost + narrow.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002b9520, 280U, 0xd70d394bcf6868e4ULL, "ContactPairQuery", kCeiling, contact_pair_query,
         EngineReturn::Integer)
