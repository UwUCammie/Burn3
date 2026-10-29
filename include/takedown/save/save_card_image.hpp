#pragma once

#include "takedown/save/save_data.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace takedown::save {

enum class SaveCardImageErrc {
    invalid_image = 1,
    unsupported_layout,
    missing_save,
    missing_file,
    cycle_in_chain,
    insufficient_space,
};

[[nodiscard]] std::error_code make_error_code(SaveCardImageErrc error) noexcept;

// Geometry of the FAT16 filesystem inside a PS2 save-card image.
struct SaveCardImageGeometry {
    std::uint16_t bytes_per_sector{};
    std::uint8_t sectors_per_cluster{};
    std::uint16_t reserved_sectors{};
    std::uint8_t fat_count{};
    std::uint16_t root_entry_count{};
    std::uint32_t total_sectors{};
    std::uint16_t sectors_per_fat{};

    [[nodiscard]] std::uint32_t cluster_size() const noexcept {
        return static_cast<std::uint32_t>(bytes_per_sector) * sectors_per_cluster;
    }
};

// Read path for a PS2 save-card image as PCSX2 stores it: a single FAT16
// filesystem image (a ".ps2"/"memcard.ps2" file). A user dropping one in the
// save-data host folder can have its save directories enumerated and read.
// This is format compatibility support (the in-memory "memcard" origin) behind
// the user-facing "save data" / "save files" names.
class Ps2MemcardImage {
  public:
    // Parses the FAT16 layout of a supplied in-memory image. Must contain the
    // whole image. Failure is reported in error and leaves the image unusable.
    Ps2MemcardImage(std::span<const std::byte> image, std::error_code& error);

    [[nodiscard]] SaveCardImageGeometry geometry() const noexcept { return geometry_; }

    // Lists the save-directory names in the root directory. The ICON/SYS
    // system directory is skipped (it is not a save).
    [[nodiscard]] std::vector<std::string> list_save_directories(std::error_code& error) const;

    // Reads every file in the named save directory. Returns nullopt (with the
    // error cleared) when the directory is absent. The logical (".ps2"-stripped)
    // name and exact content bytes are returned for each save file.
    [[nodiscard]] std::optional<std::vector<SaveFile>>
    read_save_directory(std::string_view save_name, std::error_code& error) const;

    // Reads one save file from the named save directory. Returns nullopt (with
    // the error cleared) when the file is absent.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    read_file(std::string_view save_name, std::string_view file_name,
              std::error_code& error) const;

  private:
    std::vector<std::byte> image_;
    SaveCardImageGeometry geometry_{};
};

} // namespace takedown::save

namespace std {

template <> struct is_error_code_enum<takedown::save::SaveCardImageErrc> : true_type {};

} // namespace std