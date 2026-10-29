#include "takedown/platform/input_bindings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace takedown::platform {
namespace {

std::string trim(std::string_view v) {
    const auto first = v.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = v.find_last_not_of(" \t\r\n");
    return std::string(v.substr(first, last - first + 1U));
}

std::string lower(std::string_view v) {
    std::string out(v);
    for (char& character : out) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return out;
}

// Names used in controls.ini, in PadControl order.
constexpr std::array<std::string_view, kPadControlCount> names{
    "up",
    "right",
    "down",
    "left",
    "start",
    "select",
    "throttle",
    "handbrake",
    "brake",
    "camera",
    "left_shoulder",
    "boost",
    "left_trigger",
    "right_trigger",
    "steer_left",
    "steer_right",
    "stick_up",
    "stick_down",
};

struct NamedCode {
    std::string_view name;
    std::uint16_t code;
};

constexpr NamedCode buttons[] = {
    {"South", SDL_GAMEPAD_BUTTON_SOUTH},
    {"East", SDL_GAMEPAD_BUTTON_EAST},
    {"West", SDL_GAMEPAD_BUTTON_WEST},
    {"North", SDL_GAMEPAD_BUTTON_NORTH},
    {"Back", SDL_GAMEPAD_BUTTON_BACK},
    {"Guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"Start", SDL_GAMEPAD_BUTTON_START},
    {"LeftStick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"RightStick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"LeftShoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"RightShoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"DpadUp", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"DpadRight", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"DpadDown", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"DpadLeft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"Misc1", SDL_GAMEPAD_BUTTON_MISC1},
    {"RightPaddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"LeftPaddle1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"RightPaddle2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"LeftPaddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"Touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"Misc2", SDL_GAMEPAD_BUTTON_MISC2},
    {"Misc3", SDL_GAMEPAD_BUTTON_MISC3},
    {"Misc4", SDL_GAMEPAD_BUTTON_MISC4},
    {"Misc5", SDL_GAMEPAD_BUTTON_MISC5},
    {"Misc6", SDL_GAMEPAD_BUTTON_MISC6},
};

constexpr NamedCode axes[] = {
    {"LeftX", SDL_GAMEPAD_AXIS_LEFTX},
    {"LeftY", SDL_GAMEPAD_AXIS_LEFTY},
    {"RightX", SDL_GAMEPAD_AXIS_RIGHTX},
    {"RightY", SDL_GAMEPAD_AXIS_RIGHTY},
    {"LeftTrigger", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"RightTrigger", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

std::optional<unsigned int> integer(std::string_view s) {
    unsigned int value = 0U;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 10);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

// A control by its game name or by the DualShock 2 name.
std::optional<std::size_t> control(std::string_view text) {
    const std::string key = lower(text);
    for (std::size_t i = 0U; i < kPadControlCount; ++i) {
        if (key == names[i]) {
            return i;
        }
    }
    constexpr std::string_view aliases[kPadControlCount] = {
        "dpad_up", "dpad_right", "dpad_down", "dpad_left", "start", "back",
        "cross",   "circle",     "square",    "triangle",  "l1",    "r1",
        "l2",      "r2",         "stick_left", "stick_right", "stick_up", "stick_down",
    };
    for (std::size_t i = 0U; i < kPadControlCount; ++i) {
        if (key == aliases[i]) {
            return i;
        }
    }
    return std::nullopt;
}

// How far one source is pushed, 0..1. Sticks apply the dead zone; triggers
// and buttons do not.
float amount(const InputSource& source, const RawInputState& raw, const PlayerBindings& player,
             float zone) {
    if (source.kind == InputSourceKind::Key) {
        return player.keyboard_and_mouse && source.code < kScancodeCount && raw.keys[source.code]
                   ? 1.0F
                   : 0.0F;
    }
    if (source.kind == InputSourceKind::MouseButton) {
        return player.keyboard_and_mouse && source.code != 0U && source.code <= 8U &&
                       ((static_cast<unsigned int>(raw.mouse_buttons) >> (source.code - 1)) & 1U) != 0U
                   ? 1.0F
                   : 0.0F;
    }
    if (player.gamepad == 0U) {
        return 0.0F;
    }
    if (player.gamepad > kMaximumGamepads) {
        return 0.0F;
    }
    const auto& pad = raw.gamepads[player.gamepad - 1U];
    if (!pad.connected) {
        return 0.0F;
    }
    if (source.kind == InputSourceKind::PadButton) {
        return source.code < kGamepadButtonCount && pad.buttons[source.code] ? 1.0F : 0.0F;
    }
    if (source.code >= kGamepadAxisCount) {
        return 0.0F;
    }
    const auto v = pad.axes[source.code];
    const bool trigger = source.code >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    const float directed = (trigger ? static_cast<float>(std::max<int>(v, 0)) / 32767.0F
                                    : static_cast<float>(v) / (v < 0 ? 32768.0F : 32767.0F)) *
                           static_cast<float>(source.direction);
    if (trigger) {
        return std::min(std::max(directed, 0.0F), 1.0F);
    }
    return std::min(std::max((directed - zone) / (1.0F - zone), 0.0F), 1.0F);
}

} // namespace

std::string_view pad_control_name(PadControl c) noexcept {
    const auto i = static_cast<std::size_t>(c);
    if (i < names.size()) {
        return names[i];
    }
    return "unknown";
}

std::optional<InputSource> parse_input_source(std::string_view text) {
    auto name = trim(text);
    const auto folded = lower(name);
    if (folded.starts_with("mouse")) {
        const auto n = integer(std::string_view(name).substr(5));
        if (n && *n != 0U && *n <= 8U) {
            return InputSource{InputSourceKind::MouseButton, static_cast<std::uint16_t>(*n), 1};
        }
        return std::nullopt;
    }
    if (folded.starts_with("pad:")) {
        auto key = name.substr(4);
        std::int8_t direction = 1;
        if (!key.empty() && (key.back() == '+' || key.back() == '-')) {
            direction = key.back() == '-' ? -1 : 1;
            key.pop_back();
        }
        for (auto item : buttons) {
            if (lower(key) == lower(item.name)) {
                return InputSource{InputSourceKind::PadButton, item.code, 1};
            }
        }
        for (auto item : axes) {
            if (lower(key) == lower(item.name)) {
                return InputSource{InputSourceKind::PadAxis, item.code, direction};
            }
        }
        return std::nullopt;
    }
    if (folded.starts_with("key:")) {
        name = name.substr(4);
    }
    if (lower(name) == "enter") {
        name = "Return";
    }
    const int scancode = SDL_GetScancodeFromName(name.c_str());
    if (scancode > 0 && scancode < static_cast<int>(kScancodeCount)) {
        return InputSource{InputSourceKind::Key, static_cast<std::uint16_t>(scancode), 1};
    }
    if (lower(name).starts_with("scancode")) {
        const auto n = integer(std::string_view(name).substr(8));
        if (n && *n != 0U && *n < kScancodeCount) {
            return InputSource{InputSourceKind::Key, static_cast<std::uint16_t>(*n), 1};
        }
    }
    return std::nullopt;
}

std::string format_input_source(const InputSource& s) {
    if (s.kind == InputSourceKind::Key) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(s.code));
        // A comma would split the entry in controls.ini, so such keys use the code.
        return name != nullptr && *name != '\0' && std::string_view(name).find(',') == std::string_view::npos
                   ? std::string(name)
                   : "Scancode" + std::to_string(static_cast<int>(s.code));
    }
    if (s.kind == InputSourceKind::MouseButton) {
        return "Mouse" + std::to_string(static_cast<int>(s.code));
    }
    if (s.kind == InputSourceKind::PadButton) {
        for (auto item : buttons) {
            if (item.code == s.code) {
                return "Pad:" + std::string(item.name);
            }
        }
    }
    if (s.kind == InputSourceKind::PadAxis) {
        for (auto item : axes) {
            if (item.code == s.code) {
                return "Pad:" + std::string(item.name) + (s.direction < 0 ? "-" : "+");
            }
        }
    }
    return "Unknown";
}

InputBindings default_input_bindings() {
    InputBindings b;
    constexpr std::string_view keys[kPadControlCount] = {
        "Up", "Right", "Down", "Left", "Return", "Backspace", "Z", "X", "A",
        "S",  "Q",     "E",    "1",    "3",      "",          "",  "",  "",
    };
    constexpr std::string_view pad[kPadControlCount] = {
        "Pad:DpadUp",       "Pad:DpadRight",     "Pad:DpadDown",  "Pad:DpadLeft",
        "Pad:Start",        "Pad:Back",          "Pad:South",     "Pad:East",
        "Pad:West",         "Pad:North",         "Pad:LeftShoulder", "Pad:RightShoulder",
        "",                 "",                  "Pad:LeftX-",    "Pad:LeftX+",
        "Pad:LeftY-",       "Pad:LeftY+",
    };
    for (std::size_t p = 0U; p < kInputPlayers; ++p) {
        b.players[p].gamepad = static_cast<std::uint32_t>(p + 1U);
        b.players[p].keyboard_and_mouse = p == 0U;
        for (std::size_t c = 0U; c < kPadControlCount; ++c) {
            if (p == 0U && !keys[c].empty()) {
                b.players[p].controls[c].push_back(*parse_input_source(keys[c]));
            }
            if (!pad[c].empty()) {
                b.players[p].controls[c].push_back(*parse_input_source(pad[c]));
            }
        }
        // Analogue triggers drive throttle and brake alongside the face buttons.
        b.players[p].controls[6].push_back(*parse_input_source("Pad:RightTrigger"));
        b.players[p].controls[8].push_back(*parse_input_source("Pad:LeftTrigger"));
    }
    b.players[0].controls[4].push_back(*parse_input_source("Keypad Enter"));
    b.players[0].controls[9].push_back(*parse_input_source("Escape"));
    return b;
}

namespace {

void set_buttons(HostControllerState& state, const std::array<bool, 14>& pressed) {
    state.dpad_up = pressed[0];
    state.dpad_right = pressed[1];
    state.dpad_down = pressed[2];
    state.dpad_left = pressed[3];
    state.start = pressed[4];
    state.select = pressed[5];
    state.cross = pressed[6];
    state.circle = pressed[7];
    state.square = pressed[8];
    state.triangle = pressed[9];
    state.left_shoulder = pressed[10];
    state.right_shoulder = pressed[11];
    state.left_trigger = pressed[12];
    state.right_trigger = pressed[13];
}

std::array<bool, 14> digital(const HostControllerState& state) {
    return {state.dpad_up,       state.dpad_right,    state.dpad_down,  state.dpad_left,
            state.start,         state.select,        state.cross,      state.circle,
            state.square,        state.triangle,      state.left_shoulder, state.right_shoulder,
            state.left_trigger,  state.right_trigger};
}

} // namespace

HostControllerState map_player_input(const InputBindings& bindings, std::size_t player,
                                     const RawInputState& raw) noexcept {
    HostControllerState state{};
    if (player >= kInputPlayers) {
        return state;
    }
    const PlayerBindings& player_bindings = bindings.players[player];
    std::array<float, kPadControlCount> values{};
    std::array<bool, 14> pressed{};
    const float zone = std::isfinite(bindings.stick_dead_zone)
                           ? std::min(std::max(bindings.stick_dead_zone, 0.0F), 0.95F)
                           : 0.24F;
    const float threshold = std::isfinite(bindings.trigger_threshold)
                                ? std::min(std::max(bindings.trigger_threshold, 0.01F), 1.0F)
                                : 0.25F;
    for (std::size_t c = 0U; c < kPadControlCount; ++c) {
        for (const InputSource& source : player_bindings.controls[c]) {
            const float a = amount(source, raw, player_bindings, zone);
            values[c] = std::max(values[c], a);
            if (c < pressed.size()) {
                pressed[c] = pressed[c] || (source.kind == InputSourceKind::PadAxis ? a >= threshold : a > 0.0F);
            }
        }
    }
    set_buttons(state, pressed);
    for (std::size_t c = 0U; c < state.button_pressure.size(); ++c) {
        state.button_pressure[c] = static_cast<std::uint8_t>(std::lround(values[c] * 255.0F));
    }
    const auto to_axis = [](float v) {
        const float limited = std::min(std::max(v, -1.0F), 1.0F);
        return static_cast<std::int16_t>(std::lround(limited * (v < 0.0F ? 32768.0F : 32767.0F)));
    };
    state.left_stick_x = to_axis(values[15] - values[14]);
    state.left_stick_y = to_axis(values[17] - values[16]);
    return state;
}

bool gamepad_has_input(const RawGamepadState& pad, const InputBindings& bindings) noexcept {
    if (!pad.connected) {
        return false;
    }
    if (pad.buttons.any()) {
        return true;
    }
    const float zone = std::isfinite(bindings.stick_dead_zone)
                           ? std::min(std::max(bindings.stick_dead_zone, 0.0F), 0.95F)
                           : 0.24F;
    const float threshold = std::isfinite(bindings.trigger_threshold)
                                ? std::min(std::max(bindings.trigger_threshold, 0.01F), 1.0F)
                                : 0.25F;
    for (std::size_t axis = 0U; axis < kGamepadAxisCount; ++axis) {
        const float value = std::abs(static_cast<float>(pad.axes[axis])) / 32767.0F;
        if (value > (axis < 4U ? zone : threshold)) {
            return true;
        }
    }
    return false;
}

void update_gamepad_follow(GamepadFollow& follow, const InputBindings& bindings,
                           const RawInputState& raw, const std::uint64_t now_ms) noexcept {
    for (std::size_t slot = 0U; slot < kMaximumGamepads; ++slot) {
        if (gamepad_has_input(raw.gamepads[slot], bindings)) {
            follow.last_input_ms[slot] = now_ms;
        }
    }
    // The gamepad slot a player reads now: the one it follows, else its own.
    const auto reading = [&](std::size_t other) {
        const unsigned int bound = bindings.players[other].gamepad;
        return follow.followed[other] != 0U ? follow.followed[other] : bound;
    };
    for (std::size_t player = 0U; player < kInputPlayers; ++player) {
        const unsigned int bound = bindings.players[player].gamepad;
        if (bound < 1U || bound > kMaximumGamepads) {
            follow.followed[player] = 0U;
            continue;
        }
        const unsigned long long last = follow.last_input_ms[reading(player) - 1U];
        const auto& pad = raw.gamepads[reading(player) - 1U];
        if (pad.connected && last != 0U && now_ms - last < kGamepadQuietMs) {
            continue;
        }
        for (std::uint32_t slot = 1U; slot <= kMaximumGamepads; ++slot) {
            if (slot == reading(player) || follow.last_input_ms[slot - 1U] != now_ms) {
                continue;
            }
            bool taken = false;
            for (std::size_t other = 0U; other < kInputPlayers; ++other) {
                taken = taken || (other != player && (slot == reading(other) ||
                                                       slot == bindings.players[other].gamepad));
            }
            if (taken) {
                continue;
            }
            follow.followed[player] = slot != bound ? slot : 0U;
            break;
        }
    }
}

InputBindings followed_bindings(const InputBindings& bindings, const GamepadFollow& follow) noexcept {
    InputBindings out = bindings;
    for (std::size_t player = 0U; player < kInputPlayers; ++player) {
        if (follow.followed[player] != 0U && out.players[player].gamepad != 0U) {
            out.players[player].gamepad = follow.followed[player];
        }
    }
    return out;
}

HostControllerState merge_host_controller_states(const HostControllerState& a,
                                                 const HostControllerState& b) noexcept {
    HostControllerState result = a;
    auto da = digital(a);
    auto db = digital(b);
    for (std::size_t i = 0U; i < da.size(); ++i) {
        result.button_pressure[i] = std::max<std::uint8_t>(
            a.button_pressure[i] != 0U ? a.button_pressure[i] : (da[i] ? 255U : 0U),
            b.button_pressure[i] != 0U ? b.button_pressure[i] : (db[i] ? 255U : 0U));
        da[i] = da[i] || db[i];
    }
    set_buttons(result, da);
    if (std::abs(b.left_stick_x) > std::abs(a.left_stick_x)) {
        result.left_stick_x = b.left_stick_x;
    }
    if (std::abs(b.left_stick_y) > std::abs(a.left_stick_y)) {
        result.left_stick_y = b.left_stick_y;
    }
    return result;
}

std::filesystem::path input_bindings_path_for(const std::filesystem::path& settings) {
    return settings.parent_path() / "controls.ini";
}

// RECOVERY: every control, player and gamepad branch matches the original's
// behaviour, but the original's block layout and the temporaries of its
// string handling (key/value construction, the controls.ini split loop) do
// not match GCC's output exactly, so this function is not byte-identical.
LoadedInputBindings load_input_bindings(const std::filesystem::path& path) {
    LoadedInputBindings out;
    out.bindings = default_input_bindings();
    std::ifstream file(path);
    out.file_existed = static_cast<bool>(file);
    if (!file) {
        return out;
    }
    std::string line;
    std::string section;
    unsigned int number = 0U;
    while (std::getline(file, line)) {
        ++number;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(std::string_view(line).substr(1, line.size() - 2U)));
            continue;
        }
        const auto eq = line.find('=');
        const auto key =
            eq != std::string::npos ? lower(trim(std::string_view(line).substr(0, eq))) : std::string();
        const auto value =
            eq != std::string::npos ? trim(std::string_view(line).substr(eq + 1U)) : std::string();
        bool good = false;
        if (section == "input" && eq != std::string::npos &&
            (key == "stick_dead_zone" || key == "trigger_threshold")) {
            float f = 0.0F;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), f);
            if (ec == std::errc{} && ptr == value.data() + value.size() && std::isfinite(f) &&
                f >= 0.0F && f < 1.0F) {
                const bool trigger = key == "trigger_threshold";
                if (!trigger || f >= 0.01F) {
                    const bool stick = key == "stick_dead_zone";
                    if (!stick || f <= 0.95F) {
                        if (stick) {
                            out.bindings.stick_dead_zone = f;
                        } else {
                            out.bindings.trigger_threshold = f;
                        }
                        good = true;
                    }
                }
            }
        } else if ((section == "player1" || section == "player2") && eq != std::string::npos) {
            if (key == "gamepad") {
                auto n = integer(value);
                if (n && *n <= kMaximumGamepads) {
                    out.bindings.players[section == "player1" ? 0U : 1U].gamepad = *n;
                    good = true;
                }
            } else if (key == "keyboard_and_mouse") {
                const bool on = value == "true";
                if (on || value == "false") {
                    out.bindings.players[section == "player1" ? 0U : 1U].keyboard_and_mouse = on;
                    good = true;
                }
            } else {
                auto c = control(key);
                if (c) {
                    std::vector<InputSource> sources;
                    good = true;
                    std::size_t offset = 0U;
                    while (offset < value.size()) {
                        auto comma = value.find(',', offset);
                        auto part = trim(std::string_view(value).substr(
                            offset, comma == std::string::npos ? value.size() - offset : comma - offset));
                        auto source = parse_input_source(part);
                        if (!source || sources.size() >= 8U) {
                            good = false;
                            break;
                        }
                        sources.push_back(*source);
                        if (comma == std::string::npos) {
                            break;
                        }
                        offset = comma + 1U;
                    }
                    if (good) {
                        out.bindings.players[section == "player1" ? 0U : 1U].controls[*c] = std::move(sources);
                    }
                }
            }
        }
        if (!good) {
            out.diagnostics.push_back(path.string() + ":" + std::to_string(number) +
                                      ": invalid control binding; default retained for " + key);
        }
    }
    return out;
}

