#include "takedown/presentation/graphics_options.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace takedown::presentation {

namespace {

// Spatial edge-pass count for each anti-aliasing level; Off has none.
[[nodiscard]] std::uint8_t spatial_passes(const config::AntiAliasing anti_aliasing) noexcept {
    if (anti_aliasing != config::AntiAliasing::Off) {
        if (static_cast<std::uint8_t>(static_cast<std::uint8_t>(anti_aliasing) + 255U) <= 3U) {
            // Spatial levels 1..4 map to 2, 4, 8 and 16 passes.
            return std::array<std::uint8_t, 4>{2U, 4U, 8U, 16U}[static_cast<std::uint8_t>(static_cast<std::uint8_t>(anti_aliasing) + 255U)];
        }
        return 1U;
    }
    return 0U;
}

[[nodiscard]] float clamp_to_zero(const float value) noexcept {
    return value < 0.0F ? 0.0F : value;
}

} // namespace

SupersamplingPlan supersampling_plan(const std::uint32_t render_scale,
                                     const config::AntiAliasing anti_aliasing) noexcept {
    return SupersamplingPlan{
        .samples = static_cast<std::uint8_t>(render_scale > 4U ? 64U
                                             : render_scale > 2U ? 16U
                                             : render_scale == 2U ? 4U
                                                                  : 1U),
        .resolution_scale =
            static_cast<std::uint8_t>(std::min(std::max(render_scale, 1U), config::kMaximumRenderScale)),
        .spatial_aa = spatial_passes(anti_aliasing),
    };
}

config::AntiAliasing effective_anti_aliasing(const std::uint32_t,
                                             const config::AntiAliasing anti_aliasing) noexcept {
    return anti_aliasing;
}

std::string render_scale_text(const std::uint32_t render_scale) {
    return std::to_string(render_scale * 100) + "% (" + std::to_string(render_scale * 640) + "x" +
           std::to_string(render_scale * 448) + ")";
}

std::optional<std::uint32_t>
effective_frame_rate_limit(const config::GraphicsSettings& settings, const bool menus_or_video) noexcept {
    std::optional<std::uint32_t> limit = settings.presentation_fps_limit;
    if (menus_or_video && settings.menu_fps_limit) {
        limit = limit ? std::min(*limit, *settings.menu_fps_limit) : *settings.menu_fps_limit;
    }
    return limit;
}

float panini_strength(const float game_fov) noexcept {
    return kPaniniFullStrength +
           std::min(clamp_to_zero((game_fov - 60.0F) / 30.0F), 1.0F) * (kPaniniBoostStrength - kPaniniFullStrength);
}

PaniniSource panini_source(const float px, const float py, const float strength) noexcept {
    const float k = px * px / ((strength + 1.0F) * (strength + 1.0F));
    const float clon = (std::sqrt(clamp_to_zero(k * k * strength * strength -
                                                (k + 1.0F) * (strength * k * strength - 1.0F))) -
                         strength * k) /
                       (k + 1.0F);
    const float s = (strength + 1.0F) / (strength + clon);
    const float x_angle = std::atan2(px, clon * s);
    const float y_angle = std::atan2(py, s);
    return PaniniSource{
        .x = std::tan(x_angle),
        .y = std::tan(y_angle) / std::cos(x_angle),
    };
}

float panini_half_width(const float tangent, const float strength, const float aspect) noexcept {
    if (!(strength > 0.0F)) {
        return tangent;
    }
    const float theta = std::atan(tangent);
    const float full = (strength + 1.0F) * std::sin(theta) / (strength + std::cos(theta));
    if (!(aspect > 0.0F)) {
        return full;
    }
    const float vertical = tangent / aspect;
    // Worst ratio of a boundary point's view tangents to the frame's: above 1
    // the picture reaches outside the rendered frame.
    const auto reach = [&](float extent) {
        float worst = 0.0F;
        for (int index = 0; index < 33; ++index) {
            const float along = static_cast<float>(index) * 3.125e-2F;
            for (const auto& edge : {std::pair<float, float>{extent * along, extent / aspect},
                                     std::pair<float, float>{extent, extent * along / aspect}}) {
                const PaniniSource source = panini_source(edge.first, edge.second, strength);
                worst = std::max({worst, std::abs(source.x) / tangent, std::abs(source.y) / vertical});
            }
        }
        return worst;
    };
    if (reach(full) <= 1.0F) {
        return full;
    }
    // Bisection for the widest extent that stays inside the frame.
    float inside = 0.0F;
    float outside = full;
    for (int step = 0; step < 24; ++step) {
        const float middle = (inside + outside) * 0.5F;
        if (reach(middle) <= 1.0F) {
            inside = middle;
        } else {
            outside = middle;
        }
    }
    return inside;
}

PaniniParameters panini_parameters(const float game_fov, const float fov, const float aspect) noexcept {
    if (!(fov > 1.0F && fov < 170.0F && aspect > 0.0F)) {
        return PaniniParameters{};
    }
    const float strength = panini_strength(game_fov);
    const float tangent = std::tan(fov * 3.14159265F / 360.0F);
    return PaniniParameters{
        .tangent = static_cast<std::uint16_t>(std::min(std::max(tangent * 1000.0F + 0.5F, 1.0F), 65535.0F)),
        .strength = static_cast<std::uint8_t>(std::min(strength * 100.0F + 0.5F, 100.0F)),
        .extent = static_cast<std::uint16_t>(
            std::min(std::max(panini_half_width(tangent, strength, aspect) * 1000.0F + 0.5F, 1.0F), 65535.0F)),
    };
}

} // namespace takedown::presentation
