// 0x00290ce0 (GetGpWord23540): a getter for the 32-bit word at $gp-23540. The
// original is `jr $ra` with `lw $v0,-23540($gp)` in its delay slot, so the load
// always runs and v0 holds the word on return. lw sign-extends to 64 bits, which
// return_value does too. No stores, no calls, no loops: 2 retired instructions
// (jr and its delay slot) on every path.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// The lw offset from $gp (negative: the word lies below the global pointer).
constexpr std::int32_t kGpOffset = -23540;

template <class Context>
n3::Run get_gp_word_23540(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress word = gp + static_cast<GuestAddress>(kGpOffset);
    call.return_value(call.memory().read_u32_le(word));
    return call.finish(2U);
}

} // namespace

extern constinit const EngineFunction n3_00290ce0{0x00290ce0U, 8U, 0xb9e5f9ef52cd0717ULL, "GetGpWord23540", 2U,
                                                  n3::live<&get_gp_word_23540<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00290ce0, get_gp_word_23540)
