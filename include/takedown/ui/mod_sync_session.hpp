#pragma once

#include "takedown/content/json.hpp"
#include "takedown/content/mod_manager.hpp"
#include "takedown/ui/host_prompt.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace takedown::ui {
// Consent and launcher handoff for a mismatching lobby. Downloads never change
// the active snapshot; refresh() is called only by the Mods screen's Refresh.
class ModSyncSession {
  public:
    struct LobbyJoinRequest { std::uint32_t room{}; std::string mods; };
    explicit ModSyncSession(std::filesystem::path install_root);
    [[nodiscard]] std::optional<std::string> offer(const content::JsonValue& room,
                                                  const content::ModManager* installed = nullptr);
    [[nodiscard]] bool apply(const HostPromptInput& input);
    [[nodiscard]] bool poll();
    [[nodiscard]] std::optional<std::string> refresh(content::ModManager& manager);
    // Consumes navigation once. It never selects mods or synthesizes Refresh.
    [[nodiscard]] bool consume_mods_page_request() noexcept;
    // Only a successful explicit Refresh can produce this request. The online
    // adapter must revalidate the room and resume its original join callback.
    [[nodiscard]] std::optional<LobbyJoinRequest> consume_join_request();
    void cancel();
    void abort(std::string message);
    [[nodiscard]] bool prompt_open() const noexcept;
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool ready_to_refresh() const noexcept;
    [[nodiscard]] const HostPrompt& prompt() const noexcept { return prompt_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    [[nodiscard]] const std::string& request_id() const noexcept { return request_id_; }

  private:
    enum class State { Idle, Consent, Downloading, Ready, Failed };
    struct WantedMod { std::string id, revision, content, host_folder, installed_folder, name; std::uint64_t bytes{}; bool missing{true}; };
    void accept();
    void update_prompt();
    void failed(std::string message);
    void ready();
    std::filesystem::path root_;
    HostPrompt prompt_;
    std::vector<WantedMod> wanted_;
    std::string digest_, status_, request_id_;
    State state_{State::Idle};
    std::chrono::steady_clock::time_point began_{};
    std::optional<std::filesystem::file_time_type> result_time_;
    std::uintmax_t result_size_{};
    std::size_t detail_page_{};
    std::optional<LobbyJoinRequest> lobby_target_, join_request_;
    bool mods_page_requested_{};
};
} // namespace takedown::ui
