// 0x0019d5e0 (a0 = object): UpdateLinkedCurveState. Sets the state word at object +1932 to 0, 1 or 2 and returns
// nothing (its only caller, 0x0019d6d0, reloads v0 before it reads it). The state is 0 when the enable word at
// 0x0064d3a0 is zero, when the linked pointer at object +1888 is the default record 0x0064d3b0, or when
// GetLinkedRecordByte (0x001419b0) of the linked pointer is nonzero. Otherwise two CurveTableValue (0x0015ac00)
// calls read the linked pointer + 4288 and the default record + 4288: the state is 1 when the default value is
// <= the linked value (c.ole.s $f0, $f20 after the second call), else 2.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001419b0)
N3_CALLEE(0015ac00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kEnableWord = 0x0064d3a0U;     // lui 0x65, lw -11360
constexpr GuestAddress kDefaultRecord = 0x0064d3b0U;  // lui 0x65, addiu -11344 (the address itself is the value)
constexpr GuestAddress kLinkedOffset = 1888U;         // object +1888: the linked pointer
constexpr GuestAddress kStateOffset = 1932U;          // object +1932: the state word
constexpr GuestAddress kCurveRecordOffset = 4288U;    // the curve record is a record pointer + 4288
constexpr std::uint32_t kFrameBytes = 64U;            // the entry's addiu $sp, -64, which the callees run under
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing, delay slots included. The callees' counts are added where they run.
constexpr std::uint32_t kEntryCost = 9U;              // 0x19d5e0..0x19d600: the enable word and its branch
constexpr std::uint32_t kLinkedCost = 7U;             // 0x19d604..0x19d61c: the default-record and linked pointer tests
constexpr std::uint32_t kLookupCost = 4U;             // 0x19d620..0x19d62c: jal GetLinkedRecordByte and the byte test
constexpr std::uint32_t kStoreCost = 2U;              // 0x19d630, 0x19d638, 0x19d640: store zero, jump to the epilogue
constexpr std::uint32_t kCurveCost = 9U;              // 0x19d648..0x19d668: both jal CurveTableValue, the compare, bc1t
constexpr std::uint32_t kStateOneCost = 2U;           // 0x19d678..0x19d67c: state 1, falls into the epilogue
constexpr std::uint32_t kStateTwoCost = 3U;           // 0x19d66c..0x19d674: state 2, jumps to the epilogue
constexpr std::uint32_t kEpilogueCost = 6U;           // 0x19d680..0x19d694: the restores, jr ra and its delay slot

template <class Context>
n3::Run update_linked_curve_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u32_le(kEnableWord) == 0U) {
        m.write_u32_le(object + kStateOffset, 0U);
        return call.finish(kEntryCost + kStoreCost + kEpilogueCost);
    }
    const GuestAddress linked = m.read_u32_le(object + kLinkedOffset);
    if (linked == kDefaultRecord) {
        m.write_u32_le(object + kStateOffset, 0U);
        return call.finish(kEntryCost + kLinkedCost + kStoreCost + kEpilogueCost);
    }
    call.set_arg(0, linked);
    const n3::Run record_byte = call.invoke([](auto& machine) { return n3::fn_001419b0(machine); }, kFrameBytes);
    if (record_byte.declined) {
        return call.decline();
    }
    if (call.result() != 0U) {
        m.write_u32_le(object + kStateOffset, 0U);
        return call.finish(kEntryCost + kLinkedCost + kLookupCost + record_byte.retired + kStoreCost + kEpilogueCost);
    }
    call.set_arg(0, linked + kCurveRecordOffset);
    const n3::Run linked_curve = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
    if (linked_curve.declined) {
        return call.decline();
    }
    const std::uint32_t linked_value = call.float_result();
    call.set_arg(0, kDefaultRecord + kCurveRecordOffset);
    const n3::Run default_curve = call.invoke([](auto& machine) { return n3::fn_0015ac00(machine); }, kFrameBytes);
    if (default_curve.declined) {
        return call.decline();
    }
    // c.ole.s $f0, $f20: the default value (f0, from the second call) is at most the linked value (f20).
    const bool default_not_above = fpu::le(call.float_result(), linked_value);
    m.write_u32_le(object + kStateOffset, default_not_above ? 1U : 2U);
    const std::uint32_t retired = kEntryCost + kLinkedCost + kLookupCost + record_byte.retired + kCurveCost +
                                  linked_curve.retired + default_curve.retired +
                                  (default_not_above ? kStateOneCost : kStateTwoCost) + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0019d5e0, 184U, 0x2ab1b95a7ad13e06ULL, "UpdateLinkedCurveState", kCeiling, update_linked_curve_state,
         EngineReturn::Nothing)
