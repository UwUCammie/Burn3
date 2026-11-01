// 0x0032f350 (a0 = object pointer, an interior pointer such as owner+368 or base+7008; ReadField72). A getter: the
// original is `jr ra; lw v0,72(a0)`, so the load sits in the jr's delay slot and still executes on the way out. The
// word at a0+72 is returned in v0. Nothing is stored and nothing is called. The lw sign-extends into the 64-bit
// register, which GuestCall::return_value does for a 32-bit result.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFieldOffset = 72U;  // lw 72(a0)

// jr ra and its delay slot (the lw) are the only two instructions, and both retire on every path.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run read_field72(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0U);
    call.return_value(call.memory().read_u32_le(object + kFieldOffset));
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_0032f350{0x0032f350U, 8U, 0x58c9d127ff55b292ULL, "ReadField72", kCost,
                                                  n3::live<&read_field72<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0032f350, read_field72)
