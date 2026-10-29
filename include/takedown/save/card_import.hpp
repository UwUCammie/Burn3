#pragma once

// PCSX2 save import support for the in-game save/load flow.
//
// The runtime's save-data folder (takedown::save::default_save_data_root())
// is the user-facing "save data" location. When the user drops PCSX2 save
// data into it, the save-data menu offers to import it into one of the host
// profile slots (kProfileSlotCount slots, each a save-data store root named
// "profile-N"; see takedown/save/profile_slots.hpp). Accepted sources:
//   - a PCSX2 card file (".ps2"): the PS2 save-card filesystem, with or
//     without the per-page spare area;
//   - a PCSX2 folder card: a folder whose subfolders are save directories
//     (e.g. "Mcd001/BASLUS-21050/{BASLUS-21050,icon.sys,view.ico,
//     _pcsx2_index}");
//   - a single save directory (a folder holding "icon.sys").
//
// Terminology: this module is host-side plumbing; the in-game prompt built on
// it must never surface console hardware terms ("memory card") — the
// user-facing wording is "save data" / "profiles" / "slots". The prompt also
// shows the source's play time (PCSX2's own per-game play time, when the
// serial can be associated) and the profile name stored inside the game's
// save data.

#include "takedown/save/ps2_card_filesystem.hpp"
#include "takedown/save/save_card_image.hpp"
#include "takedown/save/save_data.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace takedown::save {

// Profile slots of the in-game save-data menu. Each slot is an independent
// save-data store root ("profile-1" .. "profile-5") under the saves root.
// Earlier builds had three slots; their "profile-1".."profile-3" folders are
// used unchanged.
inline constexpr std::size_t kProfileSlotCount = 5U;

// Part of the normalized save-directory key (normalize_save_directory_key)
// of every save of this game: the card save names are "BASLUS-21050...".
inline constexpr std::string_view kGameSaveKey = "SLUS21050";

enum class ImportSourceKind {
    // A ".ps2" card file.
    card_image,
    // A folder whose subfolders are save directories.
    card_folder,
    // A single save directory (holds "icon.sys").
    save_folder,
};

struct ImportSource {
    ImportSourceKind kind{ImportSourceKind::card_image};
    std::filesystem::path path;

    bool operator==(const ImportSource&) const = default;
};

// What the import flow can say about a detected import source.
struct Pcsx2CardSummary {
    // The source path (card file or folder).
    std::filesystem::path image_path;
    ImportSource source{};
    // Save-directory names found in the source.
    std::vector<std::string> save_directories;
    // Profile name stored inside the game's save data, when the data carries
    // one (a 32-bit byte length followed by UTF-16LE text).
    std::optional<std::string> profile_name;
    // PCSX2's per-game play time for the source's serial, in minutes, when
    // the serial could be associated with PCSX2's playtime record.
    std::optional<std::uint64_t> playtime_minutes;
    // Newest modification time of the save data (Unix seconds), when known.
    std::optional<std::int64_t> last_modified_unix;
};

// Lists candidate save-card images ("*.ps2" files) directly inside the saves
// root, sorted by path. Save directories are not card images.
[[nodiscard]] std::vector<std::filesystem::path>
detect_card_images(const std::filesystem::path& saves_root, std::error_code& error);

// Classifies one path as an import source: a ".ps2" file, a save directory
// (holds "icon.sys"), or a folder card (has at least one save-directory
// subfolder). Returns nullopt for anything else.
[[nodiscard]] std::optional<ImportSource> classify_import_path(const std::filesystem::path& path);

// Lists every import source directly inside the saves root, sorted by path:
// card images, folder cards, and loose save directories. The profile-slot
// folders, the "backups" folder, and hidden folders are never sources.
[[nodiscard]] std::vector<ImportSource>
detect_import_sources(const std::filesystem::path& saves_root, std::error_code& error);

// One entry of PCSX2's per-folder "_pcsx2_index" (YAML: flow or block
// style), keyed by file name; "$ROOT" describes the folder itself.
// Timestamps are Unix seconds.
struct Pcsx2IndexEntry {
    std::optional<std::uint32_t> order;
    std::optional<std::int64_t> time_created;
    std::optional<std::int64_t> time_modified;

    bool operator==(const Pcsx2IndexEntry&) const = default;
};

// Parses "_pcsx2_index" content. Unknown keys are ignored; malformed input
// yields whatever entries parsed cleanly (never an error: the index is
// optional metadata).
[[nodiscard]] std::map<std::string, Pcsx2IndexEntry> parse_pcsx2_index(std::string_view text);

