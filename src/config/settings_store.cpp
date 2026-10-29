#include "takedown/config/settings_store.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <utility>

namespace takedown::config {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

void add_diagnostic(LoadedSettings& result, std::string key, std::string message) {
    result.diagnostics.push_back({std::move(key), std::move(message)});
}

bool parse_bool_value(std::string_view text, bool& value) {
    const std::string lowered = to_lower_ascii(text);
    if (lowered == "true" || lowered == "1") {
        value = true;
        return true;
    }
    if (lowered == "false" || lowered == "0") {
        value = false;
        return true;
    }
    return false;
}

bool parse_float_value(std::string_view text, float& value) {
    if (text.empty()) {
        return false;
    }
    // strtof needs a terminated string; the view points into a longer line.
    const std::string buffer(text);
    char* end_pointer = nullptr;
    const float parsed = std::strtof(buffer.c_str(), &end_pointer);
    if (end_pointer != buffer.c_str() + buffer.size() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_model_quality(std::string_view text, ModelQuality& quality) {
    const std::string lowered = to_lower_ascii(text);
    if (lowered == "original") {
        quality = ModelQuality::Original;
        return true;
    }
    if (lowered == "low") {
        quality = ModelQuality::Low;
        return true;
    }
    if (lowered == "medium") {
        quality = ModelQuality::Medium;
        return true;
    }
    if (lowered == "high") {
        quality = ModelQuality::High;
        return true;
    }
    if (lowered == "ultra") {
        quality = ModelQuality::Ultra;
        return true;
    }
    return false;
}

// Plain decimal digits only: no sign, no whitespace, no overflow past 32 bits.
bool parse_u32(std::string_view text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10U + static_cast<std::uint64_t>(character - '0');
        if (parsed > 0xFFFF'FFFFULL) {
            return false;
        }
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool handle_resolution_width(const std::string_view text, AppSettings& settings) {
    return parse_u32(text, settings.graphics.resolution.width);
}

bool handle_resolution_height(const std::string_view text, AppSettings& settings) {
    return parse_u32(text, settings.graphics.resolution.height);
}

bool handle_display_mode(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    for (const DisplayMode mode : {DisplayMode::Windowed, DisplayMode::Borderless, DisplayMode::Fullscreen}) {
        if (to_string(mode) == lowered) {
            settings.graphics.display_mode = mode;
            return true;
        }
    }
    return false;
}

// Files written before display modes existed carry "fullscreen = true",
// which meant a borderless window. An explicit display_mode wins.
bool handle_legacy_fullscreen(const std::string_view text, AppSettings& settings) {
    bool fullscreen = false;
    if (!parse_bool_value(text, fullscreen)) {
        return false;
    }
    if (fullscreen && settings.graphics.display_mode == DisplayMode::Windowed) {
        settings.graphics.display_mode = DisplayMode::Borderless;
    }
    return true;
}

bool handle_vsync(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.vsync);
}

bool parse_fps_limit(const std::string_view text, std::optional<std::uint32_t>& limit) {
    const std::string lowered = to_lower_ascii(text);
    if (lowered == "unlimited" || lowered == "none") {
        limit = std::nullopt;
        return true;
    }
    std::uint32_t parsed = 0;
    if (!parse_u32(text, parsed)) {
        return false;
    }
    limit = parsed;
    return true;
}

// Small strings fit the SSO buffer, so these never allocate in practice.
std::string bool_text(const bool value) noexcept {
    return value ? "true" : "false";
}

std::string volume_text(const float value) noexcept {
    return std::to_string(value);
}

std::string fps_limit_to_text(const std::optional<std::uint32_t> limit) {
    return limit ? std::to_string(*limit) : std::string("unlimited");
}

bool handle_presentation_fps_limit(const std::string_view text, AppSettings& settings) {
    return parse_fps_limit(text, settings.graphics.presentation_fps_limit);
}

bool handle_menu_fps_limit(const std::string_view text, AppSettings& settings) {
    return parse_fps_limit(text, settings.graphics.menu_fps_limit);
}

bool handle_aspect_ratio(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    if (lowered == "4:3" || lowered == "standard") {
        settings.graphics.aspect_ratio = AspectRatio::Standard;
        return true;
    }
    if (lowered == "16:9" || lowered == "widescreen") {
        settings.graphics.aspect_ratio = AspectRatio::Widescreen;
        return true;
    }
    return false;
}

// Accepts the multiple ("2") or the percentage the menu shows ("200%").
bool handle_render_scale(std::string_view text, AppSettings& settings) {
    if (!text.empty() && text.back() == '%') {
        text.remove_suffix(1);
        std::uint32_t percent = 0;
        if (!parse_u32(text, percent) || percent == 0U || percent % 100U != 0U) {
            return false;
        }
        settings.graphics.render_scale = percent / 100U;
        return true;
    }
    return parse_u32(text, settings.graphics.render_scale);
}

bool handle_anti_aliasing(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    for (const AntiAliasing candidate : {AntiAliasing::Off, AntiAliasing::SpatialLow, AntiAliasing::SpatialMedium,
                                         AntiAliasing::SpatialHigh, AntiAliasing::SpatialUltra}) {
        if (to_string(candidate) == lowered) {
            settings.graphics.anti_aliasing = candidate;
            return true;
        }
    }
    // The sample-count names the header documents.
    for (const auto& [alias, anti_aliasing] : {std::pair{"2x", AntiAliasing::SpatialLow},
                                               std::pair{"4x", AntiAliasing::SpatialMedium},
                                               std::pair{"8x", AntiAliasing::SpatialHigh},
                                               std::pair{"16x", AntiAliasing::SpatialUltra}}) {
        if (lowered == alias) {
            settings.graphics.anti_aliasing = anti_aliasing;
            return true;
        }
    }
    return false;
}

bool handle_render_distance(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    for (const RenderDistance candidate : {RenderDistance::Original, RenderDistance::Far, RenderDistance::Maximum}) {
        if (to_string(candidate) == lowered) {
            settings.graphics.render_distance = candidate;
            return true;
        }
    }
    return false;
}

bool handle_motion_blur(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.motion_blur);
}

bool handle_field_of_view(const std::string_view text, AppSettings& settings) {
    if (to_lower_ascii(text) == "default") {
        settings.graphics.field_of_view = 0U;
        return true;
    }
    return parse_u32(text, settings.graphics.field_of_view);
}

bool handle_panini(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.panini);
}

bool handle_fov_correction(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.fov_correction);
}

