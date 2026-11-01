// 0x00165df0 (a0 = scene/list object): FillSlotValuesFromKeyTable. The descriptor pointer is the word at a0+0.
// Part 1 handles entries v = 0, 1, ... while v < (signed byte at a0+0x34450) - desc[182]. Entry v (24 bytes,
// the table at desc[84]) gives a u64 key; when it equals KEYS[j] for some j of the 128-entry table at 0x00518aa8,
// the word VALUES[j] (table at 0x005190a8) is stored at slot v, a0+0x34420+4v. v counts up for every entry.
// Part 2, when n = desc[182] is not zero, runs five key sections, one per bit of desc[181]; the key is the u64 at
// desc+0, +8, +16, +24, +32. The first four take the slot v and count v up whether or not the key is found; the
// 0x10 section takes slot v without counting it. A section stores VALUES[j] into its slot only when the key is found
// at index j. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kKeys = 0x00518aa8U;          // 128 u64 keys (0x520000 - 30040)
constexpr GuestAddress kValues = 0x005190a8U;        // 128 u32 values (0x520000 - 28504)
constexpr std::uint32_t kTableSize = 128U;
constexpr GuestAddress kSlotBase = 0x00034420U;      // a0 + 214048 + 4 * slot
constexpr GuestAddress kTotalOffset = 0x00034450U;   // a0 + 214096: signed count byte (slot 12 lands on it)
constexpr std::uint32_t kEntriesOffset = 84U;        // desc + 84: pointer to the 24-byte entries
constexpr std::uint32_t kEntryStride = 24U;
constexpr std::uint32_t kCountOffset = 182U;         // desc + 182: n, unsigned
constexpr std::uint32_t kMaskOffset = 181U;          // desc + 181: section bits
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 6U;          // lui, lui, daddu, addiu, beq, addiu (delay)
constexpr std::uint32_t kHeadCost = 9U;              // lw, lb, lbu, subu, slt, bne, nop (delay)
constexpr std::uint32_t kEntryCost = 8U;             // lw, sll, addu, sll, addu, ld, beq, daddu (delay)
constexpr std::uint32_t kTestCost = 3U;              // slti, bne, nop (delay): j < 128
constexpr std::uint32_t kStepCost = 1U;              // addiu j, after a miss
constexpr std::uint32_t kEntryCompareCost = 5U;      // sll, addu, ld, beq, nop (delay)
constexpr std::uint32_t kEntryFoundCost = 11U;       // address, value load, sw (the delay slot of the beq to v++)
constexpr std::uint32_t kIncrementCost = 1U;         // addiu v, the step after every entry
constexpr std::uint32_t kBlezCost = 2U;              // blez n, nop (delay)
constexpr std::uint32_t kMaskCost = 4U;              // lbu, andi, beq, nop (delay)
constexpr std::uint32_t kReloadCost = 1U;            // lw desc, sections 2-5 (fall-through or delay slot)
constexpr std::uint32_t kSlotPrefixCost = 7U;        // ld key, daddu, addiu v, daddu j, lui, beq, addiu (delay)
constexpr std::uint32_t kPlainPrefixCost = 5U;       // ld key, lui, daddu j, beq, addiu (delay)
constexpr std::uint32_t kSectionCompareCost = 7U;    // sll, lui, addu, addu, ld, beq, nop (delay)
constexpr std::uint32_t kSectionFoundCost = 10U;     // address, value load, sw
constexpr std::uint32_t kMissBranchCost = 1U;        // beq to the next section (its delay slot is that reload)
constexpr std::uint32_t kLastMissCost = 2U;          // beq and nop (delay) to the return
constexpr std::uint32_t kReturnCost = 2U;            // jr $ra, nop (delay)

constexpr std::uint32_t kSectionCount = 5U;

struct KeySection {
    std::uint32_t bit;
    std::uint32_t key_offset;
    bool takes_slot;  // true: stores at v and counts v up; false: stores at v and leaves it
};

