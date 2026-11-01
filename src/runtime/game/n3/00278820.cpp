// 0x00278820 (HasNonZeroField200): a predicate on the object at a0. Returns
// v0 = 1 when the 32-bit word at a0 + 200 is nonzero, else 0 (lw, then
// sltu v0, $zero, v0 in the jr delay slot). Leaf: no stores, no calls, no
// loops, so all 3 instructions retire on every path.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFieldOffset = 200U;

template <class Context>
n3::Run has_nonzero_field_200(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress object = call.arg(0);
    const std::uint32_t field = call.memory().read_u32_le(object + kFieldOffset);
    call.return_value(field != 0U ? 1U : 0U);
    return call.finish(3U);
}

} // namespace

extern constinit const EngineFunction n3_00278820{0x00278820U, 12U, 0x89c139e7ddd78a8fULL, "HasNonZeroField200", 3U,
                                                  n3::live<&has_nonzero_field_200<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00278820, has_nonzero_field_200)