bool handle_high_resolution_ui(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.high_resolution_ui);
}

bool handle_fsr(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    for (const FsrPreset candidate : {FsrPreset::Off, FsrPreset::NativeAa, FsrPreset::Quality, FsrPreset::Balanced,
                                      FsrPreset::Performance, FsrPreset::UltraPerformance}) {
        if (to_string(candidate) == lowered) {
            set_fsr_preset(settings.graphics, candidate);
            return true;
        }
    }
    return false;
}

// Percent, 0 to 100, kept in steps of 10.
bool handle_fsr_sharpness(const std::string_view text, AppSettings& settings) {
    std::uint32_t percent = 0U;
    if (!parse_u32(text, percent) || percent > 100U) {
        return false;
    }
    set_fsr_sharpness(settings.graphics, (percent + 5U) / 10U);
    return true;
}

bool handle_fps_counter(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.fps_counter);
}

bool handle_dithering(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.dithering);
}

bool handle_bloom(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.graphics.bloom);
}

bool handle_frame_rate(const std::string_view text, AppSettings& settings) {
    const std::string lowered = to_lower_ascii(text);
    if (lowered == "retail") {
        settings.graphics.frame_rate = FrameRateMode::Retail;
        return true;
    }
    if (lowered == "variable") {
        settings.graphics.frame_rate = FrameRateMode::Variable;
        return true;
    }
    return false;
}

bool handle_model_quality(const std::string_view text, AppSettings& settings) {
    return parse_model_quality(text, settings.graphics.model_quality);
}

bool handle_master_volume(const std::string_view text, AppSettings& settings) {
    return parse_float_value(text, settings.audio.master_volume);
}

bool handle_music_volume(const std::string_view text, AppSettings& settings) {
    return parse_float_value(text, settings.audio.music_volume);
}

