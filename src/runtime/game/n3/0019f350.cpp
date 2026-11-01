// 0x0019f350 (a0 = descriptor D): BroadcastRecordWordToVectorW. D+4 is a pointer P to an array of 112-byte
// records, D+16 a signed 16-bit base, D+18 a signed 16-bit count and D+28 a signed 8-bit record index.
// On entry the record P + 112 * index is located; its word at +64 is a destination array pointer, read on every
// pass from that fixed slot. For i = 0 while i < count (signed, tested before every pass, the first included),
// the word at P + 112 * index + 100 is stored at destination + 16 * (base + i) + 12. P, index, base, count and
// the destination pointer are re-read from memory on every pass, because the stores can alias the descriptor
// and the records; only the entry-time slot address is kept. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPointerOffset = 4U;         // lw 4(D): P
constexpr GuestAddress kBaseOffset = 16U;           // lh 16(D): base
constexpr GuestAddress kCountOffset = 18U;          // lh 18(D): count
constexpr GuestAddress kIndexOffset = 28U;          // lb 28(D): record index
constexpr std::uint32_t kRecordBytes = 112U;        // record stride
constexpr GuestAddress kDestinationOffset = 64U;    // record + 64 holds the destination pointer
constexpr GuestAddress kWordOffset = 100U;          // record + 100: the word that is stored
constexpr GuestAddress kStoreOffset = 12U;          // destination row + 12
constexpr std::uint32_t kRowShift = 4U;             // 16 bytes per destination row

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 9U;            // lb, daddu, lw, sll, subu, sll, addu, beq, addiu (delay slot)
constexpr std::uint32_t kTestCost = 4U;             // lh, slt, bne and its nop
constexpr std::uint32_t kPassCost = 14U;            // lb through sw, one pass
constexpr std::uint32_t kExitCost = 2U;             // jr $ra and its nop

// Calls retiring more than this are left to the original (call_cost returns 0).
constexpr std::uint32_t kCeiling = 20000U;

// lb and lh sign-extend into the 64-bit register; the 32-bit image is what the arithmetic uses.
std::uint32_t sign_extend_byte(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}
std::uint32_t sign_extend_half(const std::uint16_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost): returns the
// retired instruction count, or a value above kCeiling as soon as the count passes it.
template <class Memory>
std::uint32_t broadcast(Memory& m, const GuestAddress descriptor) {
    // Entry: the record found here fixes the destination slot (t1 = record + 20, read at +44) for all passes.
    const std::uint32_t entry_index = sign_extend_byte(m.read_u8(descriptor + kIndexOffset));
    const GuestAddress entry_pointer = m.read_u32_le(descriptor + kPointerOffset);
    const GuestAddress destination_slot = entry_pointer + kRecordBytes * entry_index + kDestinationOffset;
    std::uint32_t retired = kEntryCost;
    std::uint32_t i = 0U;
    for (;;) {
        retired += kTestCost;
        const auto count = static_cast<std::int32_t>(sign_extend_half(m.read_u16_le(descriptor + kCountOffset)));
        if (static_cast<std::int32_t>(i) >= count) {
            break;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const std::uint32_t record_index = sign_extend_byte(m.read_u8(descriptor + kIndexOffset));
        const std::uint32_t base = sign_extend_half(m.read_u16_le(descriptor + kBaseOffset));
        const GuestAddress pointer = m.read_u32_le(descriptor + kPointerOffset);
        const GuestAddress destination = m.read_u32_le(destination_slot);
        const GuestAddress record = pointer + kRecordBytes * record_index;
        const std::uint32_t word = m.read_u32_le(record + kWordOffset);
        const GuestAddress row = destination + ((base + i) << kRowShift);
        m.write_u32_le(row + kStoreOffset, word);
        retired += kPassCost;
        ++i;
    }
    return retired + kExitCost;
}

// call_cost: the exact count on a DryMemory, or 0 when it is above kCeiling (the original then runs).
template <class Context>
std::uint32_t broadcast_record_word_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired = broadcast(dry, context.cpu.read_gpr_u32(4U));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run broadcast_record_word(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(broadcast(call.memory(), call.arg(0)));
}

} // namespace

extern constinit const EngineFunction n3_0019f350{0x0019f350U, 116U, 0x1f0224ab5639a26dULL,
                                                  "BroadcastRecordWordToVectorW", kCeiling, n3::live<&broadcast_record_word<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &broadcast_record_word_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0019f350, broadcast_record_word, broadcast_record_word_cost)
