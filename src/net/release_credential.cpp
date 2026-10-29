#include "takedown/net/release_credential.hpp"

#include "takedown/net/browser_auth.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace takedown::net {
namespace {

constexpr std::string_view kKeyFileName = "release.key";

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring buffer(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path.parent_path();
#endif
}

std::string read_key_file(const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1U);
}

// Game builds live in <install>/.launcher/native/<build>/ and the active runtime's folder name is in
// <install>/.launcher/runtime.json ("directory").
std::filesystem::path launcher_runtime_bin(const std::filesystem::path& executable_dir) {
    const auto launcher = executable_dir.parent_path().parent_path();
    std::ifstream stream(launcher / "runtime.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto name = text.find("\"directory\"");
    if (name == std::string::npos) {
        return {};
    }
    const auto open = text.find('"', text.find(':', name) + 1U);
    const auto close = open == std::string::npos ? open : text.find('"', open + 1U);
    if (close == std::string::npos) {
        return {};
    }
    return launcher / text.substr(open + 1U, close - open - 1U) / "bin";
}

std::string find_release_key() {
    if (const char* const named = std::getenv("TAKEDOWN_RELEASE_KEY_FILE"); named != nullptr && *named != '\0') {
        return read_key_file(named);
    }
    const auto executable_dir = executable_directory();
    std::error_code error;
    for (const auto& directory : {executable_dir, std::filesystem::current_path(error),
                                  launcher_runtime_bin(executable_dir)}) {
        if (directory.empty()) {
            continue;
        }
        if (auto key = read_key_file(directory / kKeyFileName); !key.empty()) {
            return key;
        }
    }
    return {};
}

} // namespace

std::string load_release_key() {
    static const std::string key = find_release_key();
    return key;
}

std::string release_proof(std::string_view key, std::uint64_t timestamp, std::string_view version, unsigned wire,
                          std::string_view mods, std::string_view name) {
    return game_login_proof(key, "release:" + std::to_string(timestamp), version, wire, mods, name);
}

std::string release_hello_members(std::string_view key, std::uint64_t timestamp, std::string_view version,
                                  unsigned wire, std::string_view mods, std::string_view name) {
    if (key.empty()) {
        return {};
    }
    return ",\"release_ts\":" + std::to_string(timestamp) + ",\"release_proof\":\"" +
           release_proof(key, timestamp, version, wire, mods, name) + "\"";
}

std::string release_hello_members(std::string_view version, unsigned wire, std::string_view mods,
                                  std::string_view name) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return release_hello_members(load_release_key(), static_cast<std::uint64_t>(seconds), version, wire, mods, name);
}

} // namespace takedown::net
