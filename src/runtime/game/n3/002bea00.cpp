// 0x002bea00 (424 bytes) ProcessRecordTable, for the object a0 points at:
//   - when its byte +7 is set, a pass over the count (the signed word at 0x0057bae0) of 48-byte records from
//     0x0055efe0. A record's kind (signed byte +0) selects the target it hands on: kind 3 reads owner+176 (the owner
//     is the word at +12), and the target is that value + 4192 when it is nonzero; kinds 0, 1, 2, 4, 6 and 7 read the
//     word at owner+520; kind 5 and the others select nothing. A nonzero target makes the original call 0x002b9c10 and
//     0x002b96d0, which are not native yet, so the native declines the whole call.
//   - a nonzero byte +8, +6, +12 or +16 makes the original call 0x002be770, 0x002bddf0, 0x002bdb90 or 0x002bda00 (not
//     native yet): declined.
//   - otherwise EmptyCallback (0x001b20c0, native) runs with a0 = 0x0055e948 and the function returns with v0 = 0 (the
//     flag byte at +16 that the original read last).
// The paths the native covers store nothing outside the original's own stack frame.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001b20c0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecordTable = 0x0055efe0U;      // record i is at kRecordTable + 48 * i
constexpr GuestAddress kRecordCount = 0x0057bae0U;      // signed word: the number of records
constexpr std::uint32_t kRecordBytes = 48U;
constexpr GuestAddress kRecordOwner = 12U;              // word: the owner of the record
constexpr GuestAddress kKindThreeField = 176U;          // kind 3: word at owner+176
constexpr GuestAddress kKindThreeOffset = 4192U;        // ... and the target is that word + 4192 when nonzero
constexpr GuestAddress kKindTargetField = 520U;         // the other target kinds: word at owner+520
constexpr GuestAddress kObjectLoopFlag = 7U;            // byte: run the record pass when nonzero
constexpr GuestAddress kObjectSubcallFlags[] = {8U, 6U, 12U, 16U};
constexpr GuestAddress kCallbackArgument = 0x0055e948U;
constexpr std::uint32_t kFrameBytes = 80U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of the original (delay slots included) on each part of its path.
constexpr std::uint32_t kEntryCost = 9U;                // 002bea00-002bea20: frame, the byte +7 test, the object copy
constexpr std::uint32_t kPassEntryCost = 2U;            // 002bea24-002bea28: jump to the pass test, index = 0
constexpr std::uint32_t kPassTestCost = 5U;             // 002beb00-002beb10: count load, slt, bne and its slot
constexpr std::uint32_t kPassEndCost = 3U;              // 002beacc-002bead0 (no target), 002beafc (index += 1)
constexpr std::uint32_t kKindThreeEmptyCost = 17U;      // kind 3, owner+176 is zero
constexpr std::uint32_t kKindThreeCost = 19U;           // kind 3, owner+176 is nonzero
constexpr std::uint32_t kKindLowCost = 22U;             // kinds 0, 1, 2
constexpr std::uint32_t kKindHighCost = 33U;            // kinds 4, 6, 7
constexpr std::uint32_t kKindFiveCost = 31U;            // kind 5
constexpr std::uint32_t kKindOtherCost = 27U;           // every other kind
constexpr std::uint32_t kTailCost = 23U;                // 002beb14-002beba4 with the EmptyCallback jal and its slot

struct RecordTarget {
    GuestAddress address;   // the target the callees would act on; 0 when there is none
    std::uint32_t cost;     // retired by the original to decide that, up to its test of the target
};

template <class Memory>
RecordTarget find_record_target(Memory& memory, const GuestAddress record) {
    const auto kind = static_cast<std::int8_t>(memory.read_u8(record));
    if (kind == 3) {
        const GuestAddress owner = memory.read_u32_le(record + kRecordOwner);
        const GuestAddress special = memory.read_u32_le(owner + kKindThreeField);
        if (special == 0U) {
            return {0U, kKindThreeEmptyCost};
        }
        return {special + kKindThreeOffset, kKindThreeCost};
    }
    if ((kind >= 0 && kind <= 2) || kind == 4 || kind == 6 || kind == 7) {
        const GuestAddress owner = memory.read_u32_le(record + kRecordOwner);
        return {memory.read_u32_le(owner + kKindTargetField), kind <= 2 ? kKindLowCost : kKindHighCost};
    }
    return {0U, kind == 5 ? kKindFiveCost : kKindOtherCost};
}

template <class Context>
n3::Run process_record_table(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);
    std::uint32_t retired = kEntryCost;

    if (memory.read_u8(object + kObjectLoopFlag) != 0U) {
        retired += kPassEntryCost;
        const auto count = static_cast<std::int32_t>(memory.read_u32_le(kRecordCount));
        for (std::int32_t index = 0; index < count; ++index) {
            retired += kPassTestCost;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const RecordTarget target =
                find_record_target(memory, kRecordTable + kRecordBytes * static_cast<GuestAddress>(index));
            if (target.address != 0U) {
                return call.decline();
            }
            retired += target.cost + kPassEndCost;
        }
        retired += kPassTestCost;
    }

    for (const GuestAddress flag : kObjectSubcallFlags) {
        if (memory.read_u8(object + flag) != 0U) {
            return call.decline();
        }
    }

    call.set_arg(0, kCallbackArgument);
    const n3::Run callback = call.invoke([](auto& m) { return n3::fn_001b20c0(m); }, kFrameBytes);
    if (callback.declined) {
        return call.decline();
    }
    call.return_value(0U);
    return call.finish(retired + kTailCost + callback.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002bea00, 424U, 0xd668ad903b16d6c5ULL, "ProcessRecordTable", kCeiling, process_record_table,
         EngineReturn::Integer)
