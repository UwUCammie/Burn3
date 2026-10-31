#pragma once

// The SETTINGS page: one page with tabs (Audio, Camera, Controls, Gameplay,
// Video, in that order along the top) that replaces the separate Options
// sub-pages. The game still draws the page's header, title, background and
// footer; this model holds the tabs and rows, turns host input (controller,
// keyboard and mouse) into edits, and lays the page out as host UI items in
// the game menu's 640x448 picture, with the game's own fonts and artwork.
//
// The model is pure: it does not know where a setting lives. The caller
// describes every row (label, kind, values, current value) each frame and
// applies the edits the model reports. Values never wrap: holding a direction
// stops at the first or last value, and the tab keys stop at the first or last
// tab.

#include "takedown/platform/parallel_gs_host_ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::ui {
class ControlStylePrompt;

enum class SettingsTab : std::uint8_t { Audio, Camera, Controls, Gameplay, Video };
inline constexpr std::size_t kSettingsTabCount = 5U;
[[nodiscard]] std::string_view settings_tab_label(SettingsTab tab) noexcept;

enum class SettingsRowKind : std::uint8_t {
    Section, // a sub-section heading (not selectable)
    Toggle,  // two values, Off and On: a switch
    Choice,  // a list of values: arrows, the value and one notch per value
    Slider,  // a range of levels: a bar of cells and the value text
    Action,  // a button: Confirm (or Left/Right) runs it; `values` holds one text shown beside it
};

struct SettingsRow {
    // The caller's identity of the row, returned with its edits.
    std::uint32_t id{};
    SettingsRowKind kind{SettingsRowKind::Choice};
    std::string label;
    // Value texts (Toggle: Off, On; Choice: every value; Slider: empty, see
    // `levels`; Action: an optional single text).
    std::vector<std::string> values;
    std::size_t value{};
    // Slider: number of levels (value 0 to levels - 1) and the text shown for
    // the current level.
    std::size_t levels{};
    std::string level_text;
    // One line shown under the list while the row has the focus.
    std::string description;
    // Greyed and not editable.
    bool disabled{false};
};

struct SettingsPageContent {
    std::array<std::vector<SettingsRow>, kSettingsTabCount> tabs;
    // A message box over the page (the controls editor's capture prompt);
    // empty when none. While it is shown the page takes no input.
    std::string popup;
};

// One frame of host input. Directions and buttons are the held state; the
// model makes the edges and the hold repeat itself. The pointer is in game
// picture pixels (640x448), or absent when it is outside the picture.
struct SettingsInput {
    bool up{false}, down{false}, left{false}, right{false};
    bool confirm{false};
    bool back{false};
    bool previous_tab{false}, next_tab{false};
    std::optional<std::array<float, 2>> pointer;
    bool pointer_moved{false};
    bool click{false}; // the left button went down this frame
    bool click_held{false}; // the left button is held (slider dragging)
    int wheel{0}; // steps, positive away from the player
    float seconds{1.0f / 60.0f}; // since the previous frame
};

struct SettingsEdit {
    std::uint32_t id{};
    // A Toggle/Choice/Slider value, or for an Action the direction it was run
    // with (-1 left, 0 confirm, 1 right).
    std::size_t value{};
    int direction{};
    bool action{false};
};

enum class SettingsSound : std::uint8_t { None, Move, Change, Tab, Back, Blocked };

struct SettingsFrame {
    std::vector<SettingsEdit> edits;
    SettingsSound sound{SettingsSound::None};
    bool close{false};
};

class SettingsMenu {
  public:
    void open(SettingsTab tab) noexcept;
    [[nodiscard]] SettingsTab tab() const noexcept { return tab_; }
    [[nodiscard]] std::size_t focus() const noexcept { return focus_; }

    // Updates the rows (keeping the focus on the same row id) and applies one
    // frame of input.
    SettingsFrame update(const SettingsPageContent& content, const SettingsInput& input);

    // The page as host UI items. `font_directory` holds the game fonts
    // (GlobalFont, HeadFont, SmallFont) and `art_directory` the menu artwork.
    [[nodiscard]] platform::ParallelGsHostUi host_ui(const std::string& font_directory,
                                                     const std::string& art_directory) const;

  private:
    struct Repeat {
        bool held{false};
        float timer{0.0f};
        [[nodiscard]] bool step(bool now, float seconds) noexcept;
    };
    [[nodiscard]] const std::vector<SettingsRow>& rows() const noexcept;
    void select_tab(SettingsTab tab) noexcept;
    void move_focus(int direction, SettingsFrame& frame);
    void keep_visible() noexcept;
    void change(int direction, SettingsFrame& frame);
    void set_value(const SettingsRow& row, std::size_t value, SettingsFrame& frame);
    void activate(int direction, SettingsFrame& frame);
    void pointer(const SettingsInput& input, SettingsFrame& frame);
    [[nodiscard]] std::optional<std::size_t> first_selectable() const noexcept;

    SettingsPageContent content_{};
    SettingsTab tab_{SettingsTab::Audio};
    std::size_t focus_{0U};
    std::size_t scroll_{0U};
    std::array<std::size_t, kSettingsTabCount> remembered_focus_{};
    Repeat up_, down_, left_, right_;
    bool confirm_held_{false}, back_held_{false}, previous_held_{false}, next_held_{false};
    bool hover_active_{false};
    // Seconds since the page opened and since the tab changed (the opening
    // animation).
    float clock_{0.0f};
    float tab_clock_{0.0f};
    bool dragging_{false};
    std::uint32_t drag_id_{};
};

// Layout, in game picture pixels, shared by drawing and pointer hit tests.
namespace settings_layout {
// Below y 90: on a wide picture the renderer keeps items in the header band
// (0..90) on the header's left and right edges.
inline constexpr float kTabTop = 92.0f;
inline constexpr float kTabHeight = 24.0f;
inline constexpr float kTabLeft = 56.0f;
inline constexpr float kTabRight = 584.0f;
inline constexpr float kListTop = 126.0f;
inline constexpr float kRowHeight = 24.0f;
inline constexpr std::size_t kVisibleRows = 9U;
inline constexpr float kListLeft = 36.0f;
inline constexpr float kListRight = 604.0f;
inline constexpr float kControlLeft = 352.0f;
inline constexpr float kControlRight = 588.0f;
inline constexpr float kDescriptionTop = 352.0f;
} // namespace settings_layout

// The online player count badge drawn after a main menu row's text, in the
// SETTINGS page's style: `x` is where the row's text ends, `y` its vertical
// centre and `height` its height, in picture pixels, and `alpha` its opacity (the row's own, so the badge slides, grows
// and fades with it).
[[nodiscard]] platform::ParallelGsHostUi online_count_badge_ui(const std::string& font_directory, std::uint32_t count,
                                                               float x, float y, float height, float alpha);

// The first-run controller layout question (ui/control_style.hpp), drawn in
// the SETTINGS page's style over whatever the game shows.
[[nodiscard]] platform::ParallelGsHostUi control_style_prompt_ui(const std::string& font_directory,
                                                                 const std::string& art_directory,
                                                                 const ControlStylePrompt& prompt);

} // namespace takedown::ui
