// 0x00271920 (a0 = H, a block header): RebaseBlockOffsets. Four words of the header (H+20, H+24,
// H+28 and H+32) each get H added (u32 wrap). Then, for the signed count n at H+4, entry i
// (0 <= i < n) starts at E = (the word at H+20) + 16*i, and each of the words at E+0, E+4 and E+8
// gets E added (E+12 is not touched). The count and the word at H+20 are re-read on every pass, as the
// listing re-reads them, since a store can alias them. Returns nothing (v1 is scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRebasedOffsets[] = {20U, 24U, 28U, 32U};  // header words rebased first, in this order
constexpr GuestAddress kCountOffset = 4U;                          // H+4: signed entry count
constexpr GuestAddress kEntriesOffset = 20U;                       // H+20: pointer to the entry array
constexpr GuestAddress kEntryWordOffsets[] = {0U, 4U, 8U};         // words of an entry that get relocated
constexpr std::uint32_t kEntryBytes = 16U;                         // entry stride (sll $a1, $a2, 4)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call
// reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 14U;  // daddu, four rebases (the last sw is the beq delay slot), beq
constexpr std::uint32_t kTestCost = 4U;    // lw, slt, bne and its nop: n + 1 times for n >= 0, once otherwise
constexpr std::uint32_t kBodyCost = 13U;   // index step, E, and the three relocations (lw, addu, sw each)
constexpr std::uint32_t kReturnCost = 2U;  // jr $ra and its nop

// lw, addu, sw on one word: the word gets the base added (u32 wrap), as the listing does.
template <class Memory>
void rebase_word(Memory& m, const GuestAddress word, const GuestAddress base) {
    const std::uint32_t value = m.read_u32_le(word) + base;
    m.write_u32_le(word, value);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t rebase_header(Memory& m, const GuestAddress header) {
    std::uint32_t retired = kEntryCost;
    for (const GuestAddress offset : kRebasedOffsets) {
        rebase_word(m, header + offset, header);
    }
    std::uint32_t index = 0U;
    for (;;) {
        // The test re-reads the count on every pass (lw, slt, bne and its nop).
        retired += kTestCost;
        const auto count = static_cast<std::int32_t>(m.read_u32_le(header + kCountOffset));
        if (static_cast<std::int32_t>(index) >= count) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kBodyCost;
        // E is formed from the pointer as it is now and the index before its step (sll before addiu).
        const GuestAddress entry = m.read_u32_le(header + kEntriesOffset) + kEntryBytes * index;
        ++index;
        for (const GuestAddress offset : kEntryWordOffsets) {
            rebase_word(m, entry + offset, entry);
        }
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t rebase_block_offsets_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = rebase_header(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run rebase_block_offsets(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(rebase_header(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00271920{0x00271920U, 132U, 0x5a9a7b5d4834458dULL, "RebaseBlockOffsets",
                                                  kCeiling, n3::live<&rebase_block_offsets<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &rebase_block_offsets_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00271920, rebase_block_offsets, rebase_block_offsets_cost)
