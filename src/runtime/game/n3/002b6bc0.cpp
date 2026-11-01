// 0x002b6bc0 (a0 = P, a1 = Q): InitInputSlotState. Fills the input-slot state record P:
// P[0] = Q (word); byte P+20 = 0; bytes P+16..P+19 = 0xFF; bytes P+21..P+24 = 0xFF; words P+4, P+8,
// P+12 = 0; bytes P+25..P+27 = 0; and last byte P+28 = (word(Q+80) == 4) ? 1 : 0. Returns 1 in v0,
// which the caller tests with beq. Single path: 48 retired instructions (4 before the loop, 4 passes
// of 7 in the loop with both nops, 16 after it).
//
// The loop at 0x002b6bd0 is a do-while over a2 = 0..3 with a constant bound. It always runs exactly
// four times, so it is straight code here, with no data-dependent repeat and no call_cost.
//
// The stores keep the listing's order. The only memory read, word(Q+80), comes after every store to
// P, in case Q+80 falls inside P's record.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kRetired = 48U;
constexpr GuestAddress kLoopFirstOffset = 16U;  // bytes P+16..P+19, one per loop pass
constexpr std::uint32_t kLoopPasses = 4U;
constexpr std::uint8_t kSetByte = 0xFFU;
constexpr GuestAddress kKindOffset = 80U;       // word Q+80 is compared with kKindWanted
constexpr std::uint32_t kKindWanted = 4U;

template <class Context>
n3::Run init_input_slot_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t owner = call.arg(1);

    m.write_u32_le(record, owner);
    m.write_u8(record + 20U, 0U);
    for (std::uint32_t pass = 0U; pass < kLoopPasses; ++pass) {
        m.write_u8(record + kLoopFirstOffset + pass, kSetByte);
    }
    m.write_u8(record + 21U, kSetByte);
    m.write_u8(record + 22U, kSetByte);
    m.write_u8(record + 23U, kSetByte);
    m.write_u8(record + 24U, kSetByte);
    m.write_u32_le(record + 4U, 0U);
    m.write_u32_le(record + 8U, 0U);
    m.write_u32_le(record + 12U, 0U);
    m.write_u8(record + 25U, 0U);
    m.write_u8(record + 26U, 0U);
    m.write_u8(record + 27U, 0U);
    const auto flag = static_cast<std::uint8_t>((m.read_u32_le(owner + kKindOffset) == kKindWanted) ? 1U : 0U);
    m.write_u8(record + 28U, flag);

    // The registers the original leaves behind: v1 holds the flag, a2 the loop's final count.
    call.cpu().write_gpr_sign_extended_32(3U, flag);
    call.cpu().write_gpr_sign_extended_32(6U, kLoopPasses);
    call.return_value(1U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002b6bc0{0x002b6bc0U, 108U, 0x644722ef231db8d8ULL, "InitInputSlotState",
                                                  kRetired, n3::live<&init_input_slot_state<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b6bc0, init_input_slot_state)
