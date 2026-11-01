// 0x00148c20 (a0 = list header, a1 = start index, a2 = step budget): ScanForFlaggedEntry. The header holds
// the entry table pointer (u32 at +8, 10-byte entries), the entry count (u16 at +12) and the wrap byte (u8 at
// +14). From the start index (low 16 bits of a1) the scan reads the entry there, without checking the index
// against the count, and returns 0 at the first entry whose byte +3 has its low three bits equal to 5.
// Otherwise it moves to the next index: past the last entry it returns 1 when the wrap byte is clear, and
// restarts at index 0 when it is set. Each pass of the loop tests the budget first (returning 1 at zero, the
// test reads all of a2), then spends one unit of it; from the second pass on the budget is 16 bits. Returns v0.
// Writes no memory.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kEntriesOffset = 8U;      // u32: the entry table
constexpr GuestAddress kCountOffset = 12U;       // u16: entries in the list
constexpr GuestAddress kWrapOffset = 14U;        // u8: restart at index 0 past the last entry
constexpr std::uint32_t kEntryBytes = 10U;
constexpr GuestAddress kFlagsOffset = 3U;        // byte 3 of an entry
constexpr std::uint32_t kFlagsMask = 7U;
constexpr std::uint32_t kFlaggedValue = 5U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 5U;         // 0x148c20..0x148c30: header reads and the jump to the budget test
constexpr std::uint32_t kBudgetTestCost = 4U;    // 0x148c78..0x148c84: the budget test on every pass
constexpr std::uint32_t kEntryCost = 9U;         // 0x148c34..0x148c54: the entry's flags test
constexpr std::uint32_t kFoundCost = 3U;         // 0x148c90..0x148c98: v0 = 0 and return
constexpr std::uint32_t kAdvanceCost = 5U;       // 0x148c58..0x148c68: next index and its count test
constexpr std::uint32_t kEndTestCost = 2U;       // 0x148c6c..0x148c70: the wrap byte test past the last entry
constexpr std::uint32_t kRestartCost = 1U;       // 0x148c74: index back to 0 when the list wraps
constexpr std::uint32_t kNotFoundCost = 4U;      // 0x148c88..0x148c8c, 0x148c94..0x148c98: v0 = 1 and return

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct ScanOutcome {
    std::uint32_t retired;  // retired instruction count (past kCeiling the scan stops early)
    std::uint32_t result;   // v0: 0 when a flagged entry is found, 1 otherwise
};

// The whole function on guest memory (the implementation) or on a read-only view (call_cost).
template <class Memory>
ScanOutcome scan_flagged_entries(const Memory& m, const GuestAddress header, const std::uint64_t budget,
                                 const std::uint32_t start) {
    std::uint32_t retired = kSetupCost;
    const GuestAddress entries = m.read_u32_le(header + kEntriesOffset);
    const std::uint32_t wrap = m.read_u8(header + kWrapOffset);
    const std::uint32_t count = m.read_u16_le(header + kCountOffset);
    std::uint64_t steps = budget;
    std::uint32_t index = start & 0xffffU;
    for (;;) {
        if (retired > kCeiling) {
            return {retired, 0U};
        }
        retired += kBudgetTestCost;
        const bool exhausted = steps == 0U;
        steps = (static_cast<std::uint32_t>(steps) - 1U) & 0xffffU;
        if (exhausted) {
            return {retired + kNotFoundCost, 1U};
        }
        retired += kEntryCost;
        const GuestAddress entry = entries + index * kEntryBytes;
        if ((m.read_u8(entry + kFlagsOffset) & kFlagsMask) == kFlaggedValue) {
            return {retired + kFoundCost, 0U};
        }
        retired += kAdvanceCost;
        index = (index + 1U) & 0xffffU;
        if (index < count) {
            continue;
        }
        retired += kEndTestCost;
        if (wrap == 0U) {
            return {retired + kNotFoundCost, 1U};
        }
        retired += kRestartCost;
        index = 0U;
    }
}

// A read-only view of guest memory for the call_cost dry run. The loads are checked the way the interpreter
// checks them (lhu needs a 2-byte aligned address, lw a 4-byte one, lbu any, and each must lie in guest memory):
// a load the original would fault on is not performed and is recorded, so call_cost declines the call and the
// original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint16_t read_u16_le(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u16_le(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t scan_for_flagged_entry_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const ScanOutcome outcome = scan_flagged_entries(dry, context.cpu.read_gpr_u32(4U),
                                                     context.cpu.read_gpr_u64(6U), context.cpu.read_gpr_u32(5U));
    if (outcome.retired > kCeiling || dry.faulted()) {
        return 0U;
    }
    return outcome.retired;
}

template <class Context>
n3::Run scan_for_flagged_entry(Context& context) {
    auto call = n3::guest_call(context);
    const ScanOutcome outcome = scan_flagged_entries(call.memory(), call.arg(0), call.arg64(2U), call.arg(1U));
    call.return_value(outcome.result);
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00148c20{0x00148c20U, 124U, 0xa2c5d7cc62409527ULL, "ScanForFlaggedEntry",
                                                  kCeiling, n3::live<&scan_for_flagged_entry<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &scan_for_flagged_entry_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00148c20, scan_for_flagged_entry, scan_for_flagged_entry_cost)