std::string format_input_bindings(const InputBindings& b) {
    std::ostringstream out;
    out << "# Takedown native controls; comma separates alternate sources. Empty unbinds.\n"
           "# Keyboard keys, Mouse1..Mouse8, Pad:South, Pad:LeftX-, Pad:RightTrigger.\n"
           "[input]\nstick_dead_zone = "
        << b.stick_dead_zone << "\ntrigger_threshold = " << b.trigger_threshold << '\n';
    for (std::size_t p = 0U; p < kInputPlayers; ++p) {
        const PlayerBindings& player = b.players[p];
        out << "\n[player" << p + 1U << "]\ngamepad = " << player.gamepad
            << "\nkeyboard_and_mouse = " << (player.keyboard_and_mouse ? "true" : "false") << '\n';
        for (std::size_t c = 0U; c < kPadControlCount; ++c) {
            out << names[c] << " = ";
            for (std::size_t i = 0U; i < player.controls[c].size(); ++i) {
                if (i != 0U) {
                    out << ", ";
                }
                out << format_input_source(player.controls[c][i]);
            }
            out << '\n';
        }
    }
    return out.str();
}

namespace {

// Writes next to the target and swaps it in, so a crash never leaves a
// half-written file.
bool replace_file(const std::filesystem::path& path, const std::string& text, std::error_code& error) {
    error.clear();
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        return false;
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        if (!file) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
}

} // namespace

