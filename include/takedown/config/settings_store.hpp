#pragma once

#include "takedown/config/app_settings.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::config {

// The on-disk settings file is a small, versioned INI document:
//
//   # takedown-recomp settings
//   version = 1
//
//   [graphics]
//   resolution_width = 1920
//   resolution_height = 1080
//   display_mode = windowed         (windowed|borderless|fullscreen; older
//                                    files' "fullscreen = true" reads as borderless)
//   vsync = true
//   presentation_fps_limit = unlimited (unlimited|1..1000)
//   frame_rate = retail              (retail|variable)
//   menu_fps_limit = 240             (unlimited|1..1000; menus and videos)
//   model_quality = original         (original|low|medium|high|ultra)
//   aspect_ratio = 4:3               (4:3|16:9)
//   render_scale = 1                 (1..6 or 100%..600%; of the game's 640x448)
//   anti_aliasing = off              (off|2x|4x|8x|16x supersampling)
//   render_distance = original       (original|far|maximum)
//   motion_blur = true
//   dithering = true                 (false hides the game's dither pattern)
//   field_of_view = default          (default|40..140 degrees at full boost)
//   bloom = true
//
// Keys added after version 1 shipped are optional: an older file simply
// lacks them and gets their defaults, so the version did not change.
//
//   [audio]
//   master_volume = 1.0
//   music_volume = 1.0
//   effects_volume = 1.0
//   mute_when_unfocused = false
//
//   [save_data]
//   directory = /some/path           (absent = platform default)
//
//   [mods]                           (written only when changed from default)
//   directory = /some/path           (absent = <game folder>/../mods)
//   enabled = true                   (false = ignore the mods folder)
//
// Parsing rules (deliberately minimal and stable):
// - Lines starting with '#' or ';' are comments; blank lines are ignored.
//   There are NO inline comments: a value runs to end of line, so paths may
//   contain '#'.
// - A missing key keeps its default; an unrecognized key or section produces
//   a diagnostic and is ignored (forward compatibility with newer files).
// - A malformed value produces a diagnostic and keeps the default; the merged
//   result is always run through normalized() before use.
// - "version" must be <= kSettingsFileVersion. A newer major version is
//   reported via LoadedSettings::version_supported=false and defaults are
//   used (we never guess at unknown schemas).

inline constexpr std::uint32_t kSettingsFileVersion = 1;

struct SettingsStoreDiagnostic {
    std::string key;    // "version", "audio.master_volume", "[unknown]", ...
    std::string message;
};

struct LoadedSettings {
    AppSettings settings{};   // always normalized before return
    bool file_existed{false};
    bool version_supported{true};
    std::vector<SettingsStoreDiagnostic> diagnostics;
};

// Loads the INI file at `path`. A missing file is not an error: the result
// carries defaults with file_existed=false. Unreadable files that DO exist
// (permission, directory, ...) are reported as a diagnostic on "version".
[[nodiscard]] LoadedSettings load_settings_file(const std::filesystem::path& path);

// Serializes the versioned INI document for `settings` (canonical key order).
[[nodiscard]] std::string serialize_settings(const AppSettings& settings);

// Writes the INI atomically (temp file in the same directory, then rename).
// Creates parent directories. Returns false and sets `error` on failure.
[[nodiscard]] bool save_settings_file(const std::filesystem::path& path,
                                      const AppSettings& settings,
                                      std::error_code& error);

// Platform settings location: $XDG_CONFIG_HOME/takedown-recomp/settings.ini;
// on Windows %APPDATA%\takedown-recomp\settings.ini (unless only an older
// ~/.config file exists); otherwise ~/.config/takedown-recomp/settings.ini,
// then the temp dir.
[[nodiscard]] std::filesystem::path default_settings_path();

} // namespace takedown::config
