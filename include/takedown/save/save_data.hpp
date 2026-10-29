#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace takedown::save {

// The launcher's installation/project working directory contains saves/.
// Explicit environment, command-line and settings overrides remain supported.
// User-facing name is "save data" / "profiles" / "storage" — the on-disk
// layout stays PCSX2-compatible (see docs/SAVE_DATA.md).
[[nodiscard]] std::filesystem::path default_save_data_root();
// Previous per-user location, used only for the one-time default-root migration.
[[nodiscard]] std::filesystem::path legacy_save_data_root();
// Copies an old store only into an absent or empty destination. The old store
// remains intact; occupied destinations are never merged or overwritten.
bool migrate_save_data_root(const std::filesystem::path& source,
                            const std::filesystem::path& destination,
                            std::error_code& error);

enum class SaveDataErrc {
    invalid_save_root = 1,
    invalid_save_name,
    invalid_file_name,
    missing_save,
    missing_file,
    not_a_directory,
    invalid_slot,
    slot_occupied,
    no_game_save_data,
};

[[nodiscard]] std::error_code make_error_code(SaveDataErrc error) noexcept;

// A single logical file inside a save-data directory. The content holds the
// exact bytes of the save file (no device padding, no header, no .ps2 suffix).
struct SaveFile {
    std::string name;
    std::vector<std::byte> content;

    bool operator==(const SaveFile&) const = default;
};

// Host-side save-data storage. The on-disk layout matches the PS2 memory card
// as PCSX2 keeps it when a card is stored as a folder: one directory per save
// (named by the game's save key, e.g. "BASLUS-21050##") containing "icon.sys"
// and the save's data files. This is the "save data" / "profiles" / "storage"
// seam; the underlying layout is kept PCSX2-compatible so saves written here
// can also be opened in PCSX2 and vice-versa.
//
// Terms: a "save" (or "profile") is one game save under root_; a "save file"
// is a single file inside a save. A user drops PCSX2-exported save directories
// or an entire folder-form memory card under root_ and they are listed and
// read directly.
class SaveDataStore {
  public:
    // Opens (creating if needed) root as the save-data store. Failure is
    // reported in error and the store is left not usable.
    SaveDataStore(const std::filesystem::path& root, std::error_code& error);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // Lists the save-data directories under root_, by on-disk directory name.
    // Plain files (including a root "_pcsx2_superblock") are not saves.
    [[nodiscard]] std::vector<std::string> list_saves(std::error_code& error) const;

    [[nodiscard]] bool has_save(std::string_view save_name) const;

    // Creates the save directory. Fails if the name is invalid or already used.
    bool create_save(std::string_view save_name, std::error_code& error) const;

    // Deletes the save directory and everything inside it.
    bool remove_save(std::string_view save_name, std::error_code& error) const;

    // Reads every save file in the save directory. PCSX2's "_pcsx2_index"
    // internal index and leftover "*.writing" staging files (an interrupted
    // write_save_file) are skipped; a trailing ".ps2" on a stored file name is
    // stripped so binary-card-sourced files read under their logical name.
    [[nodiscard]] std::optional<std::vector<SaveFile>>
    read_save(std::string_view save_name, std::error_code& error) const;

    // Reads one save file. Returns nullopt (with error cleared) when the file
    // is absent.
    [[nodiscard]] std::optional<std::vector<std::byte>>
    read_save_file(std::string_view save_name, std::string_view file_name,
                   std::error_code& error) const;

    // Writes or replaces one save file. The stored file uses its plain logical
    // name (no ".ps2" suffix added), matching PCSX2's folder-form layout. The
    // bytes go to a temporary file first and replace the old file with one
    // rename, so an interrupted write never leaves a truncated save file.
    bool write_save_file(std::string_view save_name, std::string_view file_name,
                         std::span<const std::byte> content, std::error_code& error) const;

  private:
    std::filesystem::path root_;
};

} // namespace takedown::save

namespace std {

template <> struct is_error_code_enum<takedown::save::SaveDataErrc> : true_type {};

} // namespace std
