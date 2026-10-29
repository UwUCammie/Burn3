#include "takedown/ui/controls_session.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace takedown::ui {

namespace {

// SDL target name, what to press, and whether it is read as an axis.
enum class StepKind : std::uint8_t { Button, Axis };

struct LayoutStep {
    const char* target;
    const char* prompt;
    StepKind kind;
};

constexpr LayoutStep layout_steps[] = {
    {"a", "PRESS CROSS / A (BOTTOM)", StepKind::Button},
    {"b", "PRESS CIRCLE / B (RIGHT)", StepKind::Button},
    {"x", "PRESS SQUARE / X (LEFT)", StepKind::Button},
    {"y", "PRESS TRIANGLE / Y (TOP)", StepKind::Button},
    {"leftshoulder", "PRESS L1 / LB", StepKind::Button},
    {"rightshoulder", "PRESS R1 / RB", StepKind::Button},
    {"lefttrigger", "PULL L2 / LT", StepKind::Button},
    {"righttrigger", "PULL R2 / RT", StepKind::Button},
    {"back", "PRESS SELECT / BACK", StepKind::Button},
    {"start", "PRESS START", StepKind::Button},
    {"guide", "PRESS HOME / GUIDE", StepKind::Button},
    {"leftstick", "CLICK THE LEFT STICK", StepKind::Button},
    {"rightstick", "CLICK THE RIGHT STICK", StepKind::Button},
    {"dpup", "PRESS D-PAD UP", StepKind::Button},
    {"dpdown", "PRESS D-PAD DOWN", StepKind::Button},
    {"dpleft", "PRESS D-PAD LEFT", StepKind::Button},
    {"dpright", "PRESS D-PAD RIGHT", StepKind::Button},
    {"leftx", "PUSH THE LEFT STICK RIGHT", StepKind::Axis},
    {"lefty", "PUSH THE LEFT STICK DOWN", StepKind::Axis},
    {"rightx", "PUSH THE RIGHT STICK RIGHT", StepKind::Axis},
    {"righty", "PUSH THE RIGHT STICK DOWN", StepKind::Axis},
};

// Row labels, indexed by PadControl.
constexpr std::array<const char*, platform::kPadControlCount> labels = {{
    "MENU UP",    "MENU RIGHT",         "MENU DOWN",  "MENU LEFT",    "PAUSE / START",
    "SELECT",     "THROTTLE / CONFIRM", "HANDBRAKE",  "BRAKE",        "CAMERA / BACK",
    "LOOK BACK",  "BOOST",              "NEXT MUSIC TRACK", "CRASHBREAKER", "STEER LEFT",
    "STEER RIGHT", "STICK UP",          "STICK DOWN",
}};

// An axis at rest and as it reads now.
struct AxisReading {
    std::int16_t rest;
    std::int16_t now;
};

// Pad sources belong to the controller side of the editor.
bool is_pad(platform::InputSourceKind kind) {
    return kind == platform::InputSourceKind::PadButton || kind == platform::InputSourceKind::PadAxis;
}

// Players are shown from 1.
std::string player_number(std::size_t player) {
    return std::to_string(player + 1U);
}

// Long device names would overflow the controls list.
std::string short_name(const std::string& name) {
    return name.size() <= 40U ? name : name.substr(0U, 37U) + "...";
}

// True while the device sits at its resting state (nothing pressed, every
// axis within a small band of the baseline).
bool joystick_neutral(const platform::RawJoystickState& now, const platform::RawJoystickState& rest) {
    if (std::any_of(now.buttons.begin(), now.buttons.end(), [](bool down) { return down; })) {
        return false;
    }
    if (std::any_of(now.hats.begin(), now.hats.end(), [](std::uint8_t hat) { return hat != 0U; })) {
        return false;
    }
    for (std::size_t i = 0U; i < now.axes.size() && i < rest.axes.size(); ++i) {
        if (std::abs(static_cast<int>(now.axes[i]) - static_cast<int>(rest.axes[i])) > 7999) {
            return false;
        }
    }
    return true;
}

} // namespace

