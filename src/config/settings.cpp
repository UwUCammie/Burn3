#include "takedown/config/app_settings.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace takedown::config {
namespace {

[[nodiscard]] bool valid_resolution(const Resolution resolution) noexcept {
    return resolution.width >= kMinimumResolutionWidth &&
           resolution.height >= kMinimumResolutionHeight &&
           resolution.width <= kMaximumResolutionDimension &&
           resolution.height <= kMaximumResolutionDimension;
}

// nullopt (unlimited) is valid; a set cap must be 1 to kMaximumPresentationFps.
[[nodiscard]] bool valid_fps_limit(const std::optional<std::uint32_t> limit) noexcept {
    return !limit.has_value() || (*limit >= 1U && *limit <= kMaximumPresentationFps);
}

[[nodiscard]] bool valid_aspect_ratio(const AspectRatio aspect) noexcept {
    return aspect == AspectRatio::Standard || aspect == AspectRatio::Widescreen;
}

[[nodiscard]] bool valid_render_scale(const std::uint32_t scale) noexcept {
    return std::find(kRenderScales.begin(), kRenderScales.end(), scale) != kRenderScales.end();
}

[[nodiscard]] bool valid_field_of_view(const std::uint32_t field_of_view) noexcept {
    return field_of_view == 0U ||
           (field_of_view >= kMinimumFieldOfView && field_of_view <= kMaximumFieldOfView);
}

[[nodiscard]] bool valid_anti_aliasing(const AntiAliasing anti_aliasing) noexcept {
    switch (anti_aliasing) {
    case AntiAliasing::Off:
    case AntiAliasing::SpatialLow:
    case AntiAliasing::SpatialMedium:
    case AntiAliasing::SpatialHigh:
    case AntiAliasing::SpatialUltra:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_render_distance(const RenderDistance distance) noexcept {
    return static_cast<std::uint8_t>(distance) <=
           static_cast<std::uint8_t>(RenderDistance::Maximum);
}

[[nodiscard]] bool valid_model_quality(const ModelQuality quality) noexcept {
    switch (quality) {
    case ModelQuality::Original:
    case ModelQuality::Low:
    case ModelQuality::Medium:
    case ModelQuality::High:
    case ModelQuality::Ultra:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_volume(const float volume) noexcept {
    return std::isfinite(volume) && volume >= 0.0F && volume <= 1.0F;
}

[[nodiscard]] float normalized_volume(const float volume) noexcept {
    if (!std::isfinite(volume)) {
        return 1.0F;
    }
    return std::min(std::max(volume, 0.0F), 1.0F);
}

void append(ValidationIssues& issues, ValidationIssues&& more) {
    issues.insert(issues.end(), std::make_move_iterator(more.begin()),
                  std::make_move_iterator(more.end()));
}

} // namespace

std::string_view to_string(const FsrPreset preset) noexcept {
    switch (preset) {
    case FsrPreset::Off:
        return "off";
    case FsrPreset::NativeAa:
        return "native_aa";
    case FsrPreset::Quality:
        return "quality";
    case FsrPreset::Balanced:
        return "balanced";
    case FsrPreset::Performance:
        return "performance";
    case FsrPreset::UltraPerformance:
        return "ultra_performance";
    }
    return "off";
}

double Resolution::aspect_ratio() const noexcept {
    if (height == 0U) {
        return 0.0;
    }
    return static_cast<double>(width) / static_cast<double>(height);
}

std::string_view to_string(const ModelQuality quality) noexcept {
    switch (quality) {
    case ModelQuality::Original:
        return "original";
    case ModelQuality::Low:
        return "low";
    case ModelQuality::Medium:
        return "medium";
    case ModelQuality::High:
        return "high";
    case ModelQuality::Ultra:
        return "ultra";
    }
    return "unknown";
}

std::string_view to_string(const FrameRateMode mode) noexcept {
    switch (mode) {
    case FrameRateMode::Retail:
        return "retail";
    case FrameRateMode::Variable:
        return "variable";
    }
    return "unknown";
}

std::string_view to_string(const AspectRatio aspect) noexcept {
    switch (aspect) {
    case AspectRatio::Standard:
        return "4:3";
    case AspectRatio::Widescreen:
        return "16:9";
    }
    return "unknown";
}

std::string_view to_string(const AntiAliasing anti_aliasing) noexcept {
    switch (anti_aliasing) {
    case AntiAliasing::Off:
        return "off";
    case AntiAliasing::SpatialLow:
        return "spatial_low";
    case AntiAliasing::SpatialMedium:
        return "spatial_medium";
    case AntiAliasing::SpatialHigh:
        return "spatial_high";
    case AntiAliasing::SpatialUltra:
        return "spatial_ultra";
    }
    return "unknown";
}

std::string_view to_string(const RenderDistance distance) noexcept {
    switch (distance) {
    case RenderDistance::Original:
        return "original";
    case RenderDistance::Far:
        return "far";
    case RenderDistance::Maximum:
        return "maximum";
    }
    return "unknown";
}

std::string_view to_string(const DisplayMode mode) noexcept {
    switch (mode) {
    case DisplayMode::Windowed:
        return "windowed";
    case DisplayMode::Borderless:
        return "borderless";
    case DisplayMode::Fullscreen:
        return "fullscreen";
    }
    return "unknown";
}

ValidationIssues validate(const GraphicsSettings& settings) {
    ValidationIssues issues;
    if (!valid_resolution(settings.resolution)) {
        issues.push_back({"graphics.resolution",
                          "width and height must be within the supported resolution range"});
    }
    if (!valid_fps_limit(settings.presentation_fps_limit)) {
        issues.push_back({"graphics.presentation_fps_limit",
                          "use no value for unlimited, or choose a cap from 1 to 1000"});
    }
    if (!valid_fps_limit(settings.menu_fps_limit)) {
        issues.push_back({"graphics.menu_fps_limit",
                          "use no value for unlimited, or choose a cap from 1 to 1000"});
    }
    if (!valid_aspect_ratio(settings.aspect_ratio)) {
        issues.push_back({"graphics.aspect_ratio", "aspect ratio is not a recognized option"});
    }
    if (!valid_render_scale(settings.render_scale)) {
        issues.push_back({"graphics.render_scale",
                          "render scale must be 1, 2 or 4 (100%, 200% or 400%)"});
    }
    if (!valid_field_of_view(settings.field_of_view)) {
        issues.push_back({"graphics.field_of_view",
                          "field of view must be default or 40 to 140 degrees"});
    }
    if (!valid_anti_aliasing(settings.anti_aliasing)) {
        issues.push_back({"graphics.anti_aliasing", "anti-aliasing is not a recognized option"});
    }
    if (!valid_render_distance(settings.render_distance)) {
        issues.push_back(
            {"graphics.render_distance", "render distance is not a recognized option"});
    }
    if (!valid_model_quality(settings.model_quality)) {
        issues.push_back({"graphics.model_quality", "model quality is not a recognized option"});
    }
    return issues;
}

ValidationIssues validate(const AudioSettings& settings) {
    ValidationIssues issues;
    if (!valid_volume(settings.master_volume)) {
        issues.push_back(
            {"audio.master_volume", "master volume must be a finite value from 0 to 1"});
    }
    if (!valid_volume(settings.music_volume)) {
        issues.push_back(
            {"audio.music_volume", "music volume must be a finite value from 0 to 1"});
    }
    if (!valid_volume(settings.effects_volume)) {
        issues.push_back(
            {"audio.effects_volume", "effects volume must be a finite value from 0 to 1"});
    }
    return issues;
}

ValidationIssues validate(const AppSettings& settings) {
    auto issues = validate(settings.graphics);
    append(issues, validate(settings.audio));
    if (settings.save_data_directory.has_value() && settings.save_data_directory->empty()) {
        issues.push_back(
            {"save_data.directory", "the save-data directory must be non-empty when set"});
    }
    if (settings.mods_directory.has_value() && settings.mods_directory->empty()) {
        issues.push_back({"mods.directory", "the mods directory must be non-empty when set"});
    }
    return issues;
}

bool is_valid(const GraphicsSettings& settings) {
    return validate(settings).empty();
}

bool is_valid(const AudioSettings& settings) {
    return validate(settings).empty();
}

bool is_valid(const AppSettings& settings) {
    return validate(settings).empty();
}

GraphicsSettings normalized(GraphicsSettings settings) noexcept {
    if (!valid_resolution(settings.resolution)) {
        settings.resolution = GraphicsSettings{}.resolution;
    }
    if (!valid_fps_limit(settings.presentation_fps_limit)) {
        settings.presentation_fps_limit.reset();
    }
    if (!valid_fps_limit(settings.menu_fps_limit)) {
        settings.menu_fps_limit.reset();
    }
    if (!valid_aspect_ratio(settings.aspect_ratio)) {
        settings.aspect_ratio = AspectRatio::Standard;
    }
    if (!valid_render_scale(settings.render_scale)) {
        settings.render_scale = 1U;
    }
    if (!valid_field_of_view(settings.field_of_view)) {
        settings.field_of_view = 0U;
    }
    if (!valid_anti_aliasing(settings.anti_aliasing)) {
        settings.anti_aliasing = AntiAliasing::Off;
    }
    if (!valid_render_distance(settings.render_distance)) {
        settings.render_distance = RenderDistance::Original;
    }
    if (!valid_model_quality(settings.model_quality)) {
        settings.model_quality = ModelQuality::Original;
    }
    // Untagged bytes (left by code that does not know the FSR fields) become
    // the defaults.
    set_fsr_preset(settings, fsr_preset(settings));
    set_fsr_sharpness(settings, fsr_sharpness(settings));
    return settings;
}

AudioSettings normalized(AudioSettings settings) noexcept {
    settings.master_volume = normalized_volume(settings.master_volume);
    settings.music_volume = normalized_volume(settings.music_volume);
    settings.effects_volume = normalized_volume(settings.effects_volume);
    // The game's own Music and SFX levels run from 0 to 100.
    if (settings.game_music_level > 100U) {
        settings.game_music_level.reset();
    }
    if (settings.game_effects_level > 100U) {
        settings.game_effects_level.reset();
    }
    return settings;
}

AppSettings normalized(AppSettings settings) noexcept {
    settings.graphics = normalized(settings.graphics);
    settings.audio = normalized(settings.audio);
    if (settings.save_data_directory.has_value() && settings.save_data_directory->empty()) {
        settings.save_data_directory.reset();
    }
    if (settings.mods_directory.has_value() && settings.mods_directory->empty()) {
        settings.mods_directory.reset();
    }
    return settings;
}

} // namespace takedown::config
