#include "takedown/ui/mod_sync_session.hpp"

#include "takedown/content/mod_identity.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace takedown::ui {
namespace {

std::string text(const content::JsonValue& object, std::string_view key, bool required = true) {
    const auto* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        if (!required && value == nullptr) {
            return {};
        }
        throw std::runtime_error("Invalid lobby mod field: " + std::string(key));
    }
    return value->as_string();
}

bool safe_folder(std::string_view value) {
    return !value.empty() && value.size() <= 240U && value != "." && value != ".." &&
           value.front() != '.' && value.back() != '.' && value.back() != ' ' &&
           std::none_of(value.begin(), value.end(), [](char c) {
               return static_cast<unsigned char>(c) < 32U ||
                      std::string_view{"/\\:<>\"|?*"}.find(c) != std::string_view::npos;
           });
}

// RECOVERY: quote: differs from the original only by the global retvalptr temp counter (same source form).
std::string quote(std::string_view value) {
    std::string result{"\""};
    for (char c : value) {
        if (c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    return result + '"';
}

// Writes the launcher request beside a ".pending" file and swaps it in so the
// launcher never reads a half-written request.
void publish(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    const fs::path pending = path.string() + ".pending";
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        stream.write(value.data(), static_cast<std::streamsize>(value.size()));
        stream.close();
        if (!stream) {
            throw std::runtime_error("Cannot write the launcher mod request.");
        }
    }
    if (MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ignored;
        fs::remove(pending, ignored);
        throw std::runtime_error("Cannot publish the launcher mod request.");
    }
}

template <typename T>
std::optional<T> take(std::optional<T>& slot) {
    auto value = std::move(slot);
    slot.reset();
    return value;
}

constexpr std::size_t kMaxResultBytes = 1048576U;

content::JsonValue read_result(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read the launcher mod result.");
    }
    std::string bytes(kMaxResultBytes + 1U, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    if (bytes.size() > kMaxResultBytes) {
        throw std::runtime_error("Launcher mod result exceeds its size limit.");
    }
    content::JsonError error;
    auto parsed = content::parse_json(bytes, error);
    if (!parsed) {
        throw std::runtime_error("Invalid launcher mod result.");
    }
    return *parsed;
}

bool sha256_hex(std::string_view hex) {
    return hex.size() == 64U && std::all_of(hex.begin(), hex.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool valid_id(std::string_view value) {
    if (value.size() > 64U || !safe_folder(value) || value.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
    });
}

} // namespace

ModSyncSession::ModSyncSession(std::filesystem::path root)
    : root_(std::move(root)) {}

bool ModSyncSession::prompt_open() const noexcept {
    return state_ == State::Consent || state_ == State::Downloading || state_ == State::Failed;
}

bool ModSyncSession::pending() const noexcept {
    return state_ == State::Consent || state_ == State::Downloading || state_ == State::Ready;
}

bool ModSyncSession::ready_to_refresh() const noexcept { return state_ == State::Ready; }

// RECOVERY: offer: the original keeps the mod.name _M_assign out of line; here GCC inlines it (inline-budget difference).
std::optional<std::string> ModSyncSession::offer(const content::JsonValue& room,
                                                 const content::ModManager* installed) {
    cancel();
    try {
        const auto* room_id = room.find("id");
        if (room_id == nullptr || !room_id->is_number() || !std::isfinite(room_id->as_number()) ||
            room_id->as_number() < 1.0 || room_id->as_number() > 4294967295.0 ||
            std::floor(room_id->as_number()) != room_id->as_number()) {
            throw std::runtime_error("The original lobby is no longer available. Choose another lobby.");
        }
        const auto digest = text(room, "mods");
        if (!sha256_hex(digest)) {
            throw std::runtime_error("The lobby has no valid mod identity.");
        }
        const auto* list = room.find("mod_manifest");
        if (list == nullptr || !list->is_array() || list->as_array().size() > 100U) {
            throw std::runtime_error("The lobby has no usable mod list.");
        }
        std::vector<WantedMod> wanted;
        std::set<std::string> ids;
        for (const auto& entry : list->as_array()) {
            WantedMod mod;
            mod.id = text(entry, "id");
            mod.revision = text(entry, "revision", false);
            mod.content = text(entry, "content_sha256");
            mod.host_folder = text(entry, "folder", false);
            mod.name = text(entry, "name", false);
            if (mod.name.empty()) {
                mod.name = text(entry, "title", false);
            }
            if (mod.name.empty()) {
                mod.name = mod.id.empty() ? mod.host_folder : mod.id;
            }
            if (mod.name.size() > 200U ||
                !std::all_of(mod.name.begin(), mod.name.end(),
                         [](char c) { return static_cast<unsigned char>(c) > 31U; })) {
                throw std::runtime_error("Invalid lobby mod name.");
            }
            const auto* bytes = entry.find("bytes");
            if (bytes != nullptr && bytes->is_number()) {
                const double value = bytes->as_number();
                if (std::isfinite(value) && value > 0.0 && value <= 536870912.0 &&
                    std::floor(value) == value) {
                    mod.bytes = static_cast<std::uint64_t>(value);
                }
            }
            const auto* enabled = entry.find("enabled");
            if (enabled != nullptr && !(enabled->is_bool() && enabled->as_bool())) {
                throw std::runtime_error("The lobby mod list contains an inactive entry.");
            }
            if (!sha256_hex(mod.content) ||
                (!mod.revision.empty() && !(sha256_hex(mod.revision) && valid_id(mod.id))) ||
                (mod.id.empty() && !safe_folder(mod.host_folder)) || mod.id.size() > 200U ||
                !ids.insert(mod.id.empty() ? mod.host_folder : mod.id).second) {
                throw std::runtime_error("The lobby mod list contains an invalid or duplicate identity.");
            }
            if (!mod.revision.empty()) {
                if (installed != nullptr &&
                    std::filesystem::weakly_canonical(installed->directory()) !=
                        std::filesystem::weakly_canonical(root_ / "mods")) {
                    throw std::runtime_error("Lobby downloads need the mods folder beside the game. Your custom mod folder is unchanged.");
                }
                mod.missing = !std::filesystem::exists(
                    root_ / "mods" /
                    ("catalog-" + mod.id + "-" + mod.revision.substr(0U, 12U)));
            } else if (installed != nullptr) {
                mod.missing = std::ranges::none_of(installed->mods(), [&mod](const content::ManagedMod& installed_mod) {
                    return installed_mod.available &&
                           (mod.id.empty() ? mod.host_folder : mod.id) ==
                               (installed_mod.manifest.id.empty() ? installed_mod.folder
                                                                  : installed_mod.manifest.id);
                });
            }
            wanted.push_back(std::move(mod));
        }
        wanted_ = std::move(wanted);
        digest_ = digest;
        result_time_.reset();
        detail_page_ = 0U;
        lobby_target_ = LobbyJoinRequest{static_cast<std::uint32_t>(room_id->as_number()), digest};
        request_id_.clear();
        state_ = State::Consent;
        prompt_ = HostPrompt{
            .id = "lobby-mod-sync",
            .title = "MATCH LOBBY MODS?",
            .actions = {{"accept", "Download mods", PromptActionRole::Confirm},
                        {"cancel", "Cancel", PromptActionRole::Cancel}},
        };
        if (wanted_.size() > 3U) {
            prompt_.actions.push_back({.id = "details", .label = "Next mod details"});
        }
        update_prompt();
        status_ = "Lobby mod confirmation required.";
        return std::nullopt;
    } catch (const std::exception& error) {
        failed(error.what());
        return status_;
    }
}

// RECOVERY: update_prompt: the original inlines the loop-site std::string move constructor; the recompile does not (inline budget).
void ModSyncSession::update_prompt() {
    const bool download = std::any_of(wanted_.begin(), wanted_.end(),
                                      [](const WantedMod& mod) { return !mod.revision.empty(); });
    prompt_.actions.front().label = download ? "Download mods" : "Continue";
    if (wanted_.empty()) {
        prompt_.actions.front().label = "Continue";
        prompt_.body_lines = {"This lobby has no active mods.",
                              "Refresh on the Mods screen will disable your mods.",
                              "You will then join the lobby automatically."};
        return;
    }
    const auto missing = std::count_if(wanted_.begin(), wanted_.end(),
                                       [](const WantedMod& mod) { return mod.missing; });
    std::uint64_t bytes = 0U;
    for (const WantedMod& mod : wanted_) {
        if (mod.missing) {
            bytes += mod.bytes;
        }
    }
    prompt_.body_lines = {
        "Required: " + std::to_string(wanted_.size()) + "; missing: " + std::to_string(missing) +
            (bytes != 0U ? "; download >= " + std::to_string((bytes + 999999U) / 1000000U) + " MB"
                         : std::string{""}),
        download ? "Download, then Refresh to apply and join this lobby."
                 : "Refresh to apply local files and join this lobby."};
    for (std::size_t index = detail_page_ * 3U;
         index < std::min(wanted_.size(), (detail_page_ + 1U) * 3U); ++index) {
        const WantedMod& mod = wanted_[index];
        std::string label = mod.name;
        if (label.size() > 42U) {
            label = label.substr(0U, 39U) + "...";
        }
        prompt_.body_lines.push_back(
            label + (mod.revision.empty() ? " [local files]" : mod.missing ? " [download]" : " [verify]"));
    }
    prompt_.body_lines.push_back("Details " + std::to_string(detail_page_ + 1U) + "/" +
                                 std::to_string((wanted_.size() + 2U) / 3U) +
                                 ". Changes apply only on Mods Refresh.");
}

// RECOVERY: accept: one std::string _M_dispose that the original inlines is left out of line (inline budget).
void ModSyncSession::accept() {
    std::string list;
    for (const WantedMod& mod : wanted_) {
        if (mod.revision.empty()) {
            continue;
        }
        if (!list.empty()) {
            list += ',';
        }
        list += "{\"id\":" + quote(mod.id) + ",\"revision\":" + quote(mod.revision) + '}';
    }
    if (list.empty()) {
        ready();
        return;
    }
    static std::atomic<std::uint64_t> serial{0U};
    request_id_ = "sync_" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                  std::to_string(++serial);
    publish(root_ / ".launcher/mod-sync/request.json",
            "{\"schema\":1,\"request_id\":" + quote(request_id_) + ",\"mods\":[" + list + "]}");
    began_ = std::chrono::steady_clock::now();
    state_ = State::Downloading;
    status_ = "Downloading lobby mods. Keep the launcher open.";
    prompt_ = HostPrompt{
        .id = "lobby-mod-download",
        .title = "DOWNLOADING LOBBY MODS",
        .body_lines = {"Downloading the lobby's published mods.",
                       "Keep the launcher open. Your current mods stay active.",
                       "The Mods screen will open when downloads finish.",
                       "Select Refresh there to apply the mods and join."},
        .actions = {{"cancel", "Cancel", PromptActionRole::Cancel}},
    };
}

bool ModSyncSession::apply(const HostPromptInput& input) {
    if (!prompt_open()) {
        return false;
    }
    const auto action = apply_prompt_input(prompt_, input);
    if (!action) {
        return input.move_up || input.move_down;
    }
    try {
        if (*action == "details") {
            detail_page_ = (detail_page_ + 1U) % ((wanted_.size() + 2U) / 3U);
            update_prompt();
        } else if (*action == "cancel") {
            cancel();
        } else {
            accept();
        }
    } catch (const std::exception& error) {
        failed(error.what());
    }
    return true;
}

// RECOVERY: poll: differs only by inline-budget/retvalptr counter noise.
bool ModSyncSession::poll() {
    if (state_ != State::Downloading) {
        return false;
    }
    try {
        if (std::chrono::steady_clock::now() - began_ > std::chrono::minutes(15)) {
            throw std::runtime_error("Lobby mod download timed out. Open the launcher and try joining again.");
        }
        const fs::path path = root_ / ".launcher/mod-sync/result.json";
        std::error_code error;
        const auto time = fs::last_write_time(path, error);
        if (error) {
            return false;
        }
        const auto size = fs::file_size(path);
        if (result_time_ == time && result_size_ == size) {
            return false;
        }
        const auto result = read_result(path);
        result_time_ = time;
        result_size_ = size;
        if (text(result, "request_id") != request_id_) {
            return false;
        }
        const auto* schema = result.find("schema");
        const auto* success = result.find("success");
        if (schema == nullptr || !schema->is_number() || schema->as_number() != 1.0 ||
            success == nullptr || !success->is_bool()) {
            throw std::runtime_error("Invalid launcher mod result schema.");
        }
        if (!success->as_bool()) {
            throw std::runtime_error("Lobby mod download failed: " + text(result, "error"));
        }
        const auto* mods = result.find("mods");
        if (mods == nullptr || !mods->is_array() || mods->as_array().size() > 100U) {
            throw std::runtime_error("Launcher did not return its installed mod list.");
        }
        std::set<std::string> found;
        for (const auto& entry : mods->as_array()) {
            const auto id = text(entry, "id");
            const auto revision = text(entry, "revision");
            const auto folder = text(entry, "folder");
            const auto wanted = std::find_if(wanted_.begin(), wanted_.end(), [&id, &revision](const WantedMod& mod) {
                return mod.id == id && !mod.revision.empty() && mod.revision == revision;
            });
            if (wanted == wanted_.end() || !found.insert(id).second ||
                folder != "catalog-" + id + "-" + revision.substr(0U, 12U) || !safe_folder(folder)) {
                throw std::runtime_error("Launcher returned a different mod revision.");
            }
            wanted->installed_folder = folder;
        }
        for (const WantedMod& mod : wanted_) {
            if (!mod.revision.empty() && mod.installed_folder.empty()) {
                throw std::runtime_error("Launcher result is missing a required mod.");
            }
        }
        ready();
        return true;
    } catch (const std::exception& error) {
        failed(error.what());
        return true;
    }
}

// RECOVERY: refresh: vector<ModDependency>::~vector stays out of line in a cold EH path (inline budget).
std::optional<std::string> ModSyncSession::refresh(content::ModManager& manager) {
    if (!ready_to_refresh()) {
        return "Lobby mods are not ready. Keep the launcher open and wait for the download.";
    }
    try {
        content::ModManager candidate = manager;
        candidate.rescan();
        std::vector<std::string> selected;
        for (const WantedMod& wanted : wanted_) {
            std::string folder;
            for (const content::ManagedMod& installed : candidate.mods()) {
                if (!installed.available) {
                    continue;
                }
                if (!wanted.installed_folder.empty() && installed.folder != wanted.installed_folder) {
                    continue;
                }
                const auto id = installed.manifest.id.empty() ? installed.folder : installed.manifest.id;
                if (id != (wanted.id.empty() ? wanted.host_folder : wanted.id)) {
                    continue;
                }
                if (content::mod_content_identity(candidate.directory() / installed.folder) == wanted.content) {
                    folder = installed.folder;
                    break;
                }
            }
            if (folder.empty()) {
                throw std::runtime_error("Missing or changed lobby mod: " +
                                         (wanted.id.empty() ? wanted.host_folder : wanted.id));
            }
            selected.push_back(folder);
        }
        if (auto error = candidate.select_enabled_folders(selected)) {
            throw std::runtime_error(*error);
        }
        if (content::installed_mod_identity(candidate) != digest_) {
            throw std::runtime_error(
                "Lobby mod files or load order do not match the host. Your active mods are unchanged.");
        }
        if (auto error = candidate.save()) {
            throw std::runtime_error(*error);
        }
        manager = std::move(candidate);
        state_ = State::Idle;
        join_request_ = take(lobby_target_);
        mods_page_requested_ = false;
        status_ = "Lobby mods applied. Joining the original lobby...";
        return std::nullopt;
    } catch (const std::exception& error) {
        status_ = error.what();
        return status_;
    }
}

void ModSyncSession::cancel() {
    state_ = State::Idle;
    lobby_target_.reset();
    join_request_.reset();
    mods_page_requested_ = false;
    request_id_.clear();
    result_time_.reset();
    wanted_.clear();
    status_ = "Lobby mod changes cancelled.";
}

void ModSyncSession::abort(std::string message) { failed(std::move(message)); }

// RECOVERY: failed: one std::string _M_dispose (second reset) that the original inlines is left out of line.
void ModSyncSession::failed(std::string message) {
    state_ = State::Failed;
    status_ = std::move(message);
    lobby_target_.reset();
    join_request_.reset();
    mods_page_requested_ = false;
    prompt_ = HostPrompt{
        .id = "lobby-mod-error",
        .title = "LOBBY MODS",
        .body_lines = {status_, "Return to the lobby browser to try again."},
        .actions = {{"cancel", "OK", PromptActionRole::Cancel}},
    };
}

void ModSyncSession::ready() {
    state_ = State::Ready;
    mods_page_requested_ = true;
    status_ = "Lobby mods ready. Select Refresh to apply them and join the lobby.";
}

bool ModSyncSession::consume_mods_page_request() noexcept {
    return std::exchange(mods_page_requested_, false);
}

std::optional<ModSyncSession::LobbyJoinRequest> ModSyncSession::consume_join_request() {
    return take(join_request_);
}

} // namespace takedown::ui
