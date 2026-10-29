#include "takedown/save/save_card_image.hpp"

#include <algorithm>

namespace takedown::save {
namespace {

class SaveCardImageErrorCategory final : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override { return "takedown.save_card_image"; }

    [[nodiscard]] std::string message(int condition) const override {
        switch (static_cast<SaveCardImageErrc>(condition)) {
        case SaveCardImageErrc::invalid_image:
            return "save-card image is not a valid FAT16 filesystem";
        case SaveCardImageErrc::unsupported_layout:
            return "save-card image uses an unsupported FAT16 layout";
        case SaveCardImageErrc::missing_save:
            return "save-data directory does not exist in the image";
        case SaveCardImageErrc::missing_file:
            return "save-data file does not exist in the image";
        case SaveCardImageErrc::cycle_in_chain:
            return "save-card image contains a cluster-chain cycle";
        case SaveCardImageErrc::insufficient_space:
            return "save data does not fit in a save-card image";
        }
        return "unknown save-card image error";
    }
};

const SaveCardImageErrorCategory save_card_image_error_category;

constexpr std::size_t kSectorSize = 512U;

// A FAT16 cluster numbers 0 and 1 are reserved; 0xFFF7 and above mark bad
// clusters or the end of a chain.
constexpr std::uint16_t kFirstDataCluster = 2U;
constexpr std::uint16_t kLastDataCluster = 0xFFF7U;

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t offset) {
    const auto field = bytes.subspan(offset, 2U);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(field[0]) |
                                      static_cast<std::uint16_t>(field[1]) << 8U);
}

std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset) {
    const auto field = bytes.subspan(offset, 4U);
    return static_cast<std::uint32_t>(field[0]) | static_cast<std::uint32_t>(field[1]) << 8U |
           static_cast<std::uint32_t>(field[2]) << 16U | static_cast<std::uint32_t>(field[3]) << 24U;
}

std::optional<std::uint16_t> read_u16(const std::span<const std::byte>& image, std::size_t offset) {
    if (image.size() < offset + 2U) {
        return std::nullopt;
    }
    return le16(image, offset);
}

std::optional<std::span<const std::byte>> sector_at(std::span<const std::byte> image,
                                                    std::uint32_t sector_index) {
    const std::size_t begin = static_cast<std::size_t>(sector_index) * kSectorSize;
    if (image.size() <= begin || image.size() - begin < kSectorSize) {
        return std::nullopt;
    }
    return image.subspan(begin, kSectorSize);
}

std::optional<std::span<const std::byte, kSectorSize>>
fixed_sector_at(std::span<const std::byte> image, std::uint32_t sector_index) {
    const std::size_t begin = static_cast<std::size_t>(sector_index) * kSectorSize;
    if (begin >= image.size_bytes() || image.size_bytes() - begin < kSectorSize) {
        return std::nullopt;
    }
    return std::span<const std::byte, kSectorSize>(image.data() + begin, kSectorSize);
}

// FAT entry for a cluster: the next cluster in its chain.
std::optional<std::uint16_t> next_cluster(std::span<const std::byte> image,
                                          const SaveCardImageGeometry& geometry,
                                          std::uint16_t cluster) {
    const std::size_t fat_offset =
        static_cast<std::size_t>(geometry.reserved_sectors) * geometry.bytes_per_sector;
    const std::size_t entry_offset = fat_offset + static_cast<std::size_t>(cluster) * 2U;
    return read_u16(image, entry_offset);
}

// First sector of a data cluster (data clusters are numbered from 2).
std::uint32_t cluster_first_sector(const SaveCardImageGeometry& geometry, std::uint16_t cluster) {
    const std::uint32_t root_sector_count =
        (static_cast<std::uint32_t>(geometry.root_entry_count) * 32U + geometry.bytes_per_sector - 1U) /
        geometry.bytes_per_sector;
    return static_cast<std::uint32_t>(geometry.reserved_sectors) +
           static_cast<std::uint32_t>(geometry.fat_count) * geometry.sectors_per_fat +
           root_sector_count + (cluster - 2U) * geometry.sectors_per_cluster;
}

