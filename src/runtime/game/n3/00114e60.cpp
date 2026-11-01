// 0x00114e60 (no arguments): DisableInterruptsReturnPrev, the EE kernel's DIntr wrapper. Returns in v0 whether
// Status.EIE was set on entry (1: the function clears it with DI) or already clear (0: nothing changes). a0 and
// v1 end with the values the original leaves in them.
//
// The original is `di; sync.p; mfc0 Status; and EIE; bne` looped until EIE reads clear. Where DI may change EIE
// (R5900State::execute_cop0_di: kernel mode, or EXL/ERL/EDI set) one pass clears it, and nothing in the loop sets
// it again, so the loop runs exactly once and is written as straight code. Where DI cannot change EIE the loop
// never ends; call_cost hands that input to the original, so the native never runs it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr unsigned kCop0Status = 12U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr std::uint32_t kStatusEie = 0x00010000U;
// The masks and predicate of R5900State::execute_cop0_di (src/guest/r5900_state.cpp).
constexpr std::uint32_t kStatusEiDiAnyModeMask = 0x00020006U;
constexpr std::uint32_t kStatusKsuMask = 0x00000018U;

// Retired instruction counts from the listing, delay slots included. EIE clear: mfc0, lui, and, beq (taken),
// sltu, then daddu, daddu, jr, nop. EIE set: mfc0, lui, and, beq, sltu, nop, one loop pass (di through the
// bne's delay slot, 9 instructions), then jr and its daddu.
constexpr std::uint32_t kClearCost = 9U;
constexpr std::uint32_t kSetCost = 17U;

bool di_changes_eie(const std::uint32_t status) {
    return (status & kStatusEiDiAnyModeMask) != 0U || (status & kStatusKsuMask) == 0U;
}

template <class Context>
n3::Run disable_interrupts_return_prev(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    if ((cpu.read_cop0(kCop0Status) & kStatusEie) == 0U) {
        cpu.write_gpr_u64(kA0, 0U);
        cpu.write_gpr_u64(kV1, 0U);
        call.return_value(0U);
        return call.finish(kClearCost);
    }
    cpu.execute_cop0_di();
    cpu.write_gpr_u64(kA0, 1U);
    cpu.write_gpr_u64(kV1, kStatusEie);
    call.return_value(1U);
    return call.finish(kSetCost);
}

template <class Context>
std::uint32_t disable_interrupts_return_prev_cost(const Context& context) {
    const std::uint32_t status = context.cpu.read_cop0(kCop0Status);
    if ((status & kStatusEie) == 0U) {
        return kClearCost;
    }
    // DI would be ignored here, so the original loop would never end: the original runs this call.
    if (!di_changes_eie(status)) {
        return 0U;
    }
    return kSetCost;
}

} // namespace

extern constinit const EngineFunction n3_00114e60{0x00114e60U, 84U, 0x2f504edf88d4b495ULL,
                                                  "DisableInterruptsReturnPrev", kSetCost,
                                                  n3::live<&disable_interrupts_return_prev<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &disable_interrupts_return_prev_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00114e60, disable_interrupts_return_prev, disable_interrupts_return_prev_cost)
