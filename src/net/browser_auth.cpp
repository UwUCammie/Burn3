#include "takedown/net/browser_auth.hpp"

#include "takedown/content/json.hpp"
#include "takedown/content/sha256.hpp"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <array>
#include <cstring>
#include <span>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace takedown::net {

namespace {

using content::JsonValue;

constexpr std::string_view kClientTag = "v0.0.24";

class InvalidSession : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

std::string member(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error("Invalid account service response");
    }
    return value->as_string();
}

// Session identifiers and tickets are lowercase hex of a fixed length.
bool is_lower_hex(std::string_view text, std::size_t length) {
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// Device secrets use the URL-safe base64 alphabet.
bool is_url_safe_token(std::string_view text, std::size_t length) {
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

// Display names are non-empty, bounded, and free of control characters.
bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > 128U) {
        return false;
    }
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 31U || c == 127U) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> unhex(std::string_view value) {
    const auto nibble = [](char c) {
        return c >= 'a' ? c - 'a' + 10 : c - '0';
    };
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = nibble(value[i]);
        const int low = nibble(value[i + 1]);
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

// A WinHTTP handle that is closed when its owner goes out of scope.
struct Handle {
    HINTERNET value{};
    ~Handle() { WinHttpCloseHandle(value); }
};

// HTTPS uses the operating system certificate verifier. Only the fixed account
// origin is contacted, so the body and headers never reach another host.
std::string https_post(std::string_view path, const std::string& body) {
    Handle session;
    session.value = WinHttpOpen(L"Takedown/0.0.1", 0, nullptr, nullptr, 0);
    if (session.value == nullptr) {
        throw std::runtime_error("Account HTTPS connection failed");
    }
    if (!WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) {
        throw std::runtime_error("Account HTTPS timeout configuration failed");
    }
    // TLS 1.2 and 1.3 only.
    DWORD tls = 0x00002800U;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls))) {
        throw std::runtime_error("Account HTTPS TLS configuration failed");
    }
    Handle connection;
    connection.value = WinHttpConnect(session.value, L"uwucammie.com", 443, 0);
    if (connection.value == nullptr) {
        throw std::runtime_error("Account HTTPS connection failed");
    }
    const std::wstring wide(path.begin(), path.end());
    Handle request;
    request.value = WinHttpOpenRequest(connection.value, L"POST", wide.c_str(), nullptr, nullptr,
                                       nullptr, WINHTTP_FLAG_SECURE);
    if (request.value == nullptr) {
        throw std::runtime_error("Account HTTPS connection failed");
    }
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) {
        throw std::runtime_error("Account HTTPS redirect configuration failed");
    }
    if (!WinHttpSendRequest(request.value,
                            L"Content-Type: application/json\r\nOrigin: https://uwucammie.com\r\n",
                            static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        throw std::runtime_error("Unable to reach the account service securely");
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    const BOOL queried = WinHttpQueryHeaders(request.value,
                                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                             nullptr, &status, &size, nullptr);
    std::string response;
    if (!queried || status != 200) {
        // The account service answers 401 or 403 when the remembered session is void.
        if (status == 401 || status == 403) {
            throw InvalidSession("Account session was revoked or expired");
        }
        throw std::runtime_error("Account sign-in expired or unavailable. Try Online again.");
    }
    std::array<char, 2048> buffer{};
    DWORD received = 0;
    do {
        if (!WinHttpReadData(request.value, buffer.data(), 2048, &received)) {
            throw std::runtime_error("Account HTTPS response failed");
        }
        if (response.size() + received > 16384U) {
            throw std::runtime_error("Account response too large");
        }
        response.append(buffer.data(), received);
    } while (received != 0);
    return response;
}

JsonValue post(std::string_view path, const std::string& body) {
    content::JsonError error;
    auto result = parse_json(https_post(path, body), error);
    if (!result || !result->is_object()) {
        throw std::runtime_error("Invalid account service response");
    }
    return std::move(*result);
}

std::filesystem::path session_path(const std::filesystem::path& requested) {
    if (!requested.empty()) {
        return requested;
    }
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (local != nullptr) {
        return std::filesystem::path(local) / L"Takedown" / L"game-session.bin";
    }
    return {};
}

GameLogin decode_login(const JsonValue& response) {
    GameLogin result;
    result.ticket = member(response, "ticket");
    result.secret = member(response, "secret");
    result.name = member(response, "name");
    if (!is_lower_hex(result.ticket, 32U) || !is_lower_hex(result.secret, 64U) ||
        !is_valid_name(result.name)) {
        throw std::runtime_error("Invalid game login response");
    }
    // A session is present when either half is returned; both must then be valid.
    if (response.find("session_id") != nullptr || response.find("session_secret") != nullptr) {
        result.session_id = member(response, "session_id");
        result.session_secret = member(response, "session_secret");
        if (!is_lower_hex(result.session_id, 32U) || !is_lower_hex(result.session_secret, 64U)) {
            throw std::runtime_error("Invalid remembered game session");
        }
    }
    return result;
}

} // namespace

