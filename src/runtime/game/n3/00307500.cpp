// 0x00307500 (a0 = object, a1 = slot number): SelectBufferSlot, a leaf that returns nothing. The object keeps its
// slot as an i8 at +48 and one i8 flag per slot at +46 + slot. The selected slot's flag is toggled, then the word for
// (slot, flag) in the table at +16 (8 bytes per slot, 4 per flag) is ORed with 0x30000000 into +32, and +36 becomes
// +32 plus the signed halfword at +40 shifted left 5. Every load re-reads memory: the stores can alias (slot 2 makes
// +46 + slot equal +48).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kTableFlagBits = 0x30000000U;  // lui $v1, 0x3000

// lb and lh sign-extend to 32 bits.
inline std::uint32_t sign_extend8(const std::uint8_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}
inline std::uint32_t sign_extend16(const std::uint16_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

template <class Context>
n3::Run select_buffer_slot(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t new_slot = call.arg(1);

    // sb $a1, 48($a0)
    m.write_u8(object + 48U, static_cast<std::uint8_t>(new_slot));

    // lb $a1, 48($a0); addu $a2, $a1, $a0; lb $a1, 46($a2); xori $a1, $a1, 1; sb $a1, 46($a2)
    const std::uint32_t slot = sign_extend8(m.read_u8(object + 48U));
    const GuestAddress flag_base = slot + object;
    const std::uint32_t flag = sign_extend8(m.read_u8(flag_base + 46U));
    m.write_u8(flag_base + 46U, static_cast<std::uint8_t>(flag ^ 1U));

    // lb $a1, 48($a0) (re-read); sll $a2, $a1, 3; addu $a1, $a1, $a0; addu $a2, $a2, $a0;
    // lb $a1, 46($a1) (re-read); sll $a1, $a1, 2; addu $a1, $a1, $a2; lw $a1, 16($a1);
    // lui $v1, 0x3000; or $v1, $a1, $v1; sw $v1, 32($a0)
    const std::uint32_t slot_again = sign_extend8(m.read_u8(object + 48U));
    const GuestAddress slot_row = (slot_again << 3U) + object;
    const GuestAddress flag_entry = slot_again + object;
    const std::uint32_t flag_again = sign_extend8(m.read_u8(flag_entry + 46U));
    const GuestAddress word_address = (flag_again << 2U) + slot_row;
    const std::uint32_t selected = m.read_u32_le(word_address + 16U) | kTableFlagBits;
    m.write_u32_le(object + 32U, selected);

    // lh $a1, 40($a0); lw $v1, 32($a0) (re-read); sll $a1, $a1, 5; addu $v1, $v1, $a1;
    // jr $ra; sw $v1, 36($a0) (delay slot, so it runs before the return)
    const std::uint32_t scaled_half = sign_extend16(m.read_u16_le(object + 40U)) << 5U;
    const std::uint32_t stored_word = m.read_u32_le(object + 32U);
    m.write_u32_le(object + 36U, stored_word + scaled_half);

    return call.finish(23U);
}

} // namespace

extern constinit const EngineFunction n3_00307500{0x00307500U, 92U, 0xa35bfb5b94c96a28ULL, "SelectBufferSlot", 23U,
                                                  n3::live<&select_buffer_slot<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00307500, select_buffer_slot)
