// 0x0032fd90 (a0 = pair record): CheckPairProximityVisible. The record's objects A (+72) and B (+68) are tested
// together, and a null A returns 0 at once. Each object's signed index byte (+6572) selects a 48-byte entry of the
// table at 0x0055efec; the entry's pointer leads to the object's 128-bit vector at +176 (VA for A, VB for B).
// Returns v0 = 1 when every test below passes, 0 when one fails:
//   1. G0 * VA.w is not below C1, where G0 is the float at $gp-30980 and w is the vector's top lane.
//   2. G0 * VA.w - G0 * VB.w is at most C2 (the MULA.S / MSUB.S pair).
//   3. The float x at +60 is in range: x < C3 when x >= 0, and -x < 0.5 * (the float at B+9272) when x < 0.
//   4. The VU0 dot product of B's xyz vectors at +32 and +6352 is not below C4 (the sequence below).
//   5. The float at +56 is at most 10.0.
// C1..C4 are the floats at 0x01e755b8, 0x01e755e8, 0x01e755e4 and 0x01e755f8. The qmtc2 of VA into vf1 sits in the
// delay slot of the first test, so it runs on every path past the null test and is written before that test.
// The VU0 registers and the COP1 accumulator end as the original leaves them; FCR31 is not updated, as in the other
// n3 natives.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kTable = 0x0055efecU;              // lui 0x56, addiu -4116: 48-byte entries, pointer at +0
constexpr GuestAddress kEntryBytes = 48U;
constexpr GuestAddress kIndexOffset = 6572U;              // signed index byte of an object
constexpr GuestAddress kVectorOffset = 176U;              // the object's quadword, through its entry's pointer
constexpr GuestAddress kObjectAOffset = 72U;
constexpr GuestAddress kObjectBOffset = 68U;
constexpr GuestAddress kRangeOffset = 60U;                // x
constexpr GuestAddress kLimitOffset = 56U;                // compared with 10.0
constexpr GuestAddress kScaleOffset = 9272U;              // B's float, scaled by 0.5 for a negative x
constexpr GuestAddress kDotLeftOffset = 32U;              // B's vectors for the dot product
constexpr GuestAddress kDotRightOffset = 6352U;
constexpr std::int32_t kGlobalOffset = -30980;            // $gp - 30980: G0
constexpr GuestAddress kC1 = 0x01e755b8U;
constexpr GuestAddress kC2 = 0x01e755e8U;
constexpr GuestAddress kC3 = 0x01e755e4U;
constexpr GuestAddress kC4 = 0x01e755f8U;
constexpr std::uint32_t kHalfBits = 0x3f000000U;          // 0.5f (lui immediate)
constexpr std::uint32_t kTenBits = 0x41200000U;           // 10.0f (lui immediate)

// Retired instruction counts of the listing's exits, delay slots included. The declared cost is the static longest
// path of the listing (113); the largest path a real call can take is kNegativePassCost.
constexpr std::uint32_t kNullCost = 8U;                   // A is null
constexpr std::uint32_t kProductCost = 34U;               // G0 * VA.w below C1
constexpr std::uint32_t kDifferenceCost = 53U;            // G0 * VA.w - G0 * VB.w above C2
constexpr std::uint32_t kRangeCost = 78U;                 // x >= 0 and x >= C3
constexpr std::uint32_t kNegativeRangeCost = 81U;         // x < 0 and -x not below half of the float at B+9272
constexpr std::uint32_t kDotCost = 91U;                   // dot below C4, x >= 0
constexpr std::uint32_t kNegativeDotCost = 97U;           // dot below C4, x < 0
constexpr std::uint32_t kPassCost = 99U;                  // limit test passes, x >= 0
constexpr std::uint32_t kFailCost = 98U;                  // limit test fails, x >= 0
constexpr std::uint32_t kNegativePassCost = 105U;         // limit test passes, x < 0
constexpr std::uint32_t kNegativeFailCost = 104U;         // limit test fails, x < 0

