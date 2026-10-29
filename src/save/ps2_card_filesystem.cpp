#include "takedown/save/ps2_card_filesystem.hpp"

#include "takedown/save/save_card_image.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

namespace takedown::save {

namespace {

constexpr std::string_view kMagic = "Sony PS2 Memory Card Format ";
constexpr std::string_view kVersion = "1.2.0.0";
// The magic is followed by the version digits, so only its text is compared.
constexpr std::size_t kMagicCompareSize = kMagic.size() - 1U;

constexpr std::size_t kEntrySize = 512U;
constexpr std::size_t kEntryNameOffset = 64U;
constexpr std::size_t kEntryNameCapacity = 32U;
constexpr std::size_t kEntriesPerCluster = kCardClusterSize / kEntrySize;

constexpr std::uint16_t kEntryExists = 0x8000U;
constexpr std::uint16_t kEntryFile = 0x0010U;
constexpr std::uint16_t kEntryDirectory = 0x0020U;
constexpr std::uint16_t kModeParentDirectory = 0xA426U;

// Clusters 0..40 hold the superblock, indirect FAT and FAT.
constexpr std::uint32_t kFirstAllocatableCluster = 41U;
constexpr std::uint32_t kAllocatableClusters = 8135U;
constexpr std::uint32_t kIndirectFatCluster = 8U;
constexpr std::uint32_t kFatCluster = 9U;
constexpr std::size_t kIndirectFatEntries = 32U;
constexpr std::size_t kFatEntryCount = kCardPageCount / kCardPagesPerCluster;
constexpr std::uint32_t kFatFree = 0x7FFFFFFFU;
constexpr std::uint32_t kFatChainFlag = 0x80000000U;
constexpr std::uint32_t kFatEndOfChain = 0xFFFFFFFFU;

constexpr std::int64_t kJapanOffsetSeconds = 9 * 3600;

using Entry = std::array<std::byte, kEntrySize>;

[[nodiscard]] std::uint16_t read_le16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void store_le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void store_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

[[nodiscard]] CardTimestamp read_timestamp(std::span<const std::byte> entry,
                                           std::size_t offset) noexcept {
    return CardTimestamp{
        read_le16(entry, offset + 6U),
        static_cast<std::uint8_t>(entry[offset + 5U]),
        static_cast<std::uint8_t>(entry[offset + 4U]),
        static_cast<std::uint8_t>(entry[offset + 3U]),
        static_cast<std::uint8_t>(entry[offset + 2U]),
        static_cast<std::uint8_t>(entry[offset + 1U]),
    };
}

void store_timestamp(std::span<std::byte> entry, std::size_t offset,
                     const CardTimestamp& timestamp) noexcept {
    entry[offset] = std::byte{0U};
    entry[offset + 1U] = static_cast<std::byte>(timestamp.second);
    entry[offset + 2U] = static_cast<std::byte>(timestamp.minute);
    entry[offset + 3U] = static_cast<std::byte>(timestamp.hour);
    entry[offset + 4U] = static_cast<std::byte>(timestamp.day);
    entry[offset + 5U] = static_cast<std::byte>(timestamp.month);
    store_le16(entry, offset + 6U, timestamp.year);
}

unsigned clamp_field(unsigned value, unsigned upper) noexcept {
    return std::min(value == 0U ? 1U : value, upper);
}

std::int64_t civil_year(std::int64_t era, unsigned year_of_era, unsigned month) noexcept {
    return static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2U ? 1 : 0);
}

// Days since 1970-01-01 for a proleptic Gregorian date (month 1-12, day 1-31).
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const std::int64_t era = year < 0 ? (year - 399) / 400 : year / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153U * (month + (month > 2U ? -3 : 9)) + 2U) / 5U + day - 1U;
    const unsigned day_of_era = day_of_year + (year_of_era * 365U + year_of_era / 4U - year_of_era / 100U);
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

std::string entry_name(std::span<const std::byte> entry) {
    std::string name;
    for (std::size_t index = 0U; index < kEntryNameCapacity; ++index) {
        const char character = std::to_integer<char>(entry[kEntryNameOffset + index]);
        if (character == '\0') {
            break;
        }
        name.push_back(character);
    }
    return name;
}

