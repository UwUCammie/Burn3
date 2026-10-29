#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::config {

inline constexpr std::uint32_t kMinimumResolutionWidth = 320;
inline constexpr std::uint32_t kMinimumResolutionHeight = 200;
inline constexpr std::uint32_t kMaximumResolutionDimension = 16'384;
inline constexpr std::uint32_t kMaximumPresentationFps = 1'000;

struct Resolution {
    std::uint32_t width{1920};
    std::uint32_t height{1080};

    [[nodiscard]] double aspect_ratio() const noexcept;
    auto operator<=>(const Resolution&) const = default;
};

enum class ModelQuality : std::uint8_t {
    Original,
    Low,
    Medium,
    High,
    Ultra,
};

[[nodiscard]] std::string_view to_string(ModelQuality quality) noexcept;

enum class FrameRateMode : std::uint8_t {
    // The game's own fixed tick: 60 per second in races, 30 in the frontend.
    Retail,
    // One tick per displayed frame, each lasting as long as the previous frame
    // really took. Any frame rate, including a changing one, at unchanged
    // game speed.
    Variable,
};

[[nodiscard]] std::string_view to_string(FrameRateMode mode) noexcept;

// Shape of the displayed picture. Widescreen widens the game's 3D camera to
// 16:9 and presents the frame at 16:9. The Vulkan renderer draws the game's
// HUD and menus as a separate layer that keeps their 4:3 shape, with elements
// moved towards the edges they belong to (renderer/parallel-gs/takedown/ui_layout.hpp).
enum class AspectRatio : std::uint8_t {
    Standard,   // 4:3, as the game was released
    Widescreen, // 16:9
};

[[nodiscard]] std::string_view to_string(AspectRatio aspect) noexcept;

// How the game window covers the screen. Borderless covers the monitor with
// an undecorated window at the desktop's own resolution (fast alt-tab);
// Fullscreen also asks the display driver for exclusive ownership of it.
enum class DisplayMode : std::uint8_t {
    Windowed,
    Borderless,
    Fullscreen,
};

[[nodiscard]] std::string_view to_string(DisplayMode mode) noexcept;

// Supersampling anti-aliasing: every pixel is rendered from several samples
// and averaged in place. There is no temporal accumulation and no
// post-process edge blur, so the picture stays sharp and motion stays clean.
enum class AntiAliasing : std::uint8_t {
    Off,
    SpatialLow,
    SpatialMedium,
    SpatialHigh,
    SpatialUltra,
};

[[nodiscard]] std::string_view to_string(AntiAliasing anti_aliasing) noexcept;

// How far away scenery and traffic are drawn.
enum class RenderDistance : std::uint8_t {
    Original,
    Far,
    Maximum,
};

[[nodiscard]] std::string_view to_string(RenderDistance distance) noexcept;

// Internal resolution as a multiple of the game's own 640x448 picture: 1 to 6
// (100% to 600%). The renderer keeps the supersamples of a 2x2, 4x4 or 8x8
// grid as pixels: 200% uses 4 samples per game pixel, 300% and 400% use 16
// (300% filters the 4x4 grid down), 500% and 600% use 64 (the 8x8 grid
// filtered down; GPUs without a fixed 64-wide subgroup fall back to 16).
inline constexpr std::uint32_t kMaximumRenderScale = 6;
inline constexpr std::array<std::uint32_t, 6> kRenderScales{1U, 2U, 3U, 4U, 5U, 6U};

// Field of view choices (degrees) besides the game's own (0).
inline constexpr std::uint32_t kMinimumFieldOfView = 40U;
inline constexpr std::uint32_t kMaximumFieldOfView = 140U;

struct GraphicsSettings {
    bool fps_counter{false};
    Resolution resolution{};
    DisplayMode display_mode{DisplayMode::Windowed};
    bool vsync{true};

    // nullopt means unlimited. With the retail frame rate this caps host
    // presentation only; with the variable frame rate it caps the game's
    // frame rate itself.
    std::optional<std::uint32_t> presentation_fps_limit{std::nullopt};
    FrameRateMode frame_rate{FrameRateMode::Retail};
    // Cap applied on top of presentation_fps_limit while the game is in its
    // menus or playing a video. nullopt means unlimited.
    std::optional<std::uint32_t> menu_fps_limit{240U};
    ModelQuality model_quality{ModelQuality::Original};
    AspectRatio aspect_ratio{AspectRatio::Standard};
    // One of kRenderScales; 1 is the game's own resolution.
    std::uint32_t render_scale{1U};
    AntiAliasing anti_aliasing{AntiAliasing::Off};
    RenderDistance render_distance{RenderDistance::Original};
    bool motion_blur{true};
    bool bloom{true};
    // The GS dither the game draws with (a fine 4x4 pattern). Off smooths
    // gradients into bands instead; it is a renderer setting only.
    bool dithering{true};
    // Original menu/HUD geometry composited at the window resolution (else at
    // the game's resolution).
    bool high_resolution_ui{false};
    // Horizontal field of view at full boost, in degrees; 0 keeps the game's
    // own camera. The game's other camera states (start line, normal driving)
    // and its boost widening are kept relative to it.
    std::uint32_t field_of_view{0U};
    // With a changed field of view, move the chase camera towards or away
    // from the car so the car keeps its size and place on screen.
    bool fov_correction{true};
    // Panini projection of the driving view (Vulkan renderer): straightens
    // the stretched edges of a wide FOV; boosting still widens it a little.
    // Off by default: it warps the whole picture, HUD included, and pushes
    // HUD items near the top and bottom edges partly off screen.
    bool panini{false};
    // AMD FSR 3.1 upscaling of the world picture (Vulkan renderer), read and
    // written through fsr_preset()/fsr_sharpness() below. These two bytes sit
    // in what was the struct's tail padding, so its size and layout are those
    // of the release objects, some of which copy it without its padding: each
    // byte carries a tag in its high bits, and one without the tag reads as
    // the default (FSR off, sharpness 50%).
    std::uint8_t fsr_preset_tagged{0xA0U};
    std::uint8_t fsr_sharpness_tagged{0xB5U};