ControlsSession::ControlsSession(std::filesystem::path path, std::filesystem::path mappings_path)
    : path_(std::move(path)), mappings_path_(std::move(mappings_path)) {
    auto loaded = platform::load_input_bindings(path_);
    bindings_ = std::move(loaded.bindings);
    if (!loaded.diagnostics.empty()) {
        notice_ = loaded.diagnostics.front();
    }
}

const platform::ConnectedController* ControlsSession::chosen_controller() const {
    for (const auto& controller : controllers_) {
        if (controller.id == chosen_) {
            return &controller;
        }
    }
    return nullptr;
}

void ControlsSession::set_controllers(const std::vector<platform::ConnectedController>& controllers) {
    if (controllers == controllers_) {
        return;
    }
    controllers_ = controllers;
    for (const auto& controller : controllers_) {
        if (!controller.gamepad && announced_.insert(controller.id).second) {
            chosen_ = controller.id;
            notice_ = short_name(controller.name) + " needs a layout before it can be used. Use SET UP CONTROLLER.";
        }
    }
    if (chosen_controller() == nullptr) {
        chosen_ = controllers_.empty() ? 0U : controllers_.front().id;
    }
}
// RECOVERY: rows: structure and calls match; one branch probability (value.empty(), 33% vs 50%) and inlining counts differ.

std::vector<std::pair<std::string, std::string>> ControlsSession::rows() const {
    const auto& player = bindings_.players[player_];
    std::string gamepad = "None";
    if (player.gamepad != 0U) {
        gamepad = std::to_string(player.gamepad) + ": Not connected";
        for (const auto& controller : controllers_) {
            if (controller.slot == player.gamepad) {
                gamepad = std::to_string(player.gamepad) + ": " + short_name(controller.name);
            }
        }
    }
    std::string setup = "No controller found";
    if (const auto* controller = chosen_controller()) {
        setup = short_name(controller->name) + (controller->gamepad ? "" : " (needs setup)");
    }
    std::vector<std::pair<std::string, std::string>> out{
        {"PLAYER", player_number(player_)},
        {"EDIT INPUT", controller_ ? "Controller" : "Keyboard & Mouse"},
        {"GAMEPAD", gamepad},
        {"KEYBOARD & MOUSE", player.keyboard_and_mouse ? "On" : "Off"},
        {"SET UP CONTROLLER", layout_ ? "Setting up..." : setup},
        {"BIND ALL ACTIONS", guided_ ? "Binding..." : controller_ ? "Controller" : "Keyboard & Mouse"},
    };
    for (std::size_t c = 0U; c < platform::kPadControlCount; ++c) {
        std::string value;
        if (capture_ && static_cast<std::size_t>(*capture_) == c) {
            value = "Press a control...";
        } else {
            for (const auto& source : player.controls[c]) {
                if (is_pad(source.kind) == controller_) {
                    if (!value.empty()) {
                        value += " / ";
                    }
                    auto text = platform::format_input_source(source);
                    if (text.starts_with("Pad:")) {
                        text.erase(0U, 4U);
                    }
                    value += text;
                }
            }
        }
        if (value.empty()) {
            value = std::string{"Unbound"};
        }
        out.emplace_back(labels[c], std::move(value));
    }
    out.emplace_back("RESTORE PLAYER DEFAULTS", "");
    return out;
}

std::string ControlsSession::capture_prompt() const {
    if (layout_) {
        if (!layout_->have_baseline) {
            return "RELEASE EVERY BUTTON AND STICK ON THE CONTROLLER";
        }
        return std::to_string(layout_->step + 1U) + "/" + std::to_string(std::size(layout_steps)) + " " +
               layout_steps[layout_->step].prompt + ". ESC STOP, BKSP SKIP";
    }
    if (!capture_) {
        return {};
    }
    if (guided_) {
        return std::to_string(static_cast<std::size_t>(*capture_) + 1U) + "/" +
               std::to_string(platform::kPadControlCount) + " SET " + labels[static_cast<std::size_t>(*capture_)] +
               ". ESC STOP, BKSP SKIP";
    }
    return "PRESS A CONTROL TO ASSIGN IT.\nESC: CANCEL   BACKSPACE: CLEAR";
}


