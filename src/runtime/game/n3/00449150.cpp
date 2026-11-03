// 0x00449150 SelectSlotAction (a0 = slot object, a1 = slot id, low byte sign-extended): maps the object's state and
// the slot-pair records of the global pool (0x004f50c0) to an action code 0..13 (13 = nothing to do).
//
// Object fields: +10 u8 gate, +12 u16 counter (the leaves below also read the u16 limit at +14), +20 + 8 * k u8
// per-step flags, +144 s8 mode (0..6).
//
// Order of the decision:
//   gate byte zero, or mode zero          -> 13
//   0022cee0(pool, id) set  and TakeCountdownStep(O + 12) taken   -> "mode-change" tail
//   0022ce60(pool, id) set  and StepCounterBelowLimit(O + 12) taken -> "mode-change" tail
//   0022cf60(pool, id) set                -> 12
//   0022cfe0(pool, id) low byte zero      -> 13
//   otherwise (mode, counter) -> code through the jump table at 0x004d3110 (modes 1..6).
// The mode-change tail is 13 unless the mode is 6, where the original calls 0x004482a0 (a 1680-byte routine that
// is not written as a native): that case declines and the original runs.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022cee0)
N3_CALLEE(0022ce60)
N3_CALLEE(0022cf60)
N3_CALLEE(0022cfe0)
N3_CALLEE(00319360)
N3_CALLEE(00319330)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
constexpr GuestAddress kSlotPool = 0x004f50c0U;

constexpr GuestAddress kGateOffset = 10U;
constexpr GuestAddress kCounterOffset = 12U;
constexpr GuestAddress kFlagsOffset = 20U;
constexpr GuestAddress kModeOffset = 144U;
constexpr std::uint32_t kFlagStride = 8U;       // flag byte of step k at +20 + 8 * k
constexpr std::int32_t kNothing = 13;
constexpr std::int32_t kMaxMode = 6;
constexpr std::uint32_t kModeLimit = 7U;        // sltiu on the sign-extended mode byte

// Retired instruction counts of the listing blocks, delay slots included. Callees add their own count.
constexpr std::uint32_t kEntryGate = 8U;        // 00449150-0044916c
constexpr std::uint32_t kGateClosed = 2U + 5U;  // 00449184 branch to the epilogue, and the epilogue
constexpr std::uint32_t kModeCheck = 3U;        // 00449170-00449178
constexpr std::uint32_t kModeZero = 2U + 5U;
constexpr std::uint32_t kPoolCall = 7U;         // set up, jal + delay slot, bne + nop (also for 0022ce60/0022cf60)
constexpr std::uint32_t kPoolCallMasked = 8U;   // the same plus andi for 0022cfe0
constexpr std::uint32_t kLeafCall = 4U;         // jal + delay slot, beq + nop
constexpr std::uint32_t kTailAfterFirst = 1U;   // 004491f8 reload of the mode
constexpr std::uint32_t kTailAfterSecond = 2U;  // 00449220 branch and reload of the mode
constexpr std::uint32_t kTailMode = 3U;         // 004491fc-00449204
constexpr std::uint32_t kReturnNothing = 2U + 5U;   // 00449208 branch with addiu, and the epilogue
constexpr std::uint32_t kReturnTwelve = 2U + 5U;    // 004491e0
constexpr std::uint32_t kSetNothing = 1U + 5U;      // 004493cc and the epilogue
constexpr std::uint32_t kModeRange = 4U;        // 0044925c-00449268
constexpr std::uint32_t kDispatch = 7U;         // 0044926c-00449284
constexpr std::uint32_t kEpilogue = 5U;
constexpr std::uint32_t kCeiling = 20000U;

struct Pick {
    std::int32_t code;
    std::uint32_t retired;   // the mode block, up to and including the code load; epilogue not included
};

// The per-mode tests (mode 0 and the unused table slots go to the shared "nothing" ending); retired counts are the listing's blocks for each outcome.
Pick pick_for_mode(std::int32_t mode, std::uint32_t half, bool flag_set) {
    switch (mode) {
    case 1:
        if (half == 1U) return {1, 3U};
        if (half == 0U) return {0, 7U};
        return {kNothing, 7U};
    case 2:
        if (half == 0U) return {2, 4U};
        return {kNothing, 5U};
    case 3:
        if (half == 1U) return {3, 5U};
        if (half == 0U) return {2, 7U};
        return {kNothing, 8U};
    case 4:
        if (half == 1U) return {5, 5U};
        if (half == 0U) return {4, 7U};
        return {kNothing, 8U};
    case 5:
        if (half == 3U) return {8, 5U};
        if (half == 2U) return {7, 8U};
        if (half == 1U) return {6, 11U};
        if (half == 0U) return {4, 13U};
        return {kNothing, 14U};
    case 6:
        if (flag_set) return {kNothing, 7U};
        if (half == 2U) return {11, 10U};
        if (half == 1U) return {10, 13U};
        if (half == 0U) return {9, 15U};
        return {kNothing, 16U};
    default:
        return {kNothing, 1U};
    }
}

