// 0x002b51e0 (leaf, no arguments): returns the 32-bit word at $gp - 22908 in v0.
// The load is in the delay slot of `jr $ra`, so it always runs and the function
// retires exactly 2 instructions on its only path.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kGpOffset = -22908;  // $gp - 22908: global word returned in v0

template <class Context>
n3::Run get_gp_global_ptr(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress address = call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kGpOffset);
    call.return_value(call.memory().read_u32_le(address));
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_002b51e0{0x002b51e0U, 8U, 0xc8acffd7c5c3958dULL, "GetGpGlobalPtr", 2U,
                                           n3::live<&get_gp_global_ptr<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b51e0, get_gp_global_ptr)
