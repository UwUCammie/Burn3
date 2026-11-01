// 0x003fbbe0 (a0 = object): GetActiveFlag. When the byte at object+188 is set, v0 is bit 0 of the byte at
// object+232 (0 or 1). When it is clear, the original calls 0x0042b550 with a0 = object+8; that routine is the
// two-instruction stub `jr ra; daddu v0, zero, zero`, so v0 = 0. The call path never ran in the recorded game:
// call_cost returns 0 for it, so the original runs the jal itself. The implementation still reproduces it exactly
// (v0 = 0, a0 = object+8) in case it is reached. Both paths retire 12 instructions (delay slots and the stub's two
// included), so the declared cost is 12.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kActiveByte = 188U;  // lbu at object+188: nonzero selects the flag path
constexpr GuestAddress kFlagByte = 232U;    // lbu at object+232: bit 0 is the result
constexpr std::uint32_t kRetired = 12U;     // retired instructions on either path
constexpr unsigned kA0 = 4U;

template <class Context>
n3::Run get_active_flag(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    if (m.read_u8(object + kActiveByte) != 0U) {
        const std::uint32_t bit = static_cast<std::uint32_t>(m.read_u8(object + kFlagByte)) & 1U;
        call.return_value(bit != 0U ? 1U : 0U);
        return call.finish(kRetired);
    }
    // Cold path: the delay slot (addiu a0, a0, 8) runs before the jal, and the stub returns 0.
    call.cpu().write_gpr_sign_extended_32(kA0, object + 8U);
    call.return_value(0U);
    return call.finish(kRetired);
}

template <class Context>
std::uint32_t get_active_flag_cost(const Context& context) {
    const GuestAddress object = context.cpu.read_gpr_u32(kA0);
    if (!context.memory.contains(object + kActiveByte, 1U)) {
        return 0U;
    }
    if (context.memory.read_u8(object + kActiveByte) == 0U) {
        return 0U;  // the jal path never ran in the recorded game: the original runs it
    }
    if (!context.memory.contains(object + kFlagByte, 1U)) {
        return 0U;
    }
    return kRetired;
}

} // namespace

extern constinit const EngineFunction n3_003fbbe0{0x003fbbe0U, 56U, 0xc040ad8b74ae4d22ULL, "GetActiveFlag",
                                                  kRetired, n3::live<&get_active_flag<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6,
                                                  {}, &get_active_flag_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003fbbe0, get_active_flag, get_active_flag_cost)
