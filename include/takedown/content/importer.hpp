#pragma once

#include "takedown/content/elf_inspector.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace takedown::content {

inline constexpr std::string_view kImportManifestName = "takedown-manifest.txt";

enum class DiscImageFormat {
    iso,
    chd,
};

[[nodiscard]] std::string_view to_string(DiscImageFormat format) noexcept;

class ImportError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct ImportOptions {
    // Unknown serials are normally rejected before the staging directory is
    // published. This switch is intended for research on user-owned revisions.
    bool allow_unknown{false};
};

struct ImportedGame {
    std::filesystem::path root;
    std::filesystem::path system_config;
    std::filesystem::path boot_elf;
    std::filesystem::path manifest;
    std::string disc_id;
    DiscImageFormat source_format{DiscImageFormat::iso};
    bool known_retail_revision{false};
    Elf32MipsInfo executable;
};

class ContentImporter {
  public:
    // Imports a user-supplied DVD .iso or .chd into a new destination. The
    // destination is never deleted or merged. Work is done in a new sibling
    // staging directory and published with one atomic rename.
    [[nodiscard]] ImportedGame import_image(const std::filesystem::path& source_image,
                                            const std::filesystem::path& destination,
                                            const ImportOptions& options = {}) const;
};

[[nodiscard]] ImportedGame import_game_content(const std::filesystem::path& source_image,
                                               const std::filesystem::path& destination,
                                               const ImportOptions& options = {});

} // namespace takedown::content
