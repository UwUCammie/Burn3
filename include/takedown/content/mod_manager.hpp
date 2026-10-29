#pragma once

// The mods folder and its load order (docs/MODDING.md).
//
// ModOverlay answers "which file serves this game path"; it is a read-only
// snapshot. ModManager owns the folder itself: which mods exist, which are
// enabled, in what order they apply, and the load_order.txt that records that.
// It is the single source of truth the game and the in-game MODS page both
// read, so a change made while the game runs is live on the next file open.
//
// Layout, unchanged from ModOverlay:
//
//   <game>/../mods/
//     load_order.txt      one enabled mod folder per line, first applied first
//     MyMod/              one folder per mod (docs/MOD_API.md)
//       mod.json          manifest (or mod.txt: name/author/version/description)
//       icon.png          icon for the MODS page
//       game/TRACKS/_FEMAIN.M2V   file overrides mirroring the disc (also
//                         accepted at the top level)
//
// A folder not named in load_order.txt is disabled. With no load_order.txt the
// folders are all enabled in case-insensitive alphabetical order (folders whose
// name starts with '.' stay disabled), so a folder that only mirrors the disc
// behaves exactly like the overlay alone.
//
// Everything here is host-side and filesystem-only: no guest memory, no
// rendering, no settings. It is unit tested directly.

#include "takedown/content/mod_manifest.hpp"
#include "takedown/content/mod_overlay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace takedown::content {

struct ManagedMod {
    std::string folder;        // folder name on disk
    std::string display_name;  // manifest name, else the folder name
    std::string author;        // authors joined by ", ", may be empty
    std::string version;       // as written in the manifest, may be empty
    std::string description;   // may be empty
    ModManifest manifest;      // mod.json or mod.txt (docs/MOD_API.md)
    bool enabled{true};
    bool available{true};      // the folder exists on disk
    // Why an enabled mod is not applied (malformed manifest, missing or
    // disabled dependency, incompatible mod enabled, duplicate id). Empty when
    // the mod is fine. Set by every rescan and edit.
    std::vector<std::string> problems;
    // Enabled, present and without problems: its files and content are used.
    bool applied{false};
};

class ModManager {
  public:
    ModManager() = default;
    ModManager(std::filesystem::path mods_dir, std::filesystem::path game_root);

