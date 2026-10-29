#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::content {

class SystemConfigError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct SystemConfig {
    std::vector<std::pair<std::string, std::string>> entries;
    std::string boot2;

    [[nodiscard]] std::optional<std::string_view> value(std::string_view key) const noexcept;
};

// Parses the simple KEY = VALUE format used by PlayStation 2 SYSTEM.CNF files.
// Key lookup is ASCII case-insensitive and the last repeated key wins.
[[nodiscard]] SystemConfig parse_system_cnf(std::string_view text);
[[nodiscard]] SystemConfig read_system_cnf(const std::filesystem::path& file);

// Searches recursively without following directory symlinks. If several files
// exist, the shallowest path (then lexicographically first path) is selected.
[[nodiscard]] std::optional<std::filesystem::path>
find_system_cnf(const std::filesystem::path& disc_root);

// Converts values such as "cdrom0:\\SLUS_210.50;1" to "SLUS-21050".
[[nodiscard]] std::optional<std::string>
try_normalize_ps2_serial(std::string_view boot_path) noexcept;
[[nodiscard]] std::string normalize_ps2_serial(std::string_view boot_path);

[[nodiscard]] std::span<const std::string_view> known_burnout3_disc_ids() noexcept;
[[nodiscard]] bool is_known_burnout3_disc_id(std::string_view serial) noexcept;

// Resolves BOOT2 relative to disc_root one component at a time, ignoring ASCII
// case and ISO-9660's optional ;version suffix. Paths that could escape the root
// are rejected.
[[nodiscard]] std::filesystem::path locate_boot_elf(const std::filesystem::path& disc_root,
                                                    std::string_view boot2);

} // namespace takedown::content
