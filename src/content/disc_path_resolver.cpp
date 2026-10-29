#include "takedown/content/disc_path_resolver.hpp"

#include <algorithm>
#include <vector>

namespace takedown::content {
namespace {

class DiscPathErrorCategory final : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override {
        return "takedown.disc_path";
    }

    [[nodiscard]] std::string message(int condition) const override {
        switch (static_cast<DiscPathErrc>(condition)) {
        case DiscPathErrc::invalid_guest_path:
            return "invalid guest disc path";
        case DiscPathErrc::ambiguous_case_match:
            return "ambiguous case-insensitive disc path";
        case DiscPathErrc::outside_root:
            return "disc path resolves outside the content root";
        }
        return "unknown disc path error";
    }
};

const DiscPathErrorCategory disc_path_error_category;

// PS2 disc filesystems are case-insensitive, but only for ASCII letters.
char ascii_lower(const char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
               return ascii_lower(a) == ascii_lower(b);
           });
}

// Splits a guest path into its components after stripping the cdrom0:
// device, leading separators and the ISO9660 ";1" version suffix.
std::optional<std::vector<std::string>> split_guest_path(std::string_view guest_path,
                                                         std::error_code& error) {
    if (guest_path.empty() || guest_path.find('\0') != std::string_view::npos) {
        error = DiscPathErrc::invalid_guest_path;
        return std::nullopt;
    }

    std::string normalized(guest_path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    const std::size_t device_separator = normalized.find(':');
    if (device_separator != std::string::npos) {
        if (!equals_ascii_case_insensitive(std::string_view(normalized).substr(0, device_separator),
                                           "cdrom0")) {
            error = DiscPathErrc::invalid_guest_path;
            return std::nullopt;
        }
        normalized.erase(0, device_separator + 1U);
    }
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (normalized.ends_with(";1")) {
        normalized.erase(normalized.size() - 2U);
    }

    std::vector<std::string> components;
    std::size_t component_start = 0;
    while (component_start <= normalized.size()) {
        const std::size_t separator = normalized.find('/', component_start);
        const std::size_t component_end =
            separator == std::string::npos ? normalized.size() : separator;
        const std::string_view component{normalized.data() + component_start,
                                         component_end - component_start};
        if (!component.empty()) {
            if (component == "." || component == ".." ||
                component.find(':') != std::string_view::npos) {
                error = DiscPathErrc::invalid_guest_path;
                return std::nullopt;
            }
            components.emplace_back(component);
        }
        if (separator == std::string::npos) {
            break;
        }
        component_start = separator + 1U;
    }

    if (components.empty()) {
        error = DiscPathErrc::invalid_guest_path;
        return std::nullopt;
    }
    return components;
}

// Compares whole path components so "/disc2" is not treated as inside "/disc".
bool is_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_component = root.begin();
    auto candidate_component = candidate.begin();
    for (; root_component != root.end(); ++root_component, ++candidate_component) {
        if (candidate_component == candidate.end() || *root_component != *candidate_component) {
            return false;
        }
    }
    return true;
}

} // namespace

std::error_code make_error_code(DiscPathErrc error) noexcept {
    return {static_cast<int>(error), disc_path_error_category};
}

std::optional<std::string> normalize_disc_guest_path(std::string_view guest_path,
                                                     std::error_code& error) {
    error.clear();
    const auto components = split_guest_path(guest_path, error);
    if (!components) {
        return std::nullopt;
    }

    std::string key;
    for (const auto& component : *components) {
        if (!key.empty()) {
            key.push_back('/');
        }
        for (const char character : component) {
            key.push_back(ascii_lower(character));
        }
    }
    return key;
}

std::optional<std::filesystem::path> resolve_disc_path(const std::filesystem::path& root,
                                                       std::string_view guest_path,
                                                       std::error_code& error) {
    error.clear();
    const auto canonical_root = std::filesystem::canonical(root, error);
    if (error) {
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(canonical_root, error)) {
        if (!error) {
            error = std::make_error_code(std::errc::not_a_directory);
        }
        return std::nullopt;
    }

    const auto components = split_guest_path(guest_path, error);
    if (!components) {
        return std::nullopt;
    }

    std::filesystem::path current = canonical_root;
    for (const auto& component : *components) {
        std::optional<std::filesystem::path> match;
        std::filesystem::directory_iterator entry(current, error);
        const std::filesystem::directory_iterator end;
        while (!error && entry != end) {
            const std::string entry_name = entry->path().filename().string();
            if (equals_ascii_case_insensitive(entry_name, component)) {
                if (match) {
                    error = DiscPathErrc::ambiguous_case_match;
                    return std::nullopt;
                }
                match = entry->path();
            }
            // increment(error) reports I/O failures instead of throwing.
            entry.increment(error);
        }
        if (error) {
            return std::nullopt;
        }
        if (!match) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return std::nullopt;
        }

        // Canonicalize each step so a symlink cannot walk out of the root.
        current = std::filesystem::canonical(*match, error);
        if (error) {
            return std::nullopt;
        }
        if (!is_within_root(canonical_root, current)) {
            error = DiscPathErrc::outside_root;
            return std::nullopt;
        }
    }

    error.clear();
    return current;
}

} // namespace takedown::content
