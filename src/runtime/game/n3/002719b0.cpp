// 0x002719b0 (a0 = a loaded data block): RelocateOffsetTable. The block header holds a signed count at +8 and
// a word at +12 that is the offset of a table of words, relative to the block. The offset becomes an absolute
// address (the word at +12 is rewritten as offset + block), then every table word i below the count becomes
// word + block when it is nonzero. The count and the table address are read again on every pass, so a table
// that overlaps the header behaves as the original does. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kCountOffset = 8U;
constexpr std::uint32_t kTableOffset = 12U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 5U;      // lw, daddu, addu, beq, and the sw in its delay slot
constexpr std::uint32_t kTestCost = 4U;          // lw, slt, bne and the nop in its delay slot, once per pass plus one
constexpr std::uint32_t kZeroWordCost = 7U;      // lw, sll, addu, lw, beq and its nop, then the addiu
constexpr std::uint32_t kRelocateWordCost = 9U;  // as kZeroWordCost, plus the addu and sw of a nonzero word
constexpr std::uint32_t kReturnCost = 2U;        // jr and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate_table(Memory& m, const GuestAddress block) {
    const GuestAddress count_address = block + kCountOffset;
    const GuestAddress table_address = block + kTableOffset;
    std::uint32_t retired = kPrologueCost;
    const std::uint32_t absolute_table = m.read_u32_le(table_address) + block;
    m.write_u32_le(table_address, absolute_table);
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(count_address))) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress slot = m.read_u32_le(table_address) + 4U * index;
        const std::uint32_t word = m.read_u32_le(slot);
        if (word == 0U) {
            retired += kZeroWordCost;
        } else {
            m.write_u32_le(slot, word + block);
            retired += kRelocateWordCost;
        }
        ++index;
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t relocate_offset_table_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate_table(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run relocate_offset_table(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_table(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_002719b0{0x002719b0U, 80U, 0x75fc9cc9fb8df4c9ULL, "RelocateOffsetTable",
                                                  kCeiling, n3::live<&relocate_offset_table<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_offset_table_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002719b0, relocate_offset_table, relocate_offset_table_cost)
