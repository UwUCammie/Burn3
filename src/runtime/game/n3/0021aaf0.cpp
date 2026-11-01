// 0x0021aaf0 (a0 = resource block): RelocateEntryTable. The block header holds a signed entry count at
// +8 and a table offset at +12. The offset becomes an absolute pointer (+12 += a0). Then, for each entry
// index i below the count, the 16-byte entry at table + 16*i has its third word (+8) turned into an
// absolute pointer (+8 += a0). The table pointer and the count are re-read from memory on every pass, as
// the original does, because the table can overlap the header. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 8U;       // signed entry count, word at block + 8
constexpr GuestAddress kTableOffset = 12U;      // table offset, then the absolute table pointer
constexpr GuestAddress kRelocatedOffset = 8U;   // word of each 16-byte entry that is relocated

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPathCost = 5U;         // lw, daddu, addu, beq and the delay-slot sw
constexpr std::uint32_t kTestCost = 4U;         // lw, slt, bne and the delay-slot nop
constexpr std::uint32_t kBodyCost = 7U;         // lw, sll, addiu, addu, lw, addu, sw
constexpr std::uint32_t kReturnCost = 2U;       // jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns
// the retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate_entry_body(Memory& m, const GuestAddress block) {
    const std::uint32_t table_offset = m.read_u32_le(block + kTableOffset);
    m.write_u32_le(block + kTableOffset, table_offset + block);
    std::uint32_t retired = kPathCost;
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        const auto count = static_cast<std::int32_t>(m.read_u32_le(block + kCountOffset));
        if (!(static_cast<std::int32_t>(index) < count)) {
            return retired + kReturnCost;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kBodyCost;
        const GuestAddress table = m.read_u32_le(block + kTableOffset);
        const GuestAddress entry = table + (index << 4U);
        ++index;
        const std::uint32_t relocated = m.read_u32_le(entry + kRelocatedOffset) + block;
        m.write_u32_le(entry + kRelocatedOffset, relocated);
    }
}

template <class Context>
std::uint32_t relocate_entry_table_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate_entry_body(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run relocate_entry_table(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_entry_body(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_0021aaf0{0x0021aaf0U, 72U, 0xb66f9b7409d6293dULL, "RelocateEntryTable",
                                                  kCeiling, n3::live<&relocate_entry_table<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_entry_table_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0021aaf0, relocate_entry_table, relocate_entry_table_cost)