template <class Context>
n3::Run select_slot_action(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t slot_id = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(call.arg(1))));

    if (m.read_u8(object + kGateOffset) == 0U) {
        call.return_value(static_cast<std::uint32_t>(kNothing));
        return call.finish(kEntryGate + kGateClosed);
    }
    if (static_cast<std::int8_t>(m.read_u8(object + kModeOffset)) == 0) {
        call.return_value(static_cast<std::uint32_t>(kNothing));
        return call.finish(kEntryGate + kModeCheck + kModeZero);
    }
    std::uint32_t retired = kEntryGate + kModeCheck;

    // Asks one of the slot-pair natives about this slot; the result is its v0.
    auto ask_pool = [&](auto native, std::uint32_t own, bool& declined) {
        call.set_arg(0, kSlotPool);
        call.set_arg(1, slot_id);
        const n3::Run run = call.invoke(native, kFrameBytes);
        declined = run.declined;
        retired += own + run.retired;
        return call.result64();
    };
    auto ask_leaf = [&](auto native, bool& declined) {
        call.set_arg(0, object + kCounterOffset);
        const n3::Run run = call.invoke(native, kFrameBytes);
        declined = run.declined;
        retired += kLeafCall + run.retired;
        return call.result64() != 0U;
    };

    bool declined = false;
    bool mode_change = false;
    std::uint32_t tail_extra = 0U;

    const std::uint64_t first = ask_pool([](auto& x) { return n3::fn_0022cee0(x); }, kPoolCall, declined);
    if (declined) return call.decline();
    if (first != 0U) {
        const bool taken = ask_leaf([](auto& x) { return n3::fn_00319360(x); }, declined);
        if (declined) return call.decline();
        if (taken) {
            mode_change = true;
            tail_extra = kTailAfterFirst;
        }
    }
    if (!mode_change) {
        const std::uint64_t second = ask_pool([](auto& x) { return n3::fn_0022ce60(x); }, kPoolCall, declined);
        if (declined) return call.decline();
        if (second != 0U) {
            const bool taken = ask_leaf([](auto& x) { return n3::fn_00319330(x); }, declined);
            if (declined) return call.decline();
            if (taken) {
                mode_change = true;
                tail_extra = kTailAfterSecond;
            }
        }
    }
    if (mode_change) {
        retired += tail_extra + kTailMode;
        if (static_cast<std::int8_t>(m.read_u8(object + kModeOffset)) == kMaxMode) {
            return call.decline();   // 0x004482a0 is not a native
        }
        call.return_value(static_cast<std::uint32_t>(kNothing));
        return call.finish(retired + kReturnNothing);
    }

    const std::uint64_t pending = ask_pool([](auto& x) { return n3::fn_0022cf60(x); }, kPoolCall, declined);
    if (declined) return call.decline();
    if (pending != 0U) {
        call.return_value(12U);
        return call.finish(retired + kReturnTwelve);
    }

    const std::uint64_t ready = ask_pool([](auto& x) { return n3::fn_0022cfe0(x); }, kPoolCallMasked, declined);
    if (declined) return call.decline();
    if ((ready & 0xffU) == 0U) {
        call.return_value(static_cast<std::uint32_t>(kNothing));
        return call.finish(retired + kSetNothing);
    }

    const std::int32_t mode = static_cast<std::int8_t>(m.read_u8(object + kModeOffset));
    const std::uint32_t half = m.read_u16_le(object + kCounterOffset);
    retired += kModeRange;
    if (static_cast<std::uint32_t>(mode) >= kModeLimit) {
        call.return_value(static_cast<std::uint32_t>(kNothing));
        return call.finish(retired + kSetNothing);
    }
    retired += kDispatch;
    bool flag_set = false;
    if (mode == kMaxMode) {
        flag_set = m.read_u8(object + half * kFlagStride + kFlagsOffset) != 0U;
    }
    const Pick pick = pick_for_mode(mode, half, flag_set);
    call.return_value(static_cast<std::uint32_t>(pick.code));
    return call.finish(retired + pick.retired + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00449150, 660U, 0xa267a85f4dfccd67ULL, "SelectSlotAction", kCeiling, select_slot_action,
         EngineReturn::Integer)
