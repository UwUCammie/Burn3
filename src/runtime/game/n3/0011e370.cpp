// 0x0011e370 logf: a 16-byte frame is opened, $ra saved and reloaded (no net effect), and the call is a tail jump
// into the libm routine at 0x0011a5c8 (f12 = x, result in f0). Five instructions of its own plus the callee's.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0011a5c8)

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kOwnInstructions = 5U;   // addiu, sd, ld, j + delay slot
constexpr std::uint32_t kCalleeLongest = 104U;

template <class Context>
n3::Run logf_wrapper(Context& context) {
    auto call = n3::guest_call(context);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_0011a5c8(m); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kOwnInstructions + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0011e370, 20U, 0x4a5ca5adf4e7916dULL, "logf", 5U + 104U, logf_wrapper, EngineReturn::Float)
