// 0x00275ec0 NoOp: an empty function in the original, "jr $ra; nop" (2 instructions with the delay slot).
// It reads no arguments and has no effects. Only needed so ResetUsageBlock (0x001309c0) can call it as a native.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kRetired = 2U;

template <class Context>
n3::Run no_op(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00275ec0, 8U, 0x589026cfbec22f44ULL, "NoOp", 2U, no_op, EngineReturn::Nothing)
