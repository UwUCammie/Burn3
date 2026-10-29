#include "takedown/content/mod_manager.hpp"

#include "takedown/content/json.hpp"
#include "takedown/content/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace takedown::content {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kLoadOrderFile = "load_order.txt";

std::string ascii_lower_copy(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1U);
}

// Dot-folders are hidden: listed, but disabled until the player enables them.
bool hidden_folder(const std::string& folder) {
    return !folder.empty() && folder.front() == '.';
}

bool lower_or_digit(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

// A receipt entry the launcher could have written: a catalog id and a full
// lowercase SHA-256 revision.
bool valid_catalog_entry(std::string_view id, std::string_view revision) {
    return !id.empty() && id.size() <= 64U && lower_or_digit(id.front()) && id.back() != '.' &&
           id.find("..") == std::string_view::npos &&
           std::all_of(id.begin(), id.end(),
                       [](const char character) {
                           return lower_or_digit(character) || character == '.' || character == '_' ||
                                  character == '-';
                       }) &&
           revision.size() == 64U && std::all_of(revision.begin(), revision.end(), [](const char character) {
               return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
           });
}

// Retail car classes keep their bare id; a mod's own class is namespaced by
// the mod so two mods cannot collide.
std::string content_id(const std::string& owner, std::string id) {
    if (id.empty() || id.find(':') != std::string::npos || id == "COMP" || id == "MSCL" ||
        id == "CUPE" || id == "SPRT" || id == "SUPR" || id == "HSPC" || id == "HEVY" ||
        id == "TRAF" || id == "TSPC") {
        return id;
    }
    return owner + ":" + id;
}

std::string ascii_upper_copy(std::string text) {
    for (char& character : text) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return text;
}

// A picture named in a content file: relative to the item's folder and never
// outside it. `invalid_path` reports a name that tries to leave the folder.
fs::path item_image(const fs::path& item, const std::string& name, bool& invalid_path) {
    invalid_path = false;
    if (name.empty()) {
        return {};
    }
    const fs::path relative{name};
    if (relative.is_absolute() || relative.has_root_name()) {
        invalid_path = true;
        return {};
    }
    std::error_code error;
    const fs::path root = fs::weakly_canonical(item, error);
    if (error) {
        return {};
    }
    const fs::path file = fs::weakly_canonical(item / relative, error);
    if (error) {
        return {};
    }
    const fs::path within = file.lexically_relative(root);
    if (within.empty() || *within.begin() == "..") {
        invalid_path = true;
        return {};
    }
    if (!fs::is_regular_file(file, error) || error) {
        return {};
    }
    return file;
}

// Hash of a car file, the key of its rendered paint previews. Empty when the
// file cannot be read (or is implausibly large for a car).
std::string preview_hash(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > 128U * 1024U * 1024U) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    Sha256 hash;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
    }
    return input.eof() ? hash.finish() : std::string{};
}

// A JSON string literal for `text` (UTF-8 passes through unchanged).
std::string json_string(std::string_view text) {
    std::string out = "\"";
    const char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20U) {
            out += "\\u00";
            out += hex[c >> 4U];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

} // namespace

ModManager::ModManager(std::filesystem::path mods_dir, std::filesystem::path game_root)
    : directory_(std::move(mods_dir)), game_root_(std::move(game_root)) {}

std::optional<std::string> ModManager::request_thumbnail_refresh() const {
    // The launcher watches this file and renders the previews out of process.
    try {
        const fs::path path = game_root_.parent_path() / ".launcher/car-thumbnails/request.json";
        fs::create_directories(path.parent_path());
        const fs::path pending = path.string() + ".pending";
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        const auto utf8 = fs::absolute(directory_).u8string();
        stream << "{\"schema\":1,\"request_id\":\""
               << std::chrono::steady_clock::now().time_since_epoch().count() << "\",\"mods_directory\":"
               << json_string(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()))
               << "}\n";
        stream.close();
        if (!stream) {
            return "Could not request car previews.";
        }
        if (MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            return "Could not publish car preview request.";
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        return error.what();
    }
}

bool ModManager::thumbnail_cache_changed() {
    std::error_code error;
    const auto time =
        fs::last_write_time(game_root_.parent_path() / "cache/car-thumbnails/ready.json", error);
    if (error || time == thumbnail_cache_time_) {
        return false;
    }
    thumbnail_cache_time_ = time;
    return true;
}

