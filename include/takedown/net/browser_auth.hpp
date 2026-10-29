#pragma once
#include <optional>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace takedown::net {
struct BrowserSignIn {
    std::string device_id, device_secret, user_code, url;
};
struct GameLogin {
    std::string ticket, secret, name;
    std::string session_id, session_secret;
};
struct GameSession { std::string id, secret; };
// Windows credentials are encrypted for this OS user with DPAPI. Unsupported
// platforms retain no plaintext credential files. The optional path is for tests.
[[nodiscard]] std::optional<GameSession> load_game_session(const std::filesystem::path& path = {});
[[nodiscard]] bool save_game_session(const GameSession&, const std::filesystem::path& path = {});
void forget_game_session(const std::filesystem::path& path = {});
// A revoked/expired credential returns nullopt. Transport failures throw, and
// must not discard a remembered account just because the network is unavailable.
[[nodiscard]] std::optional<GameLogin> resume_game_session(const GameSession&);
// HTTPS uses the operating system certificate verifier. Only the fixed account
// origin is contacted; the matchmaking hostname never enters these requests.
[[nodiscard]] BrowserSignIn begin_browser_signin();
[[nodiscard]] std::optional<GameLogin> poll_browser_signin(const BrowserSignIn& request);
[[nodiscard]] std::string game_login_proof(std::string_view secret, std::string_view ticket,
                                           std::string_view version, unsigned wire,
                                           std::string_view mods, std::string_view name);
} // namespace takedown::net