std::optional<std::uint32_t> ControlsSession::layout_device() const {
    if (!layout_) {
        return std::nullopt;
    }
    return layout_->device.id;
}

bool ControlsSession::persist() {
    std::error_code error;
    if (!platform::save_input_bindings(path_, bindings_, error)) {
        notice_ = "Controls could not be saved: " + error.message();
        return false;
    }
    notice_.clear();
    return true;
}

// RECOVERY: activate: structure matches; GCC inlines the string _M_assign/_M_replace helpers differently.
void ControlsSession::activate(std::size_t row, int direction, const platform::RawInputState&) {
    if (capturing() || (row > kSetupRow && direction != 0)) {
        return;
    }
    const int step = direction < 0 ? -1 : 1;
    if (row == 0U) {
        player_ = (player_ + 1U) % platform::kInputPlayers;
    } else if (row == 1U) {
        controller_ = !controller_;
    } else if (row == 2U) {
        // 0 is no gamepad, then the slots in the order they connected.
        const int slots = static_cast<int>(platform::kMaximumGamepads) + 1;
        auto& gamepad = bindings_.players[player_].gamepad;
        gamepad = static_cast<std::uint32_t>((static_cast<int>(gamepad) + step + slots) % slots);
        changed_ = true;
        persist();
    } else if (row == 3U) {
        bindings_.players[player_].keyboard_and_mouse = !bindings_.players[player_].keyboard_and_mouse;
        changed_ = true;
        persist();
    } else if (row == kSetupRow) {
        if (controllers_.empty()) {
            notice_ = "No controller is connected.";
        } else {
            if (direction != 0) {
                std::size_t at = 0U;
                while (at < controllers_.size() && controllers_[at].id != chosen_) {
                    ++at;
                }
                at = at == controllers_.size() ? 0U : (at + controllers_.size() + static_cast<std::size_t>(step)) % controllers_.size();
                chosen_ = controllers_[at].id;
            }
            if (const auto* chosen = chosen_controller()) {
                layout_.emplace().device = *chosen;
            }
            armed_ = false;
        }
    } else if (row == kBindAllRow) {
        guided_ = true;
        capture_ = platform::PadControl::DpadUp;
        armed_ = false;
    } else if (row < kFirstControlRow + platform::kPadControlCount) {
        capture_ = static_cast<platform::PadControl>(row - kFirstControlRow);
        armed_ = false;
    } else if (row == kFirstControlRow + platform::kPadControlCount) {
        bindings_.players[player_] = platform::default_input_bindings().players[player_];
        changed_ = true;
        persist();
    }
}

bool ControlsSession::selected_device_neutral(const platform::RawInputState& raw) const {
    if (!controller_) {
        return !raw.keys.any() && raw.mouse_buttons == 0U;
    }
    const unsigned int slot = bindings_.players[player_].gamepad;
    if (slot == 0U) {
        return true;
    }
    if (slot > platform::kMaximumGamepads) {
        return true;
    }
    const auto& pad = raw.gamepads[slot - 1U];
    return !pad.buttons.any() &&
           std::all_of(pad.axes.begin(), pad.axes.end(), [](std::int16_t axis) { return std::abs(axis) <= 8191; });
}

void ControlsSession::advance_guided() {
    const unsigned long long next = static_cast<std::size_t>(*capture_) + 1U;
    armed_ = false;
    if (next < platform::kPadControlCount) {
        capture_ = static_cast<platform::PadControl>(next);
        return;
    }
    capture_.reset();
    guided_ = false;
    notice_ = "Every action is set for player " + player_number(player_) + ".";
}