bool handle_effects_volume(const std::string_view text, AppSettings& settings) {
    return parse_float_value(text, settings.audio.effects_volume);
}

bool handle_mute_when_unfocused(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.audio.mute_when_unfocused);
}

// The game's own Music/SFX levels run from 0 to 100.
bool parse_game_level(std::string_view text, std::optional<std::uint32_t>& level) {
    std::uint32_t parsed = 0;
    if (!parse_u32(text, parsed) || parsed > 100U) {
        return false;
    }
    level = parsed;
    return true;
}

bool handle_game_music_level(const std::string_view text, AppSettings& settings) {
    return parse_game_level(text, settings.audio.game_music_level);
}

bool handle_game_effects_level(const std::string_view text, AppSettings& settings) {
    return parse_game_level(text, settings.audio.game_effects_level);
}

bool handle_save_data_directory(const std::string_view text, AppSettings& settings) {
    if (text.empty()) {
        return false;
    }
    settings.save_data_directory = std::filesystem::path(text);
    return true;
}

bool handle_mods_directory(const std::string_view text, AppSettings& settings) {
    if (text.empty()) {
        return false;
    }
    settings.mods_directory = std::filesystem::path(text);
    return true;
}

bool handle_mods_enabled(const std::string_view text, AppSettings& settings) {
    return parse_bool_value(text, settings.mods_enabled);
}

struct SectionSpec {
    std::string_view name;
    std::initializer_list<std::pair<std::string_view, bool (*)(std::string_view, AppSettings&)>> keys;
};

constexpr SectionSpec kSections[] = {
    {"graphics",
     {
         {"resolution_width", handle_resolution_width},
         {"resolution_height", handle_resolution_height},
         {"display_mode", handle_display_mode},
         {"fullscreen", handle_legacy_fullscreen},
         {"vsync", handle_vsync},
         {"presentation_fps_limit", handle_presentation_fps_limit},
         {"frame_rate", handle_frame_rate},
         {"menu_fps_limit", handle_menu_fps_limit},
         {"model_quality", handle_model_quality},
         {"aspect_ratio", handle_aspect_ratio},
         {"render_scale", handle_render_scale},
         {"anti_aliasing", handle_anti_aliasing},
         {"render_distance", handle_render_distance},
         {"motion_blur", handle_motion_blur},
         {"dithering", handle_dithering},
         {"high_resolution_ui", handle_high_resolution_ui},
         {"fps_counter", handle_fps_counter},
         {"field_of_view", handle_field_of_view},
         {"fov_correction", handle_fov_correction},
         {"panini", handle_panini},
         {"fsr", handle_fsr},
         {"fsr_sharpness", handle_fsr_sharpness},
         {"bloom", handle_bloom},
     }},
    {"audio",
     {
         {"master_volume", handle_master_volume},
         {"music_volume", handle_music_volume},
         {"effects_volume", handle_effects_volume},
         {"mute_when_unfocused", handle_mute_when_unfocused},
         {"game_music_level", handle_game_music_level},
         {"game_effects_level", handle_game_effects_level},
     }},
    {"save_data",
     {
         {"directory", handle_save_data_directory},
     }},
    {"mods",
     {
         {"directory", handle_mods_directory},
         {"enabled", handle_mods_enabled},
     }},
};

const SectionSpec* find_section(const std::string_view name) {
    for (const SectionSpec& spec : kSections) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

std::string append_line_context(std::string&& message, const std::size_t line_number) {
    message += " (line ";
    message += std::to_string(line_number);
    message += ")";
    return std::move(message);
}

// RECOVERY: matches the original statement for statement; only the EH region
// numbers of the first two diagnostics differ (original +1 and +2), which the
// source does not visibly determine.
void apply_line(LoadedSettings& result, AppSettings& pending, std::uint32_t& file_version,
                const std::size_t line_number, const std::string_view section, const std::string_view key,
                const std::string_view value) {
    if (section.empty()) {
        if (key == "version") {
            if (!parse_u32(value, file_version)) {
                add_diagnostic(result, "version",
                               append_line_context("malformed version value; assuming version 1", line_number));
            }
        } else {
            add_diagnostic(result, std::string(key),
                           append_line_context("key outside any section is ignored", line_number));
        }
        return;
    }

    const SectionSpec* known_section = find_section(section);
    if (known_section == nullptr) {
        add_diagnostic(result, std::string("[") + std::string(section) + "]",
                       append_line_context("unknown settings section; its keys are ignored", line_number));
        return;
    }
    for (const auto& [name, handler] : known_section->keys) {
        if (name != key) {
            continue;
        }
        if (!handler(value, pending)) {
            add_diagnostic(result, std::string(section) + "." + std::string(key),
                           append_line_context("malformed value; keeping the default", line_number));
        }
        return;
    }
    add_diagnostic(result, std::string(section) + "." + std::string(key),
                   append_line_context("unrecognized key is ignored", line_number));
}

} // namespace