    // False when there is no mods folder to manage; every other call is then a
    // no-op and the overlay is empty.
    [[nodiscard]] bool active() const noexcept { return !directory_.empty(); }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] const std::filesystem::path& game_root() const noexcept { return game_root_; }

    // Reads the folder and load_order.txt. Keeps the enabled flags and order of
    // mods that are still there, appends folders that appeared (disabled, so a
    // live drop-in changes nothing until the player enables it) and keeps a
    // mod whose folder is gone as unavailable. Returns true when the list
    // changed since the previous call.
    bool rescan();

    // Transactionally select exactly these installed folders in host order.
    // Reject missing/invalid dependencies without changing the applied state.
    [[nodiscard]] std::optional<std::string>
    select_enabled_folders(const std::vector<std::string>& folders);

    // True when the folder's own timestamp moved since the last rescan, i.e.
    // a mod was added, removed or renamed. Cheap enough to poll every frame.
    [[nodiscard]] bool directory_changed() const;

    // Known mods in load order (enabled ones first in application order, but
    // disabled ones keep their place so re-enabling restores the old order).
    [[nodiscard]] const std::vector<ManagedMod>& mods() const noexcept { return mods_; }
    [[nodiscard]] std::size_t enabled_count() const noexcept;

    // Applies the MODS page's value for `row`: 0 disables the mod, k (>= 1)
    // enables it and makes it the k-th mod in the load order. False when the
    // row is out of range or the value is not a usable slot.
    bool apply_page_value(std::size_t row, std::uint32_t value);
    [[nodiscard]] bool set_enabled(std::size_t index, bool enabled);
    [[nodiscard]] bool move_up(std::size_t index);
    [[nodiscard]] bool move_down(std::size_t index);

    // Indices of the applied mods in the order they apply (the last wins):
    // the player's order, moved so every mod follows its dependencies and
    // load_after mods and precedes its load_before mods.
    [[nodiscard]] const std::vector<std::size_t>& application_order() const noexcept {
        return application_order_;
    }

    struct NativeClass {
        std::string id, name;
        int order{100};
        std::uint8_t modes{1}; // race = 1, crash = 2
        std::string problem;
    };
    [[nodiscard]] std::vector<NativeClass> car_classes() const;

    // Cars the applied mods add next to the retail ones, in application
    // order: content/cars/<car>/vehicle.json (docs/MOD_API.md):
    //   { "vehicle": "CUPEBMWM3", "template": "CUPECAR4", "name": "BMW M3 GTR",
    //     "unlock": { "type": "none" | "takedowns" | "event", "count": 50,
    //                 "event": 0 | "0A0000USC3V1", "medal": "any" | "bronze" |
    //                 "silver" | "gold", "text": "..." } }
    // The vehicle name's first four characters are its class; the game loads
    // its files from game/PVEH/<class>/<rest>.BGV, .HWD and .LWD, which the
    // mod adds. A later mod's car replaces an earlier one with the same
    // vehicle name. A file that cannot be read is listed in `problem`.
    //
    // Car skins (ModOverlay::car_skins) are listed too, with `skin_of` the
    // retail car, `template_vehicle` the same and unlock type "template" (the
    // original's own unlock). A vehicle.json with "skin_of" describes the
    // skin of that car instead of adding one: { "skin_of": "COMPCAR1",
    // "name": "...", "thumbnail": ..., "thumbnail_variants": [...] }; without
    // one the skin has the mod's name. thumbnail_variants accepts one to eight
    // paths; a missing entry means the car has no such paint choice.
    struct AddedCar {
        std::string vehicle;
        std::string template_vehicle;
        std::string skin_of;
        std::string name;
        std::string class_id;
        std::filesystem::path thumbnail;
        std::array<std::filesystem::path, 8> thumbnail_variants{};
        float mass_multiplier{1.0F}, top_speed_multiplier{1.0F};
        std::string unlock_type{"none"};
        std::uint32_t unlock_count{0U};
        std::string unlock_event;
        std::uint8_t unlock_medal{1U}; // 1 any medal (bronze or better), 2 silver, 3 gold
        std::string unlock_text;
        std::string mod;
        std::string problem;
    };
    [[nodiscard]] std::vector<AddedCar> added_cars() const;
    // Derived preview work runs in the launcher, never in the emulation loop.
    [[nodiscard]] std::optional<std::string> request_thumbnail_refresh() const;
    [[nodiscard]] bool thumbnail_cache_changed();

    // Writes load_order.txt (the enabled mods, in order). nullopt on success,
    // otherwise a message describing the failure.
    [[nodiscard]] std::optional<std::string> save() const;

    // The file override layer for the current state. The runtime re-reads this
    // whenever the manager changes, so a change is live on the next file open.
    [[nodiscard]] ModOverlay build_overlay() const;

    // Startup / menu log: one line per mod with its state, then the warnings
    // the overlay scan produced.
    [[nodiscard]] std::string describe() const;

  private:
    // Recomputes problems, applied flags and the application order.
    void validate();
    // Loads load_order.txt into `order`; false when the file does not exist.
    [[nodiscard]] bool read_load_order(std::vector<std::string>& order) const;
    void remember_directory_time();

    std::filesystem::path directory_;
    std::filesystem::path game_root_;
    std::vector<ManagedMod> mods_;
    std::vector<std::size_t> application_order_;
    std::filesystem::file_time_type directory_time_{};
    std::size_t directory_entries_{0U};
    bool directory_time_valid_{false};
    std::filesystem::file_time_type thumbnail_cache_time_{};
};

} // namespace takedown::content
