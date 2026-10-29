#include "takedown/content/mod_overlay.hpp"

#include "takedown/content/disc_path_resolver.hpp"
#include "takedown/content/json.hpp"
#include "takedown/content/sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// RECOVERY: rebuilt from the compiled GIMPLE. Every function's code matches the
// original; list_mod_folders, make_car_skins, scan_impl, the two-argument scan,
// ~ModOverride, the FoundMod sort helper and operator+(.constprop) still differ
// only in which small library calls (path/string moves and destructors,
// string append/reserve) GCC's early inliner expanded, which follows the
// compiler's internal processing order rather than anything in this file.
namespace takedown::content {
namespace {

constexpr std::string_view kLoadOrderFile = "load_order.txt";

// Classes of the cars a player can drive (PVEH/<class>/); only these become
// car skins.
constexpr std::array<std::string_view, 7> kPlayerCarClasses{
    "comp", "mscl", "cupe", "sprt", "supr", "hspc", "hevy"};

// One mod folder found under the mods directory.
struct FoundMod {
    std::string name;
    std::filesystem::path root;
};

// One file of a mod, already mapped to its game path.
struct ModFile {
    std::string key;      // normalized game path
    std::string relative; // path inside the mod folder, for messages
    std::filesystem::path path;
    std::uint64_t size{0};
};

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

// Files at the top of a mod folder that describe the mod rather than replace
// a game file.
bool is_reserved_mod_root_file(std::string_view lowered_name) {
    const auto stem = lowered_name.substr(0, lowered_name.find('.'));
    return lowered_name == "mod.txt" || lowered_name == "mod.ini" || lowered_name == "mod.json" ||
           lowered_name == "icon.png" || lowered_name == "preview.png" ||
           lowered_name == "settings.json" || lowered_name == "settings.user.json" ||
           stem == "readme" || stem == "license" || stem == "changelog";
}

// Top-level folders the mod API owns (docs/MOD_API.md); never game files.
bool is_reserved_mod_folder(std::string_view lowered_name) {
    return lowered_name == "content" || lowered_name == "patches" || lowered_name == "locale" ||
           lowered_name == "source";
}

std::vector<FoundMod> list_mod_folders(const std::filesystem::path& mods_dir) {
    std::vector<FoundMod> mods;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        mods_dir, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        std::error_code entry_error;
        if (!iterator->is_directory(entry_error) || entry_error) {
            continue;
        }
        mods.push_back({iterator->path().filename().string(), iterator->path()});
    }
    std::sort(mods.begin(), mods.end(), [](const FoundMod& left, const FoundMod& right) {
        const std::string left_key = ascii_lower_copy(left.name);
        const std::string right_key = ascii_lower_copy(right.name);
        if (left_key != right_key) {
            return left_key < right_key;
        }
        return left.name < right.name;
    });
    return mods;
}

std::string upper_copy(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return result;
}

// The ID a car skin's name is derived from: mod.json's "id", else the folder
// name.
std::string mod_identifier(const FoundMod& mod) {
    std::ifstream file(mod.root / "mod.json", std::ios::binary);
    if (file) {
        std::stringstream text;
        text << file.rdbuf();
        JsonError error;
        const auto json = parse_json(text.str(), error);
        if (json && json->is_object()) {
            const JsonValue* id = json->find("id");
            if (id != nullptr && id->is_string() && !id->as_string().empty()) {
                return id->as_string();
            }
        }
    }
    return mod.name;
}

// Turns the mod's retail player car files (PVEH/<class>/<car>.BGV) into car
// skins: the files move to the skin's own vehicle name, and the retail car's
// .HWD/.LWD are borrowed from the game when the mod has none.
void make_car_skins(const FoundMod& mod, const std::filesystem::path& game_root,
                    std::vector<ModFile>& files, std::vector<CarSkin>& skins) {
    std::string identifier;
    std::vector<ModFile> companions;
    for (const ModFile& file : files) {
        const std::string& key = file.key;
        if (key.size() < 15U || !key.starts_with("pveh/") || key[9] != '/' ||
            !key.ends_with(".bgv")) {
            continue;
        }
        const std::string class_name = key.substr(5, 4);
        const std::string stem = key.substr(10, key.size() - 14U);
        if (std::find(kPlayerCarClasses.begin(), kPlayerCarClasses.end(), class_name) ==
                kPlayerCarClasses.end() ||
            stem.empty() || stem.size() > 8U ||
            !std::all_of(stem.begin(), stem.end(), [](char character) {
                return (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9');
            })) {
            continue;
        }
        std::error_code error;
        if (!resolve_disc_path(game_root, key, error)) {
            continue; // not a retail car: an added car, served as it is
        }
        if (identifier.empty()) {
            identifier = mod_identifier(mod);
        }

        CarSkin skin{mod.name, upper_copy(class_name + stem), {}};
        skin.vehicle = car_skin_vehicle(identifier, skin.original);
        const std::string from = "pveh/" + class_name + "/" + stem + ".";
        const std::string to = "pveh/" + ascii_lower_copy(skin.vehicle.substr(0, 4)) + "/" +
                               ascii_lower_copy(skin.vehicle.substr(4)) + ".";
        for (const std::string_view extension : {"hwd", "lwd"}) {
            if (std::find_if(files.begin(), files.end(), [&from, &extension](const ModFile& other) {
                    return other.key == from + std::string(extension);
                }) != files.end()) {
                continue;
            }
            std::error_code base_error;
            const auto base = resolve_disc_path(game_root, from + std::string(extension), base_error);
            if (!base) {
                continue;
            }
            std::error_code size_error;
            const auto size = std::filesystem::file_size(*base, size_error);
            if (!size_error) {
                companions.push_back({to + std::string(extension),
                                      "(base game " + from + std::string(extension) + ")", *base,
                                      size});
            }
        }
        skins.push_back(std::move(skin));
    }

    for (const CarSkin& skin : skins) {
        if (skin.mod != mod.name) {
            continue;
        }
        const std::string original = ascii_lower_copy(skin.original);
        const std::string from = "pveh/" + original.substr(0, 4) + "/" + original.substr(4) + ".";
        const std::string to = "pveh/" + ascii_lower_copy(skin.vehicle.substr(0, 4)) + "/" +
                               ascii_lower_copy(skin.vehicle.substr(4)) + ".";
        for (ModFile& file : files) {
            for (const std::string_view extension : {"bgv", "hwd", "lwd"}) {
                if (file.key == from + std::string(extension)) {
                    file.key = to + std::string(extension);
                }
            }
        }
    }
    files.insert(files.end(), companions.begin(), companions.end());
}

} // namespace

std::string car_skin_vehicle(const std::string_view mod_id, const std::string_view original) {
    Sha256 hash;
    hash.update("burn3-car-skin-v1\n" + ascii_lower_copy(mod_id) + "\n" + upper_copy(original) +
                "\n");
    const std::string digest = hash.finish();
    std::uint64_t value = std::stoull(digest.substr(0, 15), nullptr, 16);
    std::string name = upper_copy(original.substr(0, 4)) + "X";
    for (int index = 0; index < 7; ++index) {
        name.push_back("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[value % 36U]);
        value /= 36U;
    }
    return name;
}

ModOverlay ModOverlay::scan(const std::filesystem::path& mods_dir,
                            const std::filesystem::path& game_root) {
    ModOverlay overlay;
    overlay.directory_ = mods_dir;
    std::error_code error;
    if (mods_dir.empty() || !std::filesystem::is_directory(mods_dir, error) || error) {
        return overlay;
    }

    const auto candidates = list_mod_folders(mods_dir);
    std::vector<std::string> enabled;
    const std::filesystem::path load_order_path = mods_dir / std::string(kLoadOrderFile);
    std::ifstream load_order(load_order_path);
    bool report_unlisted = false;
    if (load_order) {
        report_unlisted = true;
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(load_order, line)) {
            ++line_number;
            const std::string_view entry = trim(line);
            if (entry.empty() || entry.front() == '#' || entry.front() == ';') {
                continue;
            }
            const std::string lowered = ascii_lower_copy(entry);
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                                            [&lowered](const FoundMod& candidate) {
                                                return ascii_lower_copy(candidate.name) == lowered;
                                            });
            std::ostringstream where;
            where << kLoadOrderFile << ':' << line_number << ": ";
            if (found == candidates.end()) {
                overlay.warnings_.push_back(where.str() + "no mod folder named \"" +
                                            std::string(entry) + "\"; ignored");
                continue;
            }
            if (std::find(enabled.begin(), enabled.end(), found->name) != enabled.end()) {
                overlay.warnings_.push_back(where.str() + "mod \"" + found->name +
                                            "\" is listed twice; the first entry is used");
                continue;
            }
            enabled.push_back(found->name);
        }
    } else {
        // No load order: every folder is enabled, except dot-folders.
        for (const auto& candidate : candidates) {
            if (!candidate.name.empty() && candidate.name.front() != '.') {
                enabled.push_back(candidate.name);
            }
        }
    }
    scan_impl(overlay, mods_dir, game_root, enabled, report_unlisted);
    return overlay;
}