std::size_t ModManager::enabled_count() const noexcept {
    std::size_t count = 0U;
    for (const ManagedMod& mod : mods_) {
        count += mod.enabled ? 1U : 0U;
    }
    return count;
}

bool ModManager::read_load_order(std::vector<std::string>& order) const {
    std::ifstream file(directory_ / std::string(kLoadOrderFile));
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view entry = trim(line);
        if (entry.empty() || entry.front() == '#' || entry.front() == ';') {
            continue;
        }
        order.emplace_back(entry);
    }
    return true;
}

void ModManager::remember_directory_time() {
    std::error_code error;
    const auto time = fs::last_write_time(directory_, error);
    if (error) {
        directory_time_valid_ = false;
        return;
    }
    directory_time_ = time;
    directory_time_valid_ = true;
    directory_entries_ = 0U;
    fs::directory_iterator iterator{directory_, fs::directory_options::skip_permission_denied, error}, end;
    while (!error && iterator != end) {
        ++directory_entries_;
        iterator.increment(error);
    }
}

bool ModManager::directory_changed() const {
    if (!active()) {
        return false;
    }
    std::error_code error;
    const auto time = fs::last_write_time(directory_, error);
    if (error) {
        return false;
    }
    if (!directory_time_valid_ || time != directory_time_) {
        return true;
    }
    std::size_t entries = 0U;
    fs::directory_iterator iterator{directory_, fs::directory_options::skip_permission_denied, error}, end;
    while (!error && iterator != end) {
        ++entries;
        iterator.increment(error);
    }
    return entries != directory_entries_;
}

void ModManager::validate() {
    for (ManagedMod& mod : mods_) {
        mod.problems.clear();
        mod.applied = mod.enabled && mod.available;
    }
    // Two folders with the same manifest id: the later one is not applied.
    for (std::size_t index = 0U; index < mods_.size(); ++index) {
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (mods_[earlier].available && mods_[index].available &&
                mods_[earlier].manifest.id == mods_[index].manifest.id) {
                mods_[index].problems.push_back("same id as " + mods_[earlier].folder + " (" +
                                                mods_[index].manifest.id + ")");
                break;
            }
        }
    }
    for (ManagedMod& mod : mods_) {
        for (const std::string& problem : mod.manifest.problems) {
            mod.problems.push_back(problem);
        }
        if (!mod.problems.empty()) {
            mod.applied = false;
        }
    }

    // Dependencies and incompatibilities. Dropping a mod can break the mods
    // that need it, so repeat until nothing changes.
    const auto find_applied = [this](const std::string& id) -> const ManagedMod* {
        for (const ManagedMod& mod : mods_) {
            if (mod.applied && mod.manifest.id == id) {
                return &mod;
            }
        }
        return nullptr;
    };
    for (std::size_t round = 0U; round <= mods_.size(); ++round) {
        bool again = false;
        for (ManagedMod& mod : mods_) {
            if (!mod.applied) {
                continue;
            }
            std::vector<std::string> found;
            for (const ModDependency& dependency : mod.manifest.dependencies) {
                const ManagedMod* other = find_applied(dependency.id);
                if (other == nullptr) {
                    if (!dependency.optional) {
                        found.push_back("needs " + dependency.id +
                                        (dependency.version.empty() ? "" : " " + dependency.version));
                    }
                } else if (!dependency.version.empty() &&
                           !(other->manifest.parsed_version &&
                             mod_version_matches(*other->manifest.parsed_version, dependency.version))) {
                    found.push_back("needs " + dependency.id + " " + dependency.version + " (found " +
                                    (other->version.empty() ? std::string("no version") : other->version) + ")");
                }
            }
            for (const std::string& id : mod.manifest.incompatible) {
                if (find_applied(id) != nullptr) {
                    found.push_back("incompatible with " + id);
                }
            }
            if (!found.empty()) {
                mod.problems.insert(mod.problems.end(), found.begin(), found.end());
                mod.applied = false;
                again = true;
            }
        }
        if (!again) {
            break;
        }
    }

    // Application order: the player's order, moved so every mod follows what
    // it depends on or loads after and precedes what it loads before.
    application_order_.clear();
    std::vector<std::size_t> nodes;
    for (std::size_t index = 0U; index < mods_.size(); ++index) {
        if (mods_[index].applied) {
            nodes.push_back(index);
        }
    }
    const auto index_of = [&nodes, this](const std::string& id) -> std::optional<std::size_t> {
        for (const std::size_t node : nodes) {
            if (mods_[node].manifest.id == id) {
                return node;
            }
        }
        return std::nullopt;
    };
    // before[n]: the mods that must apply before n.
    std::map<std::size_t, std::vector<std::size_t>> before;
    for (const std::size_t node : nodes) {
        const ManagedMod& mod = mods_[node];
        for (const ModDependency& dependency : mod.manifest.dependencies) {
            if (const auto other = index_of(dependency.id)) {
                before[node].push_back(*other);
            }
        }
        for (const std::string& id : mod.manifest.load_after) {
            if (const auto other = index_of(id)) {
                before[node].push_back(*other);
            }
        }
        for (const std::string& id : mod.manifest.load_before) {
            if (const auto other = index_of(id)) {
                before[*other].push_back(node);
            }
        }
    }
    // Repeatedly take the first mod in the player's order whose
    // predecessors are all placed.
    std::vector<bool> placed(mods_.size(), false);
    while (application_order_.size() < nodes.size()) {
        bool progressed = false;
        for (const std::size_t node : nodes) {
            if (placed[node]) {
                continue;
            }
            bool ready = true;
            for (const std::size_t other : before[node]) {
                ready = ready && (placed[other] || other == node);
            }
            if (ready) {
                placed[node] = true;
                application_order_.push_back(node);
                progressed = true;
                break;
            }
        }
        if (!progressed) {
            // A cycle: the rest keep the player's order.
            for (const std::size_t node : nodes) {
                if (!placed[node]) {
                    placed[node] = true;
                    application_order_.push_back(node);
                    mods_[node].problems.push_back("load order hints form a cycle; the list order is used");
                }
            }
        }
    }
}

