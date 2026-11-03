// 0x002222e0 (24 bytes, a tail jump): forwards to method 0x00221650 with the
// global object at 0x0067d640 as `this`. Before the jump: v0 = a0, a2 = a1,
// a1 = v0 (the old a0), and a0 = 0x0067d640 (lui 0x68 + addiu -10688, the addiu
// in the jump's delay slot). The routine never returns by itself: 0x00221650
// runs on and returns to the original caller. The native therefore sets the
// continuation PC to that routine and leaves $ra alone.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kV0 = 2U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;
constexpr std::uint64_t kObject = 0x0067d640ULL;   // lui 0x68, addiu -10688
constexpr GuestAddress kMethod = 0x00221650U;      // j target

template <class Context>
n3::Run forward_to_method(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t old_a0 = call.arg64(0);
    const std::uint64_t old_a1 = call.arg64(1);
    call.cpu().write_gpr_u64(kV0, old_a0);
    call.cpu().write_gpr_u64(kA2, old_a1);
    call.cpu().write_gpr_u64(kA1, call.cpu().read_gpr_u64(kV0));
    call.cpu().write_gpr_u64(kA0, kObject);
    call.cpu().set_pc(kMethod);
    return NativeFunctionExecution{6U, 6U};
}

} // namespace

extern constinit const EngineFunction n3_002222e0{0x002222e0U, 24U, 0x1b0171752ccbca85ULL, "ForwardToMethod221650",
                                           6U, n3::live<&forward_to_method<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002222e0, forward_to_method)