void write_entry(std::span<std::byte> entry, std::uint16_t mode, std::uint32_t length,
                 const CardTimestamp& created, std::uint32_t cluster, std::uint32_t parent_index,
                 const CardTimestamp& modified, std::string_view name) {
    std::fill(entry.begin(), entry.begin() + entry.size(), std::byte{0U});
    entry[0] = static_cast<std::byte>(mode);
    entry[1] = static_cast<std::byte>(mode >> 8U);
    store_le32(entry, 4U, length);
    store_timestamp(entry, 8U, created);
    store_le32(entry, 16U, cluster);
    store_le32(entry, 20U, parent_index);
    store_timestamp(entry, 24U, modified);
    std::memcpy(entry.data() + kEntryNameOffset, name.data(), name.size());
}

bool storable_name(std::string_view name) {
    return !name.empty() && name.size() <= kEntryNameCapacity - 1U && name != "." && name != ".." &&
           name.find('\0') == std::string_view::npos && name.find('/') == std::string_view::npos;
}

} // namespace

CardTimestamp card_timestamp_from_unix(std::int64_t unix_seconds) noexcept {
    const std::int64_t local = unix_seconds + kJapanOffsetSeconds;
    std::int64_t days = local / 86400;
    std::int64_t seconds = local % 86400;
    if (seconds < 0) {
        seconds += 86400;
        --days;
    }
    // Civil-from-days, shifted so that the era starts on 0000-03-01.
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const unsigned day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_index = (5U * day_of_year + 2U) / 153U;
    const unsigned day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    const unsigned month = month_index < 10U ? month_index + 3U : month_index - 9U;
    const std::int64_t year = civil_year(era, year_of_era, month);
    return CardTimestamp{
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(year, 0, 65535)),
        static_cast<std::uint8_t>(month),
        static_cast<std::uint8_t>(day),
        static_cast<std::uint8_t>(seconds / 3600),
        static_cast<std::uint8_t>((seconds / 60) % 60),
        static_cast<std::uint8_t>(seconds % 60),
    };
}

// RECOVERY: not identical; the month/day zero clamp folds to MAX_EXPR here, the original kept a branch.
std::int64_t unix_from_card_timestamp(const CardTimestamp& timestamp) noexcept {
    // Timestamps off the card can hold zeros; keep the fields in their calendar range.
    const unsigned month = clamp_field(timestamp.month, 12U);
    const unsigned day = clamp_field(timestamp.day, 31U);
    const std::int64_t days = days_from_civil(static_cast<std::int64_t>(timestamp.year), month, day);
    return days * 86400 + static_cast<std::int64_t>(timestamp.hour) * 3600 +
           static_cast<std::int64_t>(timestamp.minute) * 60 +
           static_cast<std::int64_t>(timestamp.second) - kJapanOffsetSeconds;
}

bool looks_like_ps2_card_image(std::span<const std::byte> image) noexcept {
    return image.size() >= kMagicCompareSize &&
           std::memcmp(image.data(), kMagic.data(), kMagicCompareSize) == 0;
}

