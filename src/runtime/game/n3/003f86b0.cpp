// 0x003f86b0 LinkedObjectIsStateSix: the object in a0 keeps a pointer at +352. A null pointer gives 0. Otherwise
// IsObjectStateSix (0x00386010) looks at that object, and this returns 1 only when its answer is nonzero, else 0.
//
//   003f86b0  addiu $sp, $sp, -16        frame of 16 bytes (the callee runs under it, as in the original)
//   003f86b4  lw $a0, 352($a0)           the linked object pointer (0 when none)
//   003f86b8  beq $a0, $zero, 0x003f86d8
//   003f86bc  sd $ra, 0($sp)             (delay slot) saves ra; this native never changes ra, so it stores nothing
//   003f86c0  jal 0x00386010
//   003f86c4  nop
//   003f86c8  beq $v0, $zero, 0x003f86d8
//   003f86cc  nop
//   003f86d0  beq $zero, $zero, 0x003f86dc
//   003f86d4  addiu $v0, $zero, 1        (delay slot)
//   003f86d8  daddu $v0, $zero, $zero
//   003f86dc  ld $ra, 0($sp)
//   003f86e0  jr $ra
//   003f86e4  addiu $sp, $sp, 16
//
// Retired instructions: null pointer 4 + 1 + 3 = 8; the callee answers 0: 4 + 4 + 1 + 3 + 4 = 16; the callee answers
// nonzero: 4 + 4 + 2 + 3 + 4 = 17 (the callee always retires its own 4).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00386010)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lw 352(a0): the pointer the object keeps at +352.
constexpr GuestAddress kLinkOffset = 352U;
// addiu $sp, $sp, -16: the callee runs with the caller's frame in place.
constexpr std::uint32_t kFrameBytes = 16U;
constexpr std::uint32_t kCeiling = 20000U;

// Instructions the original retires on each path, the callee's own count not included.
constexpr std::uint32_t kEntryInstructions = 4U;        // addiu, lw, beq, and sd in the delay slot
constexpr std::uint32_t kCallBlockInstructions = 4U;    // jal, its nop, beq $v0 and its nop (always 4)
constexpr std::uint32_t kZeroResultInstructions = 1U;   // daddu $v0, $zero, $zero
constexpr std::uint32_t kOneResultInstructions = 2U;    // beq $zero, $zero and addiu $v0, $zero, 1
constexpr std::uint32_t kReturnInstructions = 3U;       // ld $ra, jr $ra and addiu $sp in the delay slot

template <class Context>
n3::Run linked_object_is_state_six(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress linked = call.memory().read_u32_le(call.arg(0) + kLinkOffset);
    call.set_arg(0, linked);
    if (linked == 0U) {
        call.return_value(0U);
        return call.finish(kEntryInstructions + kZeroResultInstructions + kReturnInstructions);
    }
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00386010(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    // The original tests all 64 bits of v0 (beq $v0, $zero), so the test does too.
    if (call.result64() == 0U) {
        call.return_value(0U);
        return call.finish(kEntryInstructions + kCallBlockInstructions + kZeroResultInstructions +
                           kReturnInstructions + callee.retired);
    }
    call.return_value(1U);
    return call.finish(kEntryInstructions + kCallBlockInstructions + kOneResultInstructions + kReturnInstructions +
                       callee.retired);
}

} // namespace

} // namespace takedown::runtime

N3_ENTRY(003f86b0, 56U, 0xd181c13d3a914544ULL, "LinkedObjectIsStateSix", kCeiling, linked_object_is_state_six,
         EngineReturn::Integer)