void ControlsSession::advance_layout() {
    armed_ = false;
    auto& setup = *layout_;
    if (++setup.step < std::size(layout_steps)) {
        return;
    }
    const auto name = setup.device.name;
    if (setup.bound.empty()) {
        notice_ = "Nothing was assigned, so the layout of " + short_name(name) + " is unchanged.";
    } else {
        // SDL mapping lines are comma separated, so a comma in the name would split it.
        std::string label = name;
        std::replace(label.begin(), label.end(), ',', ' ');
        std::string mapping = setup.device.guid + "," + label + ",";
        for (const auto& [target, binding] : setup.bound) {
            mapping += target + ":" + binding + ",";
        }
        layout_mapping_ = mapping;
        applying_mapping_ = std::move(mapping);
        applying_name_ = name;
    }
    layout_.reset();
    swallow_release_ = true;
}

void ControlsSession::layout_applied(bool applied, const std::string& error) {
    const auto name = short_name(std::exchange(applying_name_, {}));
    const auto mapping = std::exchange(applying_mapping_, {});
    if (!applied) {
        notice_ = "The layout for " + name + " could not be used: " + error;
        return;
    }
    std::error_code saved;
    if (!mappings_path_.empty() && !platform::save_gamepad_mapping(mappings_path_, mapping, saved)) {
        notice_ = "The layout works for now but could not be saved: " + saved.message();
        return;
    }
    notice_ = "Layout saved for " + name + ". Choose it under GAMEPAD.";
}
// RECOVERY: poll_layout: structure matches; one string operator= inlining decision differs.
void ControlsSession::poll_layout(const platform::RawInputState& raw, const platform::RawJoystickState& joy) {
    auto& setup = *layout_;
    const auto stop = [this](std::string message) {
        layout_.reset();
        swallow_release_ = true;
        notice_ = std::move(message);
    };
    if (!joy.connected) {
        stop(short_name(setup.device.name) + " was disconnected, so its setup stopped.");
        return;
    }
    const bool free = !raw.keys[41U] && !raw.keys[42U];
    if (!setup.have_baseline) {
        // The first neutral reading is what every later change is measured against.
        if (free && joystick_neutral(joy, joy)) {
            setup.baseline = joy;
            setup.have_baseline = true;
            armed_ = true;
        }
        return;
    }
    if (!armed_) {
        armed_ = free && joystick_neutral(joy, setup.baseline);
        return;
    }
    if (raw.keys[41U]) {
        stop("Controller setup stopped. Nothing was saved.");
        return;
    }
    if (raw.keys[42U]) {
        advance_layout();
        return;
    }
    const auto& step = layout_steps[setup.step];
    std::string key;
    std::string binding;
    for (std::size_t b = 0U; b < joy.buttons.size() && key.empty(); ++b) {
        if (joy.buttons[b]) {
            key = binding = "b" + std::to_string(b);
        }
    }
    for (std::size_t h = 0U; h < joy.hats.size() && key.empty(); ++h) {
        const auto v = joy.hats[h];
        if (v == 1U || v == 2U || v == 4U || v == 8U) {
            key = binding = "h" + std::to_string(h) + "." + std::to_string(static_cast<int>(v));
        }
    }
    bool axis = false;
    if (key.empty()) {
        // The axis that moved furthest from its resting value, past a threshold.
        int best = 16000;
        std::size_t moved = joy.axes.size();
        for (std::size_t a = 0U; a < joy.axes.size() && a < setup.baseline.axes.size(); ++a) {
            const int d = std::abs(static_cast<int>(joy.axes[a]) - static_cast<int>(setup.baseline.axes[a]));
            if (d > best) {
                best = d;
                moved = a;
            }
        }
        if (moved < joy.axes.size()) {
            const auto n = std::to_string(moved);
            const auto [rest, now] = AxisReading{setup.baseline.axes[moved], joy.axes[moved]};
            if (step.kind == StepKind::Axis) {
                key = "a" + n;
                binding = rest < now ? "a" + n : "a" + n + "~";
            } else if (static_cast<unsigned int>(rest) + 16000U > 32000U) {
                // A trigger that rests at an extreme.
                key = "a" + n;
                binding = rest < 0 ? "a" + n : "a" + n + "~";
            } else {
                key = binding = (rest < now ? "+a" : "-a") + n;
            }
            axis = true;
        }
    }
    if (key.empty()) {
        return;
    }
    if (setup.used.contains(key)) {
        advance_layout();
        return;
    }
    if (step.kind == StepKind::Axis && !axis) {
        return;
    }
    setup.used.insert(key);
    setup.bound.emplace_back(step.target, binding);
    advance_layout();
}