std::string trim_right(std::string_view value) {
    std::size_t end = value.size();
    while (end != 0U && value[end - 1U] == ' ') {
        --end;
    }
    return std::string(value.substr(0U, end));
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return value;
}

// "NAME    EXT" (8.3, space padded) as a lower-case "name.ext".
std::string directory_entry_name(std::span<const std::byte> entry) noexcept {
    std::string name = trim_right(std::string_view(reinterpret_cast<const char*>(entry.data()), 8U));
    const std::string extension =
        trim_right(std::string_view(reinterpret_cast<const char*>(entry.data() + 8), 3U));
    if (!extension.empty()) {
        name.push_back('.');
        name += extension;
    }
    return lower_ascii(name);
}

struct RawDirectoryEntry {
    std::string name;
    bool is_directory{};
    std::uint16_t first_cluster{};
    std::uint32_t file_size{};
};

// Follows a cluster chain and returns exactly `size` bytes of it.
[[nodiscard]] std::optional<std::vector<std::byte>>
read_chain(std::span<const std::byte> image, const SaveCardImageGeometry& geometry,
           std::uint16_t first_cluster, std::uint32_t size, std::error_code& error) {
    std::vector<std::byte> content;
    const std::uint32_t max_cluster = geometry.total_sectors / geometry.sectors_per_cluster + 1U;
    std::uint16_t cluster = first_cluster;
    std::uint32_t visited = 0U;
    while (cluster >= kFirstDataCluster && content.size() < size) {
        ++visited;
        if (visited > max_cluster) {
            error = SaveCardImageErrc::cycle_in_chain;
            return std::nullopt;
        }
        const std::uint32_t first_sector = cluster_first_sector(geometry, cluster);
        for (std::uint32_t sector_index = 0U;
             sector_index < geometry.sectors_per_cluster && content.size() < size; ++sector_index) {
            const auto sector_view = fixed_sector_at(image, first_sector + sector_index);
            if (!sector_view) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
            const std::size_t count = std::min<std::size_t>(size - content.size(), sector_view->size());
            for (std::size_t index = 0U; index < count; ++index) {
                content.push_back((*sector_view)[index]);
            }
        }
        if (content.size() >= size) {
            break;
        }
        const auto next = next_cluster(image, geometry, cluster);
        if (!next || *next < kFirstDataCluster || *next > kLastDataCluster) {
            break;
        }
        cluster = *next;
    }
    if (content.size() < size) {
        error = SaveCardImageErrc::invalid_image;
        return std::nullopt;
    }
    return content;
}

// Parses the 32-byte directory entries of a directory's raw bytes.
[[nodiscard]] std::optional<std::vector<RawDirectoryEntry>>
read_directory_bytes(std::span<const std::byte> area) {
    std::vector<RawDirectoryEntry> entries;
    for (std::size_t offset = 0U; offset + 32U <= area.size(); offset += 32U) {
        const auto entry = area.subspan(offset, 32U);
        if (entry[0] == std::byte{0x00U}) {
            break;
        }
        if (entry[0] == std::byte{0xE5U}) {
            continue;
        }
        entries.push_back(RawDirectoryEntry{
            .name = directory_entry_name(entry),
            .is_directory = (static_cast<unsigned>(entry[11]) & 0x10U) != 0U,
            .first_cluster = le16(entry, 26U),
            .file_size = le32(entry, 28U),
        });
    }
    return entries;
}