constexpr KeySection kSections[kSectionCount] = {
    {0x01U, 0U, true}, {0x02U, 8U, true}, {0x04U, 16U, true}, {0x08U, 24U, true}, {0x10U, 32U, false},
};

// The key search: j = 0 upwards. Each step tests j < 128 (kTestCost), compares KEYS[j] with the key (compare_cost)
// and steps j on a miss (kStepCost). Returns the matching index, or kTableSize when none matches.
template <class Memory>
std::uint32_t find_key(Memory& m, const std::uint64_t key, const std::uint32_t compare_cost,
                       std::uint32_t& retired) {
    std::uint32_t j = 0U;
    for (;;) {
        if (retired > kCeiling) {
            return kTableSize;
        }
        retired += kTestCost;
        if (j >= kTableSize) {
            return kTableSize;
        }
        retired += compare_cost;
        if (m.read_u64_le(kKeys + 8U * j) == key) {
            return j;
        }
        retired += kStepCost;
        ++j;
    }
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or a count above kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t fill_slot_values(Memory& m, const GuestAddress a0) {
    std::uint32_t retired = kPrologueCost;
    std::uint32_t v = 0U;
    std::uint32_t desc = 0U;
    std::uint32_t n = 0U;
    for (;;) {
        desc = m.read_u32_le(a0);
        const auto total = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(a0 + kTotalOffset)));
        n = m.read_u8(desc + kCountOffset);
        retired += kHeadCost;
        if (retired > kCeiling) {
            return retired;
        }
        if (static_cast<std::int32_t>(v) >= total - static_cast<std::int32_t>(n)) {
            break;
        }
        retired += kEntryCost;
        const GuestAddress entries = m.read_u32_le(desc + kEntriesOffset);
        const std::uint64_t key = m.read_u64_le(entries + kEntryStride * v);
        const std::uint32_t found = find_key(m, key, kEntryCompareCost, retired);
        if (retired > kCeiling) {
            return retired;
        }
        if (found != kTableSize) {
            retired += kEntryFoundCost;
            const std::uint32_t value = m.read_u32_le(kValues + 4U * found);
            m.write_u32_le(a0 + kSlotBase + 4U * v, value);
        }
        retired += kIncrementCost;
        ++v;
    }
    retired += kBlezCost;
    if (n == 0U) {
        return retired + kReturnCost;
    }
    for (std::uint32_t s = 0U; s < kSectionCount; ++s) {
        const KeySection& section = kSections[s];
        const bool last = s + 1U == kSectionCount;
        if (s > 0U) {
            retired += kReloadCost;
            desc = m.read_u32_le(a0);
        }
        retired += kMaskCost;
        if ((static_cast<std::uint32_t>(m.read_u8(desc + kMaskOffset)) & section.bit) == 0U) {
            continue;
        }
        const std::uint64_t key = m.read_u64_le(desc + section.key_offset);
        const std::uint32_t slot = v;
        if (section.takes_slot) {
            retired += kSlotPrefixCost;
            ++v;
        } else {
            retired += kPlainPrefixCost;
        }
        const std::uint32_t found = find_key(m, key, kSectionCompareCost, retired);
        if (retired > kCeiling) {
            return retired;
        }
        if (found != kTableSize) {
            retired += kSectionFoundCost;
            const std::uint32_t value = m.read_u32_le(kValues + 4U * found);
            m.write_u32_le(a0 + kSlotBase + 4U * slot, value);
        } else {
            retired += last ? kLastMissCost : kMissBranchCost;
        }
    }
    return retired + kReturnCost;
}

template <class Context>
std::uint32_t fill_slot_values_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = fill_slot_values(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run fill_slot_values_native(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(fill_slot_values(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00165df0{0x00165df0U, 880U, 0x5629b6fa0cd5565bULL,
                                                  "FillSlotValuesFromKeyTable", kCeiling, n3::live<&fill_slot_values_native<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, &fill_slot_values_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00165df0, fill_slot_values_native, fill_slot_values_cost)
