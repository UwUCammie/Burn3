// 0x002f4940 (a0 = owner record): ResetSlotValues. The owner holds an array of 64-byte records: the base
// pointer at +20 and the record count at +24 (unsigned). Every record gets the float -9999.0 at +48, then the
// owner's word at +28 is cleared. Returns 1 in v0; f0 is left at -9999.0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kBasePointer = 20U;      // owner + 20: base of the record array
constexpr GuestAddress kRecordCount = 24U;      // owner + 24: number of records (unsigned)
constexpr GuestAddress kFlagWord = 28U;         // owner + 28: cleared once the loop is done
constexpr GuestAddress kFieldOffset = 48U;      // record + 48: the float written
constexpr std::uint32_t kRecordShift = 6U;      // records are 64 bytes apart
constexpr std::int32_t kFillInteger = -9999;    // the float is cvt.s.w of this integer

// Retired instruction counts from the listing, delay slots included: the entry (addiu, daddu, mtc1, beq and
// its delay slot cvt.s.w), each loop test (lw, sltu, bne and its nop), each loop body (lw, sll, addiu, addu,
// swc1) and the exit (sw, jr and its delay slot addiu). n records retire 12 + 9n instructions.
constexpr std::uint32_t kEntryCost = 5U;
constexpr std::uint32_t kTestCost = 4U;
constexpr std::uint32_t kBodyCost = 5U;
constexpr std::uint32_t kExitCost = 3U;

// Calls retiring more than this are left to the original (call_cost returns 0), so a reported count is always
// the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;
// The implementation runs the whole loop, as the original does, whatever its length.
constexpr std::uint32_t kNoCeiling = 0xffffffffU;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// instruction count, or a count above `ceiling` as soon as it passes that. The test and the body read the record
// count and the base on every pass, as the listing does, because the stores may change them.
template <class Memory>
std::uint32_t reset_slots(Memory& m, const GuestAddress owner, const std::uint32_t fill,
                          const std::uint32_t ceiling) {
    std::uint32_t retired = kEntryCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (index >= m.read_u32_le(owner + kRecordCount)) {
            break;
        }
        if (retired > ceiling) {
            return retired;
        }
        const GuestAddress record = m.read_u32_le(owner + kBasePointer) + (index << kRecordShift);
        m.write_u32_le(record + kFieldOffset, fill);
        ++index;
        retired += kBodyCost;
    }
    m.write_u32_le(owner + kFlagWord, 0U);
    return retired + kExitCost;
}

template <class Context>
std::uint32_t reset_slot_values_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired =
        reset_slots(dry, context.cpu.read_gpr_u32(4U), fpu::from_int(kFillInteger), kCeiling);
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run reset_slot_values(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t fill = fpu::from_int(kFillInteger);
    const std::uint32_t retired = reset_slots(call.memory(), call.arg(0), fill, kNoCeiling);
    call.cpu().write_fpr_bits(0U, fill);
    call.return_value(1U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002f4940{0x002f4940U, 68U, 0x1b30ed194e61047bULL, "ResetSlotValues",
                                                  kCeiling, n3::live<&reset_slot_values<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &reset_slot_values_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002f4940, reset_slot_values, reset_slot_values_cost)
