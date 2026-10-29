#pragma once
#include "takedown/content/json.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace takedown::content { class ModManager; }

namespace takedown::net {
inline constexpr std::uint32_t kOnlineWireVersion = 1;
enum class OnlineMode { Race, RoadRage, TeamCrush, DoubleImpact, PartyCrash, Original };
[[nodiscard]] const char* online_mode_name(OnlineMode mode) noexcept;
[[nodiscard]] unsigned online_mode_capacity(OnlineMode mode) noexcept;
struct MatchmakingConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{21800};
    std::string player_name;
    // The worker scans and hashes the installed files before sending hello.
    // Never trust a user-supplied manifest version or cached digest.
    std::filesystem::path mods_directory;
    std::filesystem::path game_root;
    bool mods_enabled{true};
    bool browser_signin{};
    // Empty selects the per-user encrypted account cache; tests may isolate it.
    std::filesystem::path session_cache;
    std::shared_ptr<const content::ModManager> applied_mods;
    // Called on the game thread before a mismatching room join. The UI owns
    // consent, download and explicit Mods Refresh; admission is never bypassed.
    std::function<void(const content::JsonValue&)> mod_sync_required;
};
struct MatchRequest {
    enum class Kind {
        List,
        Create,
        Join,
        QuickMatch,
        Leave,
        Ready,
        Start,
        Finish,
        Update,
        Chat,
        Service
    };
    Kind kind{Kind::List};
    OnlineMode mode{OnlineMode::Race};
    std::string name;
    std::string track;
    std::uint32_t room{};
    std::uint32_t after{};
    bool ready{};
    // Zero retains the simple native-client protocol. Original menus use a
    // correlation ID so room broadcasts cannot complete a different command.
    std::uint32_t request_id{};
    unsigned capacity{};
    std::uint32_t system_flags{};
    std::string parameters;
    std::optional<std::string> user_parameters;
    std::optional<std::uint32_t> updated_system_flags;
    std::string password;
    std::string message, target, attributes, operation;
    std::map<std::string, std::string> fields;
};
struct MatchEvent {
    enum class Kind {
        Connected,
        Rooms,
        RoomChanged,
        RoomClosed,
        Started,
        Left,
        Rejected,
        Disconnected,
        Updated,
        BrowserSignIn,
        Chat,
        Service,
        Heartbeat
    };
    Kind kind{};
    content::JsonValue payload;
    std::string detail;
};
// Nonblocking from the game thread. A worker owns host TCP, framing and
// keepalives. The wire version and release version come from this build and
// cannot be overridden by config or a room request.
class MatchmakingClient {
  public:
    MatchmakingClient();
    ~MatchmakingClient();
    MatchmakingClient(const MatchmakingClient&) = delete;
    MatchmakingClient& operator=(const MatchmakingClient&) = delete;
    void connect(MatchmakingConfig config);
    void disconnect();
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool request(const MatchRequest& request);
    [[nodiscard]] std::vector<MatchEvent> poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace takedown::net