ModOverlay ModOverlay::scan(const std::filesystem::path& mods_dir,
                            const std::filesystem::path& game_root,
                            const std::vector<std::string>& enabled_mods,
                            const bool report_unlisted) {
    ModOverlay overlay;
    overlay.directory_ = mods_dir;
    scan_impl(overlay, mods_dir, game_root, enabled_mods, report_unlisted);
    return overlay;
}

void ModOverlay::scan_impl(ModOverlay& overlay, const std::filesystem::path& mods_dir,
                           const std::filesystem::path& game_root,
                           const std::vector<std::string>& enabled_mods,
                           const bool report_unlisted) {
    std::error_code error;
    const auto candidates = list_mod_folders(mods_dir);
    std::vector<FoundMod> enabled;
    enabled.reserve(enabled_mods.size());
    for (const auto& name : enabled_mods) {
        const std::string lowered = ascii_lower_copy(name);
        const auto found = std::find_if(candidates.begin(), candidates.end(),
                                        [&lowered](const FoundMod& candidate) {
                                            return ascii_lower_copy(candidate.name) == lowered;
                                        });
        if (found == candidates.end()) {
            continue;
        }
        // A name listed twice keeps its first position.
        if (std::find_if(enabled.begin(), enabled.end(), [&found](const FoundMod& mod) {
                return mod.name == found->name;
            }) == enabled.end()) {
            enabled.push_back(*found);
        }
    }

    if (report_unlisted) {
        for (const auto& candidate : candidates) {
            if (std::find_if(enabled.begin(), enabled.end(), [&candidate](const FoundMod& mod) {
                    return mod.name == candidate.name;
                }) != enabled.end()) {
                continue;
            }
            if (!candidate.name.empty() && candidate.name.front() != '.') {
                overlay.warnings_.push_back("mod folder \"" + candidate.name +
                                            "\" is not listed in " + std::string(kLoadOrderFile) +
                                            "; disabled");
            }
        }
    }

    for (const auto& mod : enabled) {
        std::vector<ModFile> files;
        std::filesystem::recursive_directory_iterator iterator(
            mod.root, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code entry_error;
            if (!iterator->is_regular_file(entry_error) || entry_error) {
                continue;
            }
            const std::filesystem::path relative = iterator->path().lexically_relative(mod.root);
            if (relative.empty() || *relative.begin() == std::filesystem::path("..")) {
                continue;
            }
            const std::string relative_text = relative.generic_string();
            const auto slash = relative_text.find('/');
            const std::string top = ascii_lower_copy(relative_text.substr(0, slash));
            if (slash == std::string::npos ? is_reserved_mod_root_file(top)
                                           : is_reserved_mod_folder(top)) {
                continue;
            }
            // game/ holds files whose names would otherwise look like mod files.
            const std::string game_path = slash != std::string::npos && top == "game"
                                              ? relative_text.substr(slash + 1U)
                                              : relative_text;
            std::error_code key_error;
            const auto key = normalize_disc_guest_path(game_path, key_error);
            if (!key) {
                overlay.warnings_.push_back(mod.name + "/" + relative_text +
                                            ": not a valid game path; ignored");
                continue;
            }
            const std::uint64_t size = iterator->file_size(entry_error);
            if (entry_error) {
                overlay.warnings_.push_back(mod.name + "/" + relative_text +
                                            ": cannot read file size; ignored");
                continue;
            }
            if (size > 0xFFFFFFFFULL) {
                overlay.warnings_.push_back(
                    mod.name + "/" + relative_text +
                    ": larger than 4 GiB, which the file service cannot serve; ignored");
                continue;
            }
            files.push_back({*key, relative_text, iterator->path(), size});
        }
        error.clear();

        if (!game_root.empty()) {
            make_car_skins(mod, game_root, files, overlay.car_skins_);
        }
        std::sort(files.begin(), files.end(), [](const ModFile& left, const ModFile& right) {
            if (left.key != right.key) {
                return left.key < right.key;
            }
            return left.relative < right.relative;
        });

        for (std::size_t index = 0; index < files.size(); ++index) {
            if (index != 0U && files[index - 1U].key == files[index].key) {
                overlay.warnings_.push_back(
                    mod.name + "/" + files[index].relative + ": same game path as " + mod.name +
                    "/" + files[index - 1U].relative +
                    " (names differ only by case, or the file is both in game/ and at the top "
                    "level); ignored");
                continue;
            }
            const ModFile& file = files[index];
            const auto found = overlay.index_.find(file.key);
            if (found != overlay.index_.end()) {
                ModOverride& existing = overlay.overrides_[found->second];
                existing.shadowed.push_back(existing.mod);
                existing.mod = mod.name;
                existing.host_path = file.path;
                existing.size = file.size;
                continue;
            }
            overlay.index_.emplace(file.key, overlay.overrides_.size());
            overlay.overrides_.push_back({file.key, file.path, mod.name, file.size});
        }
        overlay.active_mods_.push_back(mod.name);
    }

    std::sort(overlay.overrides_.begin(), overlay.overrides_.end(),
              [](const ModOverride& left, const ModOverride& right) { return left.key < right.key; });
    overlay.index_.clear();
    for (std::size_t index = 0; index < overlay.overrides_.size(); ++index) {
        ModOverride& entry = overlay.overrides_[index];
        overlay.index_.emplace(entry.key, index);
        if (!game_root.empty()) {
            std::error_code resolve_error;
            entry.replaces_base = resolve_disc_path(game_root, entry.key, resolve_error).has_value();
        }
    }
}

