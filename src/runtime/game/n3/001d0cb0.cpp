// 0x001d0cb0 (a0 = object record): SlotRecordPointer. Returns 0 in v0 when the enable word at 0x0064d3a0 is zero.
// Otherwise it returns 0x0064d3b0 + (signed byte at a0 + 5460) * 10192: the address of a record in the table that
// starts 16 bytes after the enable word, with the 32-bit wrapping arithmetic of the original's mult and addu. The
// index byte is loaded only on the enabled path. A leaf: no stores, no calls.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x65, lw -11360: the enable word.
constexpr GuestAddress kEnableWord = 0x0064d3a0U;
// lb offset of the signed table index in the object record in a0.
constexpr GuestAddress kIndexOffset = 5460U;
// Table base: lui 0x52 + addiu 9824 (0x00522660) plus lui 0x12 + ori 0xad50 (0x0012ad50).
constexpr std::uint32_t kTableBase = 0x0064d3b0U;
// addiu $v1, $zero, 10192: the size of one table record.
constexpr std::uint32_t kRecordSize = 10192U;

// Retired instruction counts from the listing, delay slots included. Enable word zero: lui, lw, bne (not taken)
// and its nop, beq and its daddu, then jr and its nop (8). Enabled: lui, lw, bne (taken) and its nop, then lb
// through the second addu (9), then jr and its nop (15).
constexpr std::uint32_t kDisabledCost = 8U;
constexpr std::uint32_t kEnabledCost = 15U;

template <class Context>
n3::Run slot_record_pointer(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    if (m.read_u32_le(kEnableWord) == 0U) {
        call.return_value(0U);
        return call.finish(kDisabledCost);
    }
    // lb sign-extends the index byte; the product and the sum wrap in 32 bits.
    const auto index = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(call.arg(0) + kIndexOffset)));
    call.return_value(kTableBase + static_cast<std::uint32_t>(index) * kRecordSize);
    return call.finish(kEnabledCost);
}

} // namespace

extern constinit const EngineFunction n3_001d0cb0{0x001d0cb0U, 68U, 0x36df4a7eeee42f31ULL, "SlotRecordPointer", 15U,
                                                  n3::live<&slot_record_pointer<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001d0cb0, slot_record_pointer)
