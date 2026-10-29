#include "takedown/ui/options_menu.hpp"

#include "takedown/presentation/graphics_options.hpp"
#include "takedown/presentation/stats_overlay.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::ui {

namespace {

constexpr std::array<OptionsMenuItem, 16U> kItems{
    OptionsMenuItem::DisplayMode,       OptionsMenuItem::AspectRatio,
    OptionsMenuItem::RenderResolution,  OptionsMenuItem::AntiAliasing,
    OptionsMenuItem::Upscaling,         OptionsMenuItem::FsrSharpness,
    OptionsMenuItem::VSync,             OptionsMenuItem::FrameRateLimit,
    OptionsMenuItem::MenuFrameRateLimit, OptionsMenuItem::FrameTiming,
    OptionsMenuItem::MotionBlur,        OptionsMenuItem::Bloom,
    OptionsMenuItem::RenderDistance,    OptionsMenuItem::ModelQuality,
    OptionsMenuItem::RestoreDefaults,   OptionsMenuItem::Close,
};

std::string_view fsr_preset_name(const config::FsrPreset preset) {
    switch (preset) {
    case config::FsrPreset::Off:
        return "Off";
    case config::FsrPreset::NativeAa:
        return "FSR Native AA";
    case config::FsrPreset::Quality:
        return "FSR Quality";
    case config::FsrPreset::Balanced:
        return "FSR Balanced";
    case config::FsrPreset::Performance:
        return "FSR Performance";
    case config::FsrPreset::UltraPerformance:
        return "FSR Ultra Performance";
    }
    return "Off";
}

// Moves `index` by `delta` inside [0, count), wrapping at both ends.
std::size_t wrapped_index(std::size_t index, int delta, std::size_t count) {
    auto next = static_cast<long long>(delta) + static_cast<long long>(index);
    next %= static_cast<long long>(count);
    if (next < 0) {
        next += static_cast<long long>(count);
    }
    return static_cast<std::size_t>(next);
}

// Next value of a closed list after `delta` steps from `current`.
template <typename T, std::size_t N>
T cycle(T current, const std::array<T, N>& values, int delta) {
    const auto found = std::find(values.begin(), values.end(), current);
    const std::size_t index =
        found == values.end() ? 0U : static_cast<std::size_t>(found - values.begin());
    return values[wrapped_index(index, delta, N)];
}

// Steps a frame-rate limit through Unlimited, 30, 60 ... 500 and back around.
std::optional<std::uint32_t> step_limit(const std::optional<std::uint32_t> limit, const int delta) {
    std::size_t index = 0U; // 0 is Unlimited, n is kOptionsMenuFrameRateLimits[n - 1]
    if (limit.has_value()) {
        const auto found = std::lower_bound(kOptionsMenuFrameRateLimits.begin(),
                                            kOptionsMenuFrameRateLimits.end(), *limit);
        if (found != kOptionsMenuFrameRateLimits.end() && *found == *limit) {
            index = static_cast<std::size_t>(found - kOptionsMenuFrameRateLimits.begin()) + 1U;
        } else {
            // Hand-edited value: move to the nearest listed neighbour.
            if (delta > 0) {
                return found == kOptionsMenuFrameRateLimits.end()
                           ? std::nullopt
                           : std::optional<std::uint32_t>{*found};
            }
            return found == kOptionsMenuFrameRateLimits.begin()
                       ? std::nullopt
                       : std::optional<std::uint32_t>{*(found - 1)};
        }
    }
    const std::size_t slot = wrapped_index(index, delta, kOptionsMenuFrameRateLimits.size() + 1U);
    if (slot == 0U) {
        return std::nullopt;
    }
    return kOptionsMenuFrameRateLimits[slot - 1U];
}

std::string limit_text(const std::optional<std::uint32_t> limit) {
    return limit.has_value() ? std::to_string(*limit) + " FPS" : std::string{"Unlimited"};
}

std::string_view display_mode_name(config::DisplayMode mode) {
    switch (mode) {
    case config::DisplayMode::Windowed:
        return "Windowed";
    case config::DisplayMode::Borderless:
        return "Borderless";
    case config::DisplayMode::Fullscreen:
        return "Fullscreen";
    }
    return "Windowed";
}

std::string_view anti_aliasing_name(config::AntiAliasing anti_aliasing) {
    switch (anti_aliasing) {
    case config::AntiAliasing::Off:
        return "Off";
    case config::AntiAliasing::SpatialLow:
        return "Spatial Low";
    case config::AntiAliasing::SpatialMedium:
        return "Spatial Medium";
    case config::AntiAliasing::SpatialHigh:
        return "Spatial High";
    case config::AntiAliasing::SpatialUltra:
        return "Spatial Ultra";
    }
    return "Off";
}

std::string_view render_distance_name(config::RenderDistance distance) {
    switch (distance) {
    case config::RenderDistance::Original:
        return "Original";
    case config::RenderDistance::Far:
        return "Far";
    case config::RenderDistance::Maximum:
        return "Maximum";
    }
    return "Original";
}

bool step_setting(config::GraphicsSettings& settings, const OptionsMenuItem item, const int delta) {
    switch (item) {
    case OptionsMenuItem::DisplayMode:
        settings.display_mode = cycle(settings.display_mode, std::array{config::DisplayMode::Windowed,
                                                 config::DisplayMode::Borderless,
                                                 config::DisplayMode::Fullscreen}, delta);
        return true;
    case OptionsMenuItem::AspectRatio:
        settings.aspect_ratio =
            cycle(settings.aspect_ratio, std::array{config::AspectRatio::Standard, config::AspectRatio::Widescreen}, delta);
        return true;
    case OptionsMenuItem::RenderResolution:
        settings.render_scale = cycle(settings.render_scale, config::kRenderScales, delta);
        return true;
    case OptionsMenuItem::AntiAliasing:
        settings.anti_aliasing = cycle(settings.anti_aliasing, std::array{config::AntiAliasing::Off,
                                                  config::AntiAliasing::SpatialLow,
                                                  config::AntiAliasing::SpatialMedium,
                                                  config::AntiAliasing::SpatialHigh,
                                                  config::AntiAliasing::SpatialUltra}, delta);
        return true;
    case OptionsMenuItem::VSync:
        settings.vsync = !settings.vsync;
        return true;
    case OptionsMenuItem::FrameRateLimit:
        settings.presentation_fps_limit = step_limit(settings.presentation_fps_limit, delta);
        return true;
    case OptionsMenuItem::MenuFrameRateLimit:
        settings.menu_fps_limit = step_limit(settings.menu_fps_limit, delta);
        return true;
    case OptionsMenuItem::FrameTiming:
        settings.frame_rate = settings.frame_rate == config::FrameRateMode::Variable
                                  ? config::FrameRateMode::Retail
                                  : config::FrameRateMode::Variable;
        return true;
    case OptionsMenuItem::MotionBlur:
        settings.motion_blur = !settings.motion_blur;
        return true;
    case OptionsMenuItem::Bloom:
        settings.bloom = !settings.bloom;
        return true;
    case OptionsMenuItem::RenderDistance:
        settings.render_distance = cycle(settings.render_distance, std::array{config::RenderDistance::Original,
                                                    config::RenderDistance::Far,
                                                    config::RenderDistance::Maximum}, delta);
        return true;
    case OptionsMenuItem::Upscaling:
        config::set_fsr_preset(
            settings, cycle(config::fsr_preset(settings),
                            std::array{config::FsrPreset::Off, config::FsrPreset::NativeAa, config::FsrPreset::Quality,
                                       config::FsrPreset::Balanced, config::FsrPreset::Performance,
                                       config::FsrPreset::UltraPerformance},
                            delta));
        return true;
    case OptionsMenuItem::FsrSharpness:
        config::set_fsr_sharpness(
            settings, static_cast<std::uint32_t>(wrapped_index(config::fsr_sharpness(settings), delta,
                                                               config::kFsrSharpnessSteps + 1U)));
        return true;
    case OptionsMenuItem::ModelQuality:
        settings.model_quality = cycle(settings.model_quality, std::array{config::ModelQuality::Original,
                                                  config::ModelQuality::Low,
                                                  config::ModelQuality::Medium,
                                                  config::ModelQuality::High,
                                                  config::ModelQuality::Ultra}, delta);
        return true;
    default:
        return false;
    }
    return false;
}


std::string_view model_quality_name(config::ModelQuality quality) {
    switch (quality) {
    case config::ModelQuality::Original:
        return "Original";
    case config::ModelQuality::Low:
        return "Low";
    case config::ModelQuality::Medium:
        return "Medium";
    case config::ModelQuality::High:
        return "High";
    case config::ModelQuality::Ultra:
        return "Ultra";
    }
    return "Original";
}

} // namespace

