// 0x00292690 (GetGpGlobalWord, no arguments): `jr ra` with `lw v0,-23452(gp)` in its delay slot. Returns the
// word at $gp-23452 in v0, sign-extended as lw does it. No stores, no calls, no loops. The jr and its delay slot
// always retire, so every call counts 2.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kGlobalOffset = -23452;  // lw -23452(gp)

// The jr and its delay slot (the lw) always retire.
constexpr std::uint32_t kCost = 2U;

template <class Context>
n3::Run get_gp_global_word(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress word_address =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kGlobalOffset);
    call.return_value(call.memory().read_u32_le(word_address));
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00292690{0x00292690U, 8U, 0x3a944cd0a62999ffULL, "GetGpGlobalWord", kCost,
                                                  n3::live<&get_gp_global_word<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00292690, get_gp_global_word)
