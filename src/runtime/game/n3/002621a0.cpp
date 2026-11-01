// 0x002621a0 (a0 = emitter context E, a1 = view object V): emits the view's sprite records when the flags byte at
// V + 88 is set. A nonzero byte calls EmitViewSpriteRecords (0x00256690) as (E, V, mode 0) and returns; a clear byte
// returns at once. Listing (dis.sh), delay slots in brackets:
//
//   002621a0  addiu $sp, $sp, -32            frame of 32 bytes
//   002621a4  lbu $v1, 88($a1)
//   002621a8  bne $v1, $zero, 0x002621b8     [sd $ra, 0($sp)]
//   002621b0  beq $zero, $zero, 0x002621c8   [ld $ra, 0($sp)]
//   002621b8  jal 0x00256690                 [daddu $a2, $zero, $zero]
//   002621c0  beq $zero, $zero, 0x002621b0   [nop]
//   002621c8  jr $ra                         [addiu $sp, $sp, 32]
//
// The frame save and reload only preserve ra, which the native never changes, so they are not reproduced. The callee's
// frame lies below this one, so it never reads the saved ra.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00256690)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kV1 = 3U;
constexpr GuestAddress kFlagsByte = 88U;      // V + 88
constexpr std::uint32_t kFrameBytes = 32U;
constexpr std::uint32_t kClearPathCost = 8U;  // 0x002621a0..0x002621cc with the byte clear
constexpr std::uint32_t kCallPathCost = 12U;  // the same listing on the call path, the callee's count not included
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run emit_view_sprites_if_flagged(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress flags_address = call.arg(1) + kFlagsByte;
    if (!call.memory().contains(flags_address, 1U)) {
        return call.decline();  // the original's lbu faults here, so the original runs
    }
    const std::uint8_t flags = call.memory().read_u8(flags_address);
    call.cpu().write_gpr_u64(kV1, flags);  // lbu zero-extends the byte into v1
    if (flags == 0U) {
        return call.finish(kClearPathCost);
    }
    call.set_arg64(2, 0U);  // daddu a2,zero,zero (the jal's delay slot); a0 and a1 pass through unchanged
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00256690(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kCallPathCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002621a0, 48U, 0x8282b75e6b2af571ULL, "EmitViewSpritesIfFlagged", kCeiling, emit_view_sprites_if_flagged,
         EngineReturn::Nothing)
