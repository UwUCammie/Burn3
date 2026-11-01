// 0x00157180 (a0 = object P): ResetSlotUnlessIdle. The word at P+636 is compared with 3; when it is equal the function
// returns at once. Otherwise it resets P's slot fields: a loop over the count word at 0x0064d37c (word -1.0f at
// P+1240+4i, bytes 0 at P+1264+i and P+1270+i), a run of byte and word stores, then the call of 0x003684a0 with
// a0 = P+292. That callee is only "j 0x00369120" and a nop, and 0x00369120 (ResetSlotTableDefaults) is a fixed
// 379-instruction block of constant stores; both are inlined here, with the 2 + 379 instructions they retire.
// A non-zero word at P+632 calls 0x00363b00 first (stub: sb at a0+281). No recorded call takes that path, so it is
// declined: call_cost returns 0 and the original runs it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kLoopCountWord = 0x0064d37cU;   // lui 0x65, lw -11396: loop trip count (signed slt)
constexpr GuestAddress kFirstWordOffset = 632U;        // non-zero calls 0x00363b00 (declined)
constexpr GuestAddress kStateOffset = 636U;            // equal to kIdleState returns early
constexpr GuestAddress kTableObjectOffset = 292U;      // a0 of the 0x003684a0 call
constexpr std::uint32_t kIdleState = 3U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;       // -1.0f bits (lui 0xbf80)
constexpr std::uint8_t kZeroByte = 0U;

// Retired instructions, delay slots included.
constexpr std::uint32_t kEarlyReturnCost = 15U;        // 7 + 4 + 4 (see brief): entry, state test, epilogue
constexpr std::uint32_t kBeforeLoopCost = 15U;         // 7 (entry, P+632 test) + 4 (P+636 != 3) + 4 (daddu, lui, beq, lui)
constexpr std::uint32_t kLoopIterationCost = 11U;      // body 7 + the taken count test 4
constexpr std::uint32_t kLoopExitCost = 4U;            // the final count test
constexpr std::uint32_t kStoresAndCallCost = 21U;      // 0x001571f0..0x00157240 including jal and its delay slot
constexpr std::uint32_t kCalleeCost = 2U + 379U;       // 0x003684a0 (j, nop) and 0x00369120
constexpr std::uint32_t kEpilogueCost = 4U;

constexpr std::uint32_t kCeiling = 20000U;

