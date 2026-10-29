#pragma once

// Runtime owner of the PC options menu: opening and closing it, keeping the
// game from seeing the buttons used in it, and saving the settings file when
// it closes. The caller feeds it the merged host controller state and the
// open/close hotkey once per host input poll, then applies the settings it
// reports as changed (renderer record, frame limits, game switches). No
// device, window or guest access, so it is unit tested directly.

#include "takedown/config/app_settings.hpp"
#include "takedown/platform/parallel_gs_host_presentation.hpp"
#include "takedown/runtime/slus_21050_graphics_options.hpp"
#include "takedown/ui/options_menu.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace takedown::ui {

struct PcOptionsSessionConfig {
    config::AppSettings settings{};
    // Where the menu saves; nullopt keeps changes for this run only.
    std::optional<std::filesystem::path> settings_path{};
    OptionsMenuEnvironment environment{};
    // False keeps the menu closed (for example during an input replay, where
    // host input must not reach or block the game).
    bool enabled{true};
};

struct PcOptionsUpdate {
    bool settings_changed{false};
    bool opened{false};
    bool closed{false};
    // Set when the menu closed with changes and saving them failed.
    std::optional<std::string> save_error{};
};

class PcOptionsSession {
  public:
    explicit PcOptionsSession(PcOptionsSessionConfig config);

    // One host input poll. `toggle_requested` is the keyboard hotkey (F10, or
    // Escape while the menu is shown); Back/Select held with Start toggles it
    // from a controller.
    PcOptionsUpdate update(const OptionsMenuButtons& buttons, bool toggle_requested);

    // Replaces the graphics settings with ones edited elsewhere (the game's own
    // Options menu page, slus_21050_native_pc_options.hpp), saving the settings
    // file when they differ from what is stored.
    PcOptionsUpdate apply_graphics(const config::GraphicsSettings& settings);

    // Remembers the game's own Music and SFX levels (0 to 100) of a loaded
    // profile in the settings file, for the next boot
    // (slus_21050_boot_volume.hpp). Saves only when they changed.
    PcOptionsUpdate remember_game_audio_levels(std::uint32_t music, std::uint32_t effects);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool menu_open() const noexcept { return open_; }
    // True while the menu is open, and after it closes until every button
    // used in it is released, so a closing press never reaches the game.
    [[nodiscard]] bool guest_input_blocked() const noexcept { return open_ || release_pending_; }

    [[nodiscard]] const config::GraphicsSettings& graphics() const noexcept {
        return menu_.settings;
    }
    [[nodiscard]] const OptionsMenu& menu() const noexcept { return menu_; }

    // What the Vulkan renderer should show: picture shape, supersampling,
    // vsync, and the menu as text while it is open.
    [[nodiscard]] platform::ParallelGsHostPresentation host_presentation() const;
    // The options the game implements itself.
    [[nodiscard]] runtime::Slus21050GraphicsOptions::Settings game_options() const noexcept;

  private:
    PcOptionsUpdate close();
    PcOptionsUpdate persist();

    config::AppSettings saved_settings_;
    std::optional<std::filesystem::path> settings_path_;
    OptionsMenu menu_{};
    OptionsMenuEdgeTracker edges_{};
    bool enabled_{true};
    bool open_{false};
    bool release_pending_{false};
};

} // namespace takedown::ui