std::span<const OptionsMenuItem> options_menu_items() noexcept {
    return kItems;
}

std::string_view options_menu_label(const OptionsMenuItem item) noexcept {
    switch (item) {
    case OptionsMenuItem::DisplayMode:
        return "Display mode";
    case OptionsMenuItem::AspectRatio:
        return "Aspect ratio";
    case OptionsMenuItem::RenderResolution:
        return "Render resolution";
    case OptionsMenuItem::AntiAliasing:
        return "Anti-aliasing";
    case OptionsMenuItem::VSync:
        return "VSync";
    case OptionsMenuItem::FrameRateLimit:
        return "Frame rate limit";
    case OptionsMenuItem::MenuFrameRateLimit:
        return "Menu and video frame rate limit";
    case OptionsMenuItem::FrameTiming:
        return "Frame timing";
    case OptionsMenuItem::MotionBlur:
        return "Motion blur";
    case OptionsMenuItem::Bloom:
        return "Bloom";
    case OptionsMenuItem::RenderDistance:
        return "Render distance";
    case OptionsMenuItem::ModelQuality:
        return "Model quality";
    case OptionsMenuItem::Upscaling:
        return "Upscaling";
    case OptionsMenuItem::FsrSharpness:
        return "FSR sharpness";
    case OptionsMenuItem::RestoreDefaults:
        return "Restore defaults";
    case OptionsMenuItem::Close:
        return "Close";
    }
    return {};
}

