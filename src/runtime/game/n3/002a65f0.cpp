// 0x002a65f0 (a0 = object O, a1 = record R, a2 = slot index or -1, f12 = value bits): SetFloatSlotsAndFlag.
// O holds a flags word at +0 and float slots from +24 (slot i at O + 24 + 4*i). When a2 is not the full 64-bit
// -1, the value goes into slot (low 32 bits of a2) with no bounds check. When a2 is -1, it goes into every slot
// i = 0 .. count-1, where count is the signed word at R + 4, read again before each slot (a slot store can
// overlap it). Either way the flags word then gets the bit 0x10, read after the slot stores. Nothing is returned.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagsOffset = 0U;                   // flags word at O + 0
constexpr GuestAddress kSlotsOffset = 24U;                  // slot i at O + 24 + 4 * i
constexpr GuestAddress kCountOffset = 4U;                   // signed count word at R + 4
constexpr std::uint32_t kDirtyBit = 0x10U;                  // OR-ed into the flags word
constexpr std::uint64_t kAllSlots = 0xffffffffffffffffULL;  // a2 == -1, compared as the whole register

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 3U;                     // addiu v1 = -1, bne a2, v1, sll (delay slot)
constexpr std::uint32_t kSingleCost = 3U;                   // addu, beq (taken), swc1 (delay slot): a2 != -1
constexpr std::uint32_t kAllHeadCost = 2U;                  // beq (taken), daddu index = 0 (delay slot): a2 == -1
constexpr std::uint32_t kTestCost = 4U;                     // lw count, slt, bne, sll (delay slot)
constexpr std::uint32_t kBodyCost = 3U;                     // addu, addiu index, swc1 (the store)
constexpr std::uint32_t kTailCost = 4U;                     // lw flags, ori, beq (taken), sw (delay slot)
constexpr std::uint32_t kReturnCost = 2U;                   // jr ra, nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t set_slots(Memory& m, const GuestAddress object, const GuestAddress record, const std::uint32_t value,
                        const std::uint32_t index, const bool all_slots) {
    std::uint32_t retired = kHeadCost;
    if (!all_slots) {
        // The index is the low 32 bits of a2, shifted left by two, wrapping.
        retired += kSingleCost;
        m.write_u32_le(object + kSlotsOffset + (index << 2U), value);
    } else {
        retired += kAllHeadCost;
        std::uint32_t slot = 0U;
        for (;;) {
            retired += kTestCost;
            // Re-read on every pass: a slot store can overlap the count word.
            if (static_cast<std::int32_t>(slot) >= static_cast<std::int32_t>(m.read_u32_le(record + kCountOffset))) {
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            m.write_u32_le(object + kSlotsOffset + (slot << 2U), value);
            ++slot;
            retired += kBodyCost;
        }
    }
    // Tail: read after the slot stores, because a slot may overlap the flags word.
    const std::uint32_t flags = m.read_u32_le(object + kFlagsOffset);
    m.write_u32_le(object + kFlagsOffset, flags | kDirtyBit);
    return retired + kTailCost + kReturnCost;
}

template <class Context>
std::uint32_t set_float_slots_and_flag_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const bool all_slots = context.cpu.read_gpr_u64(6U) == kAllSlots;
    const std::uint32_t retired = set_slots(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                            context.cpu.read_fpr_bits(12U), context.cpu.read_gpr_u32(6U), all_slots);
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run set_float_slots_and_flag(Context& context) {
    auto call = n3::guest_call(context);
    const bool all_slots = call.arg64(2U) == kAllSlots;
    const std::uint32_t retired = set_slots(call.memory(), call.arg(0U), call.arg(1U), call.cpu().read_fpr_bits(12U),
                                            call.arg(2U), all_slots);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002a65f0{0x002a65f0U, 84U, 0xb8029bb3926b4538ULL, "SetFloatSlotsAndFlag",
                                                  kCeiling, n3::live<&set_float_slots_and_flag<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &set_float_slots_and_flag_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002a65f0, set_float_slots_and_flag, set_float_slots_and_flag_cost)
