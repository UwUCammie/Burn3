#pragma once

// Custom controls (docs/USAGE.md, "Controls and keybinds").
//
// Every player is a DualShock 2 to the game. This module decides which host
// inputs press which DualShock 2 control: keyboard keys (from the renderer
// window or the runtime's own window), mouse buttons, and
// the buttons and axes of up to four gamepads. The bindings live in
// `controls.ini` next to `settings.ini`; a missing file means the defaults,
// which are the original fixed mapping (arrows, Enter, Backspace, Z/X/A/S,
// Q/E, 1/3 and the standard SDL gamepad layout on the first gamepad).
//
// Mapping needs no window or guest memory. SDL supplies portable key names.
// The window code fills
// a RawInputState and asks for each player's controller state.

#include "takedown/platform/window.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace takedown::platform {

// The DualShock 2 controls a binding can press. The stick entries push the
// left analogue stick fully in that direction (a key or button) or by the
// amount of a bound axis.
enum class PadControl : std::uint8_t {
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Start,
    Select,
    Cross,
    Circle,
    Square,
    Triangle,
    L1,
    R1,
    L2,
    R2,
    StickLeft,
    StickRight,
    StickUp,
    StickDown,
    Count,
};
inline constexpr std::size_t kPadControlCount = static_cast<std::size_t>(PadControl::Count);

// Name used in controls.ini ("cross", "stick_left", ...).
[[nodiscard]] std::string_view pad_control_name(PadControl control) noexcept;

enum class InputSourceKind : std::uint8_t {
    Key,         // keyboard scancode (USB HID usage, as SDL scancodes)
    MouseButton, // 1 left, 2 middle, 3 right, 4 and 5 side buttons
    PadButton,   // SDL gamepad button (standard layout)
    PadAxis,     // SDL gamepad axis
};

struct InputSource {
    InputSourceKind kind{InputSourceKind::Key};
    std::uint16_t code{0U};
    // Axes only: +1 binds the positive half, -1 the negative half.
    std::int8_t direction{1};

    bool operator==(const InputSource&) const = default;
};

// "Z", "Up", "Mouse1", "Pad:South", "Pad:LeftX-", "Pad:RightTrigger".
[[nodiscard]] std::optional<InputSource> parse_input_source(std::string_view text);
[[nodiscard]] std::string format_input_source(const InputSource& source);

inline constexpr std::size_t kInputPlayers = 2U;
inline constexpr std::size_t kMaximumGamepads = 4U;

struct PlayerBindings {
    std::array<std::vector<InputSource>, kPadControlCount> controls{};
    // Which connected gamepad this player uses: 1..kMaximumGamepads in the
    // order they were connected, 0 for none.
    std::uint32_t gamepad{0U};
    // Whether this player reads the keyboard and mouse bindings.
    bool keyboard_and_mouse{false};

    bool operator==(const PlayerBindings&) const = default;
};

struct InputBindings {
    std::array<PlayerBindings, kInputPlayers> players{};
    // Gamepad stick dead zone as a fraction of the full range.
    float stick_dead_zone{0.24F};
    // Analogue triggers count as pressed beyond this fraction.
    float trigger_threshold{0.25F};

    bool operator==(const InputBindings&) const = default;
};

// The original fixed mapping: keyboard and mouse plus gamepad 1 for player 1,
// gamepad 2 for player 2.
[[nodiscard]] InputBindings default_input_bindings();

struct LoadedInputBindings {
    InputBindings bindings{};
    bool file_existed{false};
    // One line per problem; the affected entry keeps its default.
    std::vector<std::string> diagnostics;
};

// controls.ini next to settings.ini.
[[nodiscard]] std::filesystem::path
input_bindings_path_for(const std::filesystem::path& settings_path);
[[nodiscard]] LoadedInputBindings load_input_bindings(const std::filesystem::path& path);
[[nodiscard]] std::string format_input_bindings(const InputBindings& bindings);
[[nodiscard]] bool save_input_bindings(const std::filesystem::path& path,
                                       const InputBindings& bindings, std::error_code& error);

