#pragma once
#include "takedown/render/native_model.hpp"
#include <algorithm>
namespace takedown::render {
// Burnout uses opaque car alpha as an effect/reflection mask. It must follow
// the surface's Fresnel value, independently of RGB opacity. Only glass uses
// alpha to blend the car itself with the scene behind it.
inline TakedownNative::State car_surface_state(bool glass, std::uint32_t scene_mask, bool scene_alpha) {
    TakedownNative::State state;
    state.depth_test = TakedownNative::DepthTest::GreaterEqual;
    state.depth_write = !glass;
    state.texture_alpha = glass;
    state.framebuffer_mask = glass ? 0xff000000U : scene_mask;
    state.framebuffer_alpha = !glass && scene_alpha;
    if (glass) {
        state.blend = true;
        state.blend_a = 0;
        state.blend_b = 1;
        state.blend_c = 0;
        state.blend_d = 1;
    }
    return state;
}
inline std::uint32_t car_surface_alpha(bool glass, float opacity, float fresnel) {
    return static_cast<std::uint32_t>(std::clamp((glass ? opacity : fresnel) * 128.0F, 0.0F, 128.0F));
}
}
