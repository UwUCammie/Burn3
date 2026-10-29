#include "takedown/content/importer.hpp"

#include "takedown/content/system_config.hpp"
#include "takedown/platform/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace takedown::content {
namespace {

std::atomic<std::uint64_t> staging_sequence{0U};

long long current_process_id() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

char ascii_lower(char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

std::string lowercase(std::string text) {
    for (char& character : text) {
        character = ascii_lower(character);
    }
    return text;
}

// Distinguishes "missing" from "could not look", so an unreadable destination
// is never mistaken for a free one.
bool path_entry_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw ImportError("could not inspect path " + path.string() + ": " + error.message());
    }
    return !error && status.type() != std::filesystem::file_type::not_found;
}

// Owns the staging directory until the import is published; anything left
// behind by a failed import is removed.
class StagingDirectory {
  public:
    explicit StagingDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;

    ~StagingDirectory() {
        if (owned_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void release() noexcept { owned_ = false; }

  private:
    std::filesystem::path path_;
    bool owned_{true};
};

std::filesystem::path
create_unique_staging_directory(const std::filesystem::path& destination) {
    const auto parent = destination.parent_path();
    const std::string leaf = destination.filename().string();
    for (unsigned attempt = 0; attempt < 1024U; ++attempt) {
        std::filesystem::path candidate =
            parent / ("." + leaf + ".staging-" +
                      std::to_string(current_process_id()) + "-" +
                      std::to_string(staging_sequence.fetch_add(1U)));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (!error || error == std::errc::file_exists) {
            continue;
        }
        throw ImportError("could not create import staging directory " + candidate.string() +
                          ": " + error.message());
    }
    throw ImportError("could not allocate a unique import staging directory");
}

DiscImageFormat detect_format(const std::filesystem::path& source) {
    const std::string extension = lowercase(source.extension().string());
    if (extension == ".iso") {
        return DiscImageFormat::iso;
    }
    if (extension == ".chd") {
        return DiscImageFormat::chd;
    }
    throw ImportError("source image must have a .iso or .chd extension: " + source.string());
}

void require_success(const std::vector<std::string>& arguments, std::string_view operation) {
    try {
        const auto result = platform::run_process(arguments);
        if (!result.succeeded()) {
            throw ImportError(std::string(operation) + " failed with " +
                              platform::describe_process_result(result));
        }
    } catch (const ImportError&) {
        throw;
    } catch (const std::exception& error) {
        throw ImportError(std::string(operation) + " could not start: " + error.what());
    }
}

enum class ChdMediaKind {
    cd,
    dvd,
};

ChdMediaKind inspect_chd_media(const std::filesystem::path& image) {
    platform::CapturedProcessResult result;
    try {
        result = platform::run_process_and_capture(
            std::array<std::string, 4>{"chdman", "info", "-i", image.string()});
    } catch (const std::exception& error) {
        throw ImportError("CHD inspection could not start: " + std::string(error.what()));
    }
    if (!result.process.succeeded()) {
        throw ImportError("CHD inspection failed with " +
                          platform::describe_process_result(result.process) + ": " +
                          result.output);
    }
    // CD images carry track metadata; DVD images have none.
    const std::string info = lowercase(result.output);
    if (info.find("tag='cht2'") != std::string::npos ||
        info.find("tag='chcd'") != std::string::npos) {
        return ChdMediaKind::cd;
    }
    return ChdMediaKind::dvd;
}

enum class CdTrackEncoding {
    mode1_2048,
    mode1_2352,
};

std::string_view trim_leading_space(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    return value;
}

CdTrackEncoding read_single_track_cue(const std::filesystem::path& cue) {
    std::ifstream input(cue, std::ios::binary);
    if (!input) {
        throw ImportError("could not open extracted CHD cue sheet: " + cue.string());
    }
    unsigned track_count = 0;
    std::optional<CdTrackEncoding> encoding;
    std::string line;
    while (std::getline(input, line)) {
        const std::string normalized_storage = lowercase(line);
        const std::string_view normalized = trim_leading_space(normalized_storage);
        if (!normalized.starts_with("track ")) {
            continue;
        }
        ++track_count;
        if (normalized.find("mode1/2048") != std::string_view::npos) {
            encoding = CdTrackEncoding::mode1_2048;
        } else if (normalized.find("mode1/2352") != std::string_view::npos) {
            encoding = CdTrackEncoding::mode1_2352;
        }
    }
    if (!input.eof() && input.fail()) {
        throw ImportError("could not read extracted CHD cue sheet: " + cue.string());
    }
    if (track_count != 1U || !encoding) {
        throw ImportError("only a single MODE1/2048 or MODE1/2352 CHD track is supported");
    }
    return *encoding;
}

// Strips the 16-byte sync/header and 288-byte EDC/ECC tail from each raw
// sector, leaving the 2048-byte user data that bsdtar can read as ISO-9660.
void convert_mode1_2352_to_iso(const std::filesystem::path& raw,
                               const std::filesystem::path& cooked) {
    constexpr std::array<unsigned char, 12> kMode1Sync{0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                                       0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U};
    std::error_code size_error;
    const std::uintmax_t raw_size = std::filesystem::file_size(raw, size_error);
    if (size_error || raw_size == 0U || raw_size % 2352U != 0U) {
        throw ImportError("MODE1/2352 track has an invalid byte length");
    }
    std::ifstream input(raw, std::ios::binary);
    std::ofstream output(cooked, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        throw ImportError("could not open files for MODE1 sector conversion");
    }
    std::array<unsigned char, 2352> sector{};
    const std::uintmax_t sector_count = raw_size / 2352U;
    for (std::uintmax_t index = 0; index < sector_count; ++index) {
        input.read(reinterpret_cast<char*>(sector.data()), 2352);
        if (input.gcount() != 2352) {
            throw ImportError("short read in MODE1/2352 track at sector " +
                              std::to_string(index));
        }
        if (!std::equal(kMode1Sync.begin(), kMode1Sync.end(), sector.begin()) ||
            sector[15] != 1U) {
            throw ImportError("invalid MODE1/2352 sector header at sector " +
                              std::to_string(index));
        }
        output.write(reinterpret_cast<const char*>(sector.data() + 16), 2048);
        if (!output) {
            throw ImportError("could not write cooked ISO at sector " + std::to_string(index));
        }
    }
    output.flush();
    if (!output) {
        throw ImportError("could not finish the cooked ISO image");
    }
}

std::filesystem::path
extract_chd_image(const std::filesystem::path& source, const std::filesystem::path& staging,
                  std::vector<std::filesystem::path>& temporary_files) {
    if (inspect_chd_media(source) == ChdMediaKind::dvd) {
        std::filesystem::path iso = staging / ".takedown-source.iso";
        require_success({"chdman", "extractdvd", "-i", source.string(), "-o", iso.string()},
                        "DVD CHD extraction");
        temporary_files.push_back(iso);
        return iso;
    }

    const auto cue = staging / ".takedown-source.cue";
    const auto track = staging / ".takedown-source.bin";
    require_success({"chdman", "extractcd", "-i", source.string(), "-o", cue.string(), "-ob",
                     track.string()},
                    "CD CHD extraction");
    temporary_files.push_back(cue);
    temporary_files.push_back(track);
    if (read_single_track_cue(cue) == CdTrackEncoding::mode1_2048) {
        return track;
    }
    const auto iso = staging / ".takedown-source.iso";
    convert_mode1_2352_to_iso(track, iso);
    temporary_files.push_back(iso);
    return iso;
}

void remove_temporary_files(const std::vector<std::filesystem::path>& files) {
    for (const auto& file : files) {
        std::error_code remove_error;
        const bool removed = std::filesystem::remove(file, remove_error);
        if (remove_error || !removed) {
            throw ImportError("could not remove temporary import file: " + file.string());
        }
    }
}

std::string manifest_path(const std::filesystem::path& root,
                          const std::filesystem::path& file) {
    const std::filesystem::path relative = file.lexically_relative(root);
    std::string generic = relative.generic_string();
    if (relative.empty() || generic.starts_with("..")) {
        throw ImportError("manifest path is outside the imported disc tree: " + file.string());
    }
    return generic;
}

void write_manifest(const std::filesystem::path& root, DiscImageFormat format,
                    std::string_view disc_id, bool known,
                    const std::filesystem::path& system_config,
                    const std::filesystem::path& boot_elf, const Elf32MipsInfo& elf) {
    const auto manifest = root / kImportManifestName;
    std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ImportError("could not create import manifest: " + manifest.string());
    }
    output << "manifest_version=1\n"
           << "source_format=" << to_string(format) << '\n'
           << "disc_id=" << disc_id << '\n'
           << "known_retail_revision=" << (known ? "true" : "false") << '\n'
           << "system_cnf=" << manifest_path(root, system_config) << '\n'
           << "boot_elf=" << manifest_path(root, boot_elf) << '\n'
           << "elf_class=32\n"
           << "elf_byte_order=little-endian\n"
           << "elf_machine=MIPS\n"
           << "elf_entry_point=0x" << std::hex << std::setw(8) << std::setfill('0')
           << elf.entry_point << '\n';
    output.flush();
    if (!output) {
        throw ImportError("could not finish import manifest: " + manifest.string());
    }
}

// rename() may replace an empty directory on some hosts, so the destination is
// checked again immediately before publishing.
void publish_without_replacing(const std::filesystem::path& staging,
                               const std::filesystem::path& destination) {
    if (path_entry_exists(destination)) {
        throw ImportError("import destination already exists: " + destination.string());
    }
    std::error_code error;
    std::filesystem::rename(staging, destination, error);
    if (error) {
        throw ImportError("could not publish imported content to " + destination.string() + ": " +
                          error.message());
    }
}

} // namespace