// Renders a save directory's "_pcsx2_index" (flow style, as PCSX2 writes
// it): "$ROOT" plus one entry per file in order.
[[nodiscard]] std::string format_pcsx2_index(const CardSaveDirectory& directory);

// Reads a PCSX2-layout save directory: every regular file except PCSX2's
// "_pcsx2_index"/"_pcsx2_superblock" bookkeeping. Timestamps and file order
// come from "_pcsx2_index" when present, otherwise from the files' own
// modification times (files then sort by name, "icon.sys" first).
[[nodiscard]] std::optional<CardSaveDirectory>
read_folder_save_directory(const std::filesystem::path& directory, std::error_code& error);

// Reads every save directory of an import source.
[[nodiscard]] std::optional<std::vector<CardSaveDirectory>>
read_import_source(const ImportSource& source, std::error_code& error);

// Extracts the profile name from the game's save data bytes: a 32-bit
// little-endian byte length followed by UTF-16LE text. Returns nullopt when
// the bytes do not carry a plausible name (wrong length, NUL-padded empty,
// non-printable content).
[[nodiscard]] std::optional<std::string>
extract_profile_name(std::span<const std::byte> save_data_bytes);

// The first plausible profile name in any file of the save directories.
[[nodiscard]] std::optional<std::string>
find_profile_name(const std::vector<CardSaveDirectory>& directories);

// Normalizes a decoded save-directory name to a plain uppercase
// alphanumeric key: "baslus-2.1050", "BASLUS-21050", and "BASLUS21050" all
// normalize to "BASLUS21050".
[[nodiscard]] std::string
normalize_save_directory_key(std::string_view save_directory);

// Guesses the disc serial behind save-directory names. Grounded in the
// observed PCSX2 layout: a Burnout 3 card keeps "BASLUS-21050" while the
// per-game play time record is keyed "SLUS-21050" — the "BA" prefix is the
// card's save-name prefix, not part of the serial. Returns e.g.
// "SLUS-21050", or nullopt when no directory maps to a serial.
[[nodiscard]] std::optional<std::string>
guess_serial(const std::vector<std::string>& save_directories);

// Parses PCSX2's playtime.dat content (whitespace-separated
// "<serial> <minutes> <last-played-epoch>" lines) and returns the minutes
// recorded for the serial, when present.
[[nodiscard]] std::optional<std::uint64_t>
playtime_minutes_for_serial(std::string_view playtime_dat_bytes,
                            std::string_view serial);

// Locates PCSX2's playtime.dat in the standard per-user configuration
// locations ($XDG_CONFIG_HOME/PCSX2/inis/playtime.dat, then
// ~/.config/PCSX2/inis/playtime.dat, then the Windows Documents/PCSX2/inis
// folder). Returns nullopt when none exists.
[[nodiscard]] std::optional<std::filesystem::path> find_pcsx2_playtime_file();

// Formats a play time in minutes as a compact user-facing string
// ("163 h 29 m").
[[nodiscard]] std::string format_playtime(std::uint64_t minutes);

// Summarizes an import source for the import prompt: enumerates the save
// directories, extracts the profile name from the game's save data, and
// looks up the play time. playtime_dat_path may be empty to use the standard
// PCSX2 location; a missing file simply leaves the play time unset. Returns
// nullopt when the source holds no readable save data, with the reason in
// error.
[[nodiscard]] std::optional<Pcsx2CardSummary>
summarize_import_source(const ImportSource& source,
                        const std::filesystem::path& playtime_dat_path, std::error_code& error);

// summarize_import_source() for a ".ps2" card file.
[[nodiscard]] std::optional<Pcsx2CardSummary>
summarize_card_image(const std::filesystem::path& image_path,
                     const std::filesystem::path& playtime_dat_path,
                     std::error_code& error);

// The save-data store root of a profile slot (1-based slot index).
[[nodiscard]] std::filesystem::path
profile_slot_root(const std::filesystem::path& saves_root, std::size_t slot_index);

// Returns the first profile slot (1-based) that holds no save directories,
// or nullopt when all slots already hold save data.
[[nodiscard]] std::optional<std::size_t>
first_free_profile_slot(const std::filesystem::path& saves_root);

// Imports every save directory of the card image into the given empty
// profile slot (1-based). An occupied slot is refused (see
// import_into_profile_slot() in profile_slots.hpp for the confirmed
// overwrite with backup).
bool import_card_into_profile_slot(const std::filesystem::path& saves_root,
                                   const std::filesystem::path& image_path,
                                   std::size_t slot_index, std::error_code& error);

} // namespace takedown::save