std::vector<std::string> discover_mod_folders(const std::filesystem::path& mods_dir) {
    std::vector<std::string> names;
    for (const auto& mod : list_mod_folders(mods_dir)) {
        names.push_back(mod.name);
    }
    return names;
}

ModMetadata read_mod_metadata(const std::filesystem::path& mod_root) {
    ModMetadata metadata;
    for (const std::string_view file_name : {"mod.txt", "mod.ini"}) {
        std::ifstream file(mod_root / std::string(file_name));
        if (!file) {
            continue;
        }
        std::string line;
        while (std::getline(file, line)) {
            const std::string_view stripped = trim(line);
            if (stripped.empty() || stripped.front() == '#' || stripped.front() == ';') {
                continue;
            }
            std::size_t separator = stripped.find('=');
            const std::size_t colon = stripped.find(':');
            if (separator == std::string_view::npos) {
                separator = colon;
            } else if (colon < separator) {
                separator = colon;
            }
            if (separator == std::string_view::npos) {
                continue;
            }
            const std::string key = ascii_lower_copy(trim(stripped.substr(0, separator)));
            const std::string value(trim(stripped.substr(separator + 1U)));
            if (key == "name" || key == "title") {
                if (metadata.name.empty()) {
                    metadata.name = value;
                }
            } else if (key == "author") {
                metadata.author = value;
            } else if (key == "version") {
                metadata.version = value;
            } else if (key == "description" || key == "desc") {
                metadata.description = value;
            }
        }
        return metadata;
    }
    return metadata;
}

