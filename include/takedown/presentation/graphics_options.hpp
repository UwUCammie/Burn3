#pragma once

// Pure policy shared by the options menu, the frame-rate limiter and the
// Vulkan renderer bridge: how the stored graphics settings translate into
// renderer and frame-limit decisions. No device, guest or clock access.

#include "takedown/config/app_settings.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace takedown::presentation {

// The compatibility grid supplies world resolution only. A native spatial
// edge pass follows the world resolve, independently of that grid and UI.
struct SupersamplingPlan {
    std::uint8_t samples{1U};
    std::uint8_t resolution_scale{1U};
    std::uint8_t spatial_aa{0U};

    bool operator==(const SupersamplingPlan&) const = default;
};

// All AA quality levels work at every supported world resolution.
[[nodiscard]] SupersamplingPlan supersampling_plan(std::uint32_t render_scale,
                                                   config::AntiAliasing anti_aliasing) noexcept;

// Anti-aliasing level that actually takes effect for the combination; the
// menus show it when it differs from the stored choice.
[[nodiscard]] config::AntiAliasing effective_anti_aliasing(
    std::uint32_t render_scale, config::AntiAliasing anti_aliasing) noexcept;

// "100% (640x448)": the render scale as the menus show it, with the internal
// resolution of the game's 640x448 picture at that scale.
[[nodiscard]] std::string render_scale_text(std::uint32_t render_scale);

// Panini projection of one driving frame, as the renderer gets it: the
// tangent of half the picture's horizontal angle in thousandths, the strength
// d in hundredths and the half width of the output in Panini units in
// thousandths (panini_half_width). `game_fov` is the angle the game asked for
// (60 on the start line and in crashes, rising to 90 as the car boosts) and
// `fov` the one it got after the FIELD OF VIEW setting; `aspect` is the
// picture's width over height. The strength is full (d = 1, the classic
// Panini) up to the game's 60 and eases to 0.6 at its 90, so boosting still
// widens the view with some of the fish-eye stretch.
struct PaniniParameters {
    std::uint16_t tangent{0U};
    std::uint8_t strength{0U};
    std::uint16_t extent{0U};

    bool operator==(const PaniniParameters&) const = default;
};
inline constexpr float kPaniniFullStrength = 1.0F;
inline constexpr float kPaniniBoostStrength = 0.6F;
[[nodiscard]] PaniniParameters panini_parameters(float game_fov, float fov,
                                                 float aspect) noexcept;
// The Panini strength d for the game's angle (see above).
[[nodiscard]] float panini_strength(float game_fov) noexcept;
// Half the width of the Panini picture of a frame whose half width is
// `tangent` (tangent units) and whose width over height is `aspect`, in
// Panini units: the widest picture whose every point maps inside the
// rendered frame. On the middle row that is the frame's full width; where the
// Panini curves the frame's top and bottom edges inwards it is less, so
// nothing outside the frame is ever shown. The middle is magnified by tangent
// over this. Without Panini (strength 0) it is the tangent itself.
[[nodiscard]] float panini_half_width(float tangent, float strength, float aspect) noexcept;
// The view direction of a Panini point (px, py) with strength d, as the
// rectilinear frame's tangent-plane coordinates (Sharpless' inverse).
struct PaniniSource {
    float x{0.0F};
    float y{0.0F};
};
[[nodiscard]] PaniniSource panini_source(float px, float py, float strength) noexcept;

// Frame-rate cap in force right now. In races it is the frame-rate limit; in
// menus and videos the menu limit also applies (the lower of the two wins).
// nullopt means unlimited.
[[nodiscard]] std::optional<std::uint32_t>
effective_frame_rate_limit(const config::GraphicsSettings& settings,
                           bool menus_or_video) noexcept;

} // namespace takedown::presentation