std::string options_menu_value(const OptionsMenu& menu, const OptionsMenuItem item) {
    const config::GraphicsSettings& settings = menu.settings;
    switch (item) {
    case OptionsMenuItem::DisplayMode:
        return std::string{display_mode_name(settings.display_mode)};
    case OptionsMenuItem::AspectRatio:
        return settings.aspect_ratio == config::AspectRatio::Widescreen ? "16:9 widescreen"
                                                                         : "4:3";
    case OptionsMenuItem::RenderResolution:
        return presentation::render_scale_text(settings.render_scale);
    case OptionsMenuItem::AntiAliasing: {
        std::string text{anti_aliasing_name(settings.anti_aliasing)};
        const config::AntiAliasing effective =
            presentation::effective_anti_aliasing(settings.render_scale, settings.anti_aliasing);
        if (settings.anti_aliasing != effective) {
            text += " (";
            text += anti_aliasing_name(effective);
            text += " at ";
            text += presentation::render_scale_text(settings.render_scale);
            text += ")";
        }
        return text;
    }
    case OptionsMenuItem::VSync:
        return std::string{std::string_view{settings.vsync ? "On" : "Off"}};
    case OptionsMenuItem::FrameRateLimit:
        return limit_text(settings.presentation_fps_limit);
    case OptionsMenuItem::MenuFrameRateLimit:
        return limit_text(settings.menu_fps_limit);
    case OptionsMenuItem::FrameTiming:
        return settings.frame_rate == config::FrameRateMode::Variable ? "Any frame rate"
                                                                       : "Original 30/60";
    case OptionsMenuItem::MotionBlur:
        return std::string{std::string_view{settings.motion_blur ? "On" : "Off"}};
    case OptionsMenuItem::Bloom:
        return std::string{std::string_view{settings.bloom ? "On" : "Off"}};
    case OptionsMenuItem::RenderDistance:
        return std::string{render_distance_name(settings.render_distance)};
    case OptionsMenuItem::ModelQuality:
        return std::string{model_quality_name(settings.model_quality)};
    case OptionsMenuItem::Upscaling:
        return std::string{fsr_preset_name(config::fsr_preset(settings))};
    case OptionsMenuItem::FsrSharpness:
        return std::to_string(config::fsr_sharpness(settings) * 10U) + "%";
    case OptionsMenuItem::RestoreDefaults:
    case OptionsMenuItem::Close:
        return {};
    }
    return {};
}