LoadedSettings load_settings_file(const std::filesystem::path& path) {
    LoadedSettings result{};
    AppSettings pending{};

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        result.file_existed = false;
        result.settings = normalized(std::move(pending));
        return result;
    }
    result.file_existed = true;

    std::ifstream stream(path);
    if (!stream.is_open()) {
        add_diagnostic(result, "version", "settings file exists but cannot be opened; using defaults");
        result.settings = normalized(std::move(pending));
        return result;
    }

    std::string section;
    std::string line;
    std::uint32_t parsed_file_version = 1U;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        if (trimmed.front() == '[') {
            if (trimmed.back() != ']') {
                add_diagnostic(result, std::string(trimmed),
                               append_line_context("malformed section header", line_number));
                // Keys below a broken header must not land in the previous section.
                section.clear();
                continue;
            }
            section = std::string(trim(trimmed.substr(1, trimmed.size() - 2)));
            if (find_section(section) == nullptr) {
                add_diagnostic(result, std::string("[") + section + "]",
                               append_line_context("unknown settings section; its keys are ignored", line_number));
            }
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string_view::npos) {
            add_diagnostic(result, std::string(trimmed),
                           append_line_context("malformed line; expected key = value", line_number));
            continue;
        }
        const std::string_view key = trim(trimmed.substr(0, equals));
        const std::string_view value = trim(trimmed.substr(equals + 1));
        if (key.empty()) {
            add_diagnostic(result, std::string(trimmed),
                           append_line_context("malformed line; empty key", line_number));
            continue;
        }
        apply_line(result, pending, parsed_file_version, line_number, section, key, value);
    }

    if (parsed_file_version > kSettingsFileVersion) {
        // Never guess at a newer schema: report it and use defaults.
        result.version_supported = false;
        pending = AppSettings{};
        add_diagnostic(result, "version",
                       "settings file version " + std::to_string(parsed_file_version) +
                           " is newer than the supported version " + std::to_string(kSettingsFileVersion) +
                           "; using defaults");
    }
    result.settings = normalized(std::move(pending));
    return result;
}