// The pointer in the table entry that an object's signed index byte selects; its 128-bit vector is 176 bytes past it.
GuestAddress entry_pointer(const auto& m, const GuestAddress object) {
    const auto index = static_cast<std::int8_t>(m.read_u8(object + kIndexOffset));
    return m.read_u32_le(kTable + static_cast<GuestAddress>(static_cast<std::int32_t>(index)) * kEntryBytes);
}

template <class Context>
n3::Run check_pair_proximity_visible(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    const GuestAddress object_a = m.read_u32_le(record + kObjectAOffset);
    if (object_a == 0U) {
        call.return_value(0U);
        return call.finish(kNullCost);
    }
    const GuestAddress object_b = m.read_u32_le(record + kObjectBOffset);
    const GuestAddress pointer_a = entry_pointer(m, object_a);
    const GuestAddress pointer_b = entry_pointer(m, object_b);
    const GuestAddress global = cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kGlobalOffset);
    const std::uint32_t g0 = m.read_u32_le(global);

    const guest::Gpr128 va = n3::load_quad_any(m, pointer_a + kVectorOffset);
    cpu.write_vu0_vector(1U, va);   // qmtc2 VA,vf1: runs on every path past the null test
    const std::uint32_t va_w = va.words[3];
    const std::uint32_t product = fpu::mul(g0, va_w);
    if (fpu::lt(product, m.read_u32_le(kC1))) {
        call.return_value(0U);
        return call.finish(kProductCost);
    }
    // mula.s: the accumulator holds the product from here on; msub.s below reads it and leaves it alone.
    cpu.set_fpu_accumulator_bits(product);

    const guest::Gpr128 vb = n3::load_quad_any(m, pointer_b + kVectorOffset);
    const std::uint32_t difference = fpu::msub(product, g0, vb.words[3]);   // msub.s: ACC - G0 * VB.w
    if (!fpu::le(difference, m.read_u32_le(kC2))) {
        call.return_value(0U);
        return call.finish(kDifferenceCost);
    }

    const std::uint32_t x = m.read_u32_le(record + kRangeOffset);
    const bool negative = fpu::lt(x, 0U);
    const bool in_range = negative
                              ? fpu::lt(fpu::neg(x), fpu::mul(kHalfBits, m.read_u32_le(object_b + kScaleOffset)))
                              : fpu::lt(x, m.read_u32_le(kC3));
    if (!in_range) {
        call.return_value(0U);
        return call.finish(negative ? kNegativeRangeCost : kRangeCost);
    }

    // The listing's VU0 sequence. vaddw.x sets vf1.x to 1.0 and the whole of vf1 is saved; the loads replace vf1
    // and vf2, the saved vf1 is restored, and vmaddz.x forms the dot product in vf1.x.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                       // qmfc2 v1,vf1
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, object_b + kDotLeftOffset));     // lqc2 vf2,32(B)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, object_b + kDotRightOffset));    // lqc2 vf1,6352(B)
    ee_cop2::vmul(cpu, 2U, 2U, 1U, 14U);                                       // vmul.xyz vf2,vf2,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                             // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved);                                           // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t dot = cpu.read_vu0_vector(1U).words[0];                // qmfc2 v0,vf1; mtc1 v0,f1
    cpu.write_fpr_bits(1U, dot);
    if (fpu::lt(dot, m.read_u32_le(kC4))) {
        call.return_value(0U);
        return call.finish(negative ? kNegativeDotCost : kDotCost);
    }

    const bool pass = fpu::le(m.read_u32_le(record + kLimitOffset), kTenBits);
    call.return_value(pass ? 1U : 0U);
    if (negative) {
        return call.finish(pass ? kNegativePassCost : kNegativeFailCost);
    }
    return call.finish(pass ? kPassCost : kFailCost);
}

} // namespace

extern constinit const EngineFunction n3_0032fd90{0x0032fd90U, 488U, 0xff355fbc201fdeaeULL,
                                                  "CheckPairProximityVisible", 113U, n3::live<&check_pair_proximity_visible<NativeFunctionContext>>,
                                                  EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0032fd90, check_pair_proximity_visible)
