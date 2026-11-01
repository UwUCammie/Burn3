// 0x001578e0 (a0 = owner object): ExpireTimedSlots. For each slot i below the signed count word at 0x0064d37c, the
// float timer at +1240+4*i is checked; a negative timer is an empty slot. Otherwise the slot has expired when the
// owner's time (the float at +12) is not below timer + F, F being the float at $gp-29092. A slot whose flag byte at
// +1264+i is set, or which has expired while the object the word at +200 points to has byte +6378 clear, takes the
// cold block at 0x001579c0 (calls into code the recorded game never ran): call_cost declines any call that reaches
// it. Any other expired slot is reset (timer -1.0, flag bytes +1264+i and +1270+i cleared); the rest are left alone.
// After the loop, the word at +1236 is cleared when it is set and the object it points to has byte +6378 clear.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kCountAddress = 0x0064d37cU;  // lui 0x65, -11396: the signed slot count
constexpr std::int32_t kStepOffset = -29092;         // $gp-relative: the float F
constexpr GuestAddress kTimeOffset = 12U;            // the owner's time (float)
constexpr GuestAddress kHolderOffset = 200U;         // pointer to the holder object
constexpr GuestAddress kHolderFlagOffset = 6378U;    // byte of the object a pointer leads to
constexpr GuestAddress kTimerOffset = 1240U;         // float timer of slot i at +1240+4*i
constexpr GuestAddress kFlagOffset = 1264U;          // flag byte of slot i at +1264+i
constexpr GuestAddress kSecondFlagOffset = 1270U;    // second flag byte of slot i at +1270+i
constexpr GuestAddress kLastOffset = 1236U;          // pointer word tested and cleared after the loop
constexpr std::uint32_t kResetTimer = 0xbf800000U;   // -1.0f

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 10U;        // 0x001578e0-0x00157904: prologue, jump to the loop test
constexpr std::uint32_t kTestCost = 5U;          // 0x00157988-0x00157998: slot < count (signed) and its branch
constexpr std::uint32_t kTimerCost = 8U;         // 0x00157908-0x00157924: timer load, timer < 0 test
constexpr std::uint32_t kExpiryCost = 6U;        // 0x00157928-0x0015793c: time < timer + F test
constexpr std::uint32_t kRunningFlagCost = 4U;   // 0x00157940-0x0015794c: flag test of a running slot
constexpr std::uint32_t kHolderCost = 4U;        // 0x00157950-0x0015795c: holder byte test
constexpr std::uint32_t kResetFlagCost = 4U;     // 0x00157960-0x0015796c: flag test before the reset
constexpr std::uint32_t kResetCost = 5U;         // 0x00157970-0x00157980: reset and its stores
constexpr std::uint32_t kNextCost = 1U;          // 0x00157984: slot += 1
constexpr std::uint32_t kLastTestCost = 3U;      // 0x0015799c-0x001579a4: pointer load and its test
constexpr std::uint32_t kLastHolderCost = 3U;    // 0x001579a8-0x001579b0: byte test of the pointed-to object
constexpr std::uint32_t kLastClearCost = 3U;     // 0x001579b4-0x001579bc: clear the pointer, jump to the return
constexpr std::uint32_t kLoadRaCost = 1U;        // 0x00157ad4: ld $ra
constexpr std::uint32_t kReturnCost = 7U;        // 0x00157ad8-0x00157af0: restores, jr and its delay slot

// The retired count of a call, and whether its path reached the cold block (then the count is void).
struct ExpireResult {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). It stops as soon as a path
// would reach the cold block, and as soon as the count passes kCeiling.
template <class Memory>
ExpireResult expire_slots(Memory& m, const GuestAddress owner, const GuestAddress gp) {
    const GuestAddress step_address = gp + static_cast<GuestAddress>(kStepOffset);
    std::uint32_t retired = kEntryCost;
    std::uint32_t slot = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(slot) >= static_cast<std::int32_t>(m.read_u32_le(kCountAddress))) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        const GuestAddress timer_address = owner + kTimerOffset + 4U * slot;
        const GuestAddress flag_address = owner + kFlagOffset + slot;
        const GuestAddress second_flag_address = owner + kSecondFlagOffset + slot;
        retired += kTimerCost;
        const std::uint32_t timer = m.read_u32_le(timer_address);
        if (fpu::lt(timer, 0U)) {
            retired += kNextCost;
            ++slot;
            continue;
        }
        retired += kExpiryCost;
        const std::uint32_t limit = fpu::add(timer, m.read_u32_le(step_address));
        if (fpu::lt(m.read_u32_le(owner + kTimeOffset), limit)) {
            // Still running: left alone unless its flag is set.
            retired += kRunningFlagCost;
            if (m.read_u8(flag_address) == 0U) {
                retired += kNextCost;
                ++slot;
                continue;
            }
        }
        retired += kHolderCost;
        const GuestAddress holder = m.read_u32_le(owner + kHolderOffset);
        if (m.read_u8(holder + kHolderFlagOffset) == 0U) {
            return {retired, true};
        }
        retired += kResetFlagCost;
        if (m.read_u8(flag_address) != 0U) {
            return {retired, true};
        }
        retired += kResetCost;
        m.write_u32_le(timer_address, kResetTimer);
        m.write_u8(flag_address, 0U);
        m.write_u8(second_flag_address, 0U);
        retired += kNextCost;
        ++slot;
    }
    retired += kLastTestCost;
    const GuestAddress last = m.read_u32_le(owner + kLastOffset);
    if (last == 0U) {
        return {retired + kLoadRaCost + kReturnCost, false};
    }
    retired += kLastHolderCost;
    if (m.read_u8(last + kHolderFlagOffset) != 0U) {
        return {retired + kLoadRaCost + kReturnCost, false};
    }
    m.write_u32_le(owner + kLastOffset, 0U);
    return {retired + kLastClearCost + kReturnCost, false};
}

// call_cost: the same body on a DryMemory, with the call's own $a0 and $gp. The count is the original's exact count;
// a call that reaches the cold block or retires more than kCeiling returns 0 and the original runs it.
template <class Context>
std::uint32_t expire_timed_slots_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const ExpireResult result = expire_slots(dry, context.cpu.read_gpr_u32(4U),
                                             context.cpu.read_gpr_u32(n3::kGlobalPointer));
    return (result.declined || result.retired > kCeiling) ? 0U : result.retired;
}

template <class Context>
n3::Run expire_timed_slots(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    return call.finish(expire_slots(call.memory(), call.arg(0), gp).retired);
}

} // namespace

extern constinit const EngineFunction n3_001578e0{0x001578e0U, 532U, 0x1b31b244fd345bc4ULL, "ExpireTimedSlots",
                                                  kCeiling, n3::live<&expire_timed_slots<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &expire_timed_slots_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001578e0, expire_timed_slots, expire_timed_slots_cost)
