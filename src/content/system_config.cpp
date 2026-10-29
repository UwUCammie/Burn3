#include "takedown/content/system_config.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <system_error>

namespace takedown::content {
namespace {

constexpr std::array<std::string_view, 8> kKnownDiscIds{
    "SLUS-21050", "SLES-52584", "SLES-52585", "SLKA-25206",
    "SLPM-65719", "SLPM-65958", "SLPM-66962", "SLAJ-25053",
};

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               const auto lower = [](char character) {
                   if (character >= 'A' && character <= 'Z') {
                       return static_cast<char>(character - 'A' + 'a');
                   }
                   return character;
               };
               return lower(a) == lower(b);
           });
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

// ISO-9660 names may carry a ";1" version suffix that the extracted files lack.
std::string strip_iso_version(std::string_view name) {
    const auto separator = name.rfind(';');
    if (separator == std::string_view::npos || separator + 1 == name.size()) {
        return std::string(name);
    }
    const bool numeric = std::all_of(name.begin() + static_cast<std::ptrdiff_t>(separator + 1), name.end(),
                                     [](char character) { return character >= '0' && character <= '9'; });
    if (numeric) {
        return std::string(name.substr(0, separator));
    }
    return std::string(name);
}

bool equivalent_iso_name(std::string_view left, std::string_view right) {
    return ascii_iequals(left, right) || ascii_iequals(strip_iso_version(left), strip_iso_version(right));
}

std::size_t path_depth(const std::filesystem::path& path) {
    return static_cast<std::size_t>(std::distance(path.begin(), path.end()));
}

std::optional<std::filesystem::path> find_child_case_insensitively(const std::filesystem::path& directory,
                                                                   std::string_view wanted,
                                                                   bool needs_directory) {
    std::vector<std::filesystem::path> matches;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, iterator_error);
    const std::filesystem::directory_iterator end;
    while (!iterator_error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code status_error;
        const bool symlink = entry.is_symlink(status_error);
        if (!status_error && !symlink && equivalent_iso_name(entry.path().filename().string(), wanted)) {
            const bool correct_type =
                needs_directory ? entry.is_directory(status_error) : entry.is_regular_file(status_error);
            if (!status_error && correct_type) {
                matches.push_back(entry.path());
            }
        }
        iterator.increment(iterator_error);
    }
    if (matches.empty()) {
        return std::nullopt;
    }
    // Several names can differ only by case; pick one deterministically.
    std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
        return left.generic_string() < right.generic_string();
    });
    return matches.front();
}

std::vector<std::string> boot_path_components(std::string_view boot2) {
    boot2 = trim(boot2);
    if (boot2.size() > 1 && ((boot2.front() == '"' && boot2.back() == '"') ||
                             (boot2.front() == '\'' && boot2.back() == '\''))) {
        boot2.remove_prefix(1);
        boot2.remove_suffix(1);
        boot2 = trim(boot2);
    }

    const std::size_t colon = boot2.find(':');
    if (colon != std::string_view::npos) {
        if (!ascii_iequals(trim(boot2.substr(0, colon)), "cdrom0")) {
            throw SystemConfigError("BOOT2 uses an unsupported device: " + std::string(boot2));
        }
        boot2.remove_prefix(colon + 1);
    }

    std::string normalized(boot2);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }

    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t slash = normalized.find('/', start);
        const std::size_t count = slash == std::string::npos ? std::string::npos : slash - start;
        std::string component = normalized.substr(start, count);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                throw SystemConfigError("BOOT2 path attempts to leave the disc root");
            }
            result.push_back(std::move(component));
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    if (result.empty()) {
        throw SystemConfigError("BOOT2 does not name an executable");
    }
    return result;
}

} // namespace

std::optional<std::string_view> SystemConfig::value(std::string_view key) const noexcept {
    for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
        if (ascii_iequals(iterator->first, key)) {
            return iterator->second;
        }
    }
    return std::nullopt;
}

SystemConfig parse_system_cnf(std::string_view text) {
    SystemConfig result;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const std::size_t newline = text.find('\n');
        std::string_view line = trim(text.substr(0, newline));
        if (newline == std::string_view::npos) {
            text = {};
        } else {
            text.remove_prefix(newline + 1);
        }

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }

        const std::string_view key_view = trim(line.substr(0, equals));
        std::string_view value_view = trim(line.substr(equals + 1));
        if (key_view.empty()) {
            throw SystemConfigError("SYSTEM.CNF has an empty key on line " + std::to_string(line_number));
        }
        if (value_view.size() > 1 && ((value_view.front() == '"' && value_view.back() == '"') ||
                                      (value_view.front() == '\'' && value_view.back() == '\''))) {
            value_view.remove_prefix(1);
            value_view.remove_suffix(1);
        }
        result.entries.emplace_back(std::string(key_view), std::string(value_view));
    }

    if (const auto boot = result.value("BOOT2")) {
        result.boot2 = std::string(*boot);
    }
    if (result.boot2.empty()) {
        throw SystemConfigError("SYSTEM.CNF does not contain a non-empty BOOT2 entry");
    }
    return result;
}

SystemConfig read_system_cnf(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw SystemConfigError("could not open SYSTEM.CNF: " + file.string());
    }
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input.eof() && input.fail()) {
        throw SystemConfigError("could not read SYSTEM.CNF: " + file.string());
    }
    return parse_system_cnf(contents);
}