bool ModManager::rescan() {
    if (!active()) {
        return false;
    }
    std::vector<std::string> folders = discover_mod_folders(directory_);

    // The launcher's receipt (.catalog.json) names the folder of each
    // installed catalog mod's current revision. A malformed receipt is
    // ignored as a whole.
    std::map<std::string, std::string> catalog;
    {
        std::ifstream file(directory_ / ".catalog.json", std::ios::binary);
        std::error_code error;
        if (file && fs::file_size(directory_ / ".catalog.json", error) <= 1024U * 1024U && !error) {
            std::string source(1024U * 1024U + 1U, '\0');
            file.read(source.data(), static_cast<std::streamsize>(source.size()));
            source.resize(static_cast<std::size_t>(file.gcount()));
            JsonError problem;
            const std::optional<JsonValue> document =
                source.size() <= 1024U * 1024U ? parse_json(source, problem) : std::nullopt;
            const JsonValue* schema = document ? document->find("schema") : nullptr;
            const JsonValue* entries = document ? document->find("mods") : nullptr;
            bool valid = schema != nullptr && schema->is_number() && schema->as_number() == 1.0 &&
                         entries != nullptr && entries->is_array() && entries->as_array().size() <= 10000U;
            std::set<std::string> ids;
            if (valid) {
                for (const JsonValue& entry : entries->as_array()) {
                    const JsonValue* id = entry.find("id");
                    const JsonValue* folder = entry.find("folder");
                    const JsonValue* revision = entry.find("revision");
                    if (id == nullptr || !id->is_string() || folder == nullptr || !folder->is_string() ||
                        revision == nullptr || !revision->is_string() ||
                        !valid_catalog_entry(id->as_string(), revision->as_string()) ||
                        !ids.insert(id->as_string()).second) {
                        valid = false;
                        break;
                    }
                    // The launcher's folder name for this revision.
                    const auto& name = folder->as_string();
                    if (name != "catalog-" + id->as_string() + "-" + revision->as_string().substr(0U, 12U)) {
                        valid = false;
                        break;
                    }
                    // Not installed (any more): nothing to map.
                    if (std::find(folders.begin(), folders.end(), name) == folders.end()) {
                        continue;
                    }
                    if (read_mod_manifest(directory_ / name).id == id->as_string()) {
                        catalog.emplace(id->as_string(), name);
                    }
                }
            }
            if (!valid) {
                catalog.clear();
            }
        }
    }
    // Older revisions of a catalog mod stay on disk; only the current one is
    // listed.
    folders.erase(std::remove_if(folders.begin(), folders.end(),
                                 [&catalog](const auto& folder) {
                                     if (!folder.starts_with("catalog-")) {
                                         return false;
                                     }
                                     return std::none_of(catalog.begin(), catalog.end(),
                                                         [&folder](const auto& entry) {
                                                             return entry.second == folder;
                                                         });
                                 }),
                  folders.end());

    std::vector<ManagedMod> previous = mods_;
    // A catalog mod that was updated keeps its place under its new folder.
    for (ManagedMod& mod : previous) {
        if (!mod.folder.starts_with("catalog-")) {
            continue;
        }
        const auto selected = catalog.find(mod.manifest.id);
        if (selected != catalog.end()) {
            mod.folder = selected->second;
        }
    }

    std::vector<std::string> order;
    const bool listed_file = read_load_order(order);
    std::map<std::string, std::size_t> existing;
    for (std::size_t index = 0U; index < previous.size(); ++index) {
        existing.emplace(ascii_lower_copy(previous[index].folder), index);
    }
    std::map<std::string, bool> listed;
    for (const std::string& name : order) {
        listed.emplace(ascii_lower_copy(name), true);
    }

    // Folders in load_order.txt order first, then the rest alphabetically.
    std::vector<std::string> new_order;
    const auto add_folder = [&new_order](const std::string& folder) {
        if (std::find(new_order.begin(), new_order.end(), folder) == new_order.end()) {
            new_order.push_back(folder);
        }
    };
    if (listed_file) {
        for (const std::string& name : order) {
            for (const std::string& folder : folders) {
                if (ascii_lower_copy(folder) == ascii_lower_copy(name)) {
                    add_folder(folder);
                }
            }
        }
    }
    for (const std::string& folder : folders) {
        add_folder(folder);
    }

    // Known mods keep their flags and place; one whose folder is gone stays
    // listed as unavailable.
    std::vector<ManagedMod> next;
    next.reserve(mods_.size() + folders.size());
    for (const ManagedMod& old : previous) {
        ManagedMod kept = old;
        kept.available = false;
        next.push_back(std::move(kept));
    }
    for (const std::string& folder : new_order) {
        const auto found = existing.find(ascii_lower_copy(folder));
        if (found != existing.end()) {
            next[found->second].available = true;
            continue;
        }
        ManagedMod mod;
        mod.folder = folder;
        // New folders follow load_order.txt; without one, hidden folders and
        // catalog downloads wait for the player.
        mod.enabled = listed_file ? static_cast<bool>(listed.count(ascii_lower_copy(folder)))
                                  : !(hidden_folder(folder) || folder.starts_with("catalog-"));
        next.push_back(std::move(mod));
    }
    for (ManagedMod& mod : next) {
        if (!mod.available) {
            continue;
        }
        mod.manifest = read_mod_manifest(directory_ / mod.folder);
        mod.display_name = mod.manifest.name;
        mod.author.clear();
        for (const std::string& author : mod.manifest.authors) {
            mod.author += (mod.author.empty() ? "" : ", ") + author;
        }
        mod.version = mod.manifest.version;
        mod.description = mod.manifest.description;
    }

    // What the MODS page shows changed, or (below) a mod's problems did.
    bool changed = next.size() != mods_.size();
    if (!changed) {
        for (std::size_t index = 0U; index < next.size(); ++index) {
            const ManagedMod& before = mods_[index];
            const ManagedMod& after = next[index];
            if (before.folder != after.folder || before.enabled != after.enabled ||
                before.available != after.available || before.display_name != after.display_name) {
                changed = true;
                break;
            }
        }
    }
    std::vector<std::vector<std::string>> old_problems;
    for (const ManagedMod& mod : mods_) {
        old_problems.push_back(mod.problems);
    }
    mods_ = std::move(next);
    validate();
    if (!changed) {
        for (std::size_t index = 0U; index < mods_.size(); ++index) {
            if (mods_[index].problems != old_problems[index]) {
                changed = true;
                break;
            }
        }
    }
    remember_directory_time();
    return changed;
}

