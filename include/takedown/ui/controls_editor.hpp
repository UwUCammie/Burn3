#pragma once
#include "takedown/platform/input_bindings.hpp"
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace takedown::ui {
// Native control editing, independent of the original UI presenter. An action
// can have any number of controls (buttons, triggers, keys): Confirm on its
// row captures one control to add it, or remove it when it is already bound.
// Controls from before this editor are read and written unchanged.
class ControlsEditor {
  public:
    // Rows before the per-action rows: player, edit input, gamepad,
    // keyboard and mouse, set up controller, bind all actions.
    static constexpr std::size_t kSetupRow = 4;
    static constexpr std::size_t kBindAllRow = 5;
    static constexpr std::size_t kFirstControlRow = 6;

    // `mappings_path` keeps layouts made with SET UP CONTROLLER; empty keeps
    // them for this run only.
    explicit ControlsEditor(std::filesystem::path path, std::filesystem::path mappings_path = {});
    [[nodiscard]] const platform::InputBindings& bindings() const { return bindings_; }
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> rows() const;
    [[nodiscard]] bool capturing() const { return capture_.has_value() || layout_.has_value(); }
    [[nodiscard]] bool blocks_game_input() const { return capturing() || swallow_release_; }
    [[nodiscard]] const std::string& notice() const { return notice_; }
    // What the capture popup asks for; empty when not capturing.
    [[nodiscard]] std::string capture_prompt() const;
    // The connected controllers, as the host window lists them.
    void set_controllers(const std::vector<platform::ConnectedController>& controllers);
    void activate(std::size_t row, int direction, const platform::RawInputState& raw);
    // true when bindings changed; capture waits for the initiating control to
    // be released before accepting a new key, button or half-axis. During a
    // controller setup `joystick` is that device's own inputs.
    bool poll(const platform::RawInputState& raw, const platform::RawJoystickState& joystick = {});
    // The device a controller setup is reading, for joystick_state().
    [[nodiscard]] std::optional<std::uint32_t> layout_device() const;
    // A finished setup's SDL mapping, for the host window to apply; then
    // report the outcome with layout_applied().
    [[nodiscard]] std::optional<std::string> take_layout_mapping() { return std::exchange(layout_mapping_, std::nullopt); }
    void layout_applied(bool applied, const std::string& error);

  private:
    struct LayoutSetup {
        platform::ConnectedController device;
        std::size_t step{};
        bool have_baseline{};
        platform::RawJoystickState baseline;
        std::vector<std::pair<std::string, std::string>> bound; // SDL target, binding
        std::set<std::string> used;
    };
    bool persist();
    bool selected_device_neutral(const platform::RawInputState& raw) const;
    void poll_layout(const platform::RawInputState& raw, const platform::RawJoystickState& joystick);
    void advance_layout();
    void advance_guided();
    [[nodiscard]] const platform::ConnectedController* chosen_controller() const;
    std::filesystem::path path_;
    std::filesystem::path mappings_path_;
    platform::InputBindings bindings_;
    std::vector<platform::ConnectedController> controllers_;
    std::set<std::uint32_t> announced_;
    std::uint32_t chosen_{};
    std::size_t player_{};
    bool controller_{true};
    std::optional<platform::PadControl> capture_;
    bool guided_{};
    std::optional<LayoutSetup> layout_;
    std::optional<std::string> layout_mapping_;
    // The mapping handed to the host and the device it is for, until applied.
    std::string applying_mapping_;
    std::string applying_name_;
    bool armed_{};
    bool changed_{};
    bool swallow_release_{};
    std::string notice_;
};
} // namespace takedown::ui
