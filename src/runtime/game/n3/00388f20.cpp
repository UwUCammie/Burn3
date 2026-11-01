// 0x00388f20 (a0 = pool object): PopFreeRecord. The pool keeps a stack of record numbers: the count word at +7736 is
// how many are on it, and stack slot i is the byte at +7492 + i. An empty stack returns 0 and stores nothing.
// Otherwise the count drops by one, the number in the slot the count now names selects the 52-byte record at
// pool + 52 * number, bit 0 of that record's flags byte (+51) is set, and the result is the record's pointer 4 bytes
// in. The count and the slot are read again after the stores, as the original does, so the result follows whatever
// the stores wrote.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCountOffset = 7736U;      // u32: numbers on the stack
constexpr GuestAddress kSlotOffset = 7492U;       // u8 slots: slot i is the byte at kSlotOffset + i
constexpr GuestAddress kRecordStride = 52U;       // bytes per record
constexpr GuestAddress kFlagsOffset = 51U;        // the record's flags byte; bit 0 marks a popped record
constexpr GuestAddress kPayloadOffset = 4U;       // the result points this far into the record

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEmptyCost = 7U;          // lw, bne, nop, beq, daddu, jr $ra, nop
constexpr std::uint32_t kPopCost = 29U;           // lw, bne, nop, the 24 instructions of the pop, jr $ra, nop

// The record that stack slot `slot` names: pool + 52 * (the number stored in that slot).
template <class Memory>
GuestAddress record_in_slot(Memory& m, const GuestAddress pool, const std::uint32_t slot) {
    const GuestAddress number = m.read_u8(pool + slot + kSlotOffset);
    return pool + kRecordStride * number;
}

template <class Context>
n3::Run pop_free_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pool = call.arg(0);
    const GuestAddress count_address = pool + kCountOffset;

    const std::uint32_t count = m.read_u32_le(count_address);
    if (count == 0U) {
        call.return_value(0U);
        return call.finish(kEmptyCost);
    }
    m.write_u32_le(count_address, count - 1U);

    const GuestAddress record = record_in_slot(m, pool, m.read_u32_le(count_address));
    const std::uint8_t flags = m.read_u8(record + kFlagsOffset);
    m.write_u8(record + kFlagsOffset, static_cast<std::uint8_t>(flags | 1U));

    const GuestAddress result_record = record_in_slot(m, pool, m.read_u32_le(count_address));
    call.return_value(result_record + kPayloadOffset);
    return call.finish(kPopCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00388f20, 124U, 0xf432a0110c0e4ea2ULL, "PopFreeRecord", 29U, pop_free_record, EngineReturn::Integer)