namespace {

// A validated, read-only window on a card image (with or without spare areas).
class CardImageView {
  public:
    CardImageView(std::span<const std::byte> image, std::error_code& error) : image_(image) {
        if (!looks_like_ps2_card_image(image) || image.size() < kCardPageSize) {
            error = SaveCardImageErrc::invalid_image;
            return;
        }
        if (read_le16(image, 40U) != kCardPageSize || read_le16(image, 42U) != kCardPagesPerCluster) {
            error = SaveCardImageErrc::unsupported_layout;
            return;
        }
        clusters_per_card_ = read_le32(image, 48U);
        alloc_offset_ = read_le32(image, 52U);
        alloc_end_ = read_le32(image, 56U);
        root_cluster_ = read_le32(image, 60U);
        for (std::size_t index = 0U; index < indirect_fat_clusters_.size(); ++index) {
            indirect_fat_clusters_[index] = read_le32(image, (20U + index) * 4U);
        }
        if (clusters_per_card_ == 0U || clusters_per_card_ > 1048576U ||
            clusters_per_card_ <= alloc_offset_ || alloc_end_ > clusters_per_card_ - alloc_offset_) {
            error = SaveCardImageErrc::unsupported_layout;
            return;
        }
        // Images carrying the 16-byte spare area per page are 528 bytes per page.
        if (image.size() >= std::size_t{clusters_per_card_} * 1056U) {
            stride_ = kCardPageSize + kCardSpareSize;
        } else if (image.size() >= std::size_t{clusters_per_card_} * 1024U) {
            // Plain 512-byte pages.
        } else {
            error = SaveCardImageErrc::invalid_image;
            return;
        }
        if (stride_ != kCardPageSize && image.size() != std::size_t{clusters_per_card_} * stride_ * kCardPagesPerCluster) {
            stride_ = kCardPageSize;
        }
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint32_t root_cluster() const noexcept { return root_cluster_; }
    [[nodiscard]] std::uint32_t alloc_end() const noexcept { return alloc_end_; }

    // Reads one cluster (two pages) by its absolute number.
    [[nodiscard]] bool read_cluster(std::uint32_t cluster, std::span<std::byte, kCardClusterSize> out) const noexcept {
        if (cluster >= clusters_per_card_) {
            return false;
        }
        for (std::size_t page = 0U; page < kCardPagesPerCluster; ++page) {
            const std::size_t offset =
                (std::size_t{cluster} * kCardPagesPerCluster + page) * stride_;
            if (offset + kCardPageSize > image_.size()) {
                return false;
            }
            std::memcpy(out.data() + page * kCardPageSize, image_.data() + offset, kCardPageSize);
        }
        return true;
    }

    // The FAT word of an allocatable cluster, through the indirect FAT.
    [[nodiscard]] std::optional<std::uint32_t> fat_entry(std::uint32_t fat_cluster) const noexcept {
        const std::uint32_t fat_index = fat_cluster / 256U;
        const std::uint32_t list_index = fat_index / 256U;
        if (list_index >= indirect_fat_clusters_.size()) {
            return std::nullopt;
        }
        std::array<std::byte, kCardClusterSize> cluster{};
        if (!read_cluster(indirect_fat_clusters_[list_index], cluster)) {
            return std::nullopt;
        }
        const std::uint32_t data_cluster = read_le32(cluster, std::size_t{fat_index % 256U} * 4U);
        if (!read_cluster(data_cluster, cluster)) {
            return std::nullopt;
        }
        return read_le32(cluster, std::size_t{fat_cluster % 256U} * 4U);
    }

    // The first `clusters_needed` clusters of the chain starting at `first`.
    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    cluster_chain(std::uint32_t first, std::size_t clusters_needed, std::error_code& error) const {
        std::vector<std::uint32_t> clusters;
        std::uint32_t current = first;
        while (clusters.size() < clusters_needed) {
            if (current >= alloc_end_) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
            clusters.push_back(current);
            if (clusters.size() > alloc_end_) {
                error = SaveCardImageErrc::cycle_in_chain;
                return std::nullopt;
            }
            const auto entry = fat_entry(current);
            if (!entry) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
            if (*entry == kFatEndOfChain || (*entry & kFatChainFlag) == 0U) {
                break;
            }
            current = *entry & ~kFatChainFlag;
        }
        return clusters;
    }

    // `count` directory entries of the directory whose chain starts at `first`.
    [[nodiscard]] std::optional<std::vector<Entry>>
    read_entries(std::uint32_t first, std::uint32_t count, std::error_code& error) const {
        if (count == 0U) {
            return std::vector<Entry>{};
        }
        const std::size_t clusters_needed = (std::size_t{count} + kEntriesPerCluster - 1U) / kEntriesPerCluster;
        const auto clusters = cluster_chain(first, clusters_needed, error);
        if (!clusters) {
            return std::nullopt;
        }
        if (clusters->size() < clusters_needed) {
            error = SaveCardImageErrc::invalid_image;
            return std::nullopt;
        }
        std::vector<Entry> entries;
        std::array<std::byte, kCardClusterSize> cluster{};
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (index % kEntriesPerCluster == 0U) {
                if (!read_cluster((*clusters)[index / kEntriesPerCluster] + alloc_offset_, cluster)) {
                    error = SaveCardImageErrc::invalid_image;
                    return std::nullopt;
                }
            }
            Entry& slot = entries.emplace_back();
            std::memcpy(slot.data(), cluster.data() + (index % kEntriesPerCluster) * kEntrySize, kEntrySize);
        }
        return entries;
    }