// Callee 0x00369120, inlined (see n3/00369120.cpp): constant stores into the table object at obj.
template <class Memory>
void reset_slot_table_defaults(Memory& m, const GuestAddress object) {
    for (std::uint32_t i = 0U; i < 6U; ++i) {
        const GuestAddress record = object + 32U * i;
        m.write_u32_le(record + 24U, 0U);
        m.write_u32_le(record + 20U, kMinusOne);
        for (std::uint32_t j = 0U; j < 6U; ++j) {
            m.write_u32_le(record + 28U + 4U * j, kMinusOne);
        }
    }
    m.write_u32_le(object + 212U, 0U);
    m.write_u32_le(object + 216U, kMinusOne);
    m.write_u32_le(object + 220U, kMinusOne);
    m.write_u32_le(object + 224U, 7U);
    m.write_u32_le(object + 228U, 0U);
    m.write_u32_le(object + 232U, 0U);
    m.write_u8(object + 236U, kZeroByte);
    m.write_u32_le(object + 240U, 0U);
    m.write_u32_le(object + 244U, kMinusOne);
    m.write_u32_le(object + 0U, 0U);
    m.write_u8(object + 4U, kZeroByte);
    m.write_u8(object + 5U, kZeroByte);
    m.write_u8(object + 6U, kZeroByte);
    m.write_u8(object + 7U, kZeroByte);
    m.write_u8(object + 9U, kZeroByte);
    m.write_u32_le(object + 12U, 0U);
    m.write_u32_le(object + 272U, kMinusOne);
    m.write_u32_le(object + 276U, 0U);
    m.write_u32_le(object + 280U, kMinusOne);
    m.write_u32_le(object + 284U, 0U);
    m.write_u32_le(object + 288U, kMinusOne);
    m.write_u32_le(object + 292U, 0U);
    m.write_u32_le(object + 296U, 0U);
    for (std::uint32_t k = 0U; k < 6U; ++k) {
        m.write_u32_le(object + 248U + 4U * k, kMinusOne);
    }
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the retired
// instruction count, or more than kCeiling when the input must be left to the original (P+632 non-zero, or a
// loop count too large); in that case nothing has been written.
template <class Memory>
std::uint32_t reset_slot(Memory& m, const GuestAddress object) {
    if (m.read_u32_le(object + kFirstWordOffset) != 0U) {
        return kCeiling + 1U;
    }
    if (m.read_u32_le(object + kStateOffset) == kIdleState) {
        return kEarlyReturnCost;
    }
    const std::int32_t count = static_cast<std::int32_t>(m.read_u32_le(kLoopCountWord));
    if (count > static_cast<std::int32_t>(kCeiling / kLoopIterationCost)) {
        return kCeiling + 1U;
    }
    std::uint32_t retired = kBeforeLoopCost;
    for (std::uint32_t i = 0U; static_cast<std::int32_t>(i) < count; ++i) {
        m.write_u32_le(object + 1240U + 4U * i, kMinusOne);
        m.write_u8(object + 1264U + i, kZeroByte);
        m.write_u8(object + 1270U + i, kZeroByte);
        retired += kLoopIterationCost;
    }
    retired += kLoopExitCost;
    m.write_u8(object + 1286U, kZeroByte);
    m.write_u8(object + 726U, kZeroByte);
    m.write_u8(object + 724U, kZeroByte);
    m.write_u8(object + 1471U, kZeroByte);
    m.write_u32_le(object + 1472U, kMinusOne);
    m.write_u32_le(object + 1224U, kMinusOne);
    m.write_u32_le(object + 1212U, kMinusOne);
    m.write_u8(object + 1508U, kZeroByte);
    m.write_u8(object + 1509U, kZeroByte);
    m.write_u8(object + 1510U, kZeroByte);
    m.write_u8(object + 1284U, kZeroByte);
    m.write_u8(object + 1285U, kZeroByte);
    m.write_u32_le(object + 1288U, 0U);
    m.write_u32_le(object + 832U, m.read_u32_le(object + 12U));   // lwc1 f0, 12; swc1 f0, 832 (delay slot of jal)
    reset_slot_table_defaults(m, object + kTableObjectOffset);
    m.write_u32_le(object + 1516U, 0U);
    m.write_u32_le(object + 1520U, kMinusOne);
    return retired + kStoresAndCallCost + kCalleeCost + kEpilogueCost;
}

template <class Context>
std::uint32_t reset_slot_unless_idle_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = reset_slot(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run reset_slot_unless_idle(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t retired = reset_slot(m, object);
    auto& cpu = call.cpu();
    if (retired == kEarlyReturnCost) {
        cpu.write_gpr_u64(4U, kIdleState);
        cpu.write_gpr_u64(3U, kIdleState);
    } else {
        // Temporaries as the original leaves them: lui sign-extends, the callee's loop counters end at 6.
        const std::uint64_t minus_one = 0xffffffff00000000ULL | kMinusOne;
        cpu.write_gpr_u64(4U, object + kTableObjectOffset);
        cpu.write_gpr_u64(3U, minus_one);
        cpu.write_gpr_u64(2U, minus_one);
        cpu.write_gpr_u64(5U, minus_one);
        cpu.write_gpr_u64(6U, 6U);
        cpu.write_gpr_u64(7U, 6U);
        cpu.write_gpr_u64(8U, 6U);
        cpu.write_fpr_bits(0U, kMinusOne);
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00157180{0x00157180U, 212U, 0x38f842ffb031884cULL, "ResetSlotUnlessIdle",
                                                  kCeiling, n3::live<&reset_slot_unless_idle<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &reset_slot_unless_idle_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00157180, reset_slot_unless_idle, reset_slot_unless_idle_cost)
