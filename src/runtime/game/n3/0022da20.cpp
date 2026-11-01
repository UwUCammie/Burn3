// 0x0022da20 (a0 = record): ResetSlotLinkRecord. Stores constants only: nothing is read and there are no
// branches. Byte +0 = 0, words +4 and +8 = 0xFFFFFFFF, byte +1 = 0xFF, halfword +12 = 0xFFFF, byte +3 = 0xFF
// (a2 = -1, low byte), and byte +2 = 0 in the return's delay slot. Nothing is returned: v0 is not written and
// a1, a2, v1 are scratch.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Every instruction of the listing retires on the only path, the jr's delay slot included.
constexpr std::uint32_t kRetired = 11U;

template <class Context>
n3::Run reset_slot_link_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    m.write_u8(record + 0U, 0U);
    m.write_u32_le(record + 4U, 0xFFFFFFFFU);
    m.write_u32_le(record + 8U, 0xFFFFFFFFU);
    m.write_u8(record + 1U, 0xFFU);
    m.write_u16_le(record + 12U, 0xFFFFU);
    m.write_u8(record + 3U, 0xFFU);
    m.write_u8(record + 2U, 0U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0022da20{0x0022da20U, 44U, 0xe45aa4971f7789b7ULL, "ResetSlotLinkRecord", 11U,
                                                  n3::live<&reset_slot_link_record<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0022da20, reset_slot_link_record)
