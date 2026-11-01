// 0x001042f0 (a0 = object O, a1 = mode): ReadBusyFlagOrWait. Mode 1 (a1 == 1, compared as a full 64-bit register) returns
// bit 8 of the word at O+0 as 0 or 1: the busy bit that WaitWhileBusyFlag (0x00103bc8) spins on. Any other mode calls
// WaitWhileBusyFlag(O) and returns 0. The callee declines while the busy bit is set, and then the whole call is left to
// the original. The store of $ra into the frame (0x001042fc) is not reproduced: the check ignores the frame, and $ra
// keeps the value the reload at 0x0010431c would give back.
//
//   001042f0  addiu $sp, $sp, -16        frame of 16 bytes
//   001042f4  addiu $v0, $zero, 1
//   001042f8  bne $a1, $v0, 0x00104310   64-bit compare: mode 1 falls through
//   001042fc  sd $ra, 0($sp)             (delay slot)
//   00104300  lw $v0, 0($a0)             mode 1: the word at O
//   00104304  srl $v0, $v0, 8
//   00104308  beq $zero, $zero, 0x0010431c
//   0010430c  andi $v0, $v0, 0x0001      (delay slot): bit 8 of the word
//   00104310  jal 0x00103bc8             other modes: WaitWhileBusyFlag(O)
//   00104314  nop                        (delay slot)
//   00104318  daddu $v0, $zero, $zero    v0 = 0
//   0010431c  ld $ra, 0($sp)
//   00104320  jr $ra
//   00104324  addiu $sp, $sp, 16         (delay slot)

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00103bc8)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint64_t kReadMode = 1U;             // the 64-bit compare in the bne at 0x001042f8
constexpr std::uint32_t kBusyShift = 8U;            // srl 8, then andi 1: bit 8 of the word
constexpr std::uint32_t kFrameBytes = 16U;          // the addiu $sp, $sp, -16 at the entry

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 4U;         // 0x001042f0-0x001042fc: frame, v0 = 1, the bne and its delay slot
constexpr std::uint32_t kReadCost = 4U;             // 0x00104300-0x0010430c: lw, srl, beq and its delay slot
constexpr std::uint32_t kReadReturnCost = 3U;       // 0x0010431c-0x00104324: ld, jr and its delay slot
constexpr std::uint32_t kCallCost = 2U;             // 0x00104310-0x00104314: jal and its delay slot
constexpr std::uint32_t kWaitReturnCost = 4U;       // 0x00104318-0x00104324: daddu, ld, jr and its delay slot

// A call the callee declines is left to the original, and the count a call reports is always the original's exact
// count (the dry run returns 0 above this ceiling). This function has no loop, so the ceiling is never reached.
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run read_busy_flag_or_wait(Context& context) {
    auto call = n3::guest_call(context);
    if (call.arg64(1U) == kReadMode) {
        const GuestAddress object = call.arg(0);
        // A load the original would fault on (misaligned or unmapped) is declined, so the original runs and faults itself.
        if ((object & 3U) != 0U || !call.memory().contains(object, 4U)) {
            return call.decline();
        }
        call.return_value((call.memory().read_u32_le(object) >> kBusyShift) & 1U);
        return call.finish(kPrologueCost + kReadCost + kReadReturnCost);
    }
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00103bc8(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    call.return_value(0U);
    return call.finish(kPrologueCost + kCallCost + callee.retired + kWaitReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001042f0, 56U, 0x58c0ea0e50ab46e3ULL, "ReadBusyFlagOrWait", kCeiling, read_busy_flag_or_wait,
         EngineReturn::Integer)