    bool operator==(const GraphicsSettings&) const = default;
};
static_assert(sizeof(GraphicsSettings) == 60U, "GraphicsSettings must keep the release layout");

// FSR 3.1 presets (Vulkan renderer). The render resolution is a whole multiple
// of the game's 640x448, chosen from the preset's ratio and the window size.
enum class FsrPreset : std::uint8_t {
    Off,
    NativeAa,         // 1.0x
    Quality,          // 1.5x
    Balanced,         // 1.7x
    Performance,      // 2.0x
    UltraPerformance, // 3.0x
};
inline constexpr std::uint8_t kFsrPresetTag = 0xA0U;
inline constexpr std::uint8_t kFsrSharpnessTag = 0xB0U;
// Sharpness is kept in tenths: 0 to 10 for 0% to 100%.
inline constexpr std::uint32_t kFsrSharpnessSteps = 10U;
inline constexpr std::uint32_t kFsrDefaultSharpness = 5U;

[[nodiscard]] inline FsrPreset fsr_preset(const GraphicsSettings& settings) noexcept {
    const std::uint8_t byte = settings.fsr_preset_tagged;
    if ((byte & 0xF8U) != kFsrPresetTag || (byte & 7U) > static_cast<std::uint8_t>(FsrPreset::UltraPerformance)) {
        return FsrPreset::Off;
    }
    return static_cast<FsrPreset>(byte & 7U);
}
inline void set_fsr_preset(GraphicsSettings& settings, const FsrPreset preset) noexcept {
    settings.fsr_preset_tagged = static_cast<std::uint8_t>(kFsrPresetTag | (static_cast<std::uint8_t>(preset) & 7U));
}
// 0 to 10 (tenths of full sharpness).
[[nodiscard]] inline std::uint32_t fsr_sharpness(const GraphicsSettings& settings) noexcept {
    const std::uint8_t byte = settings.fsr_sharpness_tagged;
    if ((byte & 0xF0U) != kFsrSharpnessTag || (byte & 15U) > kFsrSharpnessSteps) {
        return kFsrDefaultSharpness;
    }
    return byte & 15U;
}
inline void set_fsr_sharpness(GraphicsSettings& settings, const std::uint32_t tenths) noexcept {
    settings.fsr_sharpness_tagged =
        static_cast<std::uint8_t>(kFsrSharpnessTag | (tenths > kFsrSharpnessSteps ? kFsrSharpnessSteps : tenths));
}

[[nodiscard]] std::string_view to_string(FsrPreset preset) noexcept;

struct AudioSettings {
    // Linear gain values in the inclusive range [0, 1].
    float master_volume{1.0F};
    float music_volume{1.0F};
    float effects_volume{1.0F};
    bool mute_when_unfocused{false};
    // The game's own Music and SFX levels (0 to 100) from the last loaded
    // profile, applied at boot so the game starts at those levels instead
    // of full volume before a profile loads. nullopt until a profile loads.
    std::optional<std::uint32_t> game_music_level{std::nullopt};
    std::optional<std::uint32_t> game_effects_level{std::nullopt};

    bool operator==(const AudioSettings&) const = default;
};

struct AppSettings {
    GraphicsSettings graphics{};
    AudioSettings audio{};

    // Save-data storage directory. nullopt means the launch directory's saves/
    // (takedown::save::default_save_data_root()); when set, the path overrides
    // it. The user-facing concept is "save data" / "profiles" / "storage" —
    // see docs/SAVE_DATA.md — not a memory-card API.
    std::optional<std::filesystem::path> save_data_directory{std::nullopt};

    // Data-mod folder (docs/MODDING.md, "Data mods"). nullopt means the
    // default `mods` folder next to the game data directory. `mods_enabled`
    // false turns the override layer off without moving any files.
    std::optional<std::filesystem::path> mods_directory{std::nullopt};
    bool mods_enabled{true};

    bool operator==(const AppSettings&) const = default;
};

struct ValidationIssue {
    std::string field;
    std::string message;

    bool operator==(const ValidationIssue&) const = default;
};

using ValidationIssues = std::vector<ValidationIssue>;

[[nodiscard]] ValidationIssues validate(const GraphicsSettings& settings);
[[nodiscard]] ValidationIssues validate(const AudioSettings& settings);
[[nodiscard]] ValidationIssues validate(const AppSettings& settings);

[[nodiscard]] bool is_valid(const GraphicsSettings& settings);
[[nodiscard]] bool is_valid(const AudioSettings& settings);
[[nodiscard]] bool is_valid(const AppSettings& settings);

// Returns a valid copy. Invalid dimensions, enum values and render scales use
// defaults; volumes are clamped, and an invalid FPS cap becomes unlimited.
[[nodiscard]] GraphicsSettings normalized(GraphicsSettings settings) noexcept;
[[nodiscard]] AudioSettings normalized(AudioSettings settings) noexcept;
[[nodiscard]] AppSettings normalized(AppSettings settings) noexcept;

} // namespace takedown::config
