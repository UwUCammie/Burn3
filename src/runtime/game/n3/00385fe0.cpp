// 0x00385fe0 (44 bytes, leaf): IsStateIdleOrDone. Reads the state word at +464 of
// the object in a0 (v1 keeps it, as the original leaves it). Returns 1 when the
// state is 4 or 0, otherwise 0. No stores, no calls. The retired count depends on
// the exit: 7 for state 4 (first compare matches), 9 for state 0 (second compare
// matches), 10 for any other state (neither matches; the delay slot of the final
// unconditional branch clears v0).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStateOffset = 464U;
constexpr unsigned kStateRegister = 3U;  // v1

template <class Context>
n3::Run is_state_idle_or_done(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t state = call.memory().read_u32_le(call.arg(0) + kStateOffset);
    call.cpu().write_gpr_sign_extended_32(kStateRegister, state);
    if (state == 4U) {
        call.return_value(1U);
        return call.finish(7U);
    }
    if (state == 0U) {
        call.return_value(1U);
        return call.finish(9U);
    }
    call.return_value(0U);
    return call.finish(10U);
}

} // namespace

extern constinit const EngineFunction n3_00385fe0{0x00385fe0U, 44U, 0xcbb49990fdbb25b6ULL, "IsStateIdleOrDone", 10U,
                                                  n3::live<&is_state_idle_or_done<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00385fe0, is_state_idle_or_done)
