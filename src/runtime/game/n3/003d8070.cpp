// 0x003d8070 (a0 = object O, a2 = mode M). If the word at O+76 is nonzero, the original first calls 0x003d5510 with
// a0 = O+64 and a1 = 0. That routine has no native, so such a call is declined and the original runs it whole. Then,
// if M is 2 (the original compares all 64 bits of $s0 with 2), O+52 gets the float 0.5 and O+56 gets 0. The function
// returns nothing; its only register writes are temporaries (v1), which this native does not make. Its saves of ra, s0
// and s1 are stack traffic below the entry $sp and are not reproduced.
//
//   003d8070  addiu $sp, $sp, -48      frame, ra/s0/s1 saved (stack only)
//   003d8080  daddu $s1, $a0, $zero    lw $v1, 76($a0); bne $v1, $zero -> 003d80a8 (the call)
//   003d8090  daddu $s0, $a2, $zero    addiu $v1, $zero, 2; beq $s0, $v1 -> 003d80bc (M == 2)
//   003d80bc  lui $v1, 0x3f00          sw $v1, 52($s1); sw $zero, 56($s1)
//   003d80a0  beq $zero, $zero -> 003d80cc
//   003d80cc  lq $s1, lq $s0, jr $ra   restore, pop the frame, return

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagAt = 76U;             // lw $v1, 76($a0): nonzero sends the call to 0x003d5510
constexpr GuestAddress kHalfAt = 52U;             // sw $v1, 52($s1)
constexpr GuestAddress kZeroAt = 56U;             // sw $zero, 56($s1)
constexpr std::uint32_t kHalfBits = 0x3f000000U;  // lui $v1, 0x3f00: 0.5f
constexpr std::uint64_t kModeTwo = 2U;            // addiu $v1, $zero, 2, compared with all of $s0 by beq

// Retired counts from the listing, delay slots included: 0x003d8070-0x003d8090 (the flag test), 0x003d8094-0x003d809c
// (the mode test), 0x003d80bc-0x003d80c8 (the two stores), and 0x003d80a0-0x003d80a4 with 0x003d80cc-0x003d80d8
// (the exit).
constexpr std::uint32_t kEntryCost = 9U;
constexpr std::uint32_t kModeTestCost = 3U;
constexpr std::uint32_t kStoresCost = 4U;
constexpr std::uint32_t kExitCost = 6U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run set_mode_two_half_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    // A misaligned object, or one with an unmapped word among +52..+79 (the words this function can touch), is declined
    // before any store, so the original runs the call and handles any fault itself.
    if ((object & 3U) != 0U || !m.contains(object + kHalfAt, 28U)) {
        return call.decline();
    }
    if (m.read_u32_le(object + kFlagAt) != 0U) {
        // The original calls 0x003d5510 (object + 64, 0) here. It has no native: the whole call goes back to the original.
        return call.decline();
    }
    if (call.arg64(2) != kModeTwo) {
        return call.finish(kEntryCost + kModeTestCost + kExitCost);
    }
    m.write_u32_le(object + kHalfAt, kHalfBits);
    m.write_u32_le(object + kZeroAt, 0U);
    return call.finish(kEntryCost + kModeTestCost + kStoresCost + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003d8070, 108U, 0xee90aa84449ed58bULL, "SetModeTwoHalfState", kCeiling, set_mode_two_half_state,
         EngineReturn::Nothing)