[[nodiscard]] std::optional<std::vector<RawDirectoryEntry>>
read_directory(std::span<const std::byte> image, const SaveCardImageGeometry& geometry,
               std::uint16_t first_cluster, std::error_code& error) {
    std::vector<std::byte> area;
    const std::uint32_t max_cluster = geometry.total_sectors / geometry.sectors_per_cluster + 1U;
    std::uint16_t cluster = first_cluster;
    std::uint32_t visited = 0U;
    while (cluster > 1U) {
        ++visited;
        if (visited > max_cluster) {
            error = SaveCardImageErrc::cycle_in_chain;
            return std::nullopt;
        }
        const std::uint32_t first_sector = cluster_first_sector(geometry, cluster);
        for (std::uint32_t sector_index = 0U; sector_index < geometry.sectors_per_cluster;
             ++sector_index) {
            const auto sector_view = sector_at(image, first_sector + sector_index);
            if (!sector_view) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
            const auto& sector = *sector_view;
            area.insert(area.end(), sector.begin(), sector.end());
        }
        const auto next = next_cluster(image, geometry, cluster);
        if (!next) {
            break;
        }
        const std::uint16_t value = *next;
        if (value <= 1U) {
            break;
        }
        if (value > kLastDataCluster) {
            break;
        }
        cluster = value;
    }
    return read_directory_bytes(area);
}

// The root directory sits in a fixed region right after the FATs.
[[nodiscard]] std::optional<std::vector<RawDirectoryEntry>>
read_root_directory(std::span<const std::byte> image, const SaveCardImageGeometry& geometry,
                    std::error_code& error) {
    const std::uint32_t root_sector = static_cast<std::uint32_t>(geometry.reserved_sectors) +
                                      static_cast<std::uint32_t>(geometry.fat_count) * geometry.sectors_per_fat;
    const std::uint32_t root_sector_count =
        (static_cast<std::uint32_t>(geometry.root_entry_count) * 32U + geometry.bytes_per_sector - 1U) /
        geometry.bytes_per_sector;
    std::vector<std::byte> area;
    for (std::uint32_t index = 0U; index < root_sector_count; ++index) {
        const auto sector_view = sector_at(image, root_sector + index);
        if (!sector_view) {
            error = SaveCardImageErrc::invalid_image;
            return std::nullopt;
        }
        area.insert(area.end(), sector_view->begin(), sector_view->end());
    }
    return read_directory_bytes(area);
}

// RECOVERY: identical to the original GIMPLE except for GCC's internal SRA
// scratch-variable counter in the dump (SR.723 in the original, SR.737 here).
[[nodiscard]] std::optional<RawDirectoryEntry>
find_directory_entry(const std::vector<RawDirectoryEntry>& entries, std::string_view save_name) {
    const std::string wanted = lower_ascii(std::string(save_name));
    for (const RawDirectoryEntry& entry : entries) {
        if (entry.name == wanted && entry.is_directory) {
            return entry;
        }
    }
    return std::nullopt;
}

std::byte byte_at(std::span<const std::byte> bytes, std::size_t offset) {
    return bytes[offset];
}

} // namespace

std::error_code make_error_code(SaveCardImageErrc error) noexcept {
    return {static_cast<int>(error), save_card_image_error_category};
}

Ps2MemcardImage::Ps2MemcardImage(std::span<const std::byte> image, std::error_code& error) {
    error.clear();
    if (image.size() < kSectorSize) {
        error = SaveCardImageErrc::invalid_image;
        return;
    }
    const std::uint16_t bytes_per_sector = le16(image, 11U);
    const std::byte sectors_per_cluster = byte_at(image, 13U);
    const std::uint16_t reserved_sectors = le16(image, 14U);
    const std::byte fat_count = byte_at(image, 16U);
    const std::uint16_t root_entry_count = le16(image, 17U);
    const std::uint16_t total_sectors_16 = le16(image, 19U);
    const std::byte media = byte_at(image, 21U);
    const std::uint16_t sectors_per_fat_16 = le16(image, 22U);
    const std::uint32_t total_sectors_32 = le32(image, 32U);
    const std::uint32_t sectors_per_fat_32 = le32(image, 36U);
    if (bytes_per_sector != 512U || sectors_per_cluster == std::byte{0U} || fat_count == std::byte{0U} ||
        root_entry_count == 0U || reserved_sectors == 0U || media != std::byte{0xF8U}) {
        error = SaveCardImageErrc::unsupported_layout;
        return;
    }
    const std::uint32_t total_sectors = total_sectors_16 != 0U ? total_sectors_16 : total_sectors_32;
    const std::uint32_t sectors_per_fat =
        sectors_per_fat_16 != 0U ? sectors_per_fat_16 : sectors_per_fat_32;
    if (total_sectors == 0U || sectors_per_fat == 0U || total_sectors > image.size() / kSectorSize) {
        error = SaveCardImageErrc::unsupported_layout;
        return;
    }
    geometry_.bytes_per_sector = bytes_per_sector;
    geometry_.sectors_per_cluster = std::to_integer<std::uint8_t>(sectors_per_cluster);
    geometry_.reserved_sectors = reserved_sectors;
    geometry_.fat_count = std::to_integer<std::uint8_t>(fat_count);
    geometry_.root_entry_count = root_entry_count;
    geometry_.total_sectors = total_sectors;
    geometry_.sectors_per_fat = static_cast<std::uint16_t>(sectors_per_fat);
    image_.assign(image.begin(), image.end());
}

