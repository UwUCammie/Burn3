// 0x002fb770 ApplyCurveModifierSet (a0 = curve-set record S, a1 = input float array IN, a2 = output float table OUT;
// returns nothing). S has a u8 group count at +64 and a pointer at +60 to the group descriptors, 16 bytes each: +8 the
// curve array (8 bytes per curve) and +12 a u8 curve count. Group g is 20 words at OUT + 80 * g. Each group's words are
// set to 1.0f, then ApplyCurveModifier (0x002fb640) runs for each of the group's curves with IN and those words.
//
// The listing reads S+64 at every group test, S+60 once per group (before that group's stores), and the group's curve
// count and curve pointer at every curve test and pass, since a curve may write into the table. The reads below happen
// at the same points and in the same order. Each path's retired count is the listing's blocks (delay slots included, as
// the constants give them) plus each callee's count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002fb640)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Calls retiring more than this are left to the original (the dry run returns 0 above it).
constexpr std::uint32_t kCeiling = 20000U;

constexpr GuestAddress kDescriptorsField = 60U;  // S+60: pointer to the group descriptors
constexpr GuestAddress kGroupCountField = 64U;   // S+64: u8 group count
constexpr std::uint32_t kDescriptorStride = 16U;
constexpr GuestAddress kCurvesField = 8U;        // descriptor+8: pointer to the curve array
constexpr GuestAddress kCurveCountField = 12U;   // descriptor+12: u8 curve count
constexpr std::uint32_t kCurveStride = 8U;       // one curve: {P, n, in, out}
constexpr std::uint32_t kOutputStride = 80U;     // one group's output: 20 words
constexpr std::uint32_t kGroupWords = 20U;
constexpr std::uint32_t kOneBits = 0x3f800000U;  // 1.0f
constexpr std::uint32_t kFrameBytes = 128U;      // the listing's addiu $sp, $sp, -128

// Retired counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 14U;        // the prologue, then the jump to the group test
constexpr std::uint32_t kGroupTestCost = 4U;     // lbu, sltu, bne and its delay slot
constexpr std::uint32_t kGroupSetupCost = 30U;   // addresses, the S+60 load, 20 word stores, the jump to the curve test
constexpr std::uint32_t kCurveTestCost = 4U;     // lbu, sltu, bne and its delay slot
constexpr std::uint32_t kCurvePassCost = 7U;     // lw, sll, two moves, jal, its delay slot, and the curve count step
constexpr std::uint32_t kGroupEndCost = 1U;      // the group count step
constexpr std::uint32_t kReturnCost = 10U;       // the restores, jr and its delay slot

template <class Context>
n3::Run apply_curve_modifier_set(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress curve_set = call.arg(0U);
    const std::uint64_t inputs = call.arg64(1U);  // a1 goes on whole to every curve, as the listing's daddu does
    const GuestAddress outputs = call.arg(2U);
    std::uint32_t retired = kEntryCost;

    for (std::uint32_t group = 0U;; ++group) {
        retired += kGroupTestCost;
        const std::uint32_t group_count = memory.read_u8(curve_set + kGroupCountField);
        if (group >= group_count) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }

        // The descriptor is read before the words are set.
        const GuestAddress descriptor = memory.read_u32_le(curve_set + kDescriptorsField) + kDescriptorStride * group;
        const GuestAddress output = outputs + kOutputStride * group;
        for (std::uint32_t word = 0U; word < kGroupWords; ++word) {
            memory.write_u32_le(output + 4U * word, kOneBits);
        }
        retired += kGroupSetupCost;

        for (std::uint32_t curve = 0U;; ++curve) {
            retired += kCurveTestCost;
            const std::uint32_t curve_count = memory.read_u8(descriptor + kCurveCountField);
            if (curve >= curve_count) {
                break;
            }
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress curves = memory.read_u32_le(descriptor + kCurvesField);
            call.set_arg(0U, curves + kCurveStride * curve);
            call.set_arg64(1U, inputs);
            call.set_arg(2U, output);
            retired += kCurvePassCost;
            const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_002fb640(machine); }, kFrameBytes);
            if (callee.declined) {
                return call.decline();
            }
            retired += callee.retired;
        }
        retired += kGroupEndCost;
    }

    retired += kReturnCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002fb770, 280U, 0xefcdc52619a58ccbULL, "ApplyCurveModifierSet", kCeiling, apply_curve_modifier_set,
         EngineReturn::Nothing)
