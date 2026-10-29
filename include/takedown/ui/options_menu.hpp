#pragma once

// PC graphics and performance options menu.
//
// A host-drawn menu over the game picture (see docs/USAGE.md, "PC options
// menu"). It edits a copy of config::GraphicsSettings; the runtime applies
// every change live where the setting allows it and saves the settings file
// when the menu closes. The model here is pure: rows, values, navigation and
// the text lines to draw. Drawing uses either the built-in bitmap font
// (software renderer route) or the Vulkan renderer's own text, from the same
// lines.
//
// Navigation is shared by keyboard and controller: Up/Down choose a row,
// Left/Right change its value, Confirm changes it forward (or runs an action
// row), Cancel closes. Wording follows the PC conventions of the project:
// no console hardware terms.

#include "takedown/config/app_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::ui {

enum class OptionsMenuItem : std::uint8_t {
    DisplayMode,
    AspectRatio,
    RenderResolution,
    AntiAliasing,
    VSync,
    FrameRateLimit,
    MenuFrameRateLimit,
    FrameTiming,
    MotionBlur,
    Bloom,
    RenderDistance,
    ModelQuality,
    Upscaling,
    FsrSharpness,
    RestoreDefaults,
    Close,
};

// Rows in display order.
[[nodiscard]] std::span<const OptionsMenuItem> options_menu_items() noexcept;

// Frame-rate limits offered by both limit rows, besides Unlimited. A stored
// value outside this list (edited by hand) is kept and shown; stepping moves
// to the nearest listed neighbour.
inline constexpr std::array<std::uint32_t, 11U> kOptionsMenuFrameRateLimits{
    30U, 60U, 75U, 90U, 120U, 144U, 165U, 200U, 240U, 360U, 500U};

// How a row's setting takes effect.
enum class OptionsMenuAvailability : std::uint8_t {
    Live,           // applied immediately
    AfterRestart,   // stored now, used from the next start
    NotYetAvailable // stored, but the game does not use it yet
};

struct OptionsMenuEnvironment {
    // The Vulkan renderer draws the game; render resolution and
    // anti-aliasing exist only there.
    bool supersampling_available{true};
    // The game's own frame-rate code is native (variable frame timing);
    // otherwise the game keeps its 30/60 and limits cap presentation only.
    bool variable_frame_timing_active{true};
    // The launcher chose the frame timing on its command line, so the stored
    // value is not what the next start uses either.
    bool frame_timing_set_by_launcher{false};
};

struct OptionsMenu {
    config::GraphicsSettings settings{};
    std::size_t focus{0U};
    OptionsMenuEnvironment environment{};
};

struct OptionsMenuInput {
    bool move_up{false};
    bool move_down{false};
    bool move_left{false};
    bool move_right{false};
    bool confirm{false};
    bool cancel{false};
};

enum class OptionsMenuEvent : std::uint8_t {
    None,
    Changed, // `settings` holds a new value
    Closed,  // the menu asked to close
};

[[nodiscard]] std::string_view options_menu_label(OptionsMenuItem item) noexcept;
// Current value text of a setting row; empty for action rows.
[[nodiscard]] std::string options_menu_value(const OptionsMenu& menu, OptionsMenuItem item);
[[nodiscard]] OptionsMenuAvailability options_menu_availability(const OptionsMenu& menu,
                                                                OptionsMenuItem item) noexcept;

// Applies one frame of input edges. Focus wraps around; value changes wrap
// at both ends of each value list.
[[nodiscard]] OptionsMenuEvent apply_options_menu_input(OptionsMenu& menu,
                                                        const OptionsMenuInput& input);

enum class OptionsMenuLineStyle : std::uint8_t {
    Body,
    Title,
    Focused,
    Unavailable,
    Hint,
};

struct OptionsMenuLine {
    OptionsMenuLineStyle style{OptionsMenuLineStyle::Body};
    std::string text;

    bool operator==(const OptionsMenuLine&) const = default;
};

// The menu as text lines: title, one line per row (label, value, and a note
// for settings that are not applied live), and a control hint.
[[nodiscard]] std::vector<OptionsMenuLine> options_menu_lines(const OptionsMenu& menu);

// Draws the menu into the host's private RGBA copy of the presented frame,
// with the bitmap font shared with the stats overlay (uppercase only).
void composite_options_menu(std::span<std::uint8_t> rgba, std::uint32_t width,
                            std::uint32_t height, const OptionsMenu& menu);

// Turns consecutive controller states into menu input edges, and detects the
// controller shortcut that opens the menu (Back/Select held with Start).
struct OptionsMenuButtons {
    bool up{false};
    bool down{false};
    bool left{false};
    bool right{false};
    bool confirm{false};
    bool cancel{false};
    bool select{false};
    bool start{false};
};

class OptionsMenuEdgeTracker {
  public:
    // The first call after construction or reset() reports no edges.
    [[nodiscard]] OptionsMenuInput update(const OptionsMenuButtons& buttons) noexcept;
    // True on the frame Select+Start becomes held (either order).
    [[nodiscard]] bool shortcut_pressed() const noexcept { return shortcut_edge_; }
    void reset() noexcept;

  private:
    OptionsMenuButtons previous_{};
    bool primed_{false};
    bool shortcut_edge_{false};
};

} // namespace takedown::ui