bool ModManager::set_enabled(const std::size_t index, const bool enabled) {
    if (index >= mods_.size() || mods_[index].enabled == enabled) {
        return false;
    }
    mods_[index].enabled = enabled;
    validate();
    return true;
}

std::optional<std::string> ModManager::select_enabled_folders(const std::vector<std::string>& folders) {
    // Built on a copy so a rejected selection leaves the current state alone.
    ModManager candidate = *this;
    std::vector<ManagedMod> selected;
    for (const auto& folder : folders) {
        const auto found = std::find_if(mods_.begin(), mods_.end(), [&folder](const ManagedMod& mod) {
            return mod.folder == folder && mod.available;
        });
        if (found == mods_.end()) {
            return "Missing mod: " + folder;
        }
        if (std::find_if(selected.begin(), selected.end(), [&folder](const ManagedMod& mod) {
                return mod.folder == folder;
            }) != selected.end()) {
            return "Duplicate mod: " + folder;
        }
        selected.push_back(*found);
        selected.back().enabled = true;
    }
    // Every other mod stays known, disabled, after the selection.
    for (ManagedMod mod : mods_) {
        if (std::find(folders.begin(), folders.end(), mod.folder) != folders.end()) {
            continue;
        }
        mod.enabled = false;
        selected.push_back(std::move(mod));
    }
    candidate.mods_ = std::move(selected);
    candidate.validate();
    if (candidate.application_order_.size() != folders.size()) {
        return "These mods have missing dependencies or conflicts.";
    }
    for (std::size_t i = 0U; i < folders.size(); ++i) {
        if (candidate.mods_[candidate.application_order_[i]].folder != folders[i]) {
            return "The lobby's mod order conflicts with its dependencies.";
        }
    }
    *this = std::move(candidate);
    return std::nullopt;
}

