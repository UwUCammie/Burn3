#include "takedown/net/online_player_count.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace takedown::net {

namespace {

#if defined(_WIN32)
struct Handle {
    HINTERNET value{};
    ~Handle() {
        if (value != nullptr) {
            WinHttpCloseHandle(value);
        }
    }
};

// GET https://uwucammie.com/burn3/api/games; empty on any failure.
std::string fetch_directory() {
    Handle session;
    session.value = WinHttpOpen(L"Takedown/0.1", 0, nullptr, nullptr, 0);
    if (session.value == nullptr || !WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 3000)) {
        return {};
    }
    DWORD tls = 0x00002800U; // TLS 1.2 and 1.3
    WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls));
    Handle connection;
    connection.value = WinHttpConnect(session.value, L"uwucammie.com", 443, 0);
    if (connection.value == nullptr) {
        return {};
    }
    Handle request;
    request.value = WinHttpOpenRequest(connection.value, L"GET", L"/burn3/api/games", nullptr, nullptr, nullptr,
                                       WINHTTP_FLAG_SECURE);
    if (request.value == nullptr) {
        return {};
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        return {};
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                             &size, nullptr) ||
        status != 200) {
        return {};
    }
    std::string response;
    std::array<char, 2048> buffer{};
    DWORD received = 0;
    do {
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
            return {};
        }
        if (response.size() + received > 65536U) {
            return {};
        }
        response.append(buffer.data(), received);
    } while (received != 0);
    return response;
}
#else
std::string fetch_directory() { return {}; }
#endif

} // namespace

OnlinePlayerCount::~OnlinePlayerCount() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void OnlinePlayerCount::set_wanted(const bool wanted) {
    const bool was = wanted_.exchange(wanted);
    if (wanted && !thread_.joinable()) {
        thread_ = std::thread([this] { run(); });
    } else if (wanted && !was) {
        wake_.notify_all();
    }
}

std::optional<std::uint32_t> OnlinePlayerCount::count() const noexcept {
    const std::int64_t value = count_.load();
    if (value < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> OnlinePlayerCount::parse_players(const std::string_view json) noexcept {
    // The directory is {"online": .., "version": .., "players": N, "games": [...]};
    // "players" also names a member of each game, so take the top-level one
    // (the first, before "games").
    const auto key = json.find("\"players\"");
    if (key == std::string_view::npos) {
        return std::nullopt;
    }
    const auto games = json.find("\"games\"");
    if (games != std::string_view::npos && games < key) {
        return std::nullopt;
    }
    std::size_t at = json.find(':', key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) {
        ++at;
    }
    std::uint64_t value = 0U;
    std::size_t digits = 0U;
    while (at < json.size() && json[at] >= '0' && json[at] <= '9' && digits < 9U) {
        value = value * 10U + static_cast<std::uint64_t>(json[at] - '0');
        ++at;
        ++digits;
    }
    if (digits == 0U) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

void OnlinePlayerCount::run() {
    int failures = 0;
    while (true) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || wanted_.load(); });
            if (stopping_) {
                return;
            }
        }
        const auto players = parse_players(fetch_directory());
        if (players.has_value()) {
            if (count_.load() < 0) {
                std::fprintf(stderr, "Online players: %u\n", *players);
            }
            failures = 0;
            count_.store(static_cast<std::int64_t>(*players));
        } else if (++failures >= 3) {
            if (failures == 3) {
                std::fprintf(stderr, "Online players: the game directory is not answering\n");
            }
            count_.store(-1); // the service is not answering: show nothing rather than a stale count
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(kRefreshSeconds), [this] { return stopping_; });
        if (stopping_) {
            return;
        }
    }
}

} // namespace takedown::net
