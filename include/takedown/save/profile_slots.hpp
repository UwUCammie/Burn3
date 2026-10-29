#pragma once

// Profile slots: the PC-side save architecture behind the game's own
// save/load.
//
// The saves root holds up to kProfileSlotCount slots. Each slot is a folder
// "profile-N" laid out like a PCSX2 folder card (one folder per save
// directory, e.g. "profile-2/BASLUS-21050/{BASLUS-21050,icon.sys,view.ico,
// _pcsx2_index}"), so a slot can be copied to or from PCSX2 as is. One slot
// is active at a time ("active-slot.txt" in the saves root); the save-card
// device (takedown/save/save_card_device.hpp) presents the active slot to the
// game and writes the game's saves back into it.
//
// Safety rules: an occupied slot is never replaced without the caller asking
// for it, and a replaced (or game-deleted) save is moved into
// "<saves root>/backups/" first, never deleted.
//
//   <saves root>/
//     active-slot.txt          "N"
//     profile-1/ .. profile-5/ save directories (+ "slot-info.txt")
//     backups/profile-N-YYYYMMDD-HHMMSS[-k]/
//     *.ps2, folder cards, loose save directories   (import sources)

#include "takedown/save/card_import.hpp"
#include "takedown/save/ps2_card_filesystem.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::save {

struct ProfileSlotSummary {
    std::size_t slot{};
    bool occupied{};
    std::vector<std::string> save_directories;
    // Profile name stored inside the game's save data.
    std::optional<std::string> profile_name;
    // Newest save modification time (Unix seconds).
    std::optional<std::int64_t> last_saved_unix;
    // PCSX2 play time recorded when the slot was imported.
    std::optional<std::uint64_t> playtime_minutes;
};

// Reads one slot (1-based). An absent or empty slot is "not occupied".
[[nodiscard]] ProfileSlotSummary summarize_profile_slot(const std::filesystem::path& saves_root,
                                                        std::size_t slot);

// Summaries of every slot, 1..kProfileSlotCount.
[[nodiscard]] std::vector<ProfileSlotSummary>
list_profile_slots(const std::filesystem::path& saves_root);

// One-line user-facing slot description, e.g.
// "Slot 2: Player - 2026-09-01 14:03 - 163 h 29 m" or "Slot 4: empty".
// Local time is used for the date.
[[nodiscard]] std::string describe_profile_slot(const ProfileSlotSummary& summary);

// Reads every save directory of a slot (empty for an empty slot).
[[nodiscard]] std::optional<std::vector<CardSaveDirectory>>
read_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                  std::error_code& error);

// Makes a slot hold exactly the given save directories: changed files are
// rewritten (each through a temporary file and a rename), unchanged files are
// left alone, and save directories that are no longer present are moved into
// the backups folder. Each save directory gets a "_pcsx2_index" carrying its
// timestamps and file order.
bool write_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                        const std::vector<CardSaveDirectory>& directories, std::error_code& error);

// Moves the whole slot folder into the backups folder. Returns the backup
// path, or nullopt (error cleared) when the slot had nothing to back up.
[[nodiscard]] std::optional<std::filesystem::path>
backup_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                    std::error_code& error);

[[nodiscard]] std::filesystem::path backups_root(const std::filesystem::path& saves_root);

enum class SlotOverwrite {
    // Fail with SaveDataErrc::slot_occupied when the slot holds save data.
    refuse,
    // Back the occupied slot up, then replace its contents.
    backup_and_replace,
};

// Imports every save directory of the source into the slot (1-based). The
// imported slot holds exactly the source's save directories. backup_path
// (optional) receives the backup folder of a replaced slot.
bool import_into_profile_slot(const std::filesystem::path& saves_root, const ImportSource& source,
                              std::size_t slot, SlotOverwrite overwrite, std::error_code& error,
                              std::filesystem::path* backup_path = nullptr);

// The active slot from "active-slot.txt"; 1 when missing or invalid.
[[nodiscard]] std::size_t read_active_profile_slot(const std::filesystem::path& saves_root);
bool write_active_profile_slot(const std::filesystem::path& saves_root, std::size_t slot,
                               std::error_code& error);

} // namespace takedown::save
