// 0x0011e328 acosf: the game's libm entry for the arc cosine, a tail call. It makes a 16-byte frame, saves and
// reloads $ra, and jumps to 0x00119c20 with the stack pointer restored in the jump's delay slot, so the callee sees
// the caller's frame and f12 untouched; its result in f0 is the wrapper's.
//
// Retired: 5 (addiu, sd, ld, j, addiu) plus the callee's count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00119c20)

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kWrapper = 5U;  // 0x0011e328-0x0011e338

template <class Context>
n3::Run arc_cosine_wrapper(Context& context) {
    auto call = n3::guest_call(context);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00119c20(m); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kWrapper + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0011e328, 20U, 0x1c7d4b074cbb1255ULL, "acosf", 369U, arc_cosine_wrapper, EngineReturn::Float)
