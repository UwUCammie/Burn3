#pragma once
#include "takedown/net/matchmaking.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
namespace takedown::platform { struct TextInputEvent; }

namespace takedown::guest {
class EeMemory;
}
namespace takedown::runtime {
class NativeFunctionDispatcher;
// Revision boundary between the original online frontend and PC services.
// No console device, network configuration file, SSL module or IOP RPC is used.
class Slus21050Online {
  public:
    explicit Slus21050Online(net::MatchmakingConfig config);
    ~Slus21050Online();
    [[nodiscard]] bool register_functions(NativeFunctionDispatcher&, const guest::EeMemory&);
    // Explicit Mods Refresh replaces the admission snapshot and closes old peer
    // keys. Remembered account credentials survive this connection reset.
    void set_applied_mods(std::shared_ptr<const content::ModManager>);
    struct ModSyncFrontend { std::uint32_t screen{}, page{}; };
    [[nodiscard]] std::optional<ModSyncFrontend> mod_sync_frontend() const;
    struct ModSyncSelection { std::uint64_t vehicle_key{}; std::uint32_t paint{}; };
    using ModSyncSelectionResolver = std::function<std::optional<ModSyncSelection>(
        guest::EeMemory&, std::uint64_t, std::uint32_t)>;
    // Resolves the retained request against the applied registry immediately
    // before replay. The host callback commits any required fallback once.
    void set_mod_sync_selection_resolver(ModSyncSelectionResolver);
    // Reconnect and validate while Mods still owns the frontend. Only the
    // prepared event permits returning to an online page that polls this SDK.
    [[nodiscard]] std::optional<std::string> prepare_mod_sync_rejoin(std::uint32_t room,
                                                                   std::string_view digest);
    [[nodiscard]] bool consume_mod_sync_prepared();
    // Only after successful explicit Mods Refresh and return to the retained
    // original join page. Revalidates the fresh host before replaying its guest
    // join request, including original callbacks and private password.
    [[nodiscard]] std::optional<std::string> request_mod_sync_rejoin(std::uint32_t room,
                                                                   std::string_view digest);
    [[nodiscard]] std::optional<std::string> consume_mod_sync_error();
    [[nodiscard]] bool text_entry_focused(const guest::EeMemory&) const;
    // Called on the game thread. Queued input is consumed at the retail online
    // update boundary so original keyboard cursor/layout and callbacks run.
    void handle_text_input(const platform::TextInputEvent&, const guest::EeMemory&);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace takedown::runtime
