#pragma once
// Controller button layouts: the PlayStation 2 release's or the Xbox
// release's. Every player is a DualShock 2 to the game, so a layout is a set
// of gamepad bindings (platform/input_bindings.hpp); keyboard and mouse
// bindings are never touched.
//
// PlayStation 2 (button for button): Cross accelerates, Square brakes, R1
// boosts, R2 Crashbreaker, L1 looks back, Triangle changes the camera, L2 the
// music; Cross confirms and Triangle goes back in menus.
//
// Xbox: the right trigger accelerates, the left trigger brakes, A boosts, B
// Crashbreaker, X looks back, Y changes the camera, RB (the old Black button)
// the music. In menus A confirms and B goes back, so while the game shows a
// menu the gamepad uses menu_bindings() instead.
//
// The chosen layout lives in control_style.txt next to controls.ini; while it
// is missing the game asks once (ControlStylePrompt).
#include "takedown/platform/input_bindings.hpp"
#include "takedown/platform/window.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

namespace takedown::ui {

enum class ControlStyle : std::uint8_t { PlayStation2, Xbox };

[[nodiscard]] std::string_view control_style_name(ControlStyle style) noexcept; // "ps2", "xbox"

// `bindings` with every player's gamepad controls replaced by the layout's.
[[nodiscard]] platform::InputBindings with_control_style(const platform::InputBindings& bindings, ControlStyle style);
// The gamepad controls the game reads while it shows a menu: unchanged for
// PlayStation 2; for Xbox, A is Cross, B is Triangle, X is Square, Y is
// Circle and the shoulders and triggers are L1/R1/L2/R2 (tabs on LB/RB).
[[nodiscard]] platform::InputBindings menu_bindings(const platform::InputBindings& bindings, ControlStyle style);
// True when every player's gamepad controls are one of the stock layouts
// (either style, or the layout from before styles existed): nobody edited them.
[[nodiscard]] bool gamepad_bindings_are_stock(const platform::InputBindings& bindings);

[[nodiscard]] std::filesystem::path control_style_path_for(const std::filesystem::path& controls_path);
[[nodiscard]] std::optional<ControlStyle> load_control_style(const std::filesystem::path& path);
bool save_control_style(const std::filesystem::path& path, ControlStyle style);

// The first-run question: two cards, PlayStation 2 and Xbox. Left/right (or
// the pointer) chooses, Confirm or a click takes it. Pure: the caller feeds
// one frame of host input and draws host_ui().
class ControlStylePrompt {
  public:
    struct Input {
        bool left{false}, right{false}, confirm{false};
        std::optional<std::array<float, 2>> pointer; // game picture pixels (640x448)
        bool click{false};
        float seconds{1.0f / 60.0f};
    };
    enum class Sound : std::uint8_t { None, Move, Confirm };
    struct Frame {
        std::optional<ControlStyle> chosen;
        Sound sound{Sound::None};
    };
    void open() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    Frame update(const Input& input);
    // Card rectangles in the game picture (x, y, width, height), for drawing
    // and pointer tests.
    [[nodiscard]] static std::array<float, 4> card(ControlStyle style) noexcept;
    [[nodiscard]] ControlStyle selection() const noexcept { return selection_; }
    [[nodiscard]] float clock() const noexcept { return clock_; }
    [[nodiscard]] float closing() const noexcept { return closing_; }

  private:
    bool open_{false};
    ControlStyle selection_{ControlStyle::PlayStation2};
    std::optional<ControlStyle> chosen_;
    float clock_{0.0f};
    float closing_{0.0f};
    // Edges: the held state of the previous frame. Starts held so a button
    // still down when the prompt opens does nothing.
    bool left_held_{true}, right_held_{true}, confirm_held_{true};
};

} // namespace takedown::ui