bool ModManager::move_up(const std::size_t index) {
    if (index == 0U || index >= mods_.size()) {
        return false;
    }
    std::swap(mods_[index - 1U], mods_[index]);
    validate();
    return true;
}

bool ModManager::move_down(const std::size_t index) {
    if (index + 1U >= mods_.size()) {
        return false;
    }
    std::swap(mods_[index], mods_[index + 1U]);
    validate();
    return true;
}

bool ModManager::apply_page_value(const std::size_t row, const std::uint32_t value) {
    if (row >= mods_.size()) {
        return false;
    }
    if (value == 0U) {
        return set_enabled(row, false);
    }
    ManagedMod moving = mods_[row];
    moving.enabled = true;
    mods_.erase(mods_.begin() + static_cast<std::ptrdiff_t>(row));
    // The k-th enabled slot: before the mod currently holding it, or at the
    // end when fewer mods are enabled.
    const std::size_t rank = static_cast<std::size_t>(value) - 1U;
    std::size_t insert_at = mods_.size();
    std::size_t enabled_seen = 0U;
    for (std::size_t index = 0U; index < mods_.size(); ++index) {
        if (!mods_[index].enabled) {
            continue;
        }
        if (enabled_seen == rank) {
            insert_at = index;
            break;
        }
        ++enabled_seen;
    }
    mods_.insert(mods_.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(moving));
    validate();
    return true;
}

std::optional<std::string> ModManager::save() const {
    if (!active()) {
        return std::nullopt;
    }
    const fs::path path = directory_ / std::string(kLoadOrderFile);
    std::error_code error;
    if (!fs::is_directory(directory_, error) || error) {
        error.clear();
        if (!fs::create_directories(directory_, error) || error) {
            return "could not create " + directory_.string() + ": " + error.message();
        }
    }
    // Written next to the old file and moved over it, so a crash never leaves
    // a half-written load order.
    const fs::path pending = path.string() + ".pending";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    if (!file) {
        return "could not write " + path.string();
    }
    file << "# Mods applied in this order; the last one wins.\n"
            "# Written by the in-game MODS page; one enabled mod folder per line.\n";
    for (const ManagedMod& mod : mods_) {
        if (mod.enabled && mod.available) {
            file << mod.folder << '\n';
        }
    }
    file.close();
    if (!file) {
        return "could not write " + path.string();
    }
    if (MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return "could not replace " + path.string();
    }
    return std::nullopt;
}

