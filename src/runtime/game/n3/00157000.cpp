// 0x00157000 (a0 = object, a1 = record): SetSlotFlagAndValue. A leaf with no loops and three exits.
// The object's state word at +636 equal to 3 ends the call, and so does a set flag byte at
// (the pointer at +200) + 6378. Otherwise byte 1 goes to flag slot idx of the object (idx is the
// signed byte at record + 6572, flags at object + 1374), and the object's raw word at +12 is stored
// to value slot idx2 (the same byte loaded again after the flag store, since that store may overwrite
// it; words at object + 1296, four bytes per slot). Returns nothing. Retired counts 6, 12 and 19.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStateOffset = 636U;        // lw: state word of the object
constexpr std::uint32_t kStateDone = 3U;
constexpr GuestAddress kPointerOffset = 200U;      // lw: pointer at object + 200
constexpr GuestAddress kPointedFlagOffset = 6378U; // lbu: byte at pointer + 6378
constexpr GuestAddress kIndexOffset = 6572U;       // lb: signed slot index at record + 6572
constexpr GuestAddress kFlagArrayOffset = 1374U;   // sb: one flag byte per slot at object + 1374
constexpr GuestAddress kValueOffset = 12U;         // lwc1: raw word at object + 12
constexpr GuestAddress kValueArrayOffset = 1296U;  // swc1: one word per slot at object + 1296
constexpr std::uint8_t kSetFlag = 1U;
constexpr std::size_t kV1 = 3U;
constexpr std::size_t kA2 = 6U;

// Retired instruction counts of the three exits, delay slots included (see the listing).
constexpr std::uint32_t kStateDoneCost = 6U;
constexpr std::uint32_t kFlagSetCost = 12U;
constexpr std::uint32_t kStoreCost = 19U;

// lb: the signed byte at `address`, as the 32-bit value the register receives.
std::uint32_t load_slot_index(const auto& memory, const GuestAddress address) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(memory.read_u8(address))));
}

template <class Context>
n3::Run set_slot_flag_and_value(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress object = call.arg(0);
    const GuestAddress record = call.arg(1);
    const std::uint32_t state = m.read_u32_le(object + kStateOffset);
    cpu.write_gpr_sign_extended_32(kA2, state);
    if (state == kStateDone) {
        cpu.write_gpr_sign_extended_32(kV1, kStateDone);
        return call.finish(kStateDoneCost);
    }
    const GuestAddress pointer = m.read_u32_le(object + kPointerOffset);
    const std::uint32_t pointed_flag = m.read_u8(pointer + kPointedFlagOffset);
    if (pointed_flag != 0U) {
        cpu.write_gpr_sign_extended_32(kV1, pointed_flag);
        return call.finish(kFlagSetCost);
    }
    // The flag store may overwrite the index byte, so the index is loaded again after it.
    const std::uint32_t index = load_slot_index(m, record + kIndexOffset);
    m.write_u8(object + index + kFlagArrayOffset, kSetFlag);
    const std::uint32_t index_again = load_slot_index(m, record + kIndexOffset);
    const std::uint32_t value = m.read_u32_le(object + kValueOffset);
    const std::uint32_t slot_base = object + (index_again << 2U);
    m.write_u32_le(slot_base + kValueArrayOffset, value);
    cpu.write_gpr_sign_extended_32(kV1, slot_base);
    cpu.write_gpr_sign_extended_32(kA2, 1U);
    cpu.write_fpr_bits(0U, value);
    return call.finish(kStoreCost);
}

} // namespace

extern constinit const EngineFunction n3_00157000{0x00157000U, 84U, 0x6eda9b5592370dc0ULL, "SetSlotFlagAndValue", 19U,
                                                  n3::live<&set_slot_flag_and_value<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00157000, set_slot_flag_and_value)