bool save_input_bindings(const std::filesystem::path& path, const InputBindings& b,
                         std::error_code& error) {
    return replace_file(path, format_input_bindings(b), error);
}

std::filesystem::path gamepad_mappings_path_for(const std::filesystem::path& settings) {
    return settings.parent_path() / "gamepad_mappings.txt";
}

std::vector<std::string> load_gamepad_mappings(const std::filesystem::path& path) {
    std::vector<std::string> out;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty() && line.front() != '#' && line.find(',') != std::string::npos) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

namespace {

// The GUID is the first field of a mapping line.
std::string mapping_guid(std::string_view mapping) {
    return lower(mapping.substr(0, mapping.find(',')));
}

} // namespace

// RECOVERY: same result as the original; the original constructs the GUID
// directly into its local, which this form does not reproduce exactly.
bool save_gamepad_mapping(const std::filesystem::path& path, std::string_view mapping,
                          std::error_code& error) {
    auto mappings = load_gamepad_mappings(path);
    const auto guid = mapping_guid(mapping);
    std::erase_if(mappings, [&](const std::string& existing) { return mapping_guid(existing) == guid; });
    mappings.emplace_back(mapping);
    std::string text = "# Controller layouts made with SET UP CONTROLLER (SDL GUID,name,mapping).\n";
    for (const auto& entry : mappings) {
        text += entry;
        text += '\n';
    }
    return replace_file(path, text, error);
}

} // namespace takedown::platform
