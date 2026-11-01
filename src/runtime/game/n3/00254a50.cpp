// 0x00254a50 (ResetPadSelectState): a leaf of five instructions that stores the low bytes of
// a0 and a1 to two $gp-relative globals and clears two $gp-relative words. It returns nothing.
// The last store is the delay slot of jr $ra, so the single path retires 5 instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// $gp-relative displacements (signed 16-bit, as the listing encodes them).
constexpr std::int32_t kPadByteAOffset = -23696;   // sb $a0 (low 8 bits only)
constexpr std::int32_t kPadByteBOffset = -23692;   // sb $a1 (low 8 bits only)
constexpr std::int32_t kZeroWordAOffset = -23712;  // sw $zero
constexpr std::int32_t kZeroWordBOffset = -23708;  // sw $zero (jr $ra delay slot)

template <class Context>
n3::Run reset_pad_select_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t a0 = call.arg(0U);
    const std::uint32_t a1 = call.arg(1U);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    m.write_u8(gp + static_cast<GuestAddress>(kPadByteAOffset), static_cast<std::uint8_t>(a0));  // 0x00254a50 sb $a0
    m.write_u8(gp + static_cast<GuestAddress>(kPadByteBOffset), static_cast<std::uint8_t>(a1));  // 0x00254a54 sb $a1
    m.write_u32_le(gp + static_cast<GuestAddress>(kZeroWordAOffset), 0U);                        // 0x00254a58 sw $zero
    m.write_u32_le(gp + static_cast<GuestAddress>(kZeroWordBOffset), 0U);  // 0x00254a60 sw $zero (jr delay slot)
    return call.finish(5U);
}

} // namespace

extern constinit const EngineFunction n3_00254a50{0x00254a50U, 20U, 0x1b31b5e6394e883dULL, "ResetPadSelectState", 5U,
                                                  n3::live<&reset_pad_select_state<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00254a50, reset_pad_select_state)
