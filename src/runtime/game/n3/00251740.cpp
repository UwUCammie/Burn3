// 0x00251740 (a0 = object, a1 = vector quad V): FindContainingCell, returns v0.
// The object keeps a ring of records: its size is the word at +12 and a hint index the word at +76. The search
// starts four below the hint (plus the ring size when that is negative) and visits at most nine indexes. Each
// index is the signed remainder of the window index by the ring size (div, then mfhi), and the record pointer at
// object + 380 + 4 * remainder holds three quads at +96, +112 and +128. VU0 computes
// R = (M1 * V.x + M2 * V.z) - M3 per lane (VF2 = V; VF1 ends as R). An index hits when no lane of R has its
// sign bit set: the result is that remainder, otherwise -1. A clear byte at +68 returns -1 without touching VU0.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kFlagOffset = 68U;       // lbu: the search runs only when this byte is set
constexpr GuestAddress kRingOffset = 12U;       // lw: the ring size (the divisor)
constexpr GuestAddress kHintOffset = 76U;       // lw: the hint, the window starts four below it
constexpr GuestAddress kRecordTable = 380U;     // one record pointer per remainder
constexpr std::uint32_t kWindow = 9U;           // the indexes start .. start + 8
constexpr std::uint32_t kNotFound = 0xffffffffU;
constexpr std::int32_t kInt32Min = static_cast<std::int32_t>(0x80000000U);
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kClearCost = 9U;        // flag clear: the -1 return
constexpr std::uint32_t kEntryCost = 15U;       // prologue, flag test, window start, lqc2 vf2 and its first test
constexpr std::uint32_t kWrapCost = 3U;         // negative window start: the ring size is added
constexpr std::uint32_t kBodyCost = 21U;        // one index: divide, record lookup, VU0 sequence, sign test
constexpr std::uint32_t kStepCost = 4U;         // next index, then its test (the search goes on)
constexpr std::uint32_t kFoundCost = 2U;        // jr and its delay slot after a hit
constexpr std::uint32_t kExhaustCost = 4U;      // window exhausted: the -1 result and its return

// DIV as the interpreter computes it: a zero divisor and INT_MIN / -1 have defined results instead of trapping.
void divide_signed(const std::int32_t left, const std::int32_t right, std::int32_t& quotient,
                   std::int32_t& remainder) {
    if (right == 0) {
        quotient = left < 0 ? 1 : -1;
        remainder = left;
    } else if (left == kInt32Min && right == -1) {
        quotient = kInt32Min;
        remainder = 0;
    } else {
        quotient = left / right;
        remainder = left % right;
    }
}

template <class Context>
n3::Run find_containing_cell(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress vector_address = call.arg(1);
    if (m.read_u8(object + kFlagOffset) == 0U) {
        call.return_value(kNotFound);
        return call.finish(kClearCost);
    }
    const auto ring = static_cast<std::int32_t>(m.read_u32_le(object + kRingOffset));
    std::uint32_t candidate = m.read_u32_le(object + kHintOffset) - 4U;
    std::uint32_t retired = kEntryCost;
    if (static_cast<std::int32_t>(candidate) < 0) {
        candidate += static_cast<std::uint32_t>(ring);
        retired += kWrapCost;
    }
    // The original stores the vector quad to its frame and reloads it into VF2 (lqc2): VF2 is the quad itself.
    // The window is nine indexes at most; a limit that wraps lets none run.
    const std::uint32_t limit = candidate + kWindow;
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vector_address));
    if (static_cast<std::int32_t>(candidate) >= static_cast<std::int32_t>(limit)) {
        call.return_value(kNotFound);
        return call.finish(retired + kExhaustCost);
    }
    for (;;) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kBodyCost;
        std::int32_t quotient{};
        std::int32_t remainder{};
        divide_signed(static_cast<std::int32_t>(candidate), ring, quotient, remainder);
        cpu.write_lo(0U, static_cast<std::uint64_t>(quotient));
        cpu.write_hi(0U, static_cast<std::uint64_t>(remainder));
        const GuestAddress record =
            m.read_u32_le(object + (static_cast<std::uint32_t>(remainder) << 2U) + kRecordTable);
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 96U));                       // qmtc2 a3,vf1
        ee_cop2::vmula_broadcast(cpu, 1U, 2U, n3::kMaskXyzw, false);                       // vmulax.xyzw acc,vf1,vf2x
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 112U));                     // qmtc2 a2,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyzw, 2U, false, true, false);  // vmaddaz.xyzw acc,vf1,vf2z
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, record + 128U));                     // qmtc2 a1,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, n3::kMaskXyzw, 3U, false, false, true);  // vmsubw.xyzw vf1,vf1,vf0w
        const guest::Gpr128 difference = cpu.read_vu0_vector(1U);                          // qmfc2 a1,vf1
        const std::uint32_t signs =
            difference.words[0] | difference.words[1] | difference.words[2] | difference.words[3];
        if ((signs & 0x80000000U) == 0U) {
            call.return_value(static_cast<std::uint32_t>(remainder));
            return call.finish(retired + kFoundCost);
        }
        retired += kStepCost;
        candidate += 1U;
        if (static_cast<std::int32_t>(candidate) >= static_cast<std::int32_t>(limit)) {
            call.return_value(kNotFound);
            return call.finish(retired + kExhaustCost);
        }
    }
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(00251740, 184U, 0x48758c7b3e254637ULL, "FindContainingCell", kCeiling, find_containing_cell,
         EngineReturn::Integer)
