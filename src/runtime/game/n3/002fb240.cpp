// 0x002fb240 (a0 = resource header): RelocateOffsetTable. Turns a resource's two-level offset table into
// absolute addresses in place. The word at header+60 is an offset from the header (0 = no table) and becomes
// header + offset. The byte at header+64 counts the 16-byte groups of the table. In each group the word at +8
// is an offset from the group to its array of 8-byte sub-entries (0 = none; it becomes group + offset) and the
// byte at +12 counts that array. In each sub-entry the word at +0 is an offset from the sub-entry (0 = none; it
// becomes sub-entry + offset). Returns nothing: v0 is untouched.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTableField = 60U;        // header word: table offset
constexpr GuestAddress kGroupCountField = 64U;   // header byte: number of groups
constexpr GuestAddress kGroupStride = 16U;
constexpr GuestAddress kArrayField = 8U;         // group word: sub-entry array offset
constexpr GuestAddress kSubCountField = 12U;     // group byte: number of sub-entries
constexpr GuestAddress kSubStride = 8U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;         // lw, beq, nop
constexpr std::uint32_t kReturnCost = 2U;        // jr $ra, nop
constexpr std::uint32_t kHeaderFixCost = 4U;     // addu, daddu, beq, sw
constexpr std::uint32_t kOuterCheckCost = 4U;    // lbu, slt, bne, nop
constexpr std::uint32_t kGroupHeadCost = 6U;     // lw, sll, addu, lw, beq, nop
constexpr std::uint32_t kArrayFixCost = 4U;      // addu, daddu, beq, sw
constexpr std::uint32_t kGroupStepCost = 1U;     // addiu $t0
constexpr std::uint32_t kInnerCheckCost = 4U;    // lbu, slt, bne, nop
constexpr std::uint32_t kInnerHeadCost = 6U;     // lw, sll, addu, lw, beq, nop
constexpr std::uint32_t kEntryFixCost = 2U;      // addu, sw (only when the sub-entry word is not 0)
constexpr std::uint32_t kInnerStepCost = 1U;     // addiu $a3

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports
// is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as the run passes it.
template <class Memory>
std::uint32_t relocate_offset_table(Memory& m, const GuestAddress header) {
    std::uint32_t retired = kEntryCost;
    const std::uint32_t table_offset = m.read_u32_le(header + kTableField);
    if (table_offset == 0U) {
        return retired + kReturnCost;
    }
    retired += kHeaderFixCost;
    m.write_u32_le(header + kTableField, table_offset + header);

    std::uint32_t group = 0U;
    for (;;) {
        retired += kOuterCheckCost;
        if (group >= static_cast<std::uint32_t>(m.read_u8(header + kGroupCountField))) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kGroupHeadCost;
        // The table pointer and the group's array word are reloaded here, as the listing reloads them.
        const GuestAddress group_address = m.read_u32_le(header + kTableField) + group * kGroupStride;
        const std::uint32_t array_offset = m.read_u32_le(group_address + kArrayField);
        if (array_offset != 0U) {
            retired += kArrayFixCost;
            m.write_u32_le(group_address + kArrayField, array_offset + group_address);
            std::uint32_t sub = 0U;
            for (;;) {
                retired += kInnerCheckCost;
                if (sub >= static_cast<std::uint32_t>(m.read_u8(group_address + kSubCountField))) {
                    break;
                }
                if (retired > kCeiling) {
                    return retired;
                }
                retired += kInnerHeadCost;
                const GuestAddress entry = m.read_u32_le(group_address + kArrayField) + sub * kSubStride;
                const std::uint32_t entry_offset = m.read_u32_le(entry);
                if (entry_offset != 0U) {
                    retired += kEntryFixCost;
                    m.write_u32_le(entry, entry_offset + entry);
                }
                retired += kInnerStepCost;
                ++sub;
            }
        }
        retired += kGroupStepCost;
        ++group;
    }
    return retired + kReturnCost;
}

template <class Context>
n3::Run relocate_offset_table_native(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_offset_table(call.memory(), call.arg(0)));
}

template <class Context>
std::uint32_t relocate_offset_table_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate_offset_table(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_002fb240{0x002fb240U, 148U, 0x626feeb206381e93ULL, "RelocateOffsetTable",
                                                  kCeiling, n3::live<&relocate_offset_table_native<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_offset_table_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002fb240, relocate_offset_table_native, relocate_offset_table_cost)
