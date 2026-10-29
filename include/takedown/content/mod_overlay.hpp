#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace takedown::content {

// Data-mod file override layer (docs/MODDING.md, "Data mods").
//
// A mods folder holds one sub-folder per mod. Each mod mirrors the game's disc
// paths (mods/MyTrack/TRACKS/_FEMAIN.M2V replaces cdrom0:\TRACKS\_FEMAIN.M2V).
// The set of enabled mods and their order come from mods/load_order.txt when
// present, otherwise from the folder names in case-insensitive alphabetical
// order. Later mods win over earlier ones.
//
// The overlay is scanned once at startup and is read-only afterwards, so a
// lookup is one hash probe on an already-normalized key and an empty overlay
// costs a single emptiness test per file open. Lookup uses the same
// case-insensitive path rules as resolve_disc_path.

struct ModOverride {
    std::string key;                  // normalized lowercase '/'-joined game path
    std::filesystem::path host_path;  // file that serves this game path
    std::string mod;                  // winning mod folder name
    std::uint64_t size{0};
    bool replaces_base{false};        // the base game tree also has this path
    std::vector<std::string> shadowed; // earlier mods this one replaced
};

// A retail player car a mod draws differently (docs/MODDING.md, "Car skins").
// The mod's car file for PVEH/<class>/<car>.BGV (and its .HWD and .LWD, the
// base game's when the mod has none) is served under a skin car's own name
// instead of the retail one, so the retail car stays as it is and the skin is
// one more car to choose, driven like the original.
struct CarSkin {
    std::string mod;      // mod folder name
    std::string original; // retail vehicle name, e.g. "COMPCAR1"
    std::string vehicle;  // the skin's vehicle name (car_skin_vehicle)
};

// The skin's vehicle name: the original's class, "X" and seven base-36
// characters of a hash of the mod's ID and the original. Every player with
// the mod gets the same name whatever the mod's folder is called.
[[nodiscard]] std::string car_skin_vehicle(std::string_view mod_id, std::string_view original);

// Folder names directly under `mods_dir`, sorted case-insensitively and then
// by exact name, so the order is identical on every platform and file system.
// Dot-folders are included (to the mods menu they are simply disabled mods).
// Empty when the folder is missing.
[[nodiscard]] std::vector<std::string>
discover_mod_folders(const std::filesystem::path& mods_dir);

// What a mod may declare about itself in a top-level `mod.txt` or `mod.ini`
// (never a game file). Every field is empty when absent; consumers fall back to
// the folder name for `name`.
struct ModMetadata {
    std::string name;        // "name" or "title"
    std::string author;      // "author"
    std::string version;     // "version"
    std::string description; // "description" or "desc"
};

// Reads `mod.txt` / `mod.ini` at the root of a mod folder. Lines are
// `key = value` or `key: value`; keys are case-insensitive; `#` and `;` start
// a comment. A missing or unreadable file yields empty metadata.
[[nodiscard]] ModMetadata read_mod_metadata(const std::filesystem::path& mod_root);

class ModOverlay {
  public:
    ModOverlay() = default;

    // Scans `mods_dir`. A missing or empty folder yields an empty overlay with
    // no warnings. `game_root` (may be empty) tells replaced files from added
    // ones and retail car files (which become car skins) from others.
    [[nodiscard]] static ModOverlay scan(const std::filesystem::path& mods_dir,
                                         const std::filesystem::path& game_root);

    // Scans `mods_dir` enabling exactly `enabled_mods`, in that order (the
    // last one has the highest priority), matched case-insensitively against
    // the folder names. Names with no folder are ignored. With
    // `report_unlisted`, every folder the list does not name is reported as a
    // disabled mod; the mods menu (ModManager) owns its own enable flags and
    // order, so it passes false.
    [[nodiscard]] static ModOverlay scan(const std::filesystem::path& mods_dir,
                                         const std::filesystem::path& game_root,
                                         const std::vector<std::string>& enabled_mods,
                                         bool report_unlisted);

    [[nodiscard]] bool empty() const noexcept { return overrides_.empty(); }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    // Enabled mods in application order (the last one has the highest priority).
    [[nodiscard]] const std::vector<std::string>& active_mods() const noexcept {
        return active_mods_;
    }
    // Problems found while scanning (unknown load-order entries, case
    // collisions inside one mod, files too large for the file service, ...).
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    // Sorted by key.
    [[nodiscard]] const std::vector<ModOverride>& overrides() const noexcept { return overrides_; }
    // Retail car files the mods turned into skins, in application order
    // (only when the scan knew the game folder).
    [[nodiscard]] const std::vector<CarSkin>& car_skins() const noexcept { return car_skins_; }

    // The override for an already-normalized key (see normalize_disc_guest_path),
    // or nullptr.
    [[nodiscard]] const ModOverride* find(std::string_view key) const;

    // Human-readable startup log: one summary line, one line per mod and one
    // per override (capped at `max_override_lines`), then the warnings.
    [[nodiscard]] std::string describe(std::size_t max_override_lines = 200U) const;

  private:
    // Shared body of both scan overloads: enables `enabled_mods` (in order)
    // among the folders already listed, then fills the override index.
    static void scan_impl(ModOverlay& overlay, const std::filesystem::path& mods_dir,
                          const std::filesystem::path& game_root,
                          const std::vector<std::string>& enabled_mods, bool report_unlisted);

    std::filesystem::path directory_;
    std::vector<std::string> active_mods_;
    std::vector<std::string> warnings_;
    std::vector<ModOverride> overrides_;
    std::vector<CarSkin> car_skins_;
    std::unordered_map<std::string, std::size_t> index_;
};

// Resolves a guest path the way every host file consumer must: an enabled mod's
// file when one covers the path, otherwise the base game tree under
// `game_root` (resolve_disc_path). `overlay` may be null. When a mod covers the
// path the base tree is not consulted at all. The returned path's file size is
// whatever the replacement file's size is; callers must take sizes and sector
// counts from it, never from the base game.
[[nodiscard]] std::optional<std::filesystem::path>
resolve_game_path(const std::filesystem::path& game_root, const ModOverlay* overlay,
                  std::string_view guest_path, std::error_code& error);

// Chooses the mods folder: an explicit path wins; otherwise `<game_root>/../mods`
// (next to the game data folder). Returns nullopt when mods are disabled.
[[nodiscard]] std::optional<std::filesystem::path>
select_mods_directory(const std::filesystem::path& game_root,
                      const std::optional<std::filesystem::path>& explicit_directory,
                      bool enabled);

} // namespace takedown::content