std::optional<std::filesystem::path> find_system_cnf(const std::filesystem::path& disc_root) {
    std::vector<std::filesystem::path> candidates;
    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator iterator(
        disc_root, std::filesystem::directory_options::skip_permission_denied, iterator_error);
    const std::filesystem::recursive_directory_iterator end;
    while (!iterator_error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code status_error;
        if (entry.is_symlink(status_error)) {
            // Never follow directory links: they can loop or leave the disc.
            iterator.disable_recursion_pending();
        } else if (!status_error && entry.is_regular_file(status_error) && !status_error &&
            ascii_iequals(entry.path().filename().string(), "SYSTEM.CNF")) {
            candidates.push_back(entry.path());
        }
        iterator.increment(iterator_error);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        const std::size_t left_depth = path_depth(left);
        const std::size_t right_depth = path_depth(right);
        if (left_depth != right_depth) {
            return left_depth < right_depth;
        }
        return left.generic_string() < right.generic_string();
    });
    return candidates.front();
}

std::optional<std::string> try_normalize_ps2_serial(std::string_view boot_path) noexcept {
    const auto ascii_upper = [](char character) {
        if (character >= 'a' && character <= 'z') {
            return static_cast<char>(character - 'a' + 'A');
        }
        return character;
    };
    // The shortest accepted form is four letters and five digits.
    for (std::size_t index = 0; index + 9 <= boot_path.size(); ++index) {
        bool prefix_is_alpha = true;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            const char upper = ascii_upper(boot_path[index + offset]);
            if (upper < 'A' || upper > 'Z') {
                prefix_is_alpha = false;
                break;
            }
        }
        if (!prefix_is_alpha) {
            continue;
        }

        std::size_t cursor = index + 4;
        if (cursor < boot_path.size() && (boot_path[cursor] == '_' || boot_path[cursor] == '-')) {
            ++cursor;
        }
        const std::size_t first_digits = cursor;
        if (cursor + 3 > boot_path.size() || boot_path[cursor] < '0' || boot_path[cursor] > '9' ||
            boot_path[cursor + 1] < '0' || boot_path[cursor + 1] > '9' || boot_path[cursor + 2] < '0' ||
            boot_path[cursor + 2] > '9') {
            continue;
        }
        cursor += 3;
        if (cursor < boot_path.size() &&
            (boot_path[cursor] == '.' || boot_path[cursor] == '_' || boot_path[cursor] == '-')) {
            ++cursor;
        }
        if (cursor + 2 > boot_path.size() || boot_path[cursor] < '0' || boot_path[cursor] > '9' ||
            boot_path[cursor + 1] < '0' || boot_path[cursor + 1] > '9') {
            continue;
        }

        std::string serial;
        serial.reserve(10);
        for (std::size_t offset = 0; offset < 4; ++offset) {
            serial.push_back(ascii_upper(boot_path[index + offset]));
        }
        serial.push_back('-');
        serial.append(boot_path.substr(first_digits, 3));
        serial.append(boot_path.substr(cursor, 2));
        return serial;
    }
    return std::nullopt;
}

std::string normalize_ps2_serial(std::string_view boot_path) {
    if (auto serial = try_normalize_ps2_serial(boot_path)) {
        return *serial;
    }
    throw SystemConfigError("BOOT2 does not contain a recognizable PlayStation 2 serial: " +
                            std::string(boot_path));
}

std::span<const std::string_view> known_burnout3_disc_ids() noexcept {
    return kKnownDiscIds;
}

bool is_known_burnout3_disc_id(std::string_view serial) noexcept {
    const auto normalized = try_normalize_ps2_serial(serial);
    if (!normalized) {
        return false;
    }
    return std::find(kKnownDiscIds.begin(), kKnownDiscIds.end(), *normalized) != kKnownDiscIds.end();
}

// RECOVERY: the original placed the whole-disc fallback after `return current`
// with `child` already out of scope, but without a goto (the rebuilt dump carries
// an extra "goto predictor" note and a different block order); the exact construct
// was not found. Behaviour is identical.
std::filesystem::path locate_boot_elf(const std::filesystem::path& disc_root, std::string_view boot2) {
    const auto components = boot_path_components(boot2);
    std::filesystem::path current = disc_root;
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool needs_directory = index + 1 != components.size();
        const auto child = find_child_case_insensitively(current, components[index], needs_directory);
        if (!child) {
            goto search_disc;
        }
        current = *child;
    }
    return current;

search_disc:
    // Some dumps flatten the tree or rename the executable; fall back to searching
    // the whole disc for the final name, or for any file carrying the same serial.
    const std::string wanted = components.back();
    const auto wanted_serial = try_normalize_ps2_serial(wanted);
    std::vector<std::filesystem::path> candidates;
    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator iterator(
        disc_root, std::filesystem::directory_options::skip_permission_denied, iterator_error);
    const std::filesystem::recursive_directory_iterator end;
    while (!iterator_error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code status_error;
        if (entry.is_symlink(status_error)) {
            iterator.disable_recursion_pending();
        } else if (!status_error && entry.is_regular_file(status_error) && !status_error) {
            const std::string filename = entry.path().filename().string();
            const auto candidate_serial = try_normalize_ps2_serial(filename);
            if (equivalent_iso_name(filename, wanted) ||
                (wanted_serial && candidate_serial && *wanted_serial == *candidate_serial)) {
                candidates.push_back(entry.path());
            }
        }
        iterator.increment(iterator_error);
    }
    if (candidates.empty()) {
        throw SystemConfigError("could not locate BOOT2 executable: " + std::string(boot2));
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        const std::size_t left_depth = path_depth(left);
        const std::size_t right_depth = path_depth(right);
        if (left_depth != right_depth) {
            return left_depth < right_depth;
        }
        return left.generic_string() < right.generic_string();
    });
    return candidates.front();
}

} // namespace takedown::content
