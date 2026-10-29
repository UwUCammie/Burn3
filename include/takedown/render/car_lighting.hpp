#pragma once
#include <algorithm>
#include <cmath>

namespace takedown::render {
inline float car_diffuse_light(float irradiance) {
    if (!std::isfinite(irradiance) || irradiance <= 0.0F) return 0.0F;
    // Native albedo has a full colour range. Compress the game's unbounded
    // irradiance with a smooth shoulder before encoding into its display
    // pipeline, rather than clipping bright panels to featureless white.
    return irradiance / (1.0F + irradiance);
}
// The original effect alpha remains a separate occlusion/Fresnel channel.
// Native material reflections interpolate with the lit body instead of adding
// a second light source on top of it. Their illumination has no minimum floor.
inline float car_reflection_weight(bool glass, bool paint, bool light, float fresnel) {
    const float grazing = glass ? 0.70F : paint ? 0.20F : light ? 0.08F : 0.04F;
    const float facing = glass || paint ? 0.04F : 0.02F;
    return facing + (grazing - facing) * std::pow(std::clamp(fresnel, 0.0F, 1.0F), 2.5F);
}
}