std::vector<ModManager::NativeClass> ModManager::car_classes() const {
    std::vector<NativeClass> result;
    for (const auto index : application_order_) {
        const fs::path root = directory_ / mods_[index].folder / "content" / "classes";
        std::error_code error;
        std::vector<fs::path> files;
        for (fs::directory_iterator it{root, error}, end; !error && it != end; it.increment(error)) {
            if (it->is_directory() && fs::is_regular_file(it->path() / "class.json", error)) {
                files.push_back(it->path() / "class.json");
            }
        }
        std::sort(files.begin(), files.end(), std::less<>{});
        for (const fs::path& path : files) {
            NativeClass group;
            std::ifstream file(path);
            std::stringstream source;
            source << file.rdbuf();
            JsonError problem;
            const auto value = parse_json(source.str(), problem);
            if (!value || !value->is_object()) {
                group.problem = path.string() + ": invalid class JSON";
            } else {
                const JsonValue* id = value->find("id");
                const JsonValue* name = value->find("name");
                if (id == nullptr || !id->is_string() || id->as_string().empty() || name == nullptr ||
                    !name->is_string() || name->as_string().empty()) {
                    group.problem = path.string() + ": class needs id and name";
                } else {
                    group.id = content_id(mods_[index].manifest.id, id->as_string());
                    group.name = name->as_string();
                }
                if (const JsonValue* order = value->find("order");
                    order != nullptr && order->is_number() && std::isfinite(order->as_number())) {
                    group.order = static_cast<int>(std::min(std::max(order->as_number(), -1000000.0), 1000000.0));
                }
                if (const JsonValue* modes = value->find("modes"); modes != nullptr) {
                    group.modes = 0U;
                    if (modes->is_array()) {
                        for (const JsonValue& mode : modes->as_array()) {
                            if (mode.is_string() && mode.as_string() == "race") {
                                group.modes |= 1U;
                            } else if (mode.is_string() && mode.as_string() == "crash") {
                                group.modes |= 2U;
                            } else {
                                group.problem = path.string() + ": modes must contain race or crash";
                            }
                        }
                    }
                    if (group.modes == 0U) {
                        group.problem = path.string() + ": class has no supported modes";
                    }
                }
            }
            // A later mod's class with the same id replaces the earlier one.
            if (group.problem.empty()) {
                result.erase(std::remove_if(result.begin(), result.end(),
                                            [&group](const NativeClass& other) { return other.id == group.id; }),
                             result.end());
            }
            result.push_back(std::move(group));
        }
    }
    return result;
}

ModOverlay ModManager::build_overlay() const {
    std::vector<std::string> order;
    for (const std::size_t index : application_order_) {
        order.push_back(mods_[index].folder);
    }
    return ModOverlay::scan(directory_, game_root_, order, false);
}