OptionsMenuAvailability options_menu_availability(const OptionsMenu& menu,
                                                  const OptionsMenuItem item) noexcept {
    switch (item) {
    case OptionsMenuItem::RenderResolution:
    case OptionsMenuItem::AntiAliasing:
    case OptionsMenuItem::Upscaling:
    case OptionsMenuItem::FsrSharpness:
        return menu.environment.supersampling_available ? OptionsMenuAvailability::Live
                                                        : OptionsMenuAvailability::NotYetAvailable;
    case OptionsMenuItem::FrameRateLimit:
    case OptionsMenuItem::MenuFrameRateLimit:
        return menu.environment.variable_frame_timing_active
                   ? OptionsMenuAvailability::Live
                   : OptionsMenuAvailability::NotYetAvailable;
    case OptionsMenuItem::FrameTiming:
        return OptionsMenuAvailability::AfterRestart;
    default:
        return OptionsMenuAvailability::Live;
    }
}

OptionsMenuEvent apply_options_menu_input(OptionsMenu& menu, const OptionsMenuInput& input) {
    if (input.cancel) {
        return OptionsMenuEvent::Closed;
    }

    menu.focus = std::min(menu.focus, kItems.size() - 1U);
    if (input.move_up) {
        menu.focus = wrapped_index(menu.focus, -1, kItems.size());
    }
    if (input.move_down) {
        menu.focus = wrapped_index(menu.focus, 1, kItems.size());
    }
    const OptionsMenuItem item = kItems[menu.focus];

    int delta = 0;
    if (input.move_left) {
        delta -= 1;
    }
    if (input.move_right) {
        delta += 1;
    }

    if (input.confirm) {
        if (item == OptionsMenuItem::Close) {
            return OptionsMenuEvent::Closed;
        }
        if (item == OptionsMenuItem::RestoreDefaults) {
            config::GraphicsSettings restored{};
            // The window's size and mode are not graphics quality choices.
            restored.resolution = menu.settings.resolution;
            restored.display_mode = menu.settings.display_mode;
            if (restored == menu.settings) {
                return OptionsMenuEvent::None;
            }
            menu.settings = restored;
            return OptionsMenuEvent::Changed;
        }
        if (delta == 0) {
            delta = 1;
        }
    } else if (delta == 0) {
        return OptionsMenuEvent::None;
    }

    const config::GraphicsSettings before = menu.settings;
    if (!step_setting(menu.settings, item, delta)) {
        return OptionsMenuEvent::None;
    }
    menu.settings = config::normalized(menu.settings);
    return menu.settings == before ? OptionsMenuEvent::None : OptionsMenuEvent::Changed;
}

namespace {

// How a row's availability is explained after its value.
std::string_view availability_note(const OptionsMenu& menu, OptionsMenuItem item) {
    switch (options_menu_availability(menu, item)) {
    case OptionsMenuAvailability::Live:
        return {};
    case OptionsMenuAvailability::AfterRestart:
        if (menu.environment.frame_timing_set_by_launcher) {
            return "the launcher sets this";
        }
        return "applies after restart";
    case OptionsMenuAvailability::NotYetAvailable:
        if (item == OptionsMenuItem::FrameRateLimit ||
            item == OptionsMenuItem::MenuFrameRateLimit) {
            return "needs Any frame rate timing";
        }
        return "Vulkan renderer only";
    }
    return {};
}

} // namespace