    // `length` bytes of the file whose chain starts at `first`.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    read_content(std::uint32_t first, std::size_t length, std::error_code& error) const {
        std::vector<std::byte> content;
        if (length == 0U) {
            return content;
        }
        const std::size_t clusters_needed = (length + kCardClusterSize - 1U) / kCardClusterSize;
        const auto clusters = cluster_chain(first, clusters_needed, error);
        if (!clusters) {
            return std::nullopt;
        }
        if (clusters->size() < clusters_needed) {
            error = SaveCardImageErrc::invalid_image;
            return std::nullopt;
        }
        content.resize(clusters_needed * kCardClusterSize);
        for (std::size_t index = 0U; index < clusters_needed; ++index) {
            const std::span<std::byte, kCardClusterSize> out{content.data() + index * kCardClusterSize,
                                                             kCardClusterSize};
            if (!read_cluster((*clusters)[index] + alloc_offset_, out)) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
        }
        content.resize(length);
        return content;
    }

  private:
    std::span<const std::byte> image_;
    std::size_t stride_{kCardPageSize};
    std::uint32_t clusters_per_card_{};
    std::uint32_t alloc_offset_{};
    std::uint32_t alloc_end_{};
    std::uint32_t root_cluster_{};
    std::array<std::uint32_t, kIndirectFatEntries> indirect_fat_clusters_{};
    bool valid_{};
};

// Lays out a fresh card image: a bump allocator over the cluster area plus the
// FAT that records each allocation as a chain.
class CardImageBuilder {
  public:
    CardImageBuilder() : image_(kCardImageSize, std::byte{0xFFU}), fat_(kAllocatableClusters, kFatFree) {}