const ModOverride* ModOverlay::find(std::string_view key) const {
    if (overrides_.empty()) {
        return nullptr;
    }
    const auto found = index_.find(std::string(key));
    if (found == index_.end()) {
        return nullptr;
    }
    return &overrides_[found->second];
}

std::string ModOverlay::describe(std::size_t max_override_lines) const {
    std::ostringstream out;
    std::size_t replaced = 0;
    for (const auto& entry : overrides_) {
        replaced += entry.replaces_base ? 1U : 0U;
    }
    out << "Mods: " << directory_.string() << ": " << active_mods_.size() << " mod(s), "
        << overrides_.size() << " file override(s) (" << replaced << " replace game files, "
        << overrides_.size() - replaced << " add files)\n";
    for (std::size_t index = 0; index < active_mods_.size(); ++index) {
        out << "Mods:   load order " << index + 1U << ": " << active_mods_[index] << '\n';
    }
    std::size_t printed = 0;
    for (const auto& entry : overrides_) {
        if (printed == max_override_lines) {
            out << "Mods:   ... " << overrides_.size() - printed << " more override(s) not listed\n";
            break;
        }
        ++printed;
        out << "Mods:   " << entry.key << " <- " << entry.mod << " ("
            << (entry.replaces_base ? "replaces" : "adds") << ", " << entry.size << " bytes";
        if (!entry.shadowed.empty()) {
            out << "; overrides";
            for (const auto& name : entry.shadowed) {
                out << ' ' << name;
            }
        }
        out << ")\n";
    }
    for (const auto& skin : car_skins_) {
        out << "Mods:   car skin " << skin.vehicle << " of " << skin.original << " <- " << skin.mod
            << '\n';
    }
    for (const auto& warning : warnings_) {
        out << "Mods: warning: " << warning << '\n';
    }
    return out.str();
}

std::optional<std::filesystem::path>
resolve_game_path(const std::filesystem::path& game_root, const ModOverlay* overlay,
                  std::string_view guest_path, std::error_code& error) {
    if (overlay != nullptr && !overlay->empty()) {
        error.clear();
        const auto key = normalize_disc_guest_path(guest_path, error);
        if (key) {
            if (const ModOverride* entry = overlay->find(*key); entry != nullptr) {
                return entry->host_path;
            }
        } else {
            return std::nullopt;
        }
    }
    return resolve_disc_path(game_root, guest_path, error);
}

std::optional<std::filesystem::path>
select_mods_directory(const std::filesystem::path& game_root,
                      const std::optional<std::filesystem::path>& explicit_directory,
                      bool enabled) {
    if (!enabled) {
        return std::nullopt;
    }
    if (explicit_directory && !explicit_directory->empty()) {
        return *explicit_directory;
    }
    std::error_code error;
    const auto absolute_root = std::filesystem::absolute(game_root, error);
    if (error) {
        return std::nullopt;
    }
    return (absolute_root.lexically_normal() / ".." / "mods").lexically_normal();
}

} // namespace takedown::content