inline constexpr std::size_t kScancodeCount = 512U;
inline constexpr std::size_t kGamepadButtonCount = 26U; // SDL_GAMEPAD_BUTTON_COUNT
inline constexpr std::size_t kGamepadAxisCount = 6U;    // SDL_GAMEPAD_AXIS_COUNT

struct RawGamepadState {
    bool connected{false};
    std::bitset<kGamepadButtonCount> buttons{};
    // SDL convention: sticks -32768..32767, triggers 0..32767.
    std::array<std::int16_t, kGamepadAxisCount> axes{};
};

// Everything the host saw this instant.
struct RawInputState {
    std::bitset<kScancodeCount> keys{};
    // Bit n-1 is mouse button n.
    std::uint8_t mouse_buttons{0U};
    std::array<RawGamepadState, kMaximumGamepads> gamepads{};
};

// Player `player`'s DualShock 2 state for this instant.
[[nodiscard]] HostControllerState map_player_input(const InputBindings& bindings,
                                                   std::size_t player,
                                                   const RawInputState& raw) noexcept;

// A connected controller as SDL reports it: XInput pads, pads SDL knows the
// layout of, and joysticks it does not (which need a layout setup before they
// can be a player's gamepad).
struct ConnectedController {
    std::uint32_t id{0U}; // SDL joystick instance, unique while connected
    std::string name;     // the name the device reports
    std::string guid;     // SDL GUID string, the key of a layout mapping
    bool gamepad{false};  // SDL has a layout for it
    // 1..kMaximumGamepads when it is a gamepad slot, 0 otherwise.
    std::uint32_t slot{0U};

    bool operator==(const ConnectedController&) const = default;
};

// The device's own buttons, axes and hats, for building a layout mapping.
struct RawJoystickState {
    bool connected{false};
    std::vector<bool> buttons;
    std::vector<std::int16_t> axes;
    std::vector<std::uint8_t> hats;
};

// Layout mappings made in game, in SDL's "GUID,name,mapping" text form, one
// per line in gamepad_mappings.txt next to settings.ini.
[[nodiscard]] std::filesystem::path
gamepad_mappings_path_for(const std::filesystem::path& settings_path);
[[nodiscard]] std::vector<std::string> load_gamepad_mappings(const std::filesystem::path& path);
// Adds the mapping, replacing an earlier one for the same GUID.
[[nodiscard]] bool save_gamepad_mapping(const std::filesystem::path& path,
                                        std::string_view mapping, std::error_code& error);

// Which gamepad each player actually reads. A player bound to a gamepad slot
// that is empty or has gone quiet follows another connected gamepad as soon
// as it is used, so a controller that reconnected into a different slot (or
// a stale duplicate of one) does not leave the player without input. The
// saved bindings are not changed; a slot another player is bound to or
// reads is never taken, and a player bound to no gamepad (0) keeps none.
struct GamepadFollow {
    // Host milliseconds of each slot's last input (0 for never).
    std::array<std::uint64_t, kMaximumGamepads> last_input_ms{};
    // The slot each player reads (1..kMaximumGamepads), 0 for its binding.
    std::array<std::uint32_t, kInputPlayers> followed{};
};
// How long a player's gamepad must be quiet before another one takes over.
inline constexpr std::uint64_t kGamepadQuietMs = 1000U;

// Whether the gamepad is being used this instant: a button down, a stick
// past the dead zone or a trigger past the threshold.
[[nodiscard]] bool gamepad_has_input(const RawGamepadState& pad, const InputBindings& bindings) noexcept;
// Updates the follow state for this instant (`now_ms` > 0, increasing).
void update_gamepad_follow(GamepadFollow& follow, const InputBindings& bindings, const RawInputState& raw,
                           std::uint64_t now_ms) noexcept;
// The bindings with each player's gamepad slot replaced by the one it reads.
[[nodiscard]] InputBindings followed_bindings(const InputBindings& bindings,
                                              const GamepadFollow& follow) noexcept;

} // namespace takedown::platform
