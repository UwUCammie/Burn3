// 0x00150280 (a0 = object A, a1 = new value; only its low byte matters): SetModeByteResetState. Returns nothing (v1 is
// scratch). The byte at A+10184 is a mode. A new byte equal to it returns at once. Otherwise the new byte is stored there
// and, when it is zero, the float 1.0 goes to P+5428 (P = the word at A+9264) and the word at P+5412 is cleared when it
// equals 4. A nonzero new byte calls 0x00149140, which the native does not run: call_cost declines those calls and the
// original runs them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kModeOffset = 10184U;     // byte: the mode (the low byte of a1 is stored here)
constexpr GuestAddress kPointerOffset = 9264U;   // word: pointer P
constexpr GuestAddress kOneOffset = 5428U;       // word at P+5428: set to float 1.0 when the new mode is zero
constexpr GuestAddress kCountOffset = 5412U;     // word at P+5412: cleared when it equals kCountValue
constexpr std::uint32_t kOneBits = 0x3f800000U;  // float 1.0, raw bits
constexpr std::uint32_t kCountValue = 4U;

// Retired instruction counts of the listing's blocks, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 6U;         // 0x00150280-0x00150294: frame, mode test and its delay slot
constexpr std::uint32_t kStoreCost = 4U;         // 0x00150298-0x001502a4: mode store, reload, branch and its delay slot
constexpr std::uint32_t kZeroCost = 9U;          // 0x001502a8-0x001502c8: pointer, 1.0 store, pointer reload, count test
constexpr std::uint32_t kClearCost = 1U;         // 0x001502cc: the clearing store
constexpr std::uint32_t kJumpCost = 2U;          // 0x001502d0-0x001502d4: jump to the return and its delay slot
constexpr std::uint32_t kRestoreRaCost = 1U;     // 0x001502f0: restores ra on the early exits
constexpr std::uint32_t kReturnCost = 2U;        // 0x001502f4-0x001502f8: jr and its delay slot

// The retired count of a call, and whether its path is declined (a nonzero new mode calls 0x00149140).
struct SetModeResult {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). A nonzero new mode takes the
// declined path, which stops after the mode store: the original runs the call to 0x00149140 itself.
template <class Memory>
SetModeResult set_mode_body(Memory& m, const GuestAddress object, const std::uint32_t value) {
    const auto mode = static_cast<std::uint8_t>(value & 0xffU);
    if (mode == m.read_u8(object + kModeOffset)) {
        return {kEntryCost + kRestoreRaCost + kReturnCost, false};
    }
    m.write_u8(object + kModeOffset, mode);
    if (mode != 0U) {
        return {kEntryCost + kStoreCost, true};
    }
    const GuestAddress pointer = m.read_u32_le(object + kPointerOffset);
    m.write_u32_le(pointer + kOneOffset, kOneBits);
    // The original reads P again after that store, so the store may have changed the word at A+9264.
    const GuestAddress reloaded = m.read_u32_le(object + kPointerOffset);
    const std::uint32_t head = kEntryCost + kStoreCost + kZeroCost;
    if (m.read_u32_le(reloaded + kCountOffset) != kCountValue) {
        return {head + kRestoreRaCost + kReturnCost, false};
    }
    m.write_u32_le(reloaded + kCountOffset, 0U);
    return {head + kClearCost + kJumpCost + kReturnCost, false};
}

// call_cost: the same body on a DryMemory, with the call's own a0 and a1. A call on the declined path returns 0, so the
// original runs it; every other call reports its exact count.
template <class Context>
std::uint32_t set_mode_byte_reset_state_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const SetModeResult result =
        set_mode_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return result.declined ? 0U : result.retired;
}

template <class Context>
n3::Run set_mode_byte_reset_state(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(set_mode_body(call.memory(), call.arg(0), call.arg(1)).retired);
}

} // namespace

extern constinit const EngineFunction n3_00150280{0x00150280U, 124U, 0x71a215ce3c22cb9eULL, "SetModeByteResetState",
                                                  24U, n3::live<&set_mode_byte_reset_state<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &set_mode_byte_reset_state_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00150280, set_mode_byte_reset_state, set_mode_byte_reset_state_cost)