std::vector<std::string> Ps2MemcardImage::list_save_directories(std::error_code& error) const {
    error.clear();
    const auto root_entries = read_root_directory(image_, geometry_, error);
    if (error || !root_entries) {
        return {};
    }
    std::vector<std::string> saves;
    for (const RawDirectoryEntry& entry : *root_entries) {
        if (!entry.is_directory || entry.name == "icon.sys" || entry.name == "system") {
            continue;
        }
        saves.push_back(entry.name);
    }
    std::sort(saves.begin(), saves.end());
    return saves;
}

std::optional<std::vector<SaveFile>>
Ps2MemcardImage::read_save_directory(std::string_view save_name, std::error_code& error) const {
    error.clear();
    const auto root_entries = read_root_directory(image_, geometry_, error);
    if (error || !root_entries) {
        return std::nullopt;
    }
    const auto match = find_directory_entry(*root_entries, save_name);
    if (!match) {
        return std::nullopt;
    }
    const auto entries = read_directory(image_, geometry_, match->first_cluster, error);
    if (error || !entries) {
        return std::nullopt;
    }
    std::vector<SaveFile> files;
    for (const RawDirectoryEntry& entry : *entries) {
        if (entry.is_directory || entry.name == "_pcsx2_index") {
            continue;
        }
        std::string_view logical_name = entry.name;
        if (logical_name.ends_with(".ps2")) {
            logical_name = logical_name.substr(0U, logical_name.size() - 4U);
        }
        const auto content = read_chain(image_, geometry_, entry.first_cluster, entry.file_size, error);
        if (error || !content) {
            return std::nullopt;
        }
        files.push_back(SaveFile{std::string(logical_name), *content});
    }
    std::sort(files.begin(), files.end(),
              [](const SaveFile& left, const SaveFile& right) { return left.name < right.name; });
    return files;
}

std::optional<std::vector<std::byte>>
Ps2MemcardImage::read_file(std::string_view save_name, std::string_view file_name,
                           std::error_code& error) const {
    error.clear();
    const auto root_entries = read_root_directory(image_, geometry_, error);
    if (error || !root_entries) {
        return std::nullopt;
    }
    const auto save_match = find_directory_entry(*root_entries, save_name);
    if (!save_match) {
        return std::nullopt;
    }
    const auto entries = read_directory(image_, geometry_, save_match->first_cluster, error);
    if (error || !entries) {
        return std::nullopt;
    }
    const std::string wanted = lower_ascii(std::string(file_name));
    for (const RawDirectoryEntry& entry : *entries) {
        if (entry.is_directory) {
            continue;
        }
        std::string stored_name = entry.name;
        if (std::string_view(stored_name).ends_with(".ps2")) {
            stored_name.erase(stored_name.size() - 4U);
        }
        if (stored_name == wanted) {
            return read_chain(image_, geometry_, entry.first_cluster, entry.file_size, error);
        }
    }
    return std::nullopt;
}

} // namespace takedown::save
