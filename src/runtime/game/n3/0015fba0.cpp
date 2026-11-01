// 0x0015fba0 (a0 list L, a1 signed limit, a2 index out, a3 value out): SelectLargestValueWithinLimit.
// L has n entries (the byte at +72, re-read on every pass). Entry i holds a record number r at +6+i and
// a value index k at +30+i. The value is word k of the int array the record reaches: record +20004 holds
// a pointer P, P+4 holds a pointer Q, and value = Q[k], where record = 0x00522660 + 280*r. The largest
// value no more than the limit wins (signed compares); a tie keeps the earlier entry. *a2 is -1 at entry
// and becomes k each time an entry is taken. *a3 receives the best value (-1 when none was taken) last.
// Returns record + 19940 for the taken record, or 0 when none was taken.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecords = 0x00522660U;   // lui 0x52, +9824
constexpr std::uint32_t kRecordStride = 280U;    // record r is at kRecords + 280*r
constexpr GuestAddress kHolderOffset = 20004U;   // record +20004: pointer P
constexpr GuestAddress kResultOffset = 19940U;   // record +19940: the pointer returned for it
constexpr GuestAddress kCountOffset = 72U;       // list +72: entry count (a byte)
constexpr GuestAddress kRecordOffset = 6U;       // list +6+i: record number (a byte)
constexpr GuestAddress kIndexOffset = 30U;       // list +30+i: value index (a byte)
constexpr std::int64_t kNone = -1;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;         // setup, then the jump to the loop test
constexpr std::uint32_t kTestCost = 4U;          // lbu, slt, bne, nop: runs once more than there are entries
constexpr std::uint32_t kFetchCost = 17U;        // entry fetch, lookups and the limit test, through the bne's nop
constexpr std::uint32_t kSkippedCost = 1U;       // the index increment after an entry above the limit
constexpr std::uint32_t kBestTestCost = 3U;      // slt, beq, nop
constexpr std::uint32_t kNotBetterCost = 1U;     // the index increment
constexpr std::uint32_t kTakenCost = 4U;         // sw, daddu, daddu, then the index increment
constexpr std::uint32_t kFoundCost = 13U;        // bgez, sw, the record address, jr, nop
constexpr std::uint32_t kNotFoundCost = 6U;      // bgez, sw, beq, daddu, jr, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Selection {
    std::uint32_t retired;
    std::int64_t best;     // the value stored to *a3
    std::int64_t record;   // the record number taken, or -1
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the
// retired count, or a count above kCeiling as soon as it passes that.
template <class Memory>
Selection select_largest(Memory& m, const GuestAddress list, const std::int64_t limit,
                         const GuestAddress index_out, const GuestAddress value_out) {
    m.write_u32_le(index_out, 0xffffffffU);
    std::uint32_t retired = kEntryCost;
    std::int64_t best = kNone;
    std::int64_t best_record = kNone;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        const std::uint32_t count = m.read_u8(list + kCountOffset);
        if (index >= count) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, best, best_record};
        }
        const GuestAddress entry = list + index;
        const std::uint32_t value_index = m.read_u8(entry + kIndexOffset);
        const std::uint32_t record_number = m.read_u8(entry + kRecordOffset);
        const GuestAddress record = kRecords + kRecordStride * record_number;
        const GuestAddress holder = m.read_u32_le(record + kHolderOffset);
        const GuestAddress values = m.read_u32_le(holder + 4U);
        const auto value =
            static_cast<std::int64_t>(static_cast<std::int32_t>(m.read_u32_le(values + 4U * value_index)));
        retired += kFetchCost;
        if (limit < value) {
            retired += kSkippedCost;
        } else {
            retired += kBestTestCost;
            if (best < value) {
                m.write_u32_le(index_out, value_index);
                best_record = static_cast<std::int64_t>(record_number);
                best = value;
                retired += kTakenCost;
            } else {
                retired += kNotBetterCost;
            }
        }
        ++index;
    }
    m.write_u32_le(value_out, static_cast<std::uint32_t>(best));
    retired += best_record >= 0 ? kFoundCost : kNotFoundCost;
    return {retired, best, best_record};
}

template <class Context>
std::uint32_t select_largest_value_within_limit_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Selection selection =
        select_largest(dry, context.cpu.read_gpr_u32(4U), static_cast<std::int64_t>(context.cpu.read_gpr_u64(5U)),
                       context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U));
    return selection.retired <= kCeiling ? selection.retired : 0U;
}

template <class Context>
n3::Run select_largest_value_within_limit(Context& context) {
    auto call = n3::guest_call(context);
    const Selection selection = select_largest(call.memory(), call.arg(0), static_cast<std::int64_t>(call.arg64(1)),
                                               call.arg(2), call.arg(3));
    if (selection.record >= 0) {
        const auto record_number = static_cast<std::uint32_t>(selection.record);
        call.return_value(kRecords + kRecordStride * record_number + kResultOffset);
    } else {
        call.return_value(0U);
    }
    return call.finish(selection.retired);
}

} // namespace

extern constinit const EngineFunction n3_0015fba0{0x0015fba0U, 200U, 0x435f1f6ae6d9e28aULL,
                                                  "SelectLargestValueWithinLimit", kCeiling,
                                                  n3::live<&select_largest_value_within_limit<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &select_largest_value_within_limit_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0015fba0, select_largest_value_within_limit, select_largest_value_within_limit_cost)
