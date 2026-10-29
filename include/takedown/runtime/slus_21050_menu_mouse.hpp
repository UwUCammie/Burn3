#pragma once

// Mouse control of the game's own menus (SLUS-21050).
//
// The game draws every menu row, every message box button and the selected
// one bigger than the rest with one retained UI text callback (0x00371BE0).
// Those nodes, read from the UI draw buckets (context 0x01E903E0: bucket
// array at +44, bucket count at +48, 16-byte buckets whose node list starts at
// +8; a node links the next at +0, holds its local rectangle at +8..+20, its
// parent transform at [+0x40] + 0x20..+0x2C and its callback at +0x44), give
// each list's rows on screen in the game's 640x448 picture, which row is
// highlighted (the tallest) and, by their order, which presses reach another
// row. Nothing here knows a particular menu.
//
// The pointer moves the highlight with the game's own up/down (or left/right)
// presses, one press every other controller poll, so the game plays its sounds
// and animations as for a pad. A left click on a row selects it and confirms
// it (Cross); on a list with value boxes (0x00380250, the option pages) a click
// on the row selects it and a click on its value box steps the value (Right).
// A right click is Back (Triangle), the wheel moves the highlight, and the
// footer prompts are buttons: the left one Back, the right one Cross. Where
// the game shows no rows and no footer (a race, a loading screen) the mouse
// does nothing, except on the title screen, where a click presses Start.

#include "takedown/guest/types.hpp"
#include "takedown/platform/window.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

struct MenuMouseInput {
    float x{};          // fraction of the game picture, 0 to 1 across it
    float y{};
    bool inside{false}; // the pointer is in the window
    std::uint8_t buttons{}; // bit 0 left, bit 1 middle, bit 2 right
    int wheel{};        // steps since the previous poll, positive away from the player
};

// One row as the game draws it, in the 640x448 picture.
struct MenuMouseRow {
    float x{}, y{}, width{}, height{};
    guest::GuestAddress parent{};
};

// The selectable rows of one list (or message box), in screen order.
struct MenuMouseList {
    std::vector<MenuMouseRow> rows;
    std::optional<std::size_t> highlighted;
    bool horizontal{false};
    bool values{false}; // option rows with value boxes from x = value_x
    float value_x{};
};

// The lists on screen and whether the footer prompts are shown.
struct MenuMouseScreen {
    std::vector<MenuMouseList> lists;
    bool footer{false};
};

[[nodiscard]] MenuMouseScreen read_menu_mouse_screen(const guest::EeMemory& memory);

// A scripted pointer for test runs (TAKEDOWN_MENU_MOUSE_SCRIPT): entries
// "poll:x,y,buttons[,wheel]" separated by ';', x and y fractions of the
// picture. An entry holds from its controller poll until the next one; its
// wheel steps are given once.
class MenuMouseScript {
  public:
    [[nodiscard]] static std::optional<MenuMouseScript> parse(std::string_view text);
    [[nodiscard]] MenuMouseInput at(std::uint64_t poll);

  private:
    struct Entry {
        std::uint64_t poll{};
        MenuMouseInput input;
    };
    std::vector<Entry> entries_;
    std::uint64_t wheel_given_{~0ULL};
};

class Slus21050MenuMouse {
  public:
    static constexpr guest::GuestAddress kUiContext = 0x01E903E0U;
    static constexpr guest::GuestAddress kRowTextCallback = 0x00371BE0U;
    static constexpr guest::GuestAddress kValueBoxCallback = 0x00380250U;
    static constexpr guest::GuestAddress kFooterIconCallback = 0x00382950U;
    static constexpr float kPictureWidth = 640.0F;
    static constexpr float kPictureHeight = 448.0F;

    // One retail controller poll: the presses to add to the pad's for it.
    [[nodiscard]] platform::HostControllerState poll(const MenuMouseScreen& screen, const MenuMouseInput& input,
                                                     bool title_screen);

    struct Statistics {
        std::uint64_t moves{};   // up, down, left, right
        std::uint64_t confirms{};
        std::uint64_t backs{};
        std::uint64_t starts{};
    };
    [[nodiscard]] const Statistics& statistics() const noexcept { return statistics_; }

  private:
    enum class Action : std::uint8_t { None, Up, Down, Left, Right, Cross, Triangle, Start };
    [[nodiscard]] platform::HostControllerState press(Action action);

    std::uint8_t previous_buttons_{};
    float previous_x_{-1.0F}, previous_y_{-1.0F};
    // The row the pointer asked for (hover or click) and whether to confirm
    // or step its value once it is highlighted.
    std::optional<std::size_t> target_;
    Action after_target_{Action::None};
    std::size_t target_list_rows_{};
    unsigned target_presses_{};
    // A press lasts one poll; the next poll releases it so the game sees an edge.
    bool released_{true};
    // A click that came on a release poll, pressed on the next one.
    std::optional<Action> pending_immediate_;
    int wheel_{};
    Statistics statistics_{};
};

} // namespace takedown::runtime
