#pragma once

// The online player count next to ONLINE on the main menu of the supported US
// disc. Each frame it finds the ONLINE row of the main menu's list (its text
// box and opacity, which the game animates when the menu slides in and when
// the row is highlighted) and draws the count's badge after it as host UI.
// The count is fetched only while the main menu is showing.

#include "takedown/guest/types.hpp"
#include "takedown/net/online_player_count.hpp"
#include "takedown/platform/parallel_gs_host_ui.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace takedown::guest {
class EeMemory;
} // namespace takedown::guest

namespace takedown::runtime {

namespace slus_21050_online_count {
// The main menu's list widget and the main menu page (cursor at +20).
inline constexpr guest::GuestAddress kMainMenuList = 0x01E89544U;
inline constexpr guest::GuestAddress kMainMenuPage = 0x01E94C40U;
inline constexpr guest::GuestAddress kCurrentPage = 0x01E90430U;
inline constexpr guest::GuestAddress kCurrentPhase = 0x01E90424U;
inline constexpr std::uint32_t kOnlineRow = 3U;
} // namespace slus_21050_online_count

class Slus21050OnlineCount {
  public:
    // Once per frame.
    void update(const guest::EeMemory& memory);
    [[nodiscard]] bool visible() const noexcept { return row_.has_value(); }
    // Empty while the main menu is not showing.
    [[nodiscard]] platform::ParallelGsHostUi host_ui(const std::string& ui_directory) const;

  private:
    struct Row {
        float x, y, width, height, alpha;
    };
    std::optional<Row> row_;
    // When the ONLINE row came on screen: the badge then slides and fades in
    // after it (the host UI reaches the picture a frame or two after the game's).
    std::optional<std::chrono::steady_clock::time_point> shown_since_;
    float entrance_{0.0f}; // 0 hidden .. 1 in place
    net::OnlinePlayerCount count_;
};

} // namespace takedown::runtime
