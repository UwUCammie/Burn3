// 0x00220db0 (a0 = block base B): RelocateOffsetTables. A loaded resource stores self-relative offsets, which
// this turns into addresses in place. Its 18 pointer slots are the word at B+0, the six at B+4..B+24, the three
// at B+28..B+36 and the eight at B+40..B+68, processed in that order. A slot whose word w is not zero becomes
// S = w + B (stored back, then read again), and the word at S+4 gets S added even when it was zero. The signed
// byte at S+0 is the entry count N. Entry i is the 16-byte record at (the word at S+4) + 16*i: its first word
// gets the record's own address added, and when its second word is not zero that word gets the address added
// too. Every store goes to guest memory in order and every value is read back from memory, because slots and
// records can overlap. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kFirstTestCost = 3U;   // lw, beq, nop: the word at B+0 (no outer loop)
constexpr std::uint32_t kOuterTestCost = 6U;   // sll, addu, addiu, lw, beq, nop: a slot of an outer loop
constexpr std::uint32_t kRelocateCost = 8U;    // addu, daddu, sw, lw, lw, addu, beq, sw: S stored, S+4 updated
constexpr std::uint32_t kCheckCost = 4U;       // lb, slt, bne, nop: the entry count test
constexpr std::uint32_t kEntryCost = 9U;       // lw, sll, addu, lw, addu, sw, lw, beq, nop
constexpr std::uint32_t kEntryLinkCost = 2U;   // addu, sw: the entry's second word was not zero
constexpr std::uint32_t kEntryStepCost = 1U;   // addiu: the entry index
constexpr std::uint32_t kTailCost = 4U;        // addiu, slti, bne, nop: the tail of an outer loop
constexpr std::uint32_t kLoopInitCost = 1U;    // daddu: a loop index cleared before an outer loop
constexpr std::uint32_t kReturnCost = 2U;      // jr $ra, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The entries of the block S (the value its slot now holds). The count test runs before each entry and once
// more after the last one, and N is read again each time.
template <class Memory>
std::uint32_t relocate_entries(Memory& m, const GuestAddress pointer, std::uint32_t retired) {
    std::uint32_t index = 0U;
    for (;;) {
        retired += kCheckCost;
        const auto entry_count = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(pointer)));
        if (static_cast<std::int32_t>(index) >= entry_count || retired > kCeiling) {
            return retired;
        }
        const GuestAddress entry = m.read_u32_le(pointer + 4U) + (index << 4U);
        const std::uint32_t head = m.read_u32_le(entry) + entry;
        m.write_u32_le(entry, head);
        const std::uint32_t second = m.read_u32_le(entry + 4U);
        retired += kEntryCost;
        if (second != 0U) {
            m.write_u32_le(entry + 4U, second + entry);
            retired += kEntryLinkCost;
        }
        ++index;
        retired += kEntryStepCost;
    }
}

// One pointer slot at `slot` of the block at `base`. `outer` marks the slots of the outer loops, which also pay
// for their loop tail.
template <class Memory>
std::uint32_t process_slot(Memory& m, const GuestAddress base, const GuestAddress slot, std::uint32_t retired,
                           const bool outer) {
    retired += outer ? kOuterTestCost : kFirstTestCost;
    const std::uint32_t word = m.read_u32_le(slot);
    if (word != 0U) {
        m.write_u32_le(slot, word + base);
        const GuestAddress pointer = m.read_u32_le(slot);
        const std::uint32_t next = m.read_u32_le(pointer + 4U) + pointer;
        m.write_u32_le(pointer + 4U, next);
        retired += kRelocateCost;
        retired = relocate_entries(m, pointer, retired);
    }
    return outer ? retired + kTailCost : retired;
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate_block(Memory& m, const GuestAddress base) {
    std::uint32_t retired = process_slot(m, base, base, 0U, false);
    if (retired > kCeiling) {
        return retired;
    }
    retired += kLoopInitCost;
    for (std::uint32_t i = 0U; i < 6U; ++i) {
        retired = process_slot(m, base, base + 4U + 4U * i, retired, true);
        if (retired > kCeiling) {
            return retired;
        }
    }
    retired += kLoopInitCost;
    for (std::uint32_t i = 0U; i < 3U; ++i) {
        retired = process_slot(m, base, base + 28U + 4U * i, retired, true);
        if (retired > kCeiling) {
            return retired;
        }
    }
    retired += kLoopInitCost;
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        retired = process_slot(m, base, base + 40U + 4U * i, retired, true);
        if (retired > kCeiling) {
            return retired;
        }
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t relocate_offset_tables_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate_block(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run relocate_offset_tables(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_block(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00220db0{0x00220db0U, 536U, 0x22c3d9618287de57ULL, "RelocateOffsetTables",
                                                  kCeiling, n3::live<&relocate_offset_tables<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_offset_tables_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00220db0, relocate_offset_tables, relocate_offset_tables_cost)