    // Allocates `count` consecutive clusters (zeroed) and returns the first.
    [[nodiscard]] std::optional<std::uint32_t> allocate(std::size_t count) {
        if (count == 0U || count > kAllocatableClusters - next_free_) {
            return std::nullopt;
        }
        const std::uint32_t first = next_free_;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::uint32_t cluster = first + static_cast<std::uint32_t>(index);
            fat_[cluster] = index + 1U != count ? ((cluster + 1U) | kFatChainFlag) : kFatEndOfChain;
        }
        next_free_ += static_cast<std::uint32_t>(count);
        std::fill_n(bytes_at(first, kCardClusterSize).begin(), count * kCardClusterSize, std::byte{0U});
        return first;
    }

    // Copies `data` to the start of the allocated cluster `cluster`.
    void write_bytes(std::uint32_t cluster, const std::vector<std::byte>& data) {
        const std::span<std::byte> bytes = bytes_at(cluster, data.size());
        std::memcpy(bytes.data(), data.data(), data.size());
    }

    // The 512-byte slot of directory entry `index` in the chain starting at `first`.
    [[nodiscard]] std::span<std::byte> entry_slot(std::uint32_t first, std::size_t index) {
        return bytes_at(first + static_cast<std::uint32_t>(index / kEntriesPerCluster), kCardClusterSize)
            .subspan((index % kEntriesPerCluster) * kEntrySize, kEntrySize);
    }

    // Writes the superblock, indirect FAT and FAT, and hands the image over.
    [[nodiscard]] std::vector<std::byte> finish() {
        const std::span<std::byte> page = std::span<std::byte>{image_}.first(kCardPageSize);
        std::fill(page.begin(), page.end(), std::byte{0U});
        kMagic.copy(reinterpret_cast<char*>(page.data()), kMagic.size());
        kVersion.copy(reinterpret_cast<char*>(page.data() + kMagic.size()), kVersion.size());
        page[41] = std::byte{2U};
        page[42] = std::byte{2U};
        page[44] = std::byte{16U};
        page[47] = std::byte{0xFFU};
        store_le32(page, 48U, kCardPageCount / kCardPagesPerCluster);
        store_le32(page, 52U, kFirstAllocatableCluster);
        store_le32(page, 56U, kAllocatableClusters);
        store_le32(page, 60U, 0U);
        store_le32(page, 64U, static_cast<std::uint32_t>(kCardBlockCount - 1U));
        store_le32(page, 68U, static_cast<std::uint32_t>(kCardBlockCount - 2U));
        store_le32(page, 80U, kIndirectFatCluster);
        for (std::size_t index = 0U; index < 32U; ++index) {
            store_le32(page, (52U + index) * 4U, 0xFFFFFFFFU);
        }
        page[336] = std::byte{2U};
        page[337] = std::byte{0x52U};

        const std::span<std::byte> indirect = std::span<std::byte>{image_}.subspan(
            std::size_t{kIndirectFatCluster} * kCardClusterSize, kCardClusterSize);
        for (std::uint32_t index = 0U; index < kIndirectFatEntries; ++index) {
            store_le32(indirect, index * 4U, kFatCluster + index);
        }
        for (std::uint32_t index = 0U; index < kFatEntryCount; ++index) {
            const std::uint32_t value = index < kAllocatableClusters ? fat_[index] : kFatEndOfChain;
            store_le32(image_, std::size_t{kFatCluster} * kCardClusterSize + index * 4U, value);
        }
        return std::move(image_);
    }

  private:
    // `length` bytes at the start of an allocatable cluster (numbers are relative to cluster 41).
    [[nodiscard]] std::span<std::byte> bytes_at(std::uint32_t cluster, std::size_t length) {
        return std::span<std::byte>{image_}.subspan(
            std::size_t{cluster + kFirstAllocatableCluster} * kCardClusterSize, length);
    }

    std::vector<std::byte> image_;
    std::vector<std::uint32_t> fat_;
    std::uint32_t next_free_{};
};

} // namespace

// RECOVERY: not identical; inlined CardImageView details differ (view.image_ copy order, the size_t
// cluster-count cast position, temp span clobber placement and a few folded index expressions).
std::optional<std::vector<CardSaveDirectory>> read_ps2_card_directories(std::span<const std::byte> image,
                                                                        std::error_code& error) {
    error.clear();
    const CardImageView view(image, error);
    if (error || !view.valid()) {
        if (!error) {
            error = SaveCardImageErrc::invalid_image;
        }
        return std::nullopt;
    }

    const auto root_head = view.read_entries(view.root_cluster(), 1U, error);
    if (!root_head) {
        return std::nullopt;
    }
    const std::uint32_t root_entry_count = read_le32((*root_head)[0], 4U);
    if (root_entry_count <= 1U || view.alloc_end() * 2U < root_entry_count) {
        error = SaveCardImageErrc::invalid_image;
        return std::nullopt;
    }
    const auto root = view.read_entries(view.root_cluster(), root_entry_count, error);
    if (!root) {
        return std::nullopt;
    }

    std::vector<CardSaveDirectory> directories;
    for (std::size_t index = 2U; index < root->size(); ++index) {
        const std::span<const std::byte> entry{(*root)[index]};
        const std::uint16_t mode_bits = read_le16(entry, 0U);
        if ((mode_bits & kEntryExists) == 0U || (mode_bits & kEntryDirectory) == 0U) {
            continue;
        }
        CardSaveDirectory directory;
        directory.name = entry_name(entry);
        directory.mode = read_le16(entry, 0U);
        directory.created = read_timestamp(entry, 8U);
        directory.modified = read_timestamp(entry, 24U);
        const auto children = view.read_entries(read_le32(entry, 16U), read_le32(entry, 4U), error);
        if (!children) {
            return std::nullopt;
        }
        for (std::size_t child_index = 0U; child_index < children->size(); ++child_index) {
            const std::span<const std::byte> child{(*children)[child_index]};
            const std::uint16_t child_bits = read_le16(child, 0U);
            if ((child_bits & kEntryExists) == 0U || (child_bits & kEntryFile) == 0U) {
                continue;
            }
            auto content = view.read_content(read_le32(child, 16U), read_le32(child, 4U), error);
            if (!content) {
                return std::nullopt;
            }
            directory.files.push_back(CardFile{
                .name = entry_name(child),
                .content = std::move(*content),
                .created = read_timestamp(child, 8U),
                .modified = read_timestamp(child, 24U),
                .mode = read_le16(child, 0U),
            });
        }
        directories.push_back(std::move(directory));
    }
    return directories;
}

