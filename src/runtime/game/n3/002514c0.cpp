// 0x002514c0 (a0 = blob base B): RelocateOffsetTables, the pointer fix-up of a freshly loaded data blob. The header
// words at B+0, B+4, B+8, B+36 and B+108 hold offsets from B; each one that is not zero gets B added. Then the seven
// words at B+112+4i (i = 0..6) are fixed the same way. The word at B+36, relocated just above, is the table T: its
// word at T+0 is the entry count n, read once. For i = 0 .. n-1 (unsigned compare) the pointer at T+16+64i+52 always
// gets B added, zero or not. Leaf: the callers ignore the result, so nothing is returned.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kZeroWordCost = 3U;       // lw, beq and the delay slot, when the word is zero
constexpr std::uint32_t kRelocatedWordCost = 5U;  // the same, plus addu and sw, when it is not
constexpr std::uint32_t kSetupCost = 1U;          // daddu $a3, $zero, $zero
constexpr std::uint32_t kLoopStepCost = 7U;       // sll, addu, addiu before the word; addiu, sltiu, bne, nop after
constexpr std::uint32_t kFixedLoopCount = 7U;
constexpr std::uint32_t kTablePrologueCost = 5U;  // lw, daddu, lw, beq and its delay slot addiu
constexpr std::uint32_t kTableTestCost = 3U;      // sltu, bne and the delay slot nop, once per test
constexpr std::uint32_t kTableEntryCost = 5U;     // lw, addiu, addu, sw, addiu, once per entry
constexpr std::uint32_t kReturnCost = 2U;         // jr $ra and its delay slot

// The entry count is any 32-bit word, so the count is not bounded by the listing. A dry run that passes this many
// retired instructions stops there and call_cost declines, which leaves the call to the original.
constexpr std::uint32_t kCeiling = 20000U;

// lw, beq, and addu and sw when the word is not zero: relocates the word at address. Returns the retired count.
template <class Memory>
std::uint32_t relocate_word(Memory& m, const GuestAddress base, const GuestAddress address) {
    const std::uint32_t value = m.read_u32_le(address);
    if (value == 0U) {
        return kZeroWordCost;
    }
    m.write_u32_le(address, value + base);
    return kRelocatedWordCost;
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). Returns the retired
// instruction count, or a count above kCeiling as soon as the entry loop passes it.
template <class Memory>
std::uint32_t relocate_tables(Memory& m, const GuestAddress base) {
    std::uint32_t retired = 0U;
    retired += relocate_word(m, base, base + 0U);
    retired += relocate_word(m, base, base + 4U);
    retired += relocate_word(m, base, base + 8U);
    retired += relocate_word(m, base, base + 36U);
    retired += relocate_word(m, base, base + 108U);  // listed as 56(a2) with a2 = base + 52
    retired += kSetupCost;
    for (std::uint32_t i = 0U; i < kFixedLoopCount; ++i) {
        retired += kLoopStepCost;
        retired += relocate_word(m, base, base + 112U + 4U * i);
    }
    const GuestAddress table = m.read_u32_le(base + 36U);  // read again: the word was relocated above
    const std::uint32_t entries = m.read_u32_le(table);
    retired += kTablePrologueCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTableTestCost;
        if (index >= entries) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress pointer = table + 16U + 64U * index + 52U;
        const std::uint32_t target = m.read_u32_le(pointer);
        m.write_u32_le(pointer, target + base);
        retired += kTableEntryCost;
        ++index;
    }
    return retired + kReturnCost;
}

// The word accesses of the original on a read-only view of guest memory that keeps its own writes (n3::DryMemory).
// An access the original would fault on (misaligned, or outside guest memory) is recorded and not performed, so
// call_cost declines and the original runs and faults itself.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if (!word_in_memory(address)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (!word_in_memory(address)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool word_in_memory(const GuestAddress address) const noexcept {
        return (address & 3U) == 0U && memory_.contains(address, 4U);
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// The retired count of the whole call, decided before it runs. 0 declines: the original runs, which also covers
// a fault and a count above kCeiling.
template <class Context>
std::uint32_t relocate_offset_tables_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const std::uint32_t retired = relocate_tables(dry, context.cpu.read_gpr_u32(4U));
    if (dry.faulted() || retired > kCeiling) {
        return 0U;
    }
    return retired;
}

template <class Context>
n3::Run relocate_offset_tables(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_tables(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002514c0{0x002514c0U, 212U, 0xcef92825fc37e80cULL, "RelocateOffsetTables",
                                                  kCeiling, n3::live<&relocate_offset_tables<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_offset_tables_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002514c0, relocate_offset_tables, relocate_offset_tables_cost)