bool ControlsSession::poll(const platform::RawInputState& raw, const platform::RawJoystickState& joystick) {
    const bool changed = std::exchange(changed_, false);
    if (layout_) {
        poll_layout(raw, joystick);
        return changed;
    }
    if (!capture_) {
        if (swallow_release_ && selected_device_neutral(raw) && !raw.keys.any() && raw.mouse_buttons == 0U) {
            swallow_release_ = false;
        }
        return changed;
    }
    if (!armed_) {
        // Wait for the control that started the capture to be let go.
        armed_ = selected_device_neutral(raw) && !raw.keys[41U] && !raw.keys[42U];
        return changed;
    }
    if (raw.keys[41U]) {
        capture_.reset();
        guided_ = false;
        swallow_release_ = true;
        return changed;
    }
    const bool clear = raw.keys[42U];
    std::optional<platform::InputSource> source;
    if (!controller_) {
        for (std::size_t k = 1U; k < platform::kScancodeCount && !source; ++k) {
            if (raw.keys[k]) {
                source = platform::InputSource{platform::InputSourceKind::Key, static_cast<std::uint16_t>(k), 1};
            }
        }
        for (unsigned int m = 1U; m <= 8U && !source; ++m) {
            if (((static_cast<unsigned int>(raw.mouse_buttons) >> (m - 1U)) & 1U) != 0U) {
                source = platform::InputSource{platform::InputSourceKind::MouseButton, static_cast<std::uint16_t>(m), 1};
            }
        }
    } else {
        const unsigned int slot = bindings_.players[player_].gamepad;
        if (slot != 0U) {
            if (slot <= platform::kMaximumGamepads) {
                const auto& pad = raw.gamepads[slot - 1U];
                if (pad.connected) {
                    for (std::size_t b = 0U; b < platform::kGamepadButtonCount && !source; ++b) {
                        if (pad.buttons[b]) {
                            source = platform::InputSource{platform::InputSourceKind::PadButton, static_cast<std::uint16_t>(b), 1};
                        }
                    }
                    for (std::size_t a = 0U; a < platform::kGamepadAxisCount && !source; ++a) {
                        if (std::abs(pad.axes[a]) > 18000) {
                            source = platform::InputSource{platform::InputSourceKind::PadAxis, static_cast<std::uint16_t>(a),
                                                           static_cast<std::int8_t>(pad.axes[a] < 0 ? -1 : 1)};
                        }
                    }
                }
            }
        }
    }
    if (clear || source) {
        if (!guided_ || !clear) {
            auto& controls = bindings_.players[player_].controls[static_cast<std::size_t>(*capture_)];
            // Only sources of the device being captured are replaced; the other device keeps its own.
            std::erase_if(controls, [this](const platform::InputSource& existing) {
                return is_pad(existing.kind) == controller_;
            });
            if (!clear) {
                controls.push_back(*source);
            }
            persist();
        }
        if (guided_) {
            advance_guided();
        } else {
            capture_.reset();
        }
        swallow_release_ = true;
        return true;
    }
    return changed;
}
} // namespace takedown::ui
