// 0x002bebb0 (no arguments): UpdateUsageHighWaterMarks. Five running maxima ("high-water marks") kept as
// signed 32-bit words at gp-relative addresses. Each mark keeps the larger (signed) of itself and a value:
//   mark 1 (gp-22868) against the word at 0x0057BAE0
//   mark 2 (gp-22864) against the word at 0x00648180
//   mark 3 (gp-22860) against the word at 0x0064517C
//   mark 4 (gp-22856) against the count of set bits among bits 0..31 of the doubleword at 0x00648190
//   mark 5 (gp-22852) against the count of set bits among all 64 bits of the doubleword at 0x00648188
// The blocks run in that order. Each loads its mark, then its value, compares, and stores the value when the
// mark is less. Returns nothing (v0 is never written).
//
// The retired count is exact. Both bit loops run a fixed 32 and 64 iterations, each retiring 7 instructions
// plus one more for every set bit, so the count is 719 + c1 + c2 + 2 * (marks raised), where c1 and c2 are the
// set-bit counts. The loop trip counts never depend on data, and the longest path (every mark raised, every
// counted bit set) is 825, which is the declared cost. No call_cost resolver is needed.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Sources and mark offsets, as the listing computes them. R = 0x0055EF70 (lui 0x56, addiu -4240) is the base
// of the lui-built addresses; the lui values add 0x20000, 0xF0000 and 0xE0000 to it.
constexpr GuestAddress kSource1 = 0x0057BAE0U;       // R + 0x20000 - 13456
constexpr GuestAddress kSource2 = 0x00648180U;       // R + 0xF0000 - 28144
constexpr GuestAddress kSource3 = 0x0064517CU;       // R + 0xE0000 + 25100
constexpr GuestAddress kBits32Source = 0x00648190U;  // R + 0xF0000 - 28128 (doubleword, ld)
constexpr GuestAddress kBits64Source = 0x00648188U;  // R + 0xF0000 - 28136 (doubleword, ld)
constexpr std::uint32_t kMark1Offset = 22868U;       // lw -22868($gp)
constexpr std::uint32_t kMark2Offset = 22864U;       // lw -22864($gp)
constexpr std::uint32_t kMark3Offset = 22860U;       // lw -22860($gp)
constexpr std::uint32_t kMark4Offset = 22856U;       // lw -22856($gp)
constexpr std::uint32_t kMark5Offset = 22852U;       // lw -22852($gp)
constexpr std::uint32_t kBits32Count = 32U;          // the bit loop over the low word of Q1
constexpr std::uint32_t kBits64Count = 64U;          // the bit loop over the whole of Q2

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kFirstBlockCost = 9U;        // lui, lui, addiu, lw, addu, lw, slt, bne and its nop
constexpr std::uint32_t kSecondBlockCost = 7U;       // lui, lw, addu, lw, slt, bne and its nop
constexpr std::uint32_t kThirdBlockCost = 7U;        // as kSecondBlockCost
constexpr std::uint32_t kBitsSetupCost = 6U;         // lui, daddu, addu, addiu, ld, daddu
constexpr std::uint32_t kBitStepCost = 7U;           // and, beq, nop, addiu, slti, bne, dsll (a clear bit)
constexpr std::uint32_t kBitSetCost = 1U;            // the extra addiu of a set bit
constexpr std::uint32_t kBitsTestCost = 4U;          // lw, slt, bne and its nop
constexpr std::uint32_t kRaiseCost = 2U;             // the raise stub's beq and its delay-slot sw
constexpr std::uint32_t kTailCost = 4U;              // beq and its nop, then jr $ra and its nop
constexpr std::uint32_t kLongestPath = 825U;         // 719 + 32 + 64 + 2 * 5

static_assert(kFirstBlockCost + kSecondBlockCost + kThirdBlockCost + 2U * (kBitsSetupCost + kBitsTestCost) +
                      kTailCost + kBitStepCost * (kBits32Count + kBits64Count) ==
                  719U,
              "fixed part of the retired count");
static_assert(kLongestPath == 719U + kBits32Count + kBits64Count + 2U * 5U, "longest path");

// Counts the set bits among bits 0..(bits - 1) of `word` as the listing's loop does, and adds the instructions
// that loop retires to `retired`.
std::uint32_t count_set_bits(const std::uint64_t word, const std::uint32_t bits, std::uint32_t& retired) {
    std::uint32_t set = 0U;
    for (std::uint32_t bit = 0U; bit < bits; ++bit) {
        retired += kBitStepCost;
        if ((word & (std::uint64_t{1U} << bit)) != 0U) {
            ++set;
            retired += kBitSetCost;
        }
    }
    return set;
}

// The compare and store of one mark: the value is stored when the mark is less than it (signed).
// Returns true when it was stored.
bool raise_mark(auto& memory, const GuestAddress mark_address, const std::uint32_t mark,
                const std::uint32_t value) {
    if (static_cast<std::int32_t>(mark) >= static_cast<std::int32_t>(value)) {
        return false;
    }
    memory.write_u32_le(mark_address, value);
    return true;
}

template <class Context>
n3::Run update_usage_high_water_marks(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    std::uint32_t retired = 0U;

    const GuestAddress mark1 = gp - kMark1Offset;
    const std::uint32_t mark1_value = m.read_u32_le(mark1);
    const std::uint32_t source1 = m.read_u32_le(kSource1);
    retired += kFirstBlockCost;
    if (raise_mark(m, mark1, mark1_value, source1)) {
        retired += kRaiseCost;
    }

    const GuestAddress mark2 = gp - kMark2Offset;
    const std::uint32_t mark2_value = m.read_u32_le(mark2);
    const std::uint32_t source2 = m.read_u32_le(kSource2);
    retired += kSecondBlockCost;
    if (raise_mark(m, mark2, mark2_value, source2)) {
        retired += kRaiseCost;
    }

    const GuestAddress mark3 = gp - kMark3Offset;
    const std::uint32_t mark3_value = m.read_u32_le(mark3);
    const std::uint32_t source3 = m.read_u32_le(kSource3);
    retired += kThirdBlockCost;
    if (raise_mark(m, mark3, mark3_value, source3)) {
        retired += kRaiseCost;
    }

    retired += kBitsSetupCost;
    const std::uint64_t bits32 = m.read_u64_le(kBits32Source);
    const std::uint32_t count1 = count_set_bits(bits32, kBits32Count, retired);
    const GuestAddress mark4 = gp - kMark4Offset;
    const std::uint32_t mark4_value = m.read_u32_le(mark4);
    retired += kBitsTestCost;
    if (raise_mark(m, mark4, mark4_value, count1)) {
        retired += kRaiseCost;
    }

    retired += kBitsSetupCost;
    const std::uint64_t bits64 = m.read_u64_le(kBits64Source);
    const std::uint32_t count2 = count_set_bits(bits64, kBits64Count, retired);
    const GuestAddress mark5 = gp - kMark5Offset;
    const std::uint32_t mark5_value = m.read_u32_le(mark5);
    retired += kBitsTestCost;
    if (raise_mark(m, mark5, mark5_value, count2)) {
        retired += kRaiseCost;
    }

    retired += kTailCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002bebb0{0x002bebb0U, 292U, 0xf672704a7cd2ea9fULL,
                                                  "UpdateUsageHighWaterMarks", kLongestPath,
                                                  n3::live<&update_usage_high_water_marks<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002bebb0, update_usage_high_water_marks)
