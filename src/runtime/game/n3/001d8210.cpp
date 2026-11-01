// 0x001d8210 (a0 = block base, a1 = any 32-bit value): RelocateOffsetTable128. A one-shot pointer
// fixup of a loaded resource block. The value in a1 always goes to +4. While the flag word at +24 is
// clear, the offset word at +28 becomes an absolute pointer (offset + base), each of the 128 offset
// words at +32 .. +543 gets the base added, and the flag word at +24 is set to 1. Later calls return
// after the +4 store. Returns nothing (the original's v1 is a scratch value only).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kValueOffset = 4U;         // sw $a1, 4($a0)
constexpr GuestAddress kFlagOffset = 24U;         // 0 until the block is relocated, then 1
constexpr GuestAddress kEntryOffset = 28U;        // offset of the entry pointer
constexpr GuestAddress kTableOffset = 32U;        // first of the 128 offset words
constexpr std::uint32_t kTableWords = 128U;

// Retired instruction counts from the listing, delay slots included: the early return is
// sw, lw, bne (taken) and its nop, then jr and its nop; the relocating path is 8 instructions
// before the loop, 8 per table word (sll, addu, addiu, lw, sltiu, addu, bne and the delay-slot sw),
// then addiu, sw, jr and its nop.
constexpr std::uint32_t kEarlyReturnCost = 6U;
constexpr std::uint32_t kRelocateCost = 1036U;

template <class Context>
n3::Run relocate_offset_table_128(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress block = call.arg(0);
    const std::uint32_t value = call.arg(1);
    m.write_u32_le(block + kValueOffset, value);
    if (m.read_u32_le(block + kFlagOffset) != 0U) {
        return call.finish(kEarlyReturnCost);
    }
    const std::uint32_t entry = m.read_u32_le(block + kEntryOffset);
    m.write_u32_le(block + kEntryOffset, entry + block);
    for (std::uint32_t index = 0U; index < kTableWords; ++index) {
        const GuestAddress slot = block + kTableOffset + 4U * index;
        const std::uint32_t offset = m.read_u32_le(slot);
        m.write_u32_le(slot, offset + block);
    }
    m.write_u32_le(block + kFlagOffset, 1U);
    return call.finish(kRelocateCost);
}

} // namespace

extern constinit const EngineFunction n3_001d8210{0x001d8210U, 80U, 0xd2ef52c58cca325fULL, "RelocateOffsetTable128",
                                                  1036U, n3::live<&relocate_offset_table_128<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001d8210, relocate_offset_table_128)
