#pragma once

// A mod's manifest (docs/MOD_API.md, "Manifest"): `mod.json`, or the older
// `mod.txt` / `mod.ini` key = value file. Reading never fails: problems are
// collected in `problems` and the mod list shows them.

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::content {

// The mod API version this build implements.
inline constexpr int kModApiVersion = 1;

struct ModVersion {
    std::array<long, 3> parts{0, 0, 0};
    [[nodiscard]] std::string text() const;
    friend bool operator==(const ModVersion&, const ModVersion&) = default;
    friend auto operator<=>(const ModVersion&, const ModVersion&) = default;
};

// "1", "1.2" or "1.2.3", with an optional "-pre" or "+build" suffix (ignored).
[[nodiscard]] std::optional<ModVersion> parse_mod_version(std::string_view text);

// True when `version` satisfies `range`: space-separated comparisons
// (">=1.2", "<2.0.0", "=1.0.0", "1.0.0"), caret ("^1.2": same major, at least
// 1.2), tilde ("~1.2": same major and minor), wildcards ("1.x", "1.2.*", "*")
// or empty (anything). False for a malformed range.
[[nodiscard]] bool mod_version_matches(const ModVersion& version, std::string_view range);

struct ModDependency {
    std::string id;
    std::string version; // range, empty = any
    bool optional{false};
};

struct ModManifest {
    bool json{false};              // read from mod.json
    std::string id;                // manifest id, else the lowercased folder name
    std::string name;              // display name, else the folder name
    std::string version;           // as written
    std::optional<ModVersion> parsed_version;
    std::vector<std::string> authors;
    std::string description;
    std::string game;
    int api{kModApiVersion};
    std::vector<std::string> tags;
    std::string homepage;
    std::filesystem::path icon;     // existing icon file, empty when none
    std::filesystem::path preview;  // existing preview picture, empty when none
    std::vector<ModDependency> dependencies;
    std::vector<std::string> incompatible;
    std::vector<std::string> load_after;
    std::vector<std::string> load_before;
    std::vector<std::string> problems; // malformed manifest fields
};

// Reads the manifest of the mod folder `root` (its folder name is the
// fallback id and name).
[[nodiscard]] ModManifest read_mod_manifest(const std::filesystem::path& root);

// Whether `id` is a valid manifest id: 1 to 64 of a-z, 0-9, '.', '-', '_'.
[[nodiscard]] bool valid_mod_id(std::string_view id);

} // namespace takedown::content