std::string_view to_string(DiscImageFormat format) noexcept {
    switch (format) {
    case DiscImageFormat::iso:
        return "iso";
    case DiscImageFormat::chd:
        return "chd";
    }
    return "unknown";
}

ImportedGame ContentImporter::import_image(const std::filesystem::path& source_image,
                                           const std::filesystem::path& destination,
                                           const ImportOptions& options) const {
    if (source_image.empty()) {
        throw ImportError("source image path is empty");
    }
    if (destination.empty()) {
        throw ImportError("import destination path is empty");
    }

    std::error_code absolute_error;
    const auto source = std::filesystem::absolute(source_image, absolute_error).lexically_normal();
    if (absolute_error) {
        throw ImportError("could not resolve source image: " + absolute_error.message());
    }
    const auto final_root =
        std::filesystem::absolute(destination, absolute_error).lexically_normal();
    if (absolute_error || final_root.filename().empty()) {
        throw ImportError("could not resolve import destination");
    }

    std::error_code status_error;
    if (!std::filesystem::is_regular_file(source, status_error) || status_error) {
        throw ImportError("source image is not a readable regular file: " + source.string());
    }
    const auto format = detect_format(source);
    if (path_entry_exists(final_root)) {
        throw ImportError("import destination already exists: " + final_root.string());
    }

    std::error_code directory_error;
    std::filesystem::create_directories(final_root.parent_path(), directory_error);
    if (directory_error) {
        throw ImportError("could not create destination parent directory: " +
                          directory_error.message());
    }

    StagingDirectory staging{create_unique_staging_directory(final_root)};
    std::filesystem::path iso = source;
    std::vector<std::filesystem::path> temporary_files;
    if (format == DiscImageFormat::chd) {
        iso = extract_chd_image(source, staging.path(), temporary_files);
    }
    require_success({"bsdtar", "-xf", iso.string(), "-C", staging.path().string(),
                     "--no-same-owner", "--no-same-permissions"},
                    "ISO extraction");
    remove_temporary_files(temporary_files);

    const auto system_config = find_system_cnf(staging.path());
    if (!system_config) {
        throw ImportError("extracted disc does not contain SYSTEM.CNF");
    }
    const SystemConfig config = read_system_cnf(*system_config);
    const std::string disc_id = normalize_ps2_serial(config.boot2);
    const bool known = is_known_burnout3_disc_id(disc_id);
    if (!known && !options.allow_unknown) {
        throw ImportError("unsupported Burnout 3 disc ID " + disc_id +
                          "; pass allow_unknown only when intentionally researching this revision");
    }

    const auto boot_elf = locate_boot_elf(staging.path(), config.boot2);
    const Elf32MipsInfo elf = inspect_ps2_elf(boot_elf);
    write_manifest(staging.path(), format, disc_id, known, *system_config, boot_elf, elf);

    const auto system_relative = system_config->lexically_relative(staging.path());
    const auto boot_relative = boot_elf.lexically_relative(staging.path());
    publish_without_replacing(staging.path(), final_root);
    staging.release();

    ImportedGame result;
    result.root = final_root;
    result.system_config = final_root / system_relative;
    result.boot_elf = final_root / boot_relative;
    result.manifest = final_root / kImportManifestName;
    result.disc_id = disc_id;
    result.source_format = format;
    result.known_retail_revision = known;
    result.executable = elf;
    return result;
}

ImportedGame import_game_content(const std::filesystem::path& source_image,
                                 const std::filesystem::path& destination,
                                 const ImportOptions& options) {
    return ContentImporter{}.import_image(source_image, destination, options);
}

} // namespace takedown::content
