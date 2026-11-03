// 0x002bedb0 (8 bytes): `j 0x00272ea0` with `daddu $t0, $zero, $zero` in its delay slot. The entry of
// BuildClippedPolygonVu0 for a polygon without the scaled last point: it clears the optional output quadword (t0) and
// jumps. A tail jump, so there is no frame and ra is unchanged: the callee returns straight to the caller with its own
// v0 (the vertex count). a0..a3 and every other register reach the callee as the caller left them.
//
// Retired: 2 (the j and its delay slot) plus the callee's count.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00272ea0)

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kTailJump = 2U;     // 0x002bedb0-0x002bedb4: the j and its delay slot
constexpr std::uint32_t kCeiling = 20000U;  // the callee loops over its input points

template <class Context>
n3::Run build_clipped_polygon_unscaled(Context& context) {
    auto call = n3::guest_call(context);
    call.set_arg64(4U, 0U);  // daddu $t0, $zero, $zero: no scaled output quadword (the whole register, 64-bit)
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00272ea0(m); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kTailJump + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002bedb0, 8U, 0xa1f7fca3a4083990ULL, "BuildClippedPolygonVu0Unscaled", kCeiling, build_clipped_polygon_unscaled,
         EngineReturn::Integer)
