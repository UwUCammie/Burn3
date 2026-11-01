// 0x00161310 (a0 = object): ClearMatchingSlotKey. The object's byte at +73 is a key K. The word at 0x00527440
// points to a record whose word at +56 is the signed count N of table entries. Entry i is the 24 bytes at
// 0x00538c0c + 24 * i: its word at +0 points to a holder P, the byte at P + 21 is the count c (0..255) of key
// slots, and slot j is the byte at entry + 16 + j (the walk may run on into the following entries; nothing is
// clamped). The first slot whose byte equals K becomes 0xFF and the function returns; when no slot matches
// nothing is stored. The callers ignore the result.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecordPointer = 0x00527440U;  // lui 0x52, lw at offset 29760: the record pointer
constexpr GuestAddress kEntryCountOffset = 56U;       // record + 56: N, the signed entry count
constexpr GuestAddress kEntries = 0x00538c0cU;        // 0x00522660 (lui 0x52, addiu 9824) + 0x000165ac
constexpr GuestAddress kEntryStride = 24U;            // ((i << 1) + i) << 3
constexpr GuestAddress kSlotCountOffset = 21U;        // holder + 21: c, the slot count of the entry
constexpr GuestAddress kSlotsOffset = 16U;            // entry + 16 + j: slot j
constexpr GuestAddress kKeyOffset = 73U;              // object + 73: the key K
constexpr std::uint8_t kClearedSlot = 0xFFU;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 7U;      // lui, lui, lw, daddu, lw, beq and its delay slot
constexpr std::uint32_t kEntryTestCost = 3U;     // outer test: slt, bne and its delay slot
constexpr std::uint32_t kEntryBodyCost = 11U;    // sll, lui, addu, ori, sll, addu, addu, lw, lbu, beq, daddu
constexpr std::uint32_t kSlotTestCost = 3U;      // inner test: slt, bne and its delay slot
constexpr std::uint32_t kProbeCost = 6U;         // addu, lbu, addiu, lbu, beq and its delay slot
constexpr std::uint32_t kSlotStepCost = 1U;      // addiu (next slot), after a probe that misses
constexpr std::uint32_t kEntryStepCost = 1U;     // addiu (next entry), after the inner loop ends
constexpr std::uint32_t kNoMatchTailCost = 4U;   // beq and its delay slot, jr and its delay slot
constexpr std::uint32_t kMatchTailCost = 4U;     // addiu, sb, jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t clear_matching_slot(Memory& m, const GuestAddress object) {
    std::uint32_t retired = kPrologueCost;
    const GuestAddress record = m.read_u32_le(kRecordPointer);
    const auto entry_count = static_cast<std::int32_t>(m.read_u32_le(record + kEntryCountOffset));
    std::uint32_t entry_index = 0U;
    while (true) {
        retired += kEntryTestCost;
        if (static_cast<std::int32_t>(entry_index) >= entry_count) {
            return retired + kNoMatchTailCost;
        }
        if (retired > kCeiling) {
            return retired;
        }
        retired += kEntryBodyCost;
        const GuestAddress entry = kEntries + kEntryStride * entry_index;
        const GuestAddress holder = m.read_u32_le(entry);
        const std::uint32_t count = m.read_u8(holder + kSlotCountOffset);
        std::uint32_t slot_index = 0U;
        while (true) {
            retired += kSlotTestCost;
            if (slot_index >= count) {
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            const std::uint8_t key = m.read_u8(object + kKeyOffset);
            const GuestAddress slot = entry + kSlotsOffset + slot_index;
            if (m.read_u8(slot) == key) {
                m.write_u8(slot, kClearedSlot);
                return retired + kProbeCost + kMatchTailCost;
            }
            retired += kProbeCost + kSlotStepCost;
            ++slot_index;
        }
        retired += kEntryStepCost;
        ++entry_index;
    }
}

template <class Context>
std::uint32_t clear_matching_slot_key_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = clear_matching_slot(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run clear_matching_slot_key(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(clear_matching_slot(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_00161310{0x00161310U, 152U, 0x00e796595f9b08eaULL, "ClearMatchingSlotKey",
                                                  kCeiling, n3::live<&clear_matching_slot_key<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &clear_matching_slot_key_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00161310, clear_matching_slot_key, clear_matching_slot_key_cost)
