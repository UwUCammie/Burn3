#include "takedown/content/mod_manifest.hpp"

#include "takedown/content/json.hpp"
#include "takedown/content/mod_overlay.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace takedown::content {
namespace {

std::string ascii_lower_copy(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
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

// The file `relative` names inside the mod folder `root`, matched
// case-insensitively in its last component so a manifest written on Windows
// still finds `Icon.PNG` on a case-sensitive file system. Empty when missing
// or when `relative` tries to leave the folder.
std::filesystem::path find_mod_file(const std::filesystem::path& root, std::string_view relative) {
    if (relative.empty()) {
        return {};
    }
    const std::filesystem::path wanted = std::filesystem::path(std::string(relative)).lexically_normal();
    if (wanted.is_absolute() || wanted.empty() || *wanted.begin() == std::filesystem::path("..")) {
        return {};
    }
    std::error_code error;
    const auto direct = root / wanted;
    if (std::filesystem::is_regular_file(direct, error)) {
        return direct;
    }
    const auto parent = direct.parent_path();
    const std::string lowered = ascii_lower_copy(wanted.filename().string());
    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator iterator(
        parent, std::filesystem::directory_options::skip_permission_denied, error);
    for (; !error && iterator != end; iterator.increment(error)) {
        if (ascii_lower_copy(iterator->path().filename().string()) == lowered &&
            iterator->is_regular_file()) {
            return iterator->path();
        }
    }
    return {};
}

// A version or version pattern: up to three numbers, where "x", "X" or "*"
// ends the pattern. `given` counts the numbers written.
struct Pattern {
    std::array<long, 3> parts{0, 0, 0};
    int given{0};
};

std::optional<Pattern> parse_pattern(std::string_view text) {
    Pattern pattern;
    if (text == "*" || text == "x" || text == "X") {
        return pattern;
    }
    // Pre-release and build suffixes do not take part in comparisons.
    text = text.substr(0, text.find_first_of("-+"));
    std::size_t part = 0U;
    while (!text.empty()) {
        if (part == 3U) {
            return std::nullopt;
        }
        const auto dot = text.find('.');
        const std::string_view piece = text.substr(0, dot);
        if (piece == "x" || piece == "X" || piece == "*") {
            break;
        }
        long value = 0;
        const auto [ptr, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (ec != std::errc{} || ptr != piece.data() + piece.size() || piece.empty() || value < 0) {
            return std::nullopt;
        }
        pattern.parts[part++] = value;
        pattern.given = static_cast<int>(part);
        if (dot == std::string_view::npos) {
            break;
        }
        text = text.substr(dot + 1U);
    }
    return pattern;
}

// Whether every space-separated term of a version range parses.
bool valid_range(std::string_view range) {
    std::size_t at = 0U;
    while (at < range.size()) {
        const auto space = range.find(' ', at);
        std::string_view term =
            trim(range.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at));
        if (!term.empty()) {
            const auto op = term.find_first_not_of("<>=^~");
            if (op > 2U || !parse_pattern(trim(term.substr(op))).has_value()) {
                return false;
            }
        }
        if (space == std::string_view::npos) {
            break;
        }
        at = space + 1U;
    }
    return true;
}

// A string or a list of strings; anything else is reported as a problem.
std::vector<std::string> string_list(const JsonValue* value, std::string_view field,
                                     std::vector<std::string>& problems) {
    std::vector<std::string> out;
    if (value == nullptr) {
        return out;
    }
    if (value->is_string()) {
        out.push_back(value->as_string());
        return out;
    }
    if (!value->is_array()) {
        problems.push_back(std::string(field) + " must be a list of strings");
        return out;
    }
    for (const auto& item : value->as_array()) {
        if (item.is_string()) {
            out.push_back(item.as_string());
        } else {
            problems.push_back(std::string(field) + " must be a list of strings");
            return out;
        }
    }
    return out;
}

// Fills `manifest` from a parsed mod.json, collecting malformed fields in
// `manifest.problems`.
void read_json_manifest(const JsonValue& root, ModManifest& manifest) {
    auto& problems = manifest.problems;
    if (!root.is_object()) {
        problems.push_back("mod.json must hold an object");
        return;
    }
    const auto text = [&](std::string_view field, std::string& out, bool required) {
        const JsonValue* value = root.find(field);
        if (value == nullptr) {
            if (required) {
                problems.push_back(std::string(field) + " is missing");
            }
            return;
        }
        if (!value->is_string()) {
            problems.push_back(std::string(field) + " must be a string");
            return;
        }
        out = value->as_string();
    };

    const JsonValue* format = root.find("format");
    if (format == nullptr || !format->is_number() || format->as_number() != 1.0) {
        problems.push_back("format must be 1");
    }

    std::string id;
    text("id", id, true);
    if (!id.empty()) {
        if (valid_mod_id(id)) {
            manifest.id = id;
        } else {
            problems.push_back("id \"" + id + "\" may only use a-z, 0-9, '.', '-' and '_'");
        }
    }
    std::string name;
    text("name", name, true);
    if (!name.empty()) {
        manifest.name = name;
    }
    text("version", manifest.version, true);
    text("description", manifest.description, false);
    text("game", manifest.game, false);
    text("homepage", manifest.homepage, false);

    manifest.authors = string_list(root.find("authors"), "authors", problems);
    const JsonValue* author = root.find("author");
    if (author != nullptr && author->is_string()) {
        manifest.authors.insert(manifest.authors.begin(), author->as_string());
    }

    const JsonValue* api = root.find("api");
    if (api != nullptr) {
        if (api->is_number() && api->as_number() >= 1.0 && api->as_number() < 1000.0) {
            manifest.api = static_cast<int>(api->as_number());
        } else {
            problems.push_back("api must be a whole number from 1");
        }
    }

    manifest.tags = string_list(root.find("tags"), "tags", problems);
    manifest.incompatible = string_list(root.find("incompatible"), "incompatible", problems);
    manifest.load_after = string_list(root.find("load_after"), "load_after", problems);
    manifest.load_before = string_list(root.find("load_before"), "load_before", problems);

    const JsonValue* dependencies = root.find("dependencies");
    if (dependencies != nullptr) {
        if (!dependencies->is_array()) {
            problems.push_back("dependencies must be a list");
        } else {
            for (const auto& entry : dependencies->as_array()) {
                ModDependency dependency;
                if (entry.is_string()) {
                    dependency.id = entry.as_string();
                } else {
                    const JsonValue* dependency_id = entry.find("id");
                    if (dependency_id != nullptr && dependency_id->is_string()) {
                        dependency.id = dependency_id->as_string();
                        const JsonValue* version = entry.find("version");
                        if (version != nullptr && version->is_string()) {
                            dependency.version = version->as_string();
                        }
                        const JsonValue* optional = entry.find("optional");
                        if (optional != nullptr && optional->is_bool()) {
                            dependency.optional = optional->as_bool();
                        }
                    } else {
                        problems.push_back("a dependency needs an id");
                        continue;
                    }
                }
                if (!valid_range(dependency.version)) {
                    problems.push_back("dependency " + dependency.id + ": malformed version range \"" +
                                       dependency.version + '"');
                }
                manifest.dependencies.push_back(std::move(dependency));
            }
        }
    }
}

// Whether the first `count` parts of `version` equal the pattern's.
bool leading_parts_match(const ModVersion& version, const Pattern& pattern, int count) {
    for (int index = 0; index < count; ++index) {
        if (version.parts[index] != pattern.parts[index]) {
            return false;
        }
    }
    return true;
}

// One term of a version range against `version`.
bool term_matches(const ModVersion& version, std::string_view term) {
    std::string_view op;
    for (const std::string_view candidate : {">=", ">", "<=", "<", "^", "~", "="}) {
        if (term.substr(0, candidate.size()) == candidate) {
            op = candidate;
            break;
        }
    }
    const auto pattern = parse_pattern(trim(term.substr(op.size())));
    if (!pattern) {
        return false;
    }
    const ModVersion base{pattern->parts};
    if (op == ">=") {
        return version >= base;
    }
    if (op == ">") {
        return version > base;
    }
    if (op == "<=") {
        return version <= base;
    }
    if (op == "<") {
        return version < base;
    }
    if (op == "^") {
        return version >= base && version.parts[0] == base.parts[0];
    }
    if (op == "~") {
        return version >= base && leading_parts_match(version, *pattern, pattern->given < 2 ? pattern->given : 2);
    }
    // "=" or no operator: every written part must match.
    return leading_parts_match(version, *pattern, pattern->given);
}

// The whole file, or nullopt when it cannot be opened.
std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

std::string ModVersion::text() const {
    return std::to_string(parts[0]) + '.' + std::to_string(parts[1]) + '.' + std::to_string(parts[2]);
}

std::optional<ModVersion> parse_mod_version(std::string_view text) {
    text = trim(text);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    const auto pattern = parse_pattern(text);
    if (!pattern || pattern->given == 0) {
        return std::nullopt;
    }
    return ModVersion{pattern->parts};
}

bool mod_version_matches(const ModVersion& version, std::string_view range) {
    range = trim(range);
    if (range.empty()) {
        return true;
    }
    std::size_t at = 0U;
    bool any = false;
    while (at < range.size()) {
        const auto space = range.find(' ', at);
        const std::string_view term =
            trim(range.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at));
        if (!term.empty()) {
            if (!term_matches(version, term)) {
                return false;
            }
            any = true;
        }
        if (space == std::string_view::npos) {
            break;
        }
        at = space + 1U;
    }
    return any;
}

bool valid_mod_id(std::string_view id) {
    if (id.empty() || id.size() > 64U) {
        return false;
    }
    for (const char c : id) {
        if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

ModManifest read_mod_manifest(const std::filesystem::path& root) {
    ModManifest manifest;
    const std::string folder = root.filename().string();
    manifest.name = folder;
    manifest.id = ascii_lower_copy(folder);
    std::string icon_name = "icon.png";

    if (const auto json_path = find_mod_file(root, "mod.json"); !json_path.empty()) {
        manifest.json = true;
        const auto text = read_text(json_path);
        if (!text) {
            manifest.problems.push_back("mod.json cannot be read");
        } else {
            JsonError error;
            if (const auto document = parse_json(*text, error); !document) {
                manifest.problems.push_back("mod.json: " + error.describe());
            } else {
                read_json_manifest(*document, manifest);
                const JsonValue* icon = document->find("icon");
                if (icon != nullptr && icon->is_string()) {
                    icon_name = icon->as_string();
                    if (find_mod_file(root, icon_name).empty()) {
                        manifest.problems.push_back("icon " + icon_name + " not found");
                    }
                }
            }
        }
    } else {
        // Older mods describe themselves in mod.txt / mod.ini.
        const auto metadata = read_mod_metadata(root);
        if (!metadata.name.empty()) {
            manifest.name = metadata.name;
        }
        if (!metadata.author.empty()) {
            manifest.authors.push_back(metadata.author);
        }
        manifest.version = metadata.version;
        manifest.description = metadata.description;
    }

    if (!manifest.version.empty()) {
        manifest.parsed_version = parse_mod_version(manifest.version);
        if (!manifest.parsed_version && manifest.json) {
            manifest.problems.push_back("version \"" + manifest.version +
                                        "\" is not major.minor.patch");
        }
    }
    if (manifest.api > kModApiVersion) {
        manifest.problems.push_back("needs mod API " + std::to_string(manifest.api) +
                                    "; this game supports " + std::to_string(kModApiVersion));
    }
    manifest.icon = find_mod_file(root, icon_name);
    manifest.preview = find_mod_file(root, "preview.png");
    return manifest;
}

} // namespace takedown::content
