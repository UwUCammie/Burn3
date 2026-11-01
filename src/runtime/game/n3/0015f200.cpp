// 0x0015f200 (a0 = blob header H): RelocateRecordOffsets. H+80 holds a signed 16-bit entry count and H+84 a u32
// offset of the entry array, relative to H+80: the offset is made absolute first (H+80 + offset). Each entry i
// (64 bytes at array + 64*i) then has its word at +12 and its word at +28 relocated: a non-zero word gets the
// entry's own address added. Finally the u32 at H+88 is cleared. The count and the array pointer are re-read on
// every test and iteration, as the original does, so stores into the header are seen. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 80U;       // signed 16-bit entry count; the base of the array offset
constexpr GuestAddress kArrayOffset = 84U;       // u32 array offset, made absolute in place
constexpr GuestAddress kClearOffset = 88U;       // u32 cleared at the very end
constexpr std::uint32_t kEntryShift = 6U;        // entries are 64 bytes
constexpr GuestAddress kFirstWordOffset = 12U;   // entry word relocated when non-zero
constexpr GuestAddress kSecondWordOffset = 28U;  // entry word relocated when non-zero

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSkipCost = 5U;          // lh, blez, addiu, jr, sw (count <= 0)
constexpr std::uint32_t kPreludeCost = 8U;       // lh, blez, addiu, lw, daddu, addu, beq, sw
constexpr std::uint32_t kTestCost = 4U;          // lh, slt, bne, nop: one per loop test
constexpr std::uint32_t kBodyCost = 6U;          // lw, sll, addu, lw, beq, nop
constexpr std::uint32_t kStoreCost = 2U;         // addu, sw: one relocated word
constexpr std::uint32_t kSecondTestCost = 3U;    // lw, beq, nop
constexpr std::uint32_t kStepCost = 1U;          // addiu
constexpr std::uint32_t kExitCost = 2U;          // jr, sw

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns
// the retired instruction count, or more than kCeiling as soon as it passes that.
template <class Memory>
std::uint32_t relocate_offsets(Memory& m, const GuestAddress header) {
    const GuestAddress count_address = header + kCountOffset;
    const GuestAddress array_address = header + kArrayOffset;
    const GuestAddress clear_address = header + kClearOffset;
    if (static_cast<std::int16_t>(m.read_u16_le(count_address)) <= 0) {
        m.write_u32_le(clear_address, 0U);
        return kSkipCost;
    }
    std::uint32_t retired = kPreludeCost;
    const std::uint32_t offset = m.read_u32_le(array_address);
    m.write_u32_le(array_address, offset + count_address);
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        const auto limit = static_cast<std::int16_t>(m.read_u16_le(count_address));
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(limit)) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress base = m.read_u32_le(array_address);
        const GuestAddress entry = base + (index << kEntryShift);
        retired += kBodyCost;
        const std::uint32_t first = m.read_u32_le(entry + kFirstWordOffset);
        if (first != 0U) {
            m.write_u32_le(entry + kFirstWordOffset, first + entry);
            retired += kStoreCost;
        }
        retired += kSecondTestCost;
        const std::uint32_t second = m.read_u32_le(entry + kSecondWordOffset);
        if (second != 0U) {
            m.write_u32_le(entry + kSecondWordOffset, second + entry);
            retired += kStoreCost;
        }
        retired += kStepCost;
        ++index;
    }
    retired += kExitCost;
    m.write_u32_le(clear_address, 0U);
    return retired;
}

template <class Context>
n3::Run relocate_record_offsets(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(relocate_offsets(call.memory(), call.arg(0)));
}

template <class Context>
std::uint32_t relocate_record_offsets_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = relocate_offsets(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

} // namespace

extern constinit const EngineFunction n3_0015f200{0x0015f200U, 112U, 0xa83c506f78fd06ddULL, "RelocateRecordOffsets",
                                                  kCeiling, n3::live<&relocate_record_offsets<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &relocate_record_offsets_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0015f200, relocate_record_offsets, relocate_record_offsets_cost)
