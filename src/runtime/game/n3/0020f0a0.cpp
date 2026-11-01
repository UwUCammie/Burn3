// 0x0020f0a0 (no arguments): EmptyFunction. The original is `jr $ra` with a `nop` in its delay slot. It does
// nothing: it reads no argument registers and writes no register or memory. It returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Retired original instructions on the only path: jr $ra and its delay slot (the nop).
constexpr std::uint32_t kRetiredCount = 2U;

template <class Context>
n3::Run empty_function(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(kRetiredCount);
}

} // namespace

extern constinit const EngineFunction n3_0020f0a0{0x0020f0a0U, 8U, 0x589026cfbec22f44ULL, "EmptyFunction", 2U,
                                                  n3::live<&empty_function<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0020f0a0, empty_function)
