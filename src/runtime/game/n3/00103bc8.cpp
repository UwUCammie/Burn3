// 0x00103bc8 (a0 = object O): WaitWhileBusyFlag. Returns nothing. The original returns with v0 = 0 on every path (its loop
// exits only when the masked word is zero), so the native writes v0 = 0 too. The word at O+0 has a busy bit (0x0100) that
// this function never clears: another thread or the hardware does. When the bit is clear the function returns at once.
// When the bit is set the original spins: a counter starting at 0x01000000 counts down once per pass, and after about 16.7
// million passes the function calls 0x0010e048(0x004b1aa0) and 0x00104360(O) (both cold: the recorded game never reached
// them) and keeps spinning while the bit is set. The native covers the clear-bit path only. A set bit is declined
// (call_cost returns 0), so the original runs the spin and its cold calls. The spin is not emulated: a dry run never sees
// the bit change, so it would count 16.7 million passes just to reach the calls it declines anyway.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kBusyBit = 0x0100U;         // andi 0x0100 on the word at O+0 (lw at 0x00103be4)

// Retired instruction counts from the listing, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 11U;           // 0x00103bc8-0x00103bf0: prologue, busy test and its delay slot
constexpr std::uint32_t kEpilogueCost = 6U;         // 0x00103c20-0x00103c34: restores, jr and its delay slot

// A call on the spin path is declined, so the count a call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The retired count of a call, and whether its path is the spin (then the count is void).
struct WaitResult {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Nothing is stored to guest
// memory: the clear path only reads the busy word.
template <class Memory>
WaitResult wait_while_busy(Memory& m, const GuestAddress object) {
    if ((m.read_u32_le(object) & kBusyBit) == 0U) {
        return {kEntryCost + kEpilogueCost, false};
    }
    // The spin (see above): declined here, never run.
    return {kEntryCost, true};
}

// call_cost: the same body on a DryMemory, with the call's own $a0. A call on the spin path returns 0, and the original
// runs it.
template <class Context>
std::uint32_t wait_while_busy_cost(const Context& context) {
    const std::uint32_t object = context.cpu.read_gpr_u32(4U);
    // A load the original would fault on (misaligned or unmapped) is declined, so the original runs and faults itself.
    if ((object & 3U) != 0U || !context.memory.contains(object, 4U)) {
        return 0U;
    }
    n3::DryMemory dry{context.memory};
    const WaitResult result = wait_while_busy(dry, object);
    return result.declined ? 0U : result.retired;
}

template <class Context>
n3::Run wait_while_busy_flag(Context& context) {
    auto call = n3::guest_call(context);
    call.return_value(0U);
    return call.finish(wait_while_busy(call.memory(), call.arg(0)).retired);
}

} // namespace

extern constinit const EngineFunction n3_00103bc8{0x00103bc8U, 112U, 0xe59ae76fbd3bc0e6ULL, "WaitWhileBusyFlag",
                                                  kCeiling, n3::live<&wait_while_busy_flag<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, &wait_while_busy_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00103bc8, wait_while_busy_flag, wait_while_busy_cost)
