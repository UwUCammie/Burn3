#pragma once

// How many players are online: everyone connected to the matchmaking service,
// in the lobby or in a game, as the site's public game directory reports it
// (https://uwucammie.com/burn3/api/games, "players"). A background thread asks
// while the count is wanted (the main menu is showing) and at most every
// kRefreshSeconds; nothing is sent but the request itself.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace takedown::net {

class OnlinePlayerCount {
  public:
    static constexpr int kRefreshSeconds = 20;

    OnlinePlayerCount() = default;
    ~OnlinePlayerCount();
    OnlinePlayerCount(const OnlinePlayerCount&) = delete;
    OnlinePlayerCount& operator=(const OnlinePlayerCount&) = delete;

    // Call every poll; the first wanted call starts the thread.
    void set_wanted(bool wanted);
    // The latest count, or nullopt before the first answer (or after the
    // service stopped answering).
    [[nodiscard]] std::optional<std::uint32_t> count() const noexcept;

    // The "players" member of a game directory response (also used by tests).
    [[nodiscard]] static std::optional<std::uint32_t> parse_players(std::string_view json) noexcept;

  private:
    void run();

    std::atomic<bool> wanted_{false};
    std::atomic<std::int64_t> count_{-1};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread thread_;
};

} // namespace takedown::net
