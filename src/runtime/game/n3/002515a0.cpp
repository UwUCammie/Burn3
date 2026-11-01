// 0x002515a0 (a0 = record P): RelocatePointersToOffsets. Turns the absolute pointers in a record into offsets by
// adding D = 0 - P (u32, wrapping) to each of them. The block at P+36 starts with its entry count (read once); the
// entry words at block+68+64*i for i < count are adjusted with no zero test. Then the word at P+108, the seven words
// at P+112..P+136, and the words P+0, P+4, P+8 and P+36 (read live, after the stores above) are adjusted only when
// they are non-zero. Returns nothing: the caller at 0x002516c0 discards v1.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kBlockPointer = 36U;       // record + 36: the block whose word 0 is the entry count
constexpr GuestAddress kBlockCount = 0U;          // block + 0
constexpr GuestAddress kEntryBase = 68U;          // entry i is the word at block + 68 + 64*i
constexpr GuestAddress kEntryStride = 64U;
constexpr GuestAddress kWordBeforeSlots = 108U;   // record + 108
constexpr GuestAddress kSlotBase = 112U;          // record + 112 + 4*k for k = 0..6
constexpr std::uint32_t kSlotCount = 7U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 7U;          // 0x002515a0..0x002515b8: setup and the jump to the count test
constexpr std::uint32_t kCountTestCost = 3U;      // 0x002515d0..0x002515d8: each evaluation of entry < count
constexpr std::uint32_t kEntryAdjustCost = 5U;    // 0x002515bc..0x002515cc: one entry adjusted
constexpr std::uint32_t kFieldZeroCost = 3U;      // zero-tested word left alone: load, test, delay slot
constexpr std::uint32_t kFieldSetCost = 5U;       // zero-tested word adjusted: as above, plus the add and store
constexpr std::uint32_t kSlotLoopEntryCost = 1U;  // 0x002515f0: slot index = 0
constexpr std::uint32_t kSlotTestCost = 6U;       // 0x002515f4..0x00251608: address, load, zero test, delay slot
constexpr std::uint32_t kSlotSetCost = 2U;        // 0x0025160c..0x00251610: add and store, non-zero only
constexpr std::uint32_t kSlotStepCost = 4U;       // 0x00251614..0x00251620: index step, bound test, delay slot
constexpr std::uint32_t kReturnCost = 2U;         // 0x00251674..0x00251678: jr $ra and its delay slot

// The count comes from guest memory, so the loop is bounded by this: calls retiring more are left to the
// original (call_cost returns 0), and the count a call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// A zero-tested word: adjusted only when it is non-zero. Returns the retired count of its block.
template <class Memory>
std::uint32_t adjust_if_set(Memory& m, const GuestAddress address, const std::uint32_t distance) {
    const std::uint32_t value = m.read_u32_le(address);
    if (value == 0U) {
        return kFieldZeroCost;
    }
    m.write_u32_le(address, value + distance);
    return kFieldSetCost;
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the retired
// instruction count, or more than kCeiling as soon as the count passes it.
template <class Memory>
std::uint32_t relocate(Memory& m, const GuestAddress record) {
    const std::uint32_t distance = 0U - record;
    std::uint32_t retired = kEntryCost;
    const GuestAddress block = m.read_u32_le(record + kBlockPointer);
    const std::uint32_t count = m.read_u32_le(block + kBlockCount);
    std::uint32_t entry_index = 0U;
    for (;;) {
        retired += kCountTestCost;
        if (entry_index >= count) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress entry = block + kEntryBase + kEntryStride * entry_index;
        const std::uint32_t value = m.read_u32_le(entry);
        m.write_u32_le(entry, value + distance);
        retired += kEntryAdjustCost;
        ++entry_index;
    }
    retired += adjust_if_set(m, record + kWordBeforeSlots, distance);
    retired += kSlotLoopEntryCost;
    for (std::uint32_t slot = 0U; slot < kSlotCount; ++slot) {
        const GuestAddress word = record + kSlotBase + 4U * slot;
        const std::uint32_t value = m.read_u32_le(word);
        retired += kSlotTestCost;
        if (value != 0U) {
            m.write_u32_le(word, value + distance);
            retired += kSlotSetCost;
        }
        retired += kSlotStepCost;
    }
    retired += adjust_if_set(m, record + 0U, distance);
    retired += adjust_if_set(m, record + 4U, distance);
    retired += adjust_if_set(m, record + 8U, distance);
    retired += adjust_if_set(m, record + kBlockPointer, distance);
    return retired + kReturnCost;
}

template <class Context>
n3::Run relocate_pointers_to_offsets(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate(call.memory(), call.arg(0)));
}

template <class Context>
std::uint32_t relocate_pointers_to_offsets_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_002515a0{0x002515a0U, 220U, 0x56cba3c256a3f396ULL, "RelocatePointersToOffsets",
                                                  kCeiling, n3::live<&relocate_pointers_to_offsets<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_pointers_to_offsets_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002515a0, relocate_pointers_to_offsets, relocate_pointers_to_offsets_cost)