// RECOVERY: GIMPLE matches except the block order of the inlined availability
// note (the original places the "frame timing" arm after the "live" arm).
std::vector<OptionsMenuLine> options_menu_lines(const OptionsMenu& menu) {
    std::vector<OptionsMenuLine> lines;
    lines.push_back({OptionsMenuLineStyle::Title, "Graphics and performance"});
    const std::size_t focus = std::min(menu.focus, kItems.size() - 1U);
    for (std::size_t index = 0U; index != kItems.size(); ++index) {
        const OptionsMenuItem item = kItems[index];
        std::string text = index == focus ? "> " : "  ";
        text += options_menu_label(item);
        const std::string value = options_menu_value(menu, item);
        if (!value.empty()) {
            // Values start in one column.
            if (text.size() < 34U) {
                text.append(34U - text.size(), ' ');
            } else {
                text += ' ';
            }
            text += index == focus ? "< " + value + " >" : "  " + value;
            const std::string_view note = availability_note(menu, item);
            if (!note.empty()) {
                text += "  (";
                text += note;
                text += ')';
            }
        }
        OptionsMenuLineStyle style = OptionsMenuLineStyle::Body;
        if (index == focus) {
            style = OptionsMenuLineStyle::Focused;
        } else if (options_menu_availability(menu, item) ==
                   OptionsMenuAvailability::NotYetAvailable) {
            style = OptionsMenuLineStyle::Unavailable;
        }
        lines.push_back({style, std::move(text)});
    }
    lines.push_back({OptionsMenuLineStyle::Hint,
                     "Up/Down: select   Left/Right or Enter: change   Esc, F10 or B: close"});
    return lines;
}

void composite_options_menu(const std::span<std::uint8_t> rgba, const std::uint32_t width,
                            const std::uint32_t height, const OptionsMenu& menu) {
    if (width == 0U || height == 0U ||
        rgba.size() / 4U < static_cast<std::size_t>(width) * height) {
        return;
    }
    const std::vector<OptionsMenuLine> lines = options_menu_lines(menu);
    std::size_t widest = 0U;
    for (const OptionsMenuLine& line : lines) {
        widest = std::max(widest, line.text.size());
    }
    const std::uint32_t panel_width =
        std::min(width, static_cast<std::uint32_t>(widest) * 6U + 20U);
    const std::uint32_t panel_height =
        std::min(height, static_cast<std::uint32_t>(lines.size()) * 9U + 29U);
    const std::uint32_t left = (width - panel_width) / 2U;
    const std::uint32_t top = (height - panel_height) / 2U;

    // Darken the panel so the text reads over any picture.
    for (std::uint32_t row = top; row < top + panel_height; ++row) {
        for (std::uint32_t column = left; column < left + panel_width; ++column) {
            const std::size_t offset =
                (static_cast<std::size_t>(row) * width + column) * 4U;
            for (std::size_t channel = 0U; channel != 3U; ++channel) {
                rgba[offset + channel] = static_cast<std::uint8_t>(rgba[offset + channel] >> 2U);
            }
            rgba[offset + 3U] = 255U;
        }
    }

    std::uint32_t y = top + 10U;
    for (const OptionsMenuLine& line : lines) {
        std::uint8_t red = 216U;
        std::uint8_t green = 216U;
        std::uint8_t blue = 216U;
        switch (line.style) {
        case OptionsMenuLineStyle::Title:
            red = 255U;
            green = 216U;
            blue = 112U;
            break;
        case OptionsMenuLineStyle::Focused:
            red = 120U;
            green = 255U;
            blue = 120U;
            break;
        case OptionsMenuLineStyle::Unavailable:
        case OptionsMenuLineStyle::Hint:
            red = 140U;
            green = 140U;
            blue = 140U;
            break;
        case OptionsMenuLineStyle::Body:
            break;
        }
        std::string drawn = line.text;
        std::transform(drawn.begin(), drawn.end(), drawn.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        presentation::draw_stats_text(rgba, width, height, left + 10U, y, drawn, red, green, blue);
        y += 9U;
        if (line.style == OptionsMenuLineStyle::Title) {
            y += 9U;
        }
    }
}

OptionsMenuInput OptionsMenuEdgeTracker::update(const OptionsMenuButtons& buttons) noexcept {
    OptionsMenuInput input{};
    shortcut_edge_ = false;
    if (primed_) {
        input.move_up = buttons.up && !previous_.up;
        input.move_down = buttons.down && !previous_.down;
        input.move_left = buttons.left && !previous_.left;
        input.move_right = buttons.right && !previous_.right;
        input.confirm = buttons.confirm && !previous_.confirm;
        input.cancel = buttons.cancel && !previous_.cancel;
        shortcut_edge_ = buttons.select && buttons.start && !(previous_.select && previous_.start);
    }
    previous_ = buttons;
    primed_ = true;
    return input;
}

void OptionsMenuEdgeTracker::reset() noexcept {
    previous_ = {};
    primed_ = false;
    shortcut_edge_ = false;
}

} // namespace takedown::ui