std::string serialize_settings(const AppSettings& settings) {
    const AppSettings canonical = normalized(settings);
    std::string document;
    document += "# takedown-recomp settings\n";
    document += "version = " + std::to_string(kSettingsFileVersion) + "\n";

    document += "\n[graphics]\n";
    document += "resolution_width = " + std::to_string(canonical.graphics.resolution.width) + "\n";
    document += "resolution_height = " + std::to_string(canonical.graphics.resolution.height) + "\n";
    document += "display_mode = " + std::string(to_string(canonical.graphics.display_mode)) + "\n";
    document += "vsync = " + bool_text(canonical.graphics.vsync) + "\n";
    document += "presentation_fps_limit = " + fps_limit_to_text(canonical.graphics.presentation_fps_limit) + "\n";
    document += "frame_rate = " + std::string(to_string(canonical.graphics.frame_rate)) + "\n";
    document += "menu_fps_limit = " + fps_limit_to_text(canonical.graphics.menu_fps_limit) + "\n";
    document += "model_quality = " + std::string(to_string(canonical.graphics.model_quality)) + "\n";
    document += "aspect_ratio = " + std::string(to_string(canonical.graphics.aspect_ratio)) + "\n";
    document += "render_scale = " + std::to_string(canonical.graphics.render_scale) + "\n";
    document += "anti_aliasing = " + std::string(to_string(canonical.graphics.anti_aliasing)) + "\n";
    document += "render_distance = " + std::string(to_string(canonical.graphics.render_distance)) + "\n";
    document += "motion_blur = " + bool_text(canonical.graphics.motion_blur) + "\n";
    document += "bloom = " + bool_text(canonical.graphics.bloom) + "\n";
    document += "dithering = " + bool_text(canonical.graphics.dithering) + "\n";
    document += "high_resolution_ui = " + bool_text(canonical.graphics.high_resolution_ui) + "\n";
    document += "fps_counter = " + bool_text(canonical.graphics.fps_counter) + "\n";
    document += "field_of_view = " +
                (canonical.graphics.field_of_view == 0U ? std::string("default")
                                                        : std::to_string(canonical.graphics.field_of_view)) +
                "\n";
    document += "fov_correction = " + bool_text(canonical.graphics.fov_correction) + "\n";
    document += "panini = " + bool_text(canonical.graphics.panini) + "\n";
    document += "fsr = " + std::string(to_string(fsr_preset(canonical.graphics))) + "\n";
    document += "fsr_sharpness = " + std::to_string(fsr_sharpness(canonical.graphics) * 10U) + "\n";

    document += "\n[audio]\n";
    document += "master_volume = " + volume_text(canonical.audio.master_volume) + "\n";
    document += "music_volume = " + volume_text(canonical.audio.music_volume) + "\n";
    document += "effects_volume = " + volume_text(canonical.audio.effects_volume) + "\n";
    document += "mute_when_unfocused = " + bool_text(canonical.audio.mute_when_unfocused) + "\n";
    if (canonical.audio.game_music_level) {
        document += "game_music_level = " + std::to_string(*canonical.audio.game_music_level) + "\n";
    }
    if (canonical.audio.game_effects_level) {
        document += "game_effects_level = " + std::to_string(*canonical.audio.game_effects_level) + "\n";
    }

    document += "\n[save_data]\n";
    if (canonical.save_data_directory) {
        document += "directory = " + canonical.save_data_directory->string() + "\n";
    }

    // The [mods] section is written only when it differs from the defaults.
    if (canonical.mods_directory || !canonical.mods_enabled) {
        document += "\n[mods]\n";
        if (canonical.mods_directory) {
            document += "directory = " + canonical.mods_directory->string() + "\n";
        }
        if (!canonical.mods_enabled) {
            document += "enabled = false\n";
        }
    }
    return document;
}

bool save_settings_file(const std::filesystem::path& path, const AppSettings& settings, std::error_code& error) {
    error.clear();
    std::error_code filesystem_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = filesystem_error;
            return false;
        }
    }

    // Write a sibling temp file and rename it over the target so a crash never
    // leaves a half-written settings file behind.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            error = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        const std::string document = serialize_settings(settings);
        stream.write(document.data(), static_cast<std::streamsize>(document.size()));
        stream.flush();
        if (!stream.good()) {
            error = std::make_error_code(std::errc::io_error);
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error;
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

std::filesystem::path default_settings_path() {
    if (const char* const config_home = std::getenv("XDG_CONFIG_HOME"); config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path(config_home) / "takedown-recomp" / "settings.ini";
    }

    std::optional<std::filesystem::path> home_path;
    if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        home_path = std::filesystem::path(home) / ".config" / "takedown-recomp" / "settings.ini";
    }

    if (const char* const app_data = std::getenv("APPDATA"); app_data != nullptr && *app_data != '\0') {
        const std::filesystem::path roaming = std::filesystem::path(app_data) / "takedown-recomp" / "settings.ini";
        // Keep using an older ~/.config file until a roaming one exists.
        std::error_code exists_error;
        if (home_path && !std::filesystem::exists(roaming, exists_error) &&
            std::filesystem::exists(*home_path, exists_error)) {
            return *home_path;
        }
        return roaming;
    }
    if (home_path) {
        return *home_path;
    }

    std::error_code error;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
    if (error) {
        return "takedown-recomp-settings.ini";
    }
    return temporary / "takedown-recomp" / "settings.ini";
}

} // namespace takedown::config
