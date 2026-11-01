// 0x002e7340 (a0 = object): CanUseBoostGate. Returns 0 or 1 in v0. The 64-bit flags at +2944 decide first: bit 0x10
// or bit 0x20 set gives 0, and with both clear, bit 0x400 clear gives 1. With bit 0x400 set, the words at +2544 and
// +2548 are tested against 4 and 5. A word equal to 4 or 5 calls 0x00373940 on the object at +2240; the callee's float,
// compared with the float at $gp-30832, can then turn the result into 0. The recorded game never ran that call, so these
// exits are cold: call_cost declines every input whose path reaches the call, and the original runs it. The native never
// runs it. The function writes nothing but its own stack frame.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagsOffset = 2944U;       // ld of the 64-bit flags (8-byte aligned)
constexpr GuestAddress kFirstWordOffset = 2544U;   // lw, tested against 4 and 5
constexpr GuestAddress kSecondWordOffset = 2548U;  // lw, tested only when the first word is neither 4 nor 5
constexpr std::uint64_t kFlag10 = 0x10U;
constexpr std::uint64_t kFlag20 = 0x20U;
constexpr std::uint64_t kFlag400 = 0x400U;

// Retired instruction counts of the paths, delay slots included (brief.py's table).
constexpr std::uint32_t kFlag10Cost = 22U;     // bit 0x10 set: returns 0
constexpr std::uint32_t kFlag20Cost = 27U;     // bit 0x10 clear, bit 0x20 set: returns 0
constexpr std::uint32_t kOpenCost = 31U;       // bits 0x10 and 0x20 clear, bit 0x400 clear: returns 1
constexpr std::uint32_t kSettledCost = 41U;    // bits 0x10 and 0x20 clear, bit 0x400 set, no word 4 or 5: returns 1
// The cold exits, counted up to the call (its delay slot included). The callee's own instructions are not counted.
constexpr std::uint32_t kFirstCallCost = 30U;  // jal 0x002e73f0, reached when the word at +2544 is 4 or 5
constexpr std::uint32_t kSecondCallCost = 36U; // jal 0x002e73c8, reached when only the word at +2548 is 4 or 5
// The longest path, through both calls and their float tests: the declared cost bounds every path.
constexpr std::uint32_t kDeclaredCost = 58U;

// One path through the gate: the count retired (up to the call on a cold exit), v0 on the paths that return, and
// whether the path runs one of the calls.
struct GateResult {
    std::uint32_t retired{};
    std::uint32_t value{};
    bool reaches_call{};
};

// The listing's sltiu after addiu -4: (word - 4) < 2 unsigned, that is, the word is 4 or 5.
constexpr bool takes_call(const std::uint32_t word) noexcept { return word - 4U < 2U; }

// The gate on memory it only reads: the implementation's EeMemory, or call_cost's CheckedReader.
template <class Memory>
GateResult evaluate_gate(const Memory& m, const GuestAddress object) {
    const std::uint64_t flags = m.read_u64_le(object + kFlagsOffset);
    if ((flags & kFlag10) != 0U) {
        return {kFlag10Cost, 0U, false};
    }
    if ((flags & kFlag20) != 0U) {
        return {kFlag20Cost, 0U, false};
    }
    if ((flags & kFlag400) == 0U) {
        return {kOpenCost, 1U, false};
    }
    if (takes_call(m.read_u32_le(object + kFirstWordOffset))) {
        return {kFirstCallCost, 0U, true};
    }
    if (takes_call(m.read_u32_le(object + kSecondWordOffset))) {
        return {kSecondCallCost, 0U, true};
    }
    return {kSettledCost, 1U, false};
}

// Reads the original would fault on (ld needs 8-byte alignment, lw 4-byte, and the bytes must lie in guest memory) are
// not performed. They are recorded instead, so call_cost declines the call and the original runs and faults itself.
// contains() also admits MMIO; a game object is in main RAM, where a read done twice has no effect.
template <class Base>
class CheckedReader {
public:
    explicit CheckedReader(const Base& memory) noexcept : memory_(memory) {}
    [[nodiscard]] std::uint64_t read_u64_le(const GuestAddress address) const {
        if ((address & 7U) != 0U || !memory_.contains(address, 8U)) {
            faulted_ = true;
            return 0U;
        }
        return memory_.read_u64_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return memory_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    mutable bool faulted_ = false;
};

// call_cost: the exact count of a path that returns, and 0 (the original runs the call) on a cold exit or on a read the
// original would fault on.
template <class Context>
std::uint32_t can_use_boost_gate_cost(const Context& context) {
    const CheckedReader reader{context.memory};
    const GateResult result = evaluate_gate(reader, context.cpu.read_gpr_u32(4U));
    if (reader.faulted() || result.reaches_call) {
        return 0U;
    }
    return result.retired;
}

template <class Context>
n3::Run can_use_boost_gate(Context& context) {
    auto call = n3::guest_call(context);
    const GateResult result = evaluate_gate(call.memory(), call.arg(0));
    // call_cost declines the cold exits, so the calls are not run from here. Should one be reached anyway, v0 is left
    // alone (the callee's float is not known here) and only the count up to the call is reported.
    if (!result.reaches_call) {
        call.return_value(result.value);
    }
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_002e7340{0x002e7340U, 232U, 0xe5b3c886c808f9e2ULL, "CanUseBoostGate",
                                                  kDeclaredCost, n3::live<&can_use_boost_gate<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &can_use_boost_gate_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002e7340, can_use_boost_gate, can_use_boost_gate_cost)