std::optional<GameSession> load_game_session(const std::filesystem::path& requested) {
    const auto path = session_path(requested);
    if (path.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > 16384U) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    DATA_BLOB encrypted;
    encrypted.cbData = static_cast<DWORD>(bytes.size());
    encrypted.pbData = bytes.data();
    DATA_BLOB clear = {};
    if (CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, 1, &clear) == 0) {
        return std::nullopt;
    }
    std::string value(reinterpret_cast<const char*>(clear.pbData), clear.cbData);
    SecureZeroMemory(clear.pbData, clear.cbData);
    LocalFree(clear.pbData);
    // The plaintext is "<32 hex id>\n<64 hex secret>" and nothing else.
    const bool valid = value.size() == 97U && value[32] == '\n' &&
                       is_lower_hex(value.substr(0, 32), 32U) && is_lower_hex(value.substr(33), 64U);
    if (!valid) {
        return std::nullopt;
    }
    return GameSession{value.substr(0, 32), value.substr(33)};
}


void forget_game_session(const std::filesystem::path& requested) {
    const auto path = session_path(requested);
    if (!path.empty()) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}


bool save_game_session(const GameSession& session, const std::filesystem::path& requested) {
    if (!is_lower_hex(session.id, 32U) || !is_lower_hex(session.secret, 64U)) {
        return false;
    }
    const auto path = session_path(requested);
    if (path.empty()) {
        return false;
    }
    std::string value = session.id + '\n' + session.secret;
    DATA_BLOB clear;
    clear.cbData = static_cast<DWORD>(value.size());
    clear.pbData = reinterpret_cast<BYTE*>(value.data());
    DATA_BLOB encrypted;
    if (!CryptProtectData(&clear, L"Takedown game session", nullptr, nullptr, nullptr, 1, &encrypted)) {
        return false;
    }
    SecureZeroMemory(value.data(), value.size());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    // Write beside the target and replace it in one step, so a crash never leaves a torn file.
    const std::filesystem::path temporary = path.native() + L".tmp";
    bool stored = false;
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
        output.close();
        LocalFree(encrypted.pbData);
        stored = !output.fail() &&
                 MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!stored) {
            std::filesystem::remove(temporary, error);
        }
    } catch (...) {
    }
    return stored;
}


std::optional<GameLogin> resume_game_session(const GameSession& session) {
    if (!is_lower_hex(session.id, 32U) || !is_lower_hex(session.secret, 64U)) {
        return std::nullopt;
    }
    const std::string body = "{\"session_id\":\"" + session.id + "\",\"session_secret\":\"" +
                             session.secret + "\",\"version\":\"" + std::string(kClientTag.substr(1)) +
                             "\"}";
    try {
        return decode_login(post("/burn3/api/game-auth/resume", body));
    } catch (const InvalidSession&) {
        // A revoked or expired credential is not an error: the caller signs in again.
        return std::nullopt;
    }
}


BrowserSignIn begin_browser_signin() {
    const std::string body = "{\"version\":\"" + std::string(kClientTag.substr(1)) + "\"}";
    const JsonValue response = post("/burn3/api/game-auth/start", body);
    BrowserSignIn result;
    result.device_id = member(response, "device_id");
    result.device_secret = member(response, "device_secret");
    result.user_code = member(response, "user_code");
    if (!is_lower_hex(result.device_id, 32U) || !is_url_safe_token(result.device_secret, 43U) ||
        result.user_code.size() != 9U || result.user_code[4] != '-') {
        throw std::runtime_error("Invalid browser sign-in response");
    }
    // The user code is two groups of four uppercase hex digits.
    for (std::size_t i = 0; i < 9U; ++i) {
        if (i == 4U) {
            continue;
        }
        const char c = result.user_code[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            throw std::runtime_error("Invalid browser sign-in code");
        }
    }
    result.url = "https://uwucammie.com/burn3/account/?device=" + result.user_code;
    return result;
}


std::optional<GameLogin> poll_browser_signin(const BrowserSignIn& request) {
    const JsonValue response = post("/burn3/api/game-auth/poll",
                                    "{\"device_id\":\"" + request.device_id + "\",\"device_secret\":\"" +
                                        request.device_secret + "\"}");
    if (member(response, "status") == "pending") {
        return std::nullopt;
    }
    if (member(response, "status") != "approved") {
        throw std::runtime_error("Invalid sign-in status");
    }
    return decode_login(response);
}


std::string game_login_proof(std::string_view secret, std::string_view ticket, std::string_view version,
                             unsigned wire, std::string_view mods, std::string_view name) {
    // HMAC-SHA256 keyed by the session secret; long keys are hashed first.
    std::array<std::uint8_t, 64> key{};
    if (secret.size() > 64U) {
        content::Sha256 hash;
        hash.update(secret);
        const auto bytes = unhex(hash.finish());
        std::memmove(key.data(), bytes.data(), bytes.size());
    } else {
        std::memmove(key.data(), secret.data(), secret.size());
    }
    std::array<std::uint8_t, 64> inner{};
    std::array<std::uint8_t, 64> outer{};
    for (std::size_t i = 0; i < 64U; ++i) {
        inner[i] = static_cast<std::uint8_t>(key[i] ^ 0x36U);
        outer[i] = static_cast<std::uint8_t>(key[i] ^ 0x5CU);
    }
    content::Sha256 hash;
    hash.update(std::span<const std::uint8_t>(inner));
    const std::string message = "B3-GAME-AUTH\n" + std::string(ticket) + "\n" + std::string(version) +
                                "\n" + std::to_string(wire) + "\n" + std::string(mods) + "\n" +
                                std::string(name);
    hash.update(message);
    const auto bytes = unhex(hash.finish());
    content::Sha256 final;
    final.update(std::span<const std::uint8_t>(outer));
    final.update(std::span<const std::uint8_t>(bytes));
    return final.finish();
}

} // namespace takedown::net