std::optional<std::vector<std::byte>> build_ps2_card_image(const std::vector<CardSaveDirectory>& directories,
                                                           std::error_code& error) {
    error.clear();
    CardImageBuilder builder;
    const std::size_t root_entries = directories.size() + 2U;
    const auto root = builder.allocate((root_entries + 1U) / kEntriesPerCluster);
    if (!root) {
        error = SaveCardImageErrc::insufficient_space;
        return std::nullopt;
    }
    const CardTimestamp format_time = directories.empty() ? CardTimestamp{} : directories.front().created;
    write_entry(builder.entry_slot(*root, 0U), kCardModeDirectory,
                static_cast<std::uint32_t>(root_entries), format_time, 0U, 0U, format_time, ".");
    write_entry(builder.entry_slot(*root, 1U), kModeParentDirectory, 0U, format_time, 0U, 0U,
                format_time, "..");

    for (std::size_t dir_index = 0U; dir_index < directories.size(); ++dir_index) {
        const CardSaveDirectory& directory = directories[dir_index];
        if (!storable_name(directory.name)) {
            error = SaveCardImageErrc::invalid_image;
            return std::nullopt;
        }
        const std::size_t entries = directory.files.size() + 2U;
        const auto first = builder.allocate((entries + 1U) / kEntriesPerCluster);
        if (!first) {
            error = SaveCardImageErrc::insufficient_space;
            return std::nullopt;
        }
        const unsigned parent_index = static_cast<unsigned>(dir_index) + 2U;
        write_entry(builder.entry_slot(*root, dir_index + 2U),
                    static_cast<std::uint16_t>(directory.mode | kEntryExists | kEntryDirectory),
                    static_cast<std::uint32_t>(entries), directory.created, *first, 0U,
                    directory.modified, directory.name);
        write_entry(builder.entry_slot(*first, 0U), kCardModeDirectory, 0U, directory.created, *root,
                    parent_index, directory.modified, ".");
        write_entry(builder.entry_slot(*first, 1U), kCardModeDirectory, 0U, directory.created, 0U, 0U,
                    directory.modified, "..");

        for (std::size_t file_index = 0U; file_index < directory.files.size(); ++file_index) {
            const CardFile& file = directory.files[file_index];
            if (!storable_name(file.name) || file.content.size() > 0xFFFFFFFFULL) {
                error = SaveCardImageErrc::invalid_image;
                return std::nullopt;
            }
            std::uint32_t cluster = kFatEndOfChain;
            if (!file.content.empty()) {
                const auto data =
                    builder.allocate((file.content.size() + kCardClusterSize - 1U) / kCardClusterSize);
                if (!data) {
                    error = SaveCardImageErrc::insufficient_space;
                    return std::nullopt;
                }
                cluster = *data;
                builder.write_bytes(*data, file.content);
            }
            write_entry(builder.entry_slot(*first, file_index + 2U),
                        static_cast<std::uint16_t>(file.mode | kEntryExists | kEntryFile),
                        static_cast<std::uint32_t>(file.content.size()), file.created, cluster, 0U,
                        file.modified, file.name);
        }
    }
    return builder.finish();
}

} // namespace takedown::save
