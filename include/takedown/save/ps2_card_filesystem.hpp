#pragma once

// The PS2 save-card filesystem ("Sony PS2 Memory Card Format 1.2.0.0") as it
// sits on the standard 8 MiB card and inside PCSX2's ".ps2" card files.
//
// The retail game formats, reads, and writes this filesystem itself: its EE
// save library (libmc2) talks to the card one 512-byte page at a time through
// the MC2_S1 IOP module (see takedown/save/save_card_device.hpp). The host
// keeps each profile slot as plain save directories on disk, so this module
// is the bridge in both directions: it builds a freshly formatted card image
// holding a slot's save directories, and reads the save directories back out
// of an image (the device image after the game wrote to it, or a ".ps2" file
// the user dropped into the save-data folder).
//
// Layout (all values little endian):
//   page 0          superblock: magic, version, geometry, indirect-FAT cluster
//                   list, bad-block list, card type/flags
//   cluster 8       indirect FAT: cluster numbers of the FAT clusters
//   clusters 9..40  FAT: one u32 per allocatable cluster
//                   (0x7fffffff free, 0x80000000|next in a chain,
//                   0xffffffff end of chain)
//   cluster 41..    allocatable clusters; cluster numbers stored in the FAT
//                   and in directory entries are relative to cluster 41
//   blocks 1022/1023  backup blocks, left erased
// A directory is a cluster chain of 512-byte entries ("." and ".." first).
// The number of entries lives in the directory's own entry in its parent
// (the root's count lives in the root's "." entry).
//
// Terminology: host plumbing only. User-facing text says "save data" /
// "profiles" / "slots", never console hardware terms.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::save {

inline constexpr std::size_t kCardPageSize = 512U;
inline constexpr std::size_t kCardSpareSize = 16U;
inline constexpr std::size_t kCardPagesPerCluster = 2U;
inline constexpr std::size_t kCardPagesPerBlock = 16U;
inline constexpr std::size_t kCardPageCount = 16384U;
inline constexpr std::size_t kCardClusterSize = kCardPageSize * kCardPagesPerCluster;
inline constexpr std::size_t kCardBlockCount = kCardPageCount / kCardPagesPerBlock;
// A card image without the per-page spare (ECC) area, as the device keeps it.
inline constexpr std::size_t kCardImageSize = kCardPageCount * kCardPageSize;
// A PCSX2 ".ps2" card file: every page followed by its 16-byte spare area.
inline constexpr std::size_t kCardImageSizeWithSpare =
    kCardPageCount * (kCardPageSize + kCardSpareSize);

// Directory-entry mode bits used by the retail libraries.
inline constexpr std::uint16_t kCardModeDirectory = 0x8427U;
inline constexpr std::uint16_t kCardModeFile = 0x8497U;

// A directory-entry timestamp. The PS2 stores Japan Standard Time (UTC+9).
struct CardTimestamp {
    std::uint16_t year{2000U};
    std::uint8_t month{1U};
    std::uint8_t day{1U};
    std::uint8_t hour{};
    std::uint8_t minute{};
    std::uint8_t second{};

    bool operator==(const CardTimestamp&) const = default;
};

[[nodiscard]] CardTimestamp card_timestamp_from_unix(std::int64_t unix_seconds) noexcept;
[[nodiscard]] std::int64_t unix_from_card_timestamp(const CardTimestamp& timestamp) noexcept;

struct CardFile {
    std::string name;
    std::vector<std::byte> content;
    CardTimestamp created{};
    CardTimestamp modified{};
    std::uint16_t mode{kCardModeFile};

    bool operator==(const CardFile&) const = default;
};

// One save directory in the card's root directory.
struct CardSaveDirectory {
    std::string name;
    CardTimestamp created{};
    CardTimestamp modified{};
    std::uint16_t mode{kCardModeDirectory};
    std::vector<CardFile> files;

    bool operator==(const CardSaveDirectory&) const = default;
};

// True when the bytes start with the PS2 save-card superblock magic.
[[nodiscard]] bool looks_like_ps2_card_image(std::span<const std::byte> image) noexcept;

// Reads every save directory (root subdirectory) and its files. Accepts
// images with or without the per-page spare area. Nested subdirectories
// inside a save directory are skipped (retail saves are flat). Returns
// nullopt with SaveCardImageErrc in error for a malformed image.
[[nodiscard]] std::optional<std::vector<CardSaveDirectory>>
read_ps2_card_directories(std::span<const std::byte> image, std::error_code& error);

// Builds a freshly formatted card image (kCardImageSize bytes, no spare
// area) holding the given save directories, in order. Fails with
// SaveCardImageErrc::insufficient_space when they do not fit, or
// invalid_image for names the filesystem cannot store (empty, over 31
// bytes).
[[nodiscard]] std::optional<std::vector<std::byte>>
build_ps2_card_image(const std::vector<CardSaveDirectory>& directories, std::error_code& error);

} // namespace takedown::save
