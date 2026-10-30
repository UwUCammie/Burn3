#pragma once

// The SETTINGS page of the supported US disc: one page with the tabs Audio,
// Camera, Controls, Gameplay and Video, opened from the Options hub and the
// pause menu (Slus21050NativePcOptions makes the game page). While it is the
// game's current page, this class reads every setting it shows (the profile's
// audio and gameplay bytes in guest memory, the PC graphics settings and the
// controls editor's rows), turns host input into edits with ui::SettingsMenu,
// writes the edits back and draws the page as host UI.

#include "takedown/config/app_settings.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/platform/parallel_gs_host_ui.hpp"
#include "takedown/platform/window.hpp"
#include "takedown/ui/settings_menu.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

class Slus21050NativePcOptions;

namespace slus_21050_settings {
// The profile's audio and gameplay settings, as the retail pages edit them.
inline constexpr guest::GuestAddress kAudioOutput = 0x004F5073U;   // 0 Mono, 1 Stereo, 2 Dolby Pro Logic II
inline constexpr guest::GuestAddress kEffectsVolume = 0x004F5074U; // 0..100 in steps of 5
inline constexpr guest::GuestAddress kRadioVolume = 0x004F5075U;   // 0..100 in steps of 5
inline constexpr guest::GuestAddress kDj = 0x004F5079U;            // 0 Off, 1 On
inline constexpr guest::GuestAddress kTakedownCamera = 0x004F507BU; // 0 Off, 1 On
inline constexpr guest::GuestAddress kImpactTime = 0x004F507CU;    // 0 Manual, 1 Automatic
inline constexpr std::uint32_t kVolumeStep = 5U;
inline constexpr std::uint32_t kVolumeLevels = 21U;
} // namespace slus_21050_settings

struct SettingsPageInput {
    platform::HostControllerState pad;
    // The pointer as a fraction of the game picture, when it is over the window.
    std::optional<std::array<float, 2>> pointer;
    std::uint8_t mouse_buttons{}; // bit 0 left
    int wheel{};
    float seconds{1.0f / 60.0f};
};

struct SettingsPageControls {
    std::vector<std::pair<std::string, std::string>> rows;
    bool capturing{false};
    std::string prompt;
    std::string notice;
};

struct SettingsPageResult {
    bool active{false};
    bool graphics_changed{false};
    // A controls editor row run with a direction (-1 left, 0 confirm, 1 right).
    std::optional<std::pair<std::size_t, int>> controls_action;
};

class Slus21050SettingsPage {
  public:
    // One retail poll. `settings` is edited in place; the caller saves and
    // applies it when graphics_changed is set.
    SettingsPageResult update(guest::EeMemory& memory, Slus21050NativePcOptions& page,
                              config::GraphicsSettings& settings, const SettingsPageControls& controls,
                              const SettingsPageInput& input);

    [[nodiscard]] bool active() const noexcept { return active_; }
    // The page as host UI; `ui_directory` holds the imported fonts and artwork.
    [[nodiscard]] platform::ParallelGsHostUi host_ui(const std::string& ui_directory) const;

    // The page's rows for these settings (also used by tests).
    [[nodiscard]] static ui::SettingsPageContent content(const guest::EeMemory& memory,
                                                        const config::GraphicsSettings& settings,
                                                        const SettingsPageControls& controls);

  private:
    ui::SettingsMenu menu_;
    bool active_{false};
    ui::SettingsTab last_tab_{ui::SettingsTab::Audio};
    std::optional<std::array<float, 2>> last_pointer_;
    bool left_button_held_{true};
    std::optional<std::chrono::steady_clock::time_point> last_poll_;
};

} // namespace takedown::runtime