// RECOVERY: matches the original statement for statement; the only remaining
// difference is the operand order of three loop-exit comparisons, which comes
// from GCC's SSA name numbering (free-list reuse), not from the source.
std::vector<ModManager::AddedCar> ModManager::added_cars() const {
    std::vector<AddedCar> out;
    const auto overlay = build_overlay();
    const auto classes = car_classes();
    // Paint previews the launcher renders from a car's own car file
    // (tools/carmod/paint_thumbnails.py).
    const auto cached_previews = [&](AddedCar& car) {
        if (car.vehicle.size() <= 4) return;
        const std::string key = "pveh/" + ascii_lower_copy(car.vehicle.substr(0, 4)) + "/" +
            ascii_lower_copy(car.vehicle.substr(4)) + ".bgv";
        const auto* model = overlay.find(key);
        const auto model_hash = model == nullptr ? std::string{} : preview_hash(model->host_path);
        if (model_hash.empty()) return;
        Sha256 hash;
        hash.update("burn3-car-thumbnails-v2\n" + model_hash + "\n");
        const auto cache = game_root_.parent_path() / "cache/car-thumbnails/v2" / hash.finish();
        if (!fs::is_regular_file(cache / "ready.json")) return;
        for (std::size_t paint = 0; paint < 8; ++paint) {
            bool invalid = false;
            car.thumbnail_variants[paint] = item_image(cache, std::to_string(paint) + ".png", invalid);
        }
    };
    for (const std::size_t index : application_order_) {
        const fs::path cars = directory_ / mods_[index].folder / "content" / "cars";
        std::error_code error;
        std::vector<fs::path> files;
        for (fs::directory_iterator iterator{cars, fs::directory_options::skip_permission_denied, error}, end;
             !error && iterator != end; iterator.increment(error)) {
            const fs::path file = iterator->path() / "vehicle.json";
            if (iterator->is_directory() && fs::is_regular_file(file, error)) {
                files.push_back(file);
            }
        }
        std::sort(files.begin(), files.end(), std::less<>{});
        for (const fs::path& file : files) {
            AddedCar car;
            car.mod = mods_[index].folder;
            std::ifstream input(file, std::ios::binary);
            std::stringstream text;
            text << input.rdbuf();
            JsonError json_error;
            const auto json = parse_json(text.str(), json_error);
            const auto string_field = [&json](std::string_view key) -> std::string {
                const JsonValue* value = json && json->is_object() ? json->find(key) : nullptr;
                return value != nullptr && value->is_string() ? value->as_string() : std::string{};
            };
            if (!json || !json->is_object()) {
                car.problem = file.string() + ": " + (json ? std::string("not a JSON object") : json_error.describe());
            } else {
                car.vehicle = string_field("vehicle");
                car.template_vehicle = string_field("template");
                car.name = string_field("name");
                // A skin's description: the retail car whose car file the mod
                // has (ModOverlay::car_skins) and how the skin is shown.
                car.skin_of = ascii_upper_copy(string_field("skin_of"));
                if (!car.skin_of.empty()) {
                    car.vehicle.clear();
                    car.template_vehicle.clear();
                    for (const CarSkin& skin : overlay.car_skins()) {
                        if (skin.mod == mods_[index].folder && skin.original == car.skin_of) {
                            car.vehicle = skin.vehicle;
                            car.template_vehicle = skin.original;
                        }
                    }
                }
                car.class_id = content_id(mods_[index].manifest.id, string_field("class"));
                if (const JsonValue* handling = json->find("handling"); handling != nullptr) {
                    if (!handling->is_object()) {
                        car.problem = file.string() + ": handling must be an object";
                    } else {
                        for (const auto& [key, target] :
                             {std::pair{"mass_multiplier", &car.mass_multiplier},
                              std::pair{"top_speed_multiplier", &car.top_speed_multiplier}}) {
                            const JsonValue* value = handling->find(key);
                            if (value != nullptr) {
                                if (!value->is_number() || !std::isfinite(value->as_number()) ||
                                    value->as_number() <= 0.0 || value->as_number() > 100.0) {
                                    car.problem = file.string() + ": handling." + key +
                                                  " must be a positive finite multiplier up to 100";
                                } else {
                                    *target = static_cast<float>(value->as_number());
                                }
                            }
                        }
                    }
                }
                const auto thumbnail = string_field("thumbnail");
                bool unsafe_thumbnail = false;
                car.thumbnail = item_image(file.parent_path(),
                                           thumbnail.empty() ? std::string("thumbnail.png") : thumbnail,
                                           unsafe_thumbnail);
                if (unsafe_thumbnail) {
                    car.problem = file.string() + ": thumbnail must stay inside the car folder";
                }
                if (const JsonValue* variants = json->find("thumbnail_variants"); variants != nullptr) {
                    if (!variants->is_array() || variants->as_array().empty() ||
                        variants->as_array().size() > 8U) {
                        car.problem = file.string() + ": thumbnail_variants must contain one to eight image paths";
                    } else {
                        for (std::size_t paint = 0; paint < variants->as_array().size(); ++paint) {
                            const JsonValue& variant = variants->as_array()[paint];
                            if (!variant.is_string() || variant.as_string().empty()) {
                                car.problem = file.string() +
                                              ": thumbnail_variants must contain one to eight image paths";
                                break;
                            }
                            bool unsafe = false;
                            car.thumbnail_variants[paint] = item_image(file.parent_path(), variant.as_string(), unsafe);
                            if (unsafe) {
                                car.problem = file.string() + ": thumbnail_variants must stay inside the car folder";
                            }
                        }
                    }
                } else {
                    cached_previews(car);
                }
                if (const JsonValue* unlock = json->find("unlock"); unlock != nullptr && unlock->is_object()) {
                    if (const JsonValue* type = unlock->find("type"); type != nullptr && type->is_string()) {
                        car.unlock_type = type->as_string();
                    }
                    if (const JsonValue* count = unlock->find("count"); count != nullptr && count->is_number()) {
                        car.unlock_count = static_cast<std::uint32_t>(
                            std::min(std::max(count->as_number(), 0.0), 1000000000.0));
                    }
                    if (const JsonValue* event = unlock->find("event"); event != nullptr && event->is_string()) {
                        car.unlock_event = event->as_string();
                    } else if (event != nullptr && event->is_number()) {
                        car.unlock_event =
                            std::to_string(static_cast<long>(std::min(std::max(event->as_number(), 0.0), 1000000.0)));
                    }
                    if (const JsonValue* medal = unlock->find("medal"); medal != nullptr && medal->is_string()) {
                        const std::string name = ascii_lower_copy(medal->as_string());
                        car.unlock_medal = static_cast<std::uint8_t>(name == "gold" ? 3U : name == "silver" ? 2U : 1U);
                    }
                    if (const JsonValue* unlock_text = unlock->find("text");
                        unlock_text != nullptr && unlock_text->is_string()) {
                        car.unlock_text = unlock_text->as_string();
                    }
                }
                if (!car.skin_of.empty()) {
                    // A skin is driven and unlocked like its original.
                    car.class_id.clear();
                    car.mass_multiplier = car.top_speed_multiplier = 1.0F;
                    car.unlock_type = "template";
                    if (car.vehicle.empty()) {
                        car.problem = file.string() + ": the mod has no car file for " + car.skin_of +
                                      " (game/PVEH/<class>/<car>.BGV)";
                    } else if (car.name.empty()) {
                        car.name = mods_[index].display_name;
                    }
                } else if (car.vehicle.empty() || car.template_vehicle.empty()) {
                    car.problem = file.string() + ": needs \"vehicle\" and \"template\"";
                } else if (car.name.empty()) {
                    car.name = car.vehicle;
                }
                // A mod's own class must exist and be valid; retail classes
                // always do.
                if (!car.class_id.empty() && car.class_id.find(':') != std::string::npos &&
                    std::none_of(classes.begin(), classes.end(), [&car](const NativeClass& group) {
                        return group.problem.empty() && group.id == car.class_id;
                    })) {
                    car.problem = file.string() + ": native class is missing or invalid: " + car.class_id;
                }
            }
            // A later mod's car replaces an earlier one with the same vehicle.
            if (car.problem.empty()) {
                out.erase(std::remove_if(out.begin(), out.end(),
                                         [&car](const AddedCar& other) {
                                             return other.problem.empty() &&
                                                    ascii_lower_copy(other.vehicle) == ascii_lower_copy(car.vehicle);
                                         }),
                          out.end());
            }
            out.push_back(std::move(car));
        }
        // Skins the mod does not describe: shown with the mod's name.
        for (const CarSkin& skin : overlay.car_skins()) {
            if (skin.mod != mods_[index].folder ||
                std::any_of(out.begin(), out.end(), [&](const AddedCar& other) {
                    return other.mod == skin.mod && other.skin_of == skin.original;
                })) {
                continue;
            }
            AddedCar car;
            car.mod = skin.mod;
            car.vehicle = skin.vehicle;
            car.template_vehicle = skin.original;
            car.skin_of = skin.original;
            car.name = mods_[index].display_name;
            car.unlock_type = "template";
            cached_previews(car);
            out.push_back(std::move(car));
        }
    }
    return out;
}

std::string ModManager::describe() const {
    std::ostringstream out;
    out << "Mods: " << (active() ? directory_.string() : std::string("(none)")) << ": "
        << mods_.size() << " mod(s), " << enabled_count() << " enabled\n";
    for (const ManagedMod& mod : mods_) {
        out << "Mods:   " << (mod.enabled ? "[x] " : "[ ] ") << mod.display_name;
        if (mod.display_name != mod.folder) {
            out << " (" << mod.folder << ')';
        }
        if (!mod.version.empty()) {
            out << " v" << mod.version;
        }
        if (!mod.author.empty()) {
            out << " by " << mod.author;
        }
        if (!mod.available) {
            out << " (folder missing)";
        }
        out << '\n';
        for (const std::string& problem : mod.problems) {
            out << "Mods:       not applied: " << problem << '\n';
        }
    }
    return out.str();
}

} // namespace takedown::content
