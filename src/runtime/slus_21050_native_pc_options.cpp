// RECOVERY: rebuilt from GIMPLE; a minority of functions (install, native_pc_option_rows, lobby_frontend_work,
// lobby_next_operation, lobby_retire_widgets, extend_pause_list, update_step, queue_mods_popup, publish_camera,
// publish_mods, service, string_pack lambdas, inspect_string_table) still differ in inliner detail from the original.
#include "takedown/runtime/slus_21050_native_pc_options.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/presentation/graphics_options.hpp"
#include "takedown/ui/options_menu.hpp"

#include <iomanip>
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <bit>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>

namespace takedown::runtime {
namespace {

// Everything this module adds to the game's string table, built once: the
// UTF-16 strings, their byte offsets inside the host region and the string ids
// (retail count + position) the pages refer to.
struct StringPack {
    std::vector<std::u16string> strings;
    std::vector<std::uint32_t> offsets;
    std::uint32_t title_id{};
    std::uint32_t gameplay_title_id{};
    std::uint32_t fps_label_id{};
    std::uint32_t controls_title_id{};
    std::array<std::uint32_t, kNativeModsPageRowCount> mods_label_ids{};
    std::array<std::uint32_t, kNativePcVideoRowCount> label_ids{};
    std::array<std::vector<std::uint32_t>, kNativePcVideoRowCount> value_ids;
    std::array<std::array<std::uint32_t, 5>, config::kMaximumRenderScale> aa_value_ids{};
    std::uint32_t mods_title_id{};
    std::uint32_t camera_title_id{};
    std::uint32_t camera_correction_id{};
    std::uint32_t camera_panini_id{};
    std::array<std::uint32_t, 2> camera_off_on_id{};
    std::array<std::uint32_t, kNativeModsPageMaximumSlot + 1U> mods_value_ids{};
    std::uint32_t mods_rescan_id{};
    std::uint32_t mods_button_value_id{};
    std::uint32_t quit_label_id{};
    std::uint32_t quit_description_id{};
    std::uint32_t size_bytes{};
};

const StringPack& string_pack() {
    static const StringPack pack = [] {
        StringPack result{};
        std::map<std::string, std::uint32_t> ids;
        const auto intern = [&ids, &result](const std::string& text) {
            const auto found = ids.find(text);
            if (found != ids.end()) {
                return found->second;
            }
            const auto id = native_pc_retail::kStringCount +
                            static_cast<std::uint32_t>(result.strings.size());
            result.strings.push_back(std::u16string(text.begin(), text.end()));
            ids.emplace(text, id);
            return id;
        };
        result.title_id = intern("VIDEO");
        result.gameplay_title_id = intern("GAMEPLAY");
        result.fps_label_id = intern("FPS COUNTER");
        result.controls_title_id = intern("CONTROLLERS");
        for (auto& id : result.mods_label_ids) {
            id = native_pc_retail::kStringCount + static_cast<std::uint32_t>(result.strings.size());
            result.strings.emplace_back(u"");
        }
        const auto& rows = native_pc_option_rows();
        for (std::size_t row = 0U; row < rows.size(); ++row) {
            result.label_ids[row] = intern(rows[row].label);
        }
        for (std::size_t row = 0U; row < rows.size(); ++row) {
            for (const std::string& value : rows[row].values) {
                result.value_ids[row].push_back(intern(value));
            }
        }
        result.mods_title_id = intern("MODS");
        result.camera_title_id = intern("CAMERA");
        for (std::uint32_t index = 0U; index <= kNativeModsPageMaximumSlot; ++index) {
            result.mods_value_ids[index] = intern(std::string(native_mods_page_value_text(index)));
        }
        result.mods_rescan_id = intern(std::string(std::string_view("REFRESH MOD LIST")));
        result.camera_correction_id = intern("FOV CORRECTION");
        result.camera_panini_id = intern("PANINI PROJECTION");
        result.camera_off_on_id = {intern("Off"), intern("On")};
        result.mods_button_value_id = intern("");
        std::uint32_t offset = 0U;
        for (std::uint32_t scale = 1U; scale <= config::kMaximumRenderScale; ++scale) {
            for (std::size_t index = 0U; index < 5U; ++index) {
                result.aa_value_ids[scale - 1U][index] =
                    intern(native_pc_aa_value_text(scale, index));
            }
        }
        result.quit_label_id = intern("QUIT GAME");
        result.quit_description_id = intern("CLOSE THE GAME");
        for (const std::u16string& text : result.strings) {
            result.offsets.push_back(offset);
            offset = (offset + static_cast<std::uint32_t>((text.size() + 1U) * 2U) + 3U) & ~3U;
        }
        result.size_bytes = offset;
        return result;
    }();
    return pack;
}

void write_u16(guest::EeMemory& memory, const guest::GuestAddress address,
               const std::uint16_t value) {
    memory.write_u8(address, static_cast<std::uint8_t>(value));
    memory.write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
}

std::uint16_t read_u16(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return static_cast<std::uint16_t>(memory.read_u8(address) | (memory.read_u8(address + 1U) << 8U));
}

void write_f32(guest::EeMemory& memory, const guest::GuestAddress address, const float value) {
    memory.write_u32_le(address, std::bit_cast<std::uint32_t>(value));
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::optional<std::string> check_word(const guest::EeMemory& memory,
                                      const guest::GuestAddress address,
                                      const std::uint32_t retail_value,
                                      const std::uint32_t patched_value) {
    if (!memory.contains(address, 4U)) {
        return "address " + hex32(address) + " is outside guest memory";
    }
    const std::uint32_t word = memory.read_u32_le(address);
    if (word != retail_value && word != patched_value) {
        return "word at " + hex32(address) + " is " + hex32(word) + ", expected " +
               hex32(retail_value);
    }
    return std::nullopt;
}

// What the game's string table looks like right now: not loaded yet (0),
// unusable (1, with the reason) or the retail table (2).
enum class TableState : std::uint32_t { NotLoaded, Unusable, Retail };

struct TableInfo {
    TableState state{TableState::NotLoaded};
    std::uint32_t object{0U};
    std::uint32_t array{0U};
    std::string reason;
};

TableInfo inspect_string_table(const guest::EeMemory& memory) {
    TableInfo info;
    if (!memory.contains(native_pc_retail::kStringTablePointer, 4U)) {
        info.state = TableState::Unusable;
        info.reason = "string table pointer is outside guest memory";
        return info;
    }
    info.object = memory.read_u32_le(native_pc_retail::kStringTablePointer);
    if (info.object == 0U) {
        return info;
    }
    const auto invalid = [&info](const std::string& why) {
        info.state = TableState::Unusable;
        info.reason = why;
        return info;
    };
    if (!memory.contains(info.object, 16U)) {
        return invalid("string table object " + hex32(info.object) + " is not in guest memory");
    }
    const std::uint32_t count = memory.read_u32_le(info.object + 8U);
    info.array = memory.read_u32_le(info.object + 12U);
    if (count - native_pc_retail::kStringCount > 1024U) {
        return invalid("string table holds " + std::to_string(count) + " strings, expected " +
                       std::to_string(native_pc_retail::kStringCount));
    }
    if (info.array == 0U || !memory.contains(info.array, native_pc_retail::kStringCount * 4U)) {
        return invalid("string table array " + hex32(info.array) + " is not in guest memory");
    }
    const std::uint32_t audio =
        memory.read_u32_le(info.array + native_pc_retail::kAudioStringId * 4U);
    if (!memory.contains(audio, 12U)) {
        return invalid("string " + std::to_string(native_pc_retail::kAudioStringId) +
                       " has pointer " + hex32(audio) +
                       " (the table does not hold final string pointers)");
    }
    static constexpr char16_t kExpected[] = u"AUDIO";
    for (std::uint32_t index = 0U; index < 6U; ++index) {
        if (read_u16(memory, audio + index * 2U) != kExpected[index]) {
            return invalid("string " + std::to_string(native_pc_retail::kAudioStringId) +
                           " is not the expected UTF-16 text");
        }
    }
    info.state = TableState::Retail;
    return info;
}

std::vector<std::string> limit_values() {
    std::vector<std::string> values{"Unlimited"};
    for (const std::uint32_t limit : ui::kOptionsMenuFrameRateLimits) {
        values.push_back(std::to_string(limit) + " FPS");
    }
    return values;
}

std::uint64_t mods_state_signature(const NativeModsPageState& mods) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(mods.mods.size());
    for (const NativeModsPageMod& mod : mods.mods) {
        mix(mod.label.size());
        for (const char character : mod.label) {
            mix(static_cast<unsigned char>(character));
        }
        mix(mod.enabled ? 1U : 0U);
        mix(mod.slot);
        mix(mod.information.size());
        for (const unsigned char character : mod.information) {
            mix(character);
        }
    }
    return hash;
}

std::vector<std::string> names(const std::initializer_list<const char*> values) {
    return {values.begin(), values.end()};
}

std::vector<std::string> fsr_sharpness_values() {
    std::vector<std::string> values;
    for (std::uint32_t tenths = 0U; tenths <= config::kFsrSharpnessSteps; ++tenths) {
        values.push_back(std::to_string(tenths * 10U) + "%");
    }
    return values;
}

// State of the rows past kNativePcOptionRowCount (FSR), which the class has no
// members for: what was last published and where each row's value table is.
std::array<std::uint32_t, kNativePcFsrRowCount> g_published_fsr{};
std::array<guest::GuestAddress, kNativePcFsrRowCount> g_fsr_tables{};

std::vector<std::string> render_scale_values() {
    std::vector<std::string> values;
    for (const std::uint32_t scale : config::kRenderScales) {
        values.push_back(presentation::render_scale_text(scale));
    }
    return values;
}

// Index in the frame rate limit list (0 is "Unlimited") of the entry nearest to
// a hand-edited limit.
std::size_t nearest_limit_index(const std::optional<std::uint32_t>& limit) {
    if (!limit.has_value()) {
        return 0U;
    }
    std::size_t best = 1U;
    std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0U; index < ui::kOptionsMenuFrameRateLimits.size(); ++index) {
        const std::uint32_t listed = ui::kOptionsMenuFrameRateLimits[index];
        const std::uint32_t distance = listed > *limit ? listed - *limit : *limit - listed;
        if (distance < best_distance) {
            best_distance = distance;
            best = index + 1U;
        }
    }
    return best;
}

// The frame rate limit a list index stands for (0 is "Unlimited").
std::optional<std::uint32_t> limit_for_index(const std::size_t index) {
    if (index == 0U) {
        return std::nullopt;
    }
    return ui::kOptionsMenuFrameRateLimits[index - 1U];
}

} // namespace

std::string native_pc_aa_value_text(const std::uint32_t render_scale, const std::size_t index) {
    const auto& values = native_pc_option_rows()[2].values;
    if (index >= values.size()) {
        return {};
    }
    const auto requested = static_cast<config::AntiAliasing>(index);
    const auto effective = presentation::effective_anti_aliasing(render_scale, requested);
    std::string text = values[index];
    if (effective != requested) {
        text += " (" + values[static_cast<std::size_t>(effective)] + " at " +
                std::to_string(render_scale * 100U) + "%)";
    }
    return text;
}

const std::array<NativePcOptionRow, kNativePcVideoRowCount>& native_pc_option_rows() {
    static const std::array<NativePcOptionRow, kNativePcVideoRowCount> rows{{
        {"WIDESCREEN", names({"Off", "On"})},
        {"RENDER SCALE", render_scale_values()},
        {"ANTI-ALIASING", names({"Off", "Low", "Medium", "High", "Ultra"})},
        {"FRAME RATE LIMIT", limit_values()},
        {"MENU FRAME LIMIT", limit_values()},
        {"RENDER DISTANCE", names({"Original", "Far", "Maximum"})},
        {"MODEL QUALITY", names({"Original", "Low", "Medium", "High", "Ultra"})},
        {"MOTION BLUR", names({"Off", "On"})},
        {"DITHERING", names({"Off", "On"})},
        {"HIGH RES UI", names({"Off", "On"})},
        {"DISPLAY MODE", names({"Windowed", "Borderless", "Fullscreen"})},
        {"VSYNC", names({"Off", "On"})},
        {"UPSCALING", names({"Off", "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance",
                             "FSR Ultra Performance"})},
        {"FSR SHARPNESS", fsr_sharpness_values()},
    }};
    return rows;
}

std::size_t native_pc_option_index(const std::size_t row, const config::GraphicsSettings& settings) {
    switch (row) {
    case 0:
        return settings.aspect_ratio == config::AspectRatio::Widescreen ? 1U : 0U;
    case 1: {
        const auto found =
            std::find(config::kRenderScales.begin(), config::kRenderScales.end(), settings.render_scale);
        // Not listed: wraps to the first entry.
        return static_cast<std::size_t>(found - config::kRenderScales.begin()) %
               config::kRenderScales.size();
    }
    case 2:
        return static_cast<std::size_t>(settings.anti_aliasing);
    case 3:
        return nearest_limit_index(settings.presentation_fps_limit);
    case 4:
        return nearest_limit_index(settings.menu_fps_limit);
    case 5:
        return static_cast<std::size_t>(settings.render_distance);
    case 6:
        return static_cast<std::size_t>(settings.model_quality);
    case 7:
        return settings.motion_blur ? 1U : 0U;
    case 8:
        return settings.dithering ? 1U : 0U;
    case 9:
        return settings.high_resolution_ui ? 1U : 0U;
    case 10:
        return static_cast<std::size_t>(settings.display_mode);
    case 11:
        return settings.vsync ? 1U : 0U;
    case 12:
        return static_cast<std::size_t>(config::fsr_preset(settings));
    case 13:
        return config::fsr_sharpness(settings);
    default:
        return 0U;
    }
}

bool native_pc_option_apply(const std::size_t row, const std::size_t index,
                            config::GraphicsSettings& settings) {
    if (row >= kNativePcVideoRowCount) {
        return false;
    }
    if (native_pc_option_rows()[row].values.size() <= index) {
        return false;
    }
    switch (row) {
    case 0:
        settings.aspect_ratio =
            index == 1U ? config::AspectRatio::Widescreen : config::AspectRatio::Standard;
        break;
    case 1:
        settings.render_scale = config::kRenderScales[index];
        break;
    case 2:
        settings.anti_aliasing = static_cast<config::AntiAliasing>(index);
        break;
    case 3:
        settings.presentation_fps_limit = limit_for_index(index);
        break;
    case 4:
        settings.menu_fps_limit = limit_for_index(index);
        break;
    case 5:
        settings.render_distance = static_cast<config::RenderDistance>(index);
        break;
    case 6:
        settings.model_quality = static_cast<config::ModelQuality>(index);
        break;
    case 7:
        settings.motion_blur = index == 1U;
        break;
    case 8:
        settings.dithering = index == 1U;
        break;
    case 9:
        settings.high_resolution_ui = index == 1U;
        break;
    case 10:
        settings.display_mode = static_cast<config::DisplayMode>(index);
        break;
    case 11:
        settings.vsync = index == 1U;
        break;
    case 12:
        config::set_fsr_preset(settings, static_cast<config::FsrPreset>(index));
        break;
    case 13:
        config::set_fsr_sharpness(settings, static_cast<std::uint32_t>(index));
        break;
    default:
        return false;
    }
    return true;
}

std::uint32_t native_camera_fov_level(const config::GraphicsSettings& settings) {
    const std::uint32_t degrees =
        settings.field_of_view != 0U ? settings.field_of_view : kNativeCameraFovDefault;
    const std::uint32_t maximum =
        kNativeCameraFovMinimum + (kNativeCameraFovLevels - 1U) * kNativeCameraFovStep;
    const std::uint32_t clamped = std::clamp(degrees, kNativeCameraFovMinimum, maximum);
    return (clamped - kNativeCameraFovMinimum + kNativeCameraFovStep / 2U) / kNativeCameraFovStep;
}

std::uint32_t native_camera_fov_from_level(const std::uint32_t level) {
    const std::uint32_t degrees =
        kNativeCameraFovMinimum + std::min(level, kNativeCameraFovLevels - 1U) * kNativeCameraFovStep;
    return degrees == kNativeCameraFovDefault ? 0U : degrees;
}

std::uint32_t native_camera_slider_cells(const std::uint32_t level) {
    const std::uint32_t steps = kNativeCameraFovLevels - 1U;
    return (std::min(level, steps) * kNativeCameraSliderCells + steps / 2U) / steps;
}

std::uint32_t native_camera_fov_degrees(const std::uint32_t level) {
    const std::uint32_t degrees =
        kNativeCameraFovMinimum +
        (level < kNativeCameraFovLevels - 1U ? level : kNativeCameraFovLevels - 1U) *
            kNativeCameraFovStep;
    return degrees;
}

std::array<NativeCameraSliderMark, kNativeCameraSliderMarks>
native_camera_slider_marks(const std::uint32_t level) {
    const float fraction =
        static_cast<float>(std::min(level, kNativeCameraFovLevels - 1U)) /
        static_cast<float>(kNativeCameraFovLevels - 1U);
    const float knob = 326.0F + fraction * 249.0F - 2.725F;
    return {{
        {std::to_string(native_camera_fov_degrees(level)), knob, 152.0F, true},
        {std::to_string(kNativeCameraFovMinimum), 329.5F, 167.0F, false},
        {std::to_string(kNativeCameraFovDefault), 447.775F, 167.0F, false},
        {std::to_string(kNativeCameraFovMinimum + (kNativeCameraFovLevels - 1U) * kNativeCameraFovStep),
         566.05F, 167.0F, false},
    }};
}

std::string_view native_mods_page_value_text(const std::uint32_t index) noexcept {
    static const std::array<std::string, kNativeModsPageMaximumSlot + 1U> kText = [] {
        std::array<std::string, kNativeModsPageMaximumSlot + 1U> text;
        text[0] = "Disabled";
        for (std::uint32_t slot = 1U; slot <= kNativeModsPageMaximumSlot; ++slot) {
            text[slot] = std::to_string(slot);
        }
        return text;
    }();
    if (index < kText.size()) {
        return kText[index];
    }
    return {};
}

namespace {

// Every retail word the module acts at or beside, with its retail value. The
// native call sequences only ever land on these, so one mismatch (another
// game build, a patched executable) keeps the whole module off.
constexpr std::array<Slus21050NativePcOptions::RetailWord, 60> kRetailWords{{
    {native_pc_retail::kHubSelectSite, 0x24020004U},
    {0x0036B64CU, 0xAE080014U},
    {0x0036B660U, 0x14620008U},
    {0x0036B674U, 0x0C0DF094U},
    {0x003B8900U, 0x27BDFFF0U},
    {native_pc_retail::kFrontendDraw, 0x27BDFFD0U},
    {0x00217690U, 0x10000004U},
    {native_pc_retail::kGameplayInit, 0x27BDFFE0U},
    {native_pc_retail::kPauseItemsArgument, 0x0C0C851CU},
    {0x0032C550U, 0x27BDFED0U},
    {0x0032CC00U, 0x27BDFF60U},
    {0x0032B530U, 0x27BDFFB0U},
    {0x004DE518U, 0x0032B6C0U},
    {0x004DE51CU, 0x0032B530U},
    {native_pc_retail::kVideoInit, 0x27BDFFE0U},
    {native_pc_retail::kControllersInit, 0x27BDFFC0U},
    {native_pc_retail::kMainMenuInit, 0x27BDFFD0U},
    {native_pc_retail::kMainMenuCountSite, 0x24030005U},
    {native_pc_retail::kMainMenuItemsArgument, 0x26060018U},
    {0x0031845CU, 0x0C0C851CU},
    {native_pc_retail::kMainMenuDescriptionsArgument, 0x26060040U},
    {native_pc_retail::kMainMenuConfirmSite, 0x96030014U},
    {native_pc_retail::kMainMenuUpdateReturn, 0xDFBF0010U},
    {native_pc_retail::kHubInit, 0x27BDFFE0U},
    {native_pc_retail::kHubCountSite, 0x24020005U},
    {native_pc_retail::kHubItemsArgument, 0x26060008U},
    {native_pc_retail::kHubWidgetInitCall, 0x0C0C851CU},
    {native_pc_retail::kDetailScreen, 0x27BDFFB0U},
    {native_pc_retail::kDetailSwitchSite, 0x1000015DU},
    {native_pc_retail::kDetailSwitchDelay, 0x00000000U},
    {native_pc_retail::kDetailSwitchEnd, 0x10000006U},
    {native_pc_retail::kNoopRoutine, 0x03E00008U},
    {native_pc_retail::kPageHeaderLookup, 0x3C0301E9U},
    {native_pc_retail::kPauseInit, 0x27BDFF90U},
    {native_pc_retail::kPauseUpdate, 0x27BDFFD0U},
    {native_pc_retail::kPauseMessages, 0x27BDFFE0U},
    {native_pc_retail::kPauseListEnd, 0x3C02004FU},
    {native_pc_retail::kPauseActionSwitch, 0x2C610006U},
    {native_pc_retail::kPauseIndexSwitch, 0x24030003U},
    {native_pc_retail::kPauseOpenCall, 0x0320F809U},
    {0x00342CB8U, 0x24060003U},
    {native_pc_retail::kPauseOpenFinish, 0x0320F809U},
    {0x00342D04U, 0x24060003U},
    {0x003421ECU, 0x24020001U},
    {0x00318530U, 0x27BDFFF0U},
    {0x003BE5A0U, 0x3C024370U},
    {0x003BC5B0U, 0x27BDFFF0U},
    {0x003BC4C0U, 0x8C830010U},
    {0x00319C80U, 0x27BDFF70U},
    {0x0031D930U, 0x27BDFE20U},
    {0x0031ECD0U, 0x27BDFF40U},
    {0x0038AD80U, 0x27BDFF70U},
    {0x00319360U, 0x94820000U},
    {0x00319330U, 0x94830000U},
    {0x0030ED70U, 0x08085DB4U},
    {0x002853F0U, 0x27BDFFF0U},
    {0x0030D500U, 0x27BDFFF0U},
    {0x001D7580U, 0x27BDFFE0U},
    {0x001D7560U, 0x8C82002CU},
    {0x002F9690U, 0x27BDFF00U},
}};

} // namespace

std::span<const Slus21050NativePcOptions::RetailWord> Slus21050NativePcOptions::retail_words() noexcept {
    return kRetailWords;
}

std::optional<std::string> Slus21050NativePcOptions::signature_mismatch(const guest::EeMemory& memory) {
    for (const RetailWord& word : kRetailWords) {
        auto problem = check_word(memory, word.address, word.value, word.value);
        if (problem.has_value()) {
            return problem;
        }
    }
    // The hub's selection table entries are the only data words patched.
    {
        const auto problem = check_word(memory, native_pc_retail::kHubSelectionTable, 0U, 6U);
        if (problem.has_value()) {
            return problem;
        }
    }
    {
        const auto problem = check_word(memory, native_pc_retail::kHubSelectionTableMods, 0U, 5U);
        if (problem.has_value()) {
            return problem;
        }
    }
    {
        const auto problem = check_word(memory, native_pc_retail::kHubSelectionTableCamera, 0U, 5U);
        if (problem.has_value()) {
            return problem;
        }
    }
    if (!memory.contains(native_pc_layout::kRegion, native_pc_layout::kRegionSize)) {
        return "the reserved region " + hex32(native_pc_layout::kRegion) + " is outside guest memory";
    }
    return std::nullopt;
}

bool Slus21050NativePcOptions::patch_string_table(guest::EeMemory& memory, const bool first_time) {
    const TableInfo table = inspect_string_table(memory);
    if (table.state == TableState::NotLoaded) {
        return false;
    }
    if (table.state == TableState::Unusable) {
        // The game may still be loading: only report once it has had time to.
        if (vblanks_ > 600U && first_time) {
            unavailable_ = table.reason;
        }
        return false;
    }
    const StringPack& pack = string_pack();
    const std::uint32_t total =
        native_pc_retail::kStringCount + static_cast<std::uint32_t>(pack.strings.size());
    if (total > (native_pc_layout::kVideoLabels - native_pc_layout::kStringArray) / 4U) {
        unavailable_ = "the page's string table overlaps its video labels";
        return false;
    }
    if (table.array != native_pc_layout::kStringArray) {
        for (std::uint32_t index = 0U; index < native_pc_retail::kStringCount; ++index) {
            memory.write_u32_le(native_pc_layout::kStringArray + index * 4U,
                                memory.read_u32_le(table.array + index * 4U));
        }
    }
    for (std::size_t index = 0U; index < pack.strings.size(); ++index) {
        memory.write_u32_le(
            native_pc_layout::kStringArray +
                (native_pc_retail::kStringCount + static_cast<std::uint32_t>(index)) * 4U,
            native_pc_layout::kStrings + pack.offsets[index]);
    }
    memory.write_u32_le(table.object + 8U, total);
    memory.write_u32_le(table.object + 12U, native_pc_layout::kStringArray);
    last_table_object_ = table.object;
    return true;
}

bool Slus21050NativePcOptions::install(guest::EeMemory& memory) {
    if (const auto problem = signature_mismatch(memory); problem.has_value()) {
        unavailable_ = *problem;
        return false;
    }
    if (string_pack().size_bytes > native_pc_layout::kModsLabels - native_pc_layout::kStrings) {
        unavailable_ = "the page's strings do not fit their region";
        return false;
    }
    if (!patch_string_table(memory, true)) {
        return false;
    }
    const StringPack& pack = string_pack();
    const auto& rows = native_pc_option_rows();

    memory.write_u32_le(native_pc_layout::kMagic, native_pc_layout::kMagicValue);
    memory.write_u32_le(native_pc_layout::kVersion, native_pc_layout::kVersionValue);
    memory.write_u32_le(native_pc_layout::kDirty, 0U);

    // The page's strings, as plain UTF-16 data.
    for (std::size_t index = 0U; index < pack.strings.size(); ++index) {
        const std::u16string& text = pack.strings[index];
        std::uint32_t at = native_pc_layout::kStrings + pack.offsets[index];
        for (const char16_t unit : text) {
            write_u16(memory, at, unit);
            at += 2U;
        }
        write_u16(memory, at, 0U);
    }

    // PC OPTIONS page: vtable, page object and one 20-byte record per row.
    memory.write_u32_le(native_pc_layout::kVtable, 0U);
    memory.write_u32_le(native_pc_layout::kVtable + 4U, 0U);
    memory.write_u32_le(native_pc_layout::kVtable + 8U, native_pc_layout::kUpdateEntry);
    memory.write_u32_le(native_pc_layout::kVtable + 12U, native_pc_retail::kNoopRoutine);
    memory.write_u32_le(native_pc_layout::kPage, native_pc_layout::kVtable);
    memory.write_u8(native_pc_layout::kPage + 4U, 0U);
    memory.write_u8(native_pc_layout::kPage + 5U, 0U);
    memory.write_u8(native_pc_layout::kPage + 6U, static_cast<std::uint8_t>(kNativePcOptionRowCount));
    memory.write_u8(native_pc_layout::kPage + 7U, 0U);
    std::uint32_t table_at = native_pc_layout::kTables;
    for (std::size_t row = 0U; row < kNativePcVideoRowCount; ++row) {
        if (row < kNativePcOptionRowCount) {
            // Row records past these would run into the MODS page; the page only
            // ever shows nine (publish_video_rows fills them from the window).
            const guest::GuestAddress item =
                native_pc_layout::kPage + 8U + static_cast<std::uint32_t>(row) * 20U;
            memory.write_u32_le(item, native_pc_layout::kStrings +
                                          pack.offsets[pack.label_ids[row] - native_pc_retail::kStringCount]);
            memory.write_u32_le(item + 4U, 0U);
            memory.write_u32_le(item + 8U, table_at);
            memory.write_u8(item + 12U, static_cast<std::uint8_t>(rows[row].values.size()));
            memory.write_u8(item + 13U, 4U);
            memory.write_u8(item + 14U, 0U);
            memory.write_u8(item + 15U, 0U);
            memory.write_u32_le(item + 16U, 0U);
            video_tables_[row] = table_at;
        } else {
            g_fsr_tables[row - kNativePcOptionRowCount] = table_at;
        }
        for (const std::uint32_t id : pack.value_ids[row]) {
            memory.write_u32_le(table_at, id);
            table_at += 4U;
        }
        table_at = (table_at + 15U) & ~15U;
    }

    // CONTROLLERS page object; its rows come from the host.
    memory.write_u32_le(native_pc_layout::kControlsVtable, 0U);
    memory.write_u32_le(native_pc_layout::kControlsVtable + 4U, 0U);
    memory.write_u32_le(native_pc_layout::kControlsVtable + 8U, native_pc_layout::kControlsUpdate);
    memory.write_u32_le(native_pc_layout::kControlsVtable + 12U, native_pc_layout::kControlsCleanup);
    memory.write_u32_le(native_pc_layout::kControlsPage, native_pc_layout::kControlsVtable);
    publish_controls_rows(memory);

    // CAMERA page: the FOV slider row, then the rows that cycle through Off / On.
    memory.write_u32_le(native_pc_layout::kCameraVtable, 0U);
    memory.write_u32_le(native_pc_layout::kCameraVtable + 4U, 0U);
    memory.write_u32_le(native_pc_layout::kCameraVtable + 8U, native_pc_layout::kCameraUpdateEntry);
    memory.write_u32_le(native_pc_layout::kCameraVtable + 12U, native_pc_layout::kCameraCleanupEntry);
    memory.write_u32_le(native_pc_layout::kCameraPage, native_pc_layout::kCameraVtable);
    memory.write_u8(native_pc_layout::kCameraPage + 4U, 0U);
    memory.write_u8(native_pc_layout::kCameraPage + 5U, 0U);
    memory.write_u8(native_pc_layout::kCameraPage + 6U, 2U);
    memory.write_u8(native_pc_layout::kCameraPage + 7U, 0U);
    memory.write_u32_le(native_pc_layout::kCameraPage + 8U, native_pc_layout::kCameraLabel);
    memory.write_u32_le(native_pc_layout::kCameraPage + 12U, 0U);
    memory.write_u32_le(native_pc_layout::kCameraPage + 16U, 0U);
    memory.write_u8(native_pc_layout::kCameraPage + 20U, 0U);
    memory.write_u8(native_pc_layout::kCameraPage + 21U, 7U);
    memory.write_u8(native_pc_layout::kCameraPage + 22U, 0U);
    memory.write_u8(native_pc_layout::kCameraPage + 23U, 0U);
    memory.write_u32_le(native_pc_layout::kCameraPage + 24U, 0U);
    memory.write_u32_le(native_pc_layout::kCameraDirty, 0U);
    memory.write_u32_le(native_pc_layout::kCameraValues, 0U);
    memory.write_u32_le(native_pc_layout::kCameraValues + 4U, 1U);
    memory.write_u32_le(native_pc_layout::kCameraValues + 8U, 0U);
    for (std::uint32_t offset = 0U; offset < 0x100U; offset += 4U) {
        memory.write_u32_le(native_pc_layout::kCameraMarks + offset, 0U);
    }
    const std::array<float, 4> value_color{1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<float, 4> end_color{0.62F, 0.78F, 0.92F, 1.0F};
    for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
        write_f32(memory, native_pc_layout::kCameraMarkValueColor + lane * 4U, value_color[lane]);
        write_f32(memory, native_pc_layout::kCameraMarkEndColor + lane * 4U, end_color[lane]);
    }
    memory.write_u32_le(native_pc_layout::kCameraMarkAnchor, std::bit_cast<std::uint32_t>(0.5F));
    memory.write_u32_le(native_pc_layout::kCameraMarkAnchor + 4U, std::bit_cast<std::uint32_t>(0.5F));
    memory.write_u32_le(native_pc_layout::kCameraCorrectionTable, pack.camera_off_on_id[0]);
    memory.write_u32_le(native_pc_layout::kCameraCorrectionTable + 4U, pack.camera_off_on_id[1]);
    for (const auto& [row, label_id] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             {1U, pack.camera_correction_id}, {2U, pack.camera_panini_id}}) {
        const guest::GuestAddress item = native_pc_layout::kCameraPage + 8U + row * 20U;
        memory.write_u32_le(item, native_pc_layout::kStrings +
                                      pack.offsets[label_id - native_pc_retail::kStringCount]);
        memory.write_u32_le(item + 4U, memory.read_u32_le(native_pc_layout::kCameraValues + row * 4U));
        memory.write_u32_le(item + 8U, native_pc_layout::kCameraCorrectionTable);
        memory.write_u8(item + 12U, 2U);
        memory.write_u8(item + 13U, 4U);
        memory.write_u8(item + 14U, 0U);
        memory.write_u8(item + 15U, 0U);
        memory.write_u32_le(item + 16U, 0U);
    }
    published_camera_.reset();

    // The Options hub's selection table: PC OPTIONS (and CAMERA / MODS) are
    // routed to the pages above.
    memory.write_u32_le(native_pc_retail::kHubSelectionTable, 6U);
    memory.write_u32_le(native_pc_retail::kHubSelectionTableCamera, mods_page_enabled_ ? 5U : 0U);
    memory.write_u32_le(native_pc_layout::kModsDirty, 0U);
    memory.write_u32_le(native_pc_layout::kModsRescan, 0U);
    for (std::size_t row = 0U; row < kNativeModsPageRowCount; ++row) {
        memory.write_u32_le(native_pc_layout::kModsValues + static_cast<std::uint32_t>(row) * 4U, 0U);
    }
    if (mods_page_enabled_) {
        memory.write_u32_le(native_pc_layout::kModsVtable, 0U);
        memory.write_u32_le(native_pc_layout::kModsVtable + 4U, 0U);
        memory.write_u32_le(native_pc_layout::kModsVtable + 8U, native_pc_layout::kModsUpdateEntry);
        memory.write_u32_le(native_pc_layout::kModsVtable + 12U, native_pc_layout::kModsCleanupEntry);
        memory.write_u32_le(native_pc_layout::kModsPage, native_pc_layout::kModsVtable);
        memory.write_u8(native_pc_layout::kModsPage + 4U, 0U);
        memory.write_u8(native_pc_layout::kModsPage + 5U, 0U);
        memory.write_u8(native_pc_layout::kModsPage + 6U, 0U);
        memory.write_u8(native_pc_layout::kModsPage + 7U, 0U);
        for (std::uint32_t index = 0U; index <= kNativeModsPageMaximumSlot; ++index) {
            memory.write_u32_le(native_pc_layout::kModsValueTable + index * 4U,
                                pack.mods_value_ids[index]);
        }
        memory.write_u32_le(native_pc_layout::kModsButtonValueTable, pack.mods_button_value_id);
        memory.write_u32_le(native_pc_retail::kHubSelectionTableMods, 0U);
    }

    // SETTINGS page object: no rows of its own (the host draws the tabs and
    // rows), reached from the hub's first row through table entry 7.
    memory.write_u32_le(native_pc_layout::kSettingsVtable, 0U);
    memory.write_u32_le(native_pc_layout::kSettingsVtable + 4U, 0U);
    memory.write_u32_le(native_pc_layout::kSettingsVtable + 8U, native_pc_layout::kSettingsUpdate);
    memory.write_u32_le(native_pc_layout::kSettingsVtable + 12U, native_pc_retail::kNoopRoutine);
    memory.write_u32_le(native_pc_layout::kSettingsPage, native_pc_layout::kSettingsVtable);
    memory.write_u32_le(native_pc_layout::kSettingsPage + 4U, 0U);
    memory.write_u32_le(native_pc_retail::kHubSelectionTableSettings, native_pc_retail::kSettingsDetail);
    settings_sounds_.clear();
    settings_apply_audio_ = false;
    settings_back_ = false;

    // GAMEPLAY page object.
    memory.write_u32_le(native_pc_layout::kGameplayVtable, 0U);
    memory.write_u32_le(native_pc_layout::kGameplayVtable + 4U, 0U);
    memory.write_u32_le(native_pc_layout::kGameplayVtable + 8U, native_pc_layout::kGameplayUpdate);
    memory.write_u32_le(native_pc_layout::kGameplayVtable + 12U, native_pc_retail::kNoopRoutine);
    memory.write_u32_le(native_pc_layout::kGameplayPage, native_pc_layout::kGameplayVtable);
    memory.write_u32_le(native_pc_layout::kGameplayDirty, 0U);

    stage_ = Stage::Idle;
    hub_items_ = 0U;
    hub_rows_ = 0U;
    published_valid_ = false;
    mods_signature_valid_ = false;
    published_mods_.clear();
    mods_popup_open_ = false;
    mods_popup_pages_.clear();
    mods_information_.clear();
    return true;
}

void Slus21050NativePcOptions::publish_camera(guest::EeMemory& memory,
                                              const config::GraphicsSettings& settings) {
    const auto level = native_camera_fov_level(settings);
    const std::array<std::uint32_t, 3> wanted{level,
                                              settings.fov_correction ? 1U : 0U,
                                              settings.panini ? 1U : 0U};
    if (published_camera_ == wanted) {
        return;
    }
    memory.write_u32_le(native_pc_layout::kCameraLevel, wanted[0]);
    memory.write_u8(native_pc_layout::kCameraPage + 20U,
                    static_cast<std::uint8_t>(native_camera_slider_cells(wanted[0])));
    for (const std::uint32_t row : {1U, 2U}) {
        memory.write_u32_le(native_pc_layout::kCameraValues + row * 4U, wanted[row]);
        memory.write_u32_le(native_pc_layout::kCameraPage + 8U + row * 20U + 4U, wanted[row]);
    }
    {
        const std::string text = "FIELD OF VIEW";
        guest::GuestAddress at = native_pc_layout::kCameraLabel;
        for (const char character : text) {
            write_u16(memory, at, static_cast<unsigned char>(character));
            at += 2U;
        }
        write_u16(memory, at, 0U);
    }
    published_camera_ = wanted;
}

void Slus21050NativePcOptions::publish(guest::EeMemory& memory,
                                       const config::GraphicsSettings& settings) {
    std::array<std::uint32_t, kNativePcVideoRowCount> wanted{};
    for (std::size_t row = 0U; row < kNativePcVideoRowCount; ++row) {
        wanted[row] = static_cast<std::uint32_t>(native_pc_option_index(row, settings));
    }
    const auto published = [this](const std::size_t row) {
        return row < kNativePcOptionRowCount ? published_[row] : g_published_fsr[row - kNativePcOptionRowCount];
    };
    bool same = published_valid_;
    for (std::size_t row = 0U; same && row < kNativePcVideoRowCount; ++row) {
        same = wanted[row] == published(row);
    }
    if (same) {
        return;
    }
    // The anti-aliasing labels follow the render scale (the first label is
    // always "Off", the others note what the scale does to them).
    const auto aa_table = video_tables_[2];
    const StringPack& pack = string_pack();
    const std::uint32_t scale = std::clamp(settings.render_scale, 1U, config::kMaximumRenderScale);
    const auto& ids = pack.aa_value_ids[scale - 1U];
    for (std::uint32_t i = 0U; i < 5U; ++i) {
        memory.write_u32_le(aa_table + i * 4U, ids[i]);
    }
    for (std::size_t row = 0U; row < kNativePcVideoRowCount; ++row) {
        memory.write_u32_le(native_pc_layout::kValues + static_cast<std::uint32_t>(row) * 4U,
                            wanted[row]);
        if (row < kNativePcOptionRowCount) {
            published_[row] = wanted[row];
        } else {
            g_published_fsr[row - kNativePcOptionRowCount] = wanted[row];
        }
    }
    published_valid_ = true;
    publish_video_rows(memory);
}

void Slus21050NativePcOptions::publish_video_rows(guest::EeMemory& memory) {
    const auto& rows = native_pc_option_rows();
    // The list widget shows nine of the rows; the window scrolls natively.
    video_window_ = std::min(video_window_, kNativePcVideoRowCount - kNativeModsPageRowCount);
    memory.write_u8(native_pc_layout::kPage + 6U, static_cast<std::uint8_t>(kNativeModsPageRowCount));
    memory.write_u8(native_pc_layout::kPage + 7U, 0U);
    for (std::size_t visible = 0U; visible < kNativeModsPageRowCount; ++visible) {
        const auto logical = video_window_ + visible;
        const auto item =
            native_pc_layout::kPage + 8U + static_cast<std::uint32_t>(visible) * 20U;
        const auto label =
            native_pc_layout::kVideoLabels +
            static_cast<std::uint32_t>(visible) * native_pc_layout::kVideoLabelStride;
        const std::string& text = rows[logical].label;
        const std::size_t length = std::min<std::size_t>(text.size(), 63U);
        for (std::size_t c = 0U; c < length; ++c) {
            write_u16(memory, label + static_cast<std::uint32_t>(c) * 2U,
                      static_cast<unsigned char>(text[c]));
        }
        write_u16(memory, label + static_cast<std::uint32_t>(length) * 2U, 0U);
        memory.write_u32_le(item, label);
        memory.write_u32_le(item + 4U,
                            memory.read_u32_le(native_pc_layout::kValues +
                                               static_cast<std::uint32_t>(logical) * 4U));
        memory.write_u32_le(item + 8U, logical < kNativePcOptionRowCount
                                           ? video_tables_[logical]
                                           : g_fsr_tables[logical - kNativePcOptionRowCount]);
        memory.write_u8(item + 12U, static_cast<std::uint8_t>(rows[logical].values.size()));
        memory.write_u8(item + 13U, 4U);
    }
}

bool Slus21050NativePcOptions::settings_page_active(const guest::EeMemory& memory) const {
    return installed_ && memory.read_u32_le(native_pc_retail::kCurrentPage) == native_pc_layout::kSettingsPage &&
           memory.read_u32_le(0x01E90424U) == 5U; // the front-end phase of menus
}

void Slus21050NativePcOptions::settings_play_sound(const std::uint32_t sound) {
    if (settings_sounds_.size() < 4U) {
        settings_sounds_.push_back(sound);
    }
}

guest::GuestAddress Slus21050NativePcOptions::page_address(const PageKind kind) const noexcept {
    switch (kind) {
    case PageKind::Mods:
        return native_pc_layout::kModsPage;
    case PageKind::Camera:
        return native_pc_layout::kCameraPage;
    case PageKind::Controls:
        return native_pc_layout::kControlsPage;
    case PageKind::Gameplay:
        return native_pc_layout::kGameplayPage;
    case PageKind::Settings:
        return native_pc_layout::kSettingsPage;
    case PageKind::Pc:
        break;
    }
    return native_pc_layout::kPage;
}

std::optional<std::size_t>
Slus21050NativePcOptions::mods_page_focus(const guest::EeMemory& memory) const {
    if (!installed_ || !mods_page_enabled_ || !memory.contains(native_pc_retail::kCurrentPage, 4U) ||
        memory.read_u32_le(native_pc_retail::kCurrentPage) != native_pc_layout::kModsPage) {
        return std::nullopt;
    }
    const std::size_t focus =
        static_cast<std::size_t>(memory.read_u8(native_pc_layout::kModsPage + 4U)) |
        (static_cast<std::size_t>(memory.read_u8(native_pc_layout::kModsPage + 5U)) << 8U);
    if (focus <= mods_rows_) {
        return focus;
    }
    return std::nullopt;
}

void Slus21050NativePcOptions::publish_mods(guest::EeMemory& memory,
                                            const NativeModsPageState& mods,
                                            const std::optional<std::uint32_t> focus) {
    mods_information_.clear();
    for (const NativeModsPageMod& mod : mods.mods) {
        mods_information_.push_back(mod.information.empty() ? mod.label : mod.information);
    }
    const std::size_t count = std::min(mods.mods.size(), kNativeModsPageMaximumMods);
    mods_window_ = std::min(mods_window_, mods.mods.size() - count);
    // Without a request the selection stays on the row it is on.
    const std::uint32_t selected = focus.value_or(
        mods_signature_valid_ ? memory.read_u16_le(native_pc_layout::kModsPage + 4U) : 0U);
    mods_rows_ = count;
    std::size_t enabled = 0U;
    for (const NativeModsPageMod& mod : mods.mods) {
        enabled += mod.enabled ? 1U : 0U;
    }
    // Disabled, then one load-order slot per enabled mod.
    const std::uint32_t values = std::min<std::uint32_t>(
        std::max<std::uint32_t>(static_cast<std::uint32_t>(enabled) + 2U, 2U),
        kNativeModsPageMaximumSlot + 1U);
    for (std::size_t index = 0U; index < count; ++index) {
        const NativeModsPageMod& mod = mods.mods[mods_window_ + index];
        const guest::GuestAddress label =
            native_pc_layout::kModsLabels +
            static_cast<std::uint32_t>(index) * native_pc_layout::kModsLabelStride;
        std::string text = mod.label;
        if (text.size() > 31U) {
            text.resize(31U);
        }
        std::uint32_t at = label;
        for (const char character : text) {
            write_u16(memory, at, static_cast<unsigned char>(character));
            at += 2U;
        }
        write_u16(memory, at, 0U);
        const std::uint32_t value = mod.enabled ? std::min(values - 1U, mod.slot) : 0U;
        const guest::GuestAddress item =
            native_pc_layout::kModsPage + 8U + static_cast<std::uint32_t>(index) * 20U;
        memory.write_u32_le(item, label);
        memory.write_u32_le(item + 4U, value);
        memory.write_u32_le(item + 8U, native_pc_layout::kModsValueTable);
        memory.write_u8(item + 12U, static_cast<std::uint8_t>(values));
        memory.write_u8(item + 13U, 4U);
        memory.write_u8(item + 14U, 0U);
        memory.write_u8(item + 15U, 0U);
        memory.write_u32_le(item + 16U, 0U);
        memory.write_u32_le(native_pc_layout::kModsValues + static_cast<std::uint32_t>(index) * 4U,
                            value);
    }
    // The last row is the REFRESH MOD LIST button.
    const StringPack& pack = string_pack();
    const guest::GuestAddress rescan =
        native_pc_layout::kModsPage + 8U + static_cast<std::uint32_t>(count) * 20U;
    memory.write_u32_le(rescan, native_pc_layout::kStrings +
                                    pack.offsets[pack.mods_rescan_id - native_pc_retail::kStringCount]);
    memory.write_u32_le(rescan + 4U, 0U);
    memory.write_u32_le(rescan + 8U, native_pc_layout::kModsButtonValueTable);
    memory.write_u8(rescan + 12U, 1U);
    memory.write_u8(rescan + 13U, 4U);
    memory.write_u8(rescan + 14U, 0U);
    memory.write_u8(rescan + 15U, 0U);
    memory.write_u32_le(rescan + 16U, 0U);
    memory.write_u32_le(native_pc_layout::kModsValues + static_cast<std::uint32_t>(count) * 4U, 0U);
    write_u16(memory, native_pc_layout::kModsPage + 4U,
              static_cast<std::uint16_t>(std::min(static_cast<std::uint32_t>(count), selected)));
    write_u16(memory, native_pc_layout::kModsPage + 6U, static_cast<std::uint16_t>(count + 1U));
    published_mods_.assign(count + 1U, 0U);
    for (std::size_t row = 0U; row <= count; ++row) {
        published_mods_[row] =
            memory.read_u32_le(native_pc_layout::kModsValues + static_cast<std::uint32_t>(row) * 4U);
    }
    mods_signature_valid_ = true;
    mods_values_stale_ = true;
}

void Slus21050NativePcOptions::scroll_mods(guest::EeMemory& memory, const NativeModsPageState& mods) {
    if (mods.mods.size() <= mods_rows_ || mods_rows_ < 2U) {
        return;
    }
    const std::optional<std::size_t> focus = mods_page_focus(memory);
    const std::optional<std::size_t> last = mods_last_focus_;
    mods_last_focus_ = focus;
    if (!focus.has_value()) {
        return;
    }
    const std::size_t end = mods.mods.size() - mods_rows_;
    // Wrapping from the button to the first row, or the other way round.
    if (last.has_value() && *last == mods_rows_ && *focus == 0U && mods_window_ != 0U) {
        mods_window_ = 0U;
        publish_mods(memory, mods, 0U);
        return;
    }
    if (last.has_value() && *last == 0U && *focus == mods_rows_ && mods_window_ < end) {
        mods_window_ = end;
        publish_mods(memory, mods, static_cast<std::uint32_t>(mods_rows_));
        return;
    }
    if (*focus + 1U == mods_rows_ && mods_window_ < end) {
        ++mods_window_;
        publish_mods(memory, mods, static_cast<std::uint32_t>(mods_rows_) - 2U);
        mods_last_focus_ = mods_rows_ - 2U;
    } else if (*focus == 0U && mods_window_ != 0U) {
        --mods_window_;
        publish_mods(memory, mods, 1U);
        mods_last_focus_ = 1U;
    }
}

Slus21050NativePcOptions::Result Slus21050NativePcOptions::service(
    guest::EeMemory& memory, config::GraphicsSettings& settings, const NativeModsPageState* mods) {
    Result result{};
    ++vblanks_;
    if (unavailable_.has_value()) {
        return result;
    }
    if (!installed_) {
        mods_page_enabled_ = mods != nullptr;
        // The string table loads late: try every 16th VBlank.
        if ((vblanks_ & 15U) != 0U || !install(memory)) {
            return result;
        }
        installed_ = true;
        result.installed_now = true;
        publish(memory, settings);
        publish_camera(memory, settings);
        memory.write_u32_le(native_pc_layout::kGameplayValues + 8U, settings.fps_counter ? 1U : 0U);
        if (mods_page_enabled_ && mods != nullptr) {
            publish_mods(memory, *mods);
            mods_signature_ = mods_state_signature(*mods);
        }
        return result;
    }
    if (mods_page_enabled_ && mods != nullptr) {
        if (mods->notice_serial != mods_notice_serial_) {
            mods_notice_serial_ = mods->notice_serial;
            if (!mods->notice.empty()) {
                mods_pending_notice_ = mods->notice;
            }
        }
        const std::uint64_t signature = mods_state_signature(*mods);
        if (!mods_signature_valid_ || mods_signature_ != signature) {
            publish_mods(memory, *mods);
            mods_signature_ = signature;
        }
        scroll_mods(memory, *mods);
    }
    if (memory.read_u32_le(native_pc_layout::kGameplayDirty) != 0U) {
        memory.write_u32_le(native_pc_layout::kGameplayDirty, 0U);
        const bool enabled = memory.read_u32_le(native_pc_layout::kGameplayValues + 8U) != 0U;
        if (settings.fps_counter != enabled) {
            settings.fps_counter = enabled;
            result.settings_changed = true;
        }
    }
    memory.write_u32_le(native_pc_layout::kGameplayValues + 8U, settings.fps_counter ? 1U : 0U);
    if (memory.read_u32_le(native_pc_layout::kCameraDirty) != 0U) {
        memory.write_u32_le(native_pc_layout::kCameraDirty, 0U);
        const std::array<std::uint32_t, 3> wanted{
            memory.read_u32_le(native_pc_layout::kCameraLevel),
            memory.read_u32_le(native_pc_layout::kCameraValues + 4U) != 0U ? 1U : 0U,
            memory.read_u32_le(native_pc_layout::kCameraValues + 8U) != 0U ? 1U : 0U};
        config::GraphicsSettings updated = settings;
        updated.field_of_view = native_camera_fov_from_level(wanted[0]);
        updated.fov_correction = static_cast<bool>(wanted[1]);
        updated.panini = static_cast<bool>(wanted[2]);
        if (!(updated == settings)) {
            settings = updated;
            result.settings_changed = true;
        }
        published_camera_ = wanted;
    }
    if (memory.read_u32_le(native_pc_layout::kDirty) != 0U) {
        for (std::size_t row = 0U; row < kNativePcVideoRowCount; ++row) {
            const std::uint32_t value = memory.read_u32_le(
                native_pc_layout::kValues + static_cast<std::uint32_t>(row) * 4U);
            const std::uint32_t shown = row < kNativePcOptionRowCount
                                            ? published_[row]
                                            : g_published_fsr[row - kNativePcOptionRowCount];
            if (!published_valid_ || shown != value) {
                const config::GraphicsSettings before = settings;
                if (native_pc_option_apply(row, value, settings) && !(settings == before)) {
                    result.settings_changed = true;
                }
            }
        }
        memory.write_u32_le(native_pc_layout::kDirty, 0U);
        published_valid_ = false;
        publish(memory, settings);
        return result;
    }
    if (mods_page_enabled_) {
        if (memory.read_u32_le(native_pc_layout::kModsRescan) != 0U) {
            memory.write_u32_le(native_pc_layout::kModsRescan, 0U);
            result.mods_rescan_requested = true;
        }
        if (memory.read_u32_le(native_pc_layout::kModsDirty) != 0U) {
            memory.write_u32_le(native_pc_layout::kModsDirty, 0U);
            for (std::size_t row = 0U; row < published_mods_.size(); ++row) {
                const std::uint32_t value = memory.read_u32_le(
                    native_pc_layout::kModsValues + static_cast<std::uint32_t>(row) * 4U);
                if (published_mods_[row] != value) {
                    result.mod_edit = Result::ModEdit{mods_window_ + row, value};
                    break;
                }
            }
        }
    }
    if ((vblanks_ & 31U) == 0U) {
        if (memory.read_u32_le(native_pc_layout::kMagic) != native_pc_layout::kMagicValue) {
            installed_ = false;
            return result;
        }
        // The game rebuilds its string table when a screen loads: patch it again.
        const std::uint32_t table = memory.read_u32_le(native_pc_retail::kStringTablePointer);
        if (table != last_table_object_ || table == 0U ||
            memory.read_u32_le(table + 12U) != native_pc_layout::kStringArray) {
            static_cast<void>(patch_string_table(memory, false));
        }
        publish(memory, settings);
        publish_camera(memory, settings);
    }
    return result;
}

namespace {

// Retail routines and objects the native call sequences use.
constexpr guest::GuestAddress kFreeNodeRoutine = 0x001D7560U;
constexpr guest::GuestAddress kAllocateNodeRoutine = 0x001D7580U;
constexpr guest::GuestAddress kTextBuilderRoutine = 0x002F9690U;
constexpr guest::GuestAddress kUiPool = 0x01E903E0U;
constexpr guest::GuestAddress kUiLayer = 0x01E89624U;
constexpr guest::GuestAddress kUiNodeKind = 0x01E896F4U;
constexpr guest::GuestAddress kUiFont = 0x00665ED8U;
constexpr guest::GuestAddress kPopupObject = 0x01E898E8U;
constexpr guest::GuestAddress kPopupCloseRoutine = 0x0032B530U;
constexpr guest::GuestAddress kPopupOpenRoutine = 0x0032CC00U;
constexpr std::uint32_t kPopupButtonString = 446U;
constexpr std::uint32_t kPopupIdleVtable = 0x004DE510U;
constexpr guest::GuestAddress kPlaySoundRoutine = 0x0030ED70U;
constexpr guest::GuestAddress kSoundObject = 0x01EA2970U;
constexpr std::uint32_t kPopupSound = 75U;
constexpr guest::GuestAddress kLobbyEnterRoutine = 0x00217690U;
constexpr std::uint64_t kLobbyEnterSelector = 0x94413FC165857D40ULL;
constexpr guest::GuestAddress kFrontendObject = 0x01E90240U;
constexpr guest::GuestAddress kCurrentScreen = 0x01EA2974U;
constexpr guest::GuestAddress kCurrentPhase = 0x01E90424U;
constexpr guest::GuestAddress kPopupLiveFlag = kPopupObject + 10U;
constexpr guest::GuestAddress kPopupFocus = kPopupObject + 12U;
constexpr guest::GuestAddress kWidgetHead = 0x01E90418U;
constexpr guest::GuestAddress kScreenObjectSlot = 0x01EA2980U;
constexpr guest::GuestAddress kScreenBackdropSlot = 0x01EA2984U;
constexpr guest::GuestAddress kLobbyHomeScreen = 0x01EA3048U;
constexpr guest::GuestAddress kLobbyHomePage = 0x01E9E050U;

std::uint16_t pad_action(const guest::EeMemory& memory, const std::uint32_t slot) {
    const int controller = static_cast<std::int8_t>(memory.read_u8(0x004F6061U));
    const std::uint32_t address =
        static_cast<std::uint32_t>(controller * 300) + slot + 0x004F5E08U;
    return memory.contains(address, 2U) ? memory.read_u16_le(address) : 0U;
}

} // namespace

std::span<const guest::GuestAddress> Slus21050NativePcOptions::hook_addresses() noexcept {
    static constexpr std::array<guest::GuestAddress, 31> kAddresses{
        native_pc_retail::kHubSelectSite,
        native_pc_layout::kSettingsUpdate,
        native_pc_retail::kMainMenuCountSite,
        native_pc_retail::kMainMenuItemsArgument,
        native_pc_retail::kMainMenuDescriptionsArgument,
        native_pc_retail::kMainMenuConfirmSite,
        native_pc_retail::kFrontendDraw,
        native_pc_layout::kLobbyCallReturn,
        native_pc_retail::kGameplayInit,
        native_pc_layout::kGameplayUpdate,
        native_pc_retail::kPauseItemsArgument,
        native_pc_layout::kModsCleanupEntry,
        0x0032C550U,
        native_pc_retail::kControllersInit,
        native_pc_layout::kControlsUpdate,
        native_pc_layout::kControlsCleanup,
        native_pc_layout::kCameraUpdateEntry,
        native_pc_layout::kCameraCleanupEntry,
        native_pc_retail::kVideoInit,
        native_pc_retail::kHubCountSite,
        native_pc_retail::kHubItemsArgument,
        native_pc_retail::kDetailSwitchSite,
        native_pc_layout::kUpdateEntry,
        native_pc_layout::kCallReturn,
        native_pc_retail::kPageHeaderLookup,
        native_pc_retail::kPauseListEnd,
        native_pc_retail::kPauseActionSwitch,
        native_pc_retail::kPauseIndexSwitch,
        native_pc_retail::kPauseOpenCall,
        native_pc_retail::kPauseOpenFinish,
        native_pc_layout::kModsUpdateEntry,
    };
    return kAddresses;
}

void Slus21050NativePcOptions::issue_call(guest::R5900State& state, const GuestCall& call) {
    state.write_gpr_u64(4U, call.a0);
    state.write_gpr_u64(5U, call.a1);
    state.write_gpr_u64(6U, call.a2);
    state.write_gpr_u64(7U, call.a3);
    state.write_gpr_u64(8U, call.t0);
    state.write_gpr_u64(9U, call.t1);
    state.write_gpr_u64(10U, call.t2);
    state.write_gpr_u64(11U, call.t3);
    state.write_fpr_bits(12U, call.f12_bits);
    state.write_gpr_u64(31U, native_pc_layout::kCallReturn);
    pending_result_to_ = call.result_to;
    state.set_pc(call.target);
}

void Slus21050NativePcOptions::queue_camera_marks_free(guest::EeMemory& memory) {
    if (memory.read_u32_le(native_pc_layout::kCameraMarksLive) == 0U) {
        return;
    }
    const std::uint32_t bucket = memory.read_u32_le(native_pc_layout::kCameraMarksBucket);
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        const guest::GuestAddress slot = native_pc_layout::kCameraMarkNodes + index * 4U;
        if (memory.read_u32_le(slot) != 0U) {
            queue_[queue_size_++] =
                GuestCall{kFreeNodeRoutine, kUiPool, bucket, memory.read_u32_le(slot)};
            memory.write_u32_le(slot, 0U);
        }
    }
    memory.write_u32_le(native_pc_layout::kCameraMarksLive, 0U);
}

void Slus21050NativePcOptions::queue_camera_marks(guest::EeMemory& memory, const bool allocate,
                                                  const bool value_only) {
    const auto marks =
        native_camera_slider_marks(memory.read_u32_le(native_pc_layout::kCameraLevel));
    const auto enqueue = [this](const GuestCall& call) { queue_[queue_size_++] = call; };
    if (allocate) {
        queue_camera_marks_free(memory);
        const auto layer = static_cast<std::int16_t>(memory.read_u16_le(kUiLayer));
        const auto bucket = static_cast<std::uint32_t>(layer + 4);
        const std::uint32_t kind = memory.read_u32_le(kUiNodeKind);
        memory.write_u32_le(native_pc_layout::kCameraMarksBucket, bucket);
        memory.write_u32_le(native_pc_layout::kCameraMarksLive, 1U);
        for (std::uint32_t index = 0U; index < 4U; ++index) {
            enqueue(GuestCall{.target = kAllocateNodeRoutine,
                              .a0 = kUiPool,
                              .a1 = bucket,
                              .a2 = kind,
                              .result_to = native_pc_layout::kCameraMarkNodes + index * 4U});
        }
        const std::uint32_t font = memory.read_u32_le(kUiFont);
        for (const auto& [style, size] : std::initializer_list<std::pair<guest::GuestAddress, float>>{
                 {native_pc_layout::kCameraMarkValueStyle, 24.0F},
                 {native_pc_layout::kCameraMarkEndStyle, 18.0F}}) {
            memory.write_u32_le(style, std::bit_cast<std::uint32_t>(1.1F));
            memory.write_u32_le(style + 4U, std::bit_cast<std::uint32_t>(1.1F));
            write_f32(memory, style + 8U, size);
            memory.write_u32_le(style + 12U, 0U);
            memory.write_u32_le(style + 16U, font);
            write_u16(memory, style + 20U, 0xFFFFU);
            write_u16(memory, style + 22U, 0xFFFFU);
        }
    }
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        const auto& mark = marks[index];
        if (value_only && !mark.value) {
            continue;
        }
        const guest::GuestAddress text = native_pc_layout::kCameraMarkTexts + index * 16U;
        std::uint32_t at = text;
        for (const char unit : mark.text.substr(0U, 7U)) {
            write_u16(memory, at, static_cast<std::uint16_t>(unit));
            at += 2U;
        }
        write_u16(memory, at, 0U);
        const guest::GuestAddress position = native_pc_layout::kCameraMarkPositions + index * 8U;
        write_f32(memory, position, mark.x);
        write_f32(memory, position + 4U, mark.y);
        enqueue(GuestCall{
            .target = kTextBuilderRoutine,
            .a1 = position,
            .a2 = native_pc_layout::kCameraMarkAnchor,
            .a3 = mark.value ? native_pc_layout::kCameraMarkValueColor
                        : native_pc_layout::kCameraMarkEndColor,
            .t0 = text,
            .t1 = mark.value ? native_pc_layout::kCameraMarkValueStyle
                        : native_pc_layout::kCameraMarkEndStyle,
            .t2 = 3U,
            .t3 = 2U,
            .f12_bits = std::bit_cast<std::uint32_t>(120.0F),
            .a0_from = native_pc_layout::kCameraMarkNodes + index * 4U});
    }
}

void Slus21050NativePcOptions::finish_sequence(guest::R5900State& state) {
    stage_ = Stage::Idle;
    queue_size_ = 0U;
    queue_next_ = 0U;
    state.write_gpr_u64(2U, finish_v0_);
    state.set_pc(finish_pc_);
}

Slus21050NativePcOptions::GuestCall
Slus21050NativePcOptions::resolved(const guest::EeMemory& memory, GuestCall call) {
    if (call.a0_from != 0U) {
        call.a0 = memory.read_u32_le(call.a0_from);
    }
    return call;
}

void Slus21050NativePcOptions::run_queue(guest::R5900State& state, const guest::EeMemory& memory) {
    stage_ = Stage::Queue;
    queue_next_ = 0U;
    if (queue_size_ == 0U) {
        finish_sequence(state);
        return;
    }
    issue_call(state, resolved(memory, queue_[queue_next_++]));
}

namespace {

// UTF-8 to UTF-16 for the information popup. Malformed bytes become U+FFFD,
// tabs become spaces, carriage returns and other controls (but not line feeds)
// are dropped.
std::u16string information_text(std::string_view text) {
    std::u16string out;
    std::uint32_t code = 0U;
    std::size_t i = 0U;
    while (text.size() > i) {
        const auto lead = static_cast<unsigned char>(text[i]);
        code = lead;
        std::size_t bytes = 0U;
        if (text[i] < 0) {
            if (lead >= 0xC2U && lead <= 0xDFU) {
                bytes = 2U;
            } else if (lead >= 0xE0U && lead <= 0xEFU) {
                bytes = 3U;
            } else if (lead >= 0xF0U && lead <= 0xF4U) {
                bytes = 4U;
            }
        } else {
            bytes = 1U;
        }
        bool valid = bytes != 0U && text.size() >= i + bytes;
        if (bytes > 1U) {
            code &= ~(0xFFFFFFFFU << (7U - static_cast<unsigned>(bytes)));
        }
        for (std::size_t n = 1U; n < bytes && valid; ++n) {
            valid = static_cast<char>(text[i + n] & 0xC0) == static_cast<char>(0x80);
            code = (code << 6U) | static_cast<std::uint32_t>(text[i + n] & static_cast<char>(0x3F));
        }
        if (!valid || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU) ||
            (bytes == 3U && code <= 0x7FFU) || (bytes == 4U && code <= 0xFFFFU)) {
            code = 0xFFFDU;
            i += 1U;
        } else {
            i += bytes;
        }
        if (code == 13U) {
            continue;
        }
        if (code == 9U) {
            code = 32U;
        }
        if (code < 32U && code != 10U) {
            continue;
        }
        if (code > 0xFFFFU) {
            code -= 0x10000U;
            out.push_back(static_cast<char16_t>(0xD800U + (code >> 10U)));
            out.push_back(static_cast<char16_t>(0xDC00U + (code & 0x3FFU)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

} // namespace

namespace {

// Splits the information text into pages of about 60 characters of weight
// (a line break weighs as much as 24 characters), breaking at a space or line
// feed when one lies in the second half of the page and never inside a
// surrogate pair.
std::vector<std::u16string> information_pages(std::string_view source) {
    const auto text = information_text(source);
    std::vector<std::u16string> pages;
    std::size_t start = 0U;
    while (start < text.size()) {
        std::size_t end = start;
        std::size_t space = start;
        std::size_t weight = 0U;
        while (end < text.size()) {
            const char16_t c = text[end];
            const std::size_t cost =
                c == u'\n' ? 24U : ((c >= 0xDC00U && c <= 0xDFFFU) ? 0U : 1U);
            if (weight + cost > 60U) {
                break;
            }
            weight += cost;
            if (c == u' ' || c == u'\n') {
                space = end;
            }
            ++end;
        }
        if (end < text.size() && text[end] != u' ' && text[end] != u'\n' &&
            start + (end - start) / 2U < space) {
            end = space;
        }
        if (start < end && text[end - 1U] >= 0xD800U && text[end - 1U] <= 0xDBFFU) {
            --end;
        }
        if (start == end) {
            ++end;
        }
        pages.push_back(text.substr(start, end - start));
        start = end;
        while (start < text.size() && (text[start] == u' ' || text[start] == u'\n')) {
            ++start;
        }
    }
    if (pages.empty()) {
        pages.emplace_back(u"No mod information available.");
    }
    return pages;
}

} // namespace

void Slus21050NativePcOptions::queue_mods_popup_close() {
    if (!mods_popup_open_) {
        return;
    }
    queue_[queue_size_++] = GuestCall{kPopupCloseRoutine, kPopupObject};
    mods_popup_open_ = false;
}

void Slus21050NativePcOptions::queue_mods_popup(guest::EeMemory& memory, const bool replace) {
    if (replace) {
        queue_mods_popup_close();
    }
    auto text = mods_popup_pages_[mods_popup_page_];
    if (mods_popup_pages_.size() > 1U) {
        text += information_text("\n" + std::to_string(mods_popup_page_ + 1U) + "/" +
                                 std::to_string(mods_popup_pages_.size()) + "  (Left/Right)");
    }
    const std::size_t count = std::min<std::size_t>(text.size(), native_pc_layout::kModsPopupTextUnits - 1U);
    for (std::size_t i = 0U; i < count; ++i) {
        write_u16(memory, native_pc_layout::kModsPopupText + static_cast<std::uint32_t>(i) * 2U,
                  text[i]);
    }
    write_u16(memory, native_pc_layout::kModsPopupText + static_cast<std::uint32_t>(count) * 2U, 0U);
    for (const auto& [address, value] : std::initializer_list<std::pair<guest::GuestAddress, float>>{
             {native_pc_layout::kModsPopupPosition, 64.0F},
             {native_pc_layout::kModsPopupPosition + 4U, 276.0F},
             {native_pc_layout::kModsPopupSize, 513.0F},
             {native_pc_layout::kModsPopupSize + 4U, 132.0F},
             {native_pc_layout::kModsPopupCentre, 320.0F},
             {native_pc_layout::kModsPopupCentre + 4U, 342.0F}}) {
        memory.write_u32_le(address, std::bit_cast<std::uint32_t>(value));
    }
    const std::uint32_t array = memory.read_u32_le(
        memory.read_u32_le(native_pc_retail::kStringTablePointer) + 12U);
    const std::uint32_t button = memory.read_u32_le(array + kPopupButtonString * 4U);
    queue_[queue_size_++] = GuestCall{kPopupOpenRoutine, kPopupObject, 2U,
                                      native_pc_layout::kModsPopupText, button};
    mods_popup_open_ = true;
}

void Slus21050NativePcOptions::open_mods_information(guest::R5900State& state,
                                                     guest::EeMemory& memory,
                                                     std::string_view text) {
    queue_size_ = 0U;
    if (memory.read_u32_le(kPopupObject) == kPopupIdleVtable) {
        mods_popup_pages_ = information_pages(text);
        mods_popup_page_ = 0U;
        queue_mods_popup(memory, false);
        queue_[queue_size_++] = GuestCall{kPlaySoundRoutine, kSoundObject, kPopupSound};
    }
    run_queue(state, memory);
}

void Slus21050NativePcOptions::service_lobby_prompt(const ui::HostPrompt* prompt) {
    if (prompt != nullptr) {
        lobby_prompt_ = *prompt;
        lobby_release_wait_ = true;
    } else {
        lobby_prompt_.reset();
    }
}

void Slus21050NativePcOptions::request_mods_page(std::uint32_t return_screen,
                                                 std::uint32_t return_page) noexcept {
    lobby_open_mods_ = true;
    lobby_return_requested_ = false;
    lobby_returned_ = false;
    lobby_return_screen_ = return_screen;
    lobby_return_page_ = return_page;
    lobby_release_wait_ = true;
}

void Slus21050NativePcOptions::request_lobby_return() noexcept {
    lobby_return_requested_ = true;
    lobby_open_mods_ = false;
    lobby_returned_ = false;
    lobby_release_wait_ = true;
}

bool Slus21050NativePcOptions::consume_lobby_returned() noexcept {
    return std::exchange(lobby_returned_, false);
}

void Slus21050NativePcOptions::cancel_lobby_navigation() noexcept {
    lobby_open_mods_ = false;
    lobby_return_requested_ = false;
    lobby_returned_ = false;
    lobby_return_page_ = 0U;
    lobby_return_screen_ = 0U;
    lobby_wait_page_ = false;
}

bool Slus21050NativePcOptions::lobby_input_blocked(bool buttons_pressed) noexcept {
    const bool active = lobby_prompt_.has_value() || lobby_popup_open_ || lobby_open_mods_ ||
                        lobby_return_requested_ || lobby_wait_page_ ||
                        lobby_stage_ != LobbyStage::Idle;
    if (active) {
        lobby_release_wait_ = true;
    } else if (!buttons_pressed) {
        lobby_release_wait_ = false;
    }
    return active || lobby_release_wait_;
}

std::optional<std::string> Slus21050NativePcOptions::consume_lobby_error() {
    return std::exchange(lobby_error_, std::nullopt);
}

void Slus21050NativePcOptions::lobby_call(guest::R5900State& state, GuestCall call) {
    state.write_gpr_u64(4U, call.a0);
    state.write_gpr_u64(5U, call.a1);
    state.write_gpr_u64(6U, call.a2);
    state.write_gpr_u64(7U, call.a3);
    state.write_gpr_u64(8U, call.t0);
    state.write_gpr_u64(9U, call.t1);
    state.write_gpr_u64(10U, call.t2);
    state.write_gpr_u64(11U, call.t3);
    state.write_fpr_bits(12U, call.f12_bits);
    pending_result_to_ = call.result_to;
    state.set_pc(call.target);
    state.write_gpr_u64(31U, native_pc_layout::kLobbyCallReturn);
}

void Slus21050NativePcOptions::finish_lobby_work(guest::R5900State& state) {
    lobby_stage_ = LobbyStage::Idle;
    if (lobby_saved_state_ != nullptr) {
        // The saved state predates the call: keep the live COP0 registers.
        std::array<std::uint32_t, guest::R5900State::kCop0RegisterCount> cop0{};
        for (std::size_t i = 0U; i < cop0.size(); ++i) {
            cop0[i] = state.read_cop0(i);
        }
        state = *lobby_saved_state_;
        for (std::size_t i = 0U; i < cop0.size(); ++i) {
            state.write_cop0(i, cop0[i]);
        }
        lobby_saved_state_.reset();
    }
}

void Slus21050NativePcOptions::lobby_next_operation(guest::R5900State& state,
                                                    guest::EeMemory& memory) {
    if (lobby_return_requested_) {
        if (lobby_return_screen_ == 0U || !memory.contains(lobby_return_screen_, 16U)) {
            lobby_error_ = "The original lobby screen is no longer available. Choose the lobby again.";
            lobby_return_requested_ = false;
            finish_lobby_work(state);
            return;
        }
        lobby_returning_ = true;
        lobby_transition_subpage_ =
            lobby_return_screen_ == kLobbyHomeScreen && lobby_return_page_ == kLobbyHomePage ? 1U
                                                                                             : 0U;
        state.write_gpr_u64(2U, lobby_return_screen_);
        lobby_stage_ = LobbyStage::Lookup;
        continue_lobby_work(state, memory);
        return;
    }
    if (lobby_open_mods_) {
        if (!mods_page_enabled_) {
            lobby_error_ = "The Mods screen is not available. Your active mods have not changed.";
            lobby_open_mods_ = false;
            finish_lobby_work(state);
            return;
        }
        lobby_returning_ = false;
        lobby_transition_subpage_ = 5U;
        lobby_stage_ = LobbyStage::Lookup;
        lobby_call(state, GuestCall{.target = kLobbyEnterRoutine, .a0 = kSoundObject});
        state.write_gpr_u64(5U, kLobbyEnterSelector);
        return;
    }
    if (!lobby_prompt_.has_value()) {
        finish_lobby_work(state);
        return;
    }
    const ui::HostPrompt& prompt = *lobby_prompt_;
    if (prompt.actions.empty() || prompt.actions.size() > 3U) {
        lobby_error_ = "The lobby prompt could not be shown.";
        finish_lobby_work(state);
        return;
    }
    // The popup text is UTF-16 in the region, limited to what the buffer holds.
    const auto write_text = [&memory](guest::GuestAddress at, std::string_view source,
                                      std::size_t capacity) {
        auto text = information_text(source);
        if (capacity <= text.size()) {
            text.resize(capacity - 1U);
        }
        if (!text.empty() && text.back() >= 0xD800U && text.back() <= 0xDBFFU) {
            text.pop_back();
        }
        for (std::size_t i = 0U; i < text.size(); ++i) {
            write_u16(memory, at + static_cast<std::uint32_t>(i) * 2U, text[i]);
        }
        write_u16(memory, at + static_cast<std::uint32_t>(text.size()) * 2U, 0U);
    };
    std::string body = prompt.title + "\n\n";
    for (const std::string& line : prompt.body_lines) {
        body += line + '\n';
    }
    write_text(native_pc_layout::kLobbyPopupText, body, native_pc_layout::kLobbyPopupTextUnits);
    for (std::size_t i = 0U; i < prompt.actions.size(); ++i) {
        write_text(native_pc_layout::kLobbyPopupButtons +
                       static_cast<std::uint32_t>(i) * native_pc_layout::kLobbyPopupButtonStride,
                   prompt.actions[i].label, native_pc_layout::kLobbyPopupButtonStride / 2U);
    }
    for (const auto& [address, value] : std::initializer_list<std::pair<guest::GuestAddress, float>>{
             {native_pc_layout::kLobbyPopupPosition, 64.0F},
             {native_pc_layout::kLobbyPopupPosition + 4U, 72.0F},
             {native_pc_layout::kLobbyPopupSize, 513.0F},
             {native_pc_layout::kLobbyPopupSize + 4U, 316.0F},
             {native_pc_layout::kLobbyPopupCentre, 320.0F},
             {native_pc_layout::kLobbyPopupCentre + 4U, 230.0F}}) {
        memory.write_u32_le(address, std::bit_cast<std::uint32_t>(value));
    }
    lobby_shown_prompt_ = prompt;
    lobby_popup_open_ = true;
    lobby_stage_ = LobbyStage::Popup;
    const std::size_t count = prompt.actions.size();
    lobby_call(state,
               GuestCall{.target = kPopupOpenRoutine,
                         .a0 = kPopupObject,
                         .a1 = count == 3U ? 8U : (count == 2U ? 4U : 2U),
                         .a2 = native_pc_layout::kLobbyPopupText,
                         .a3 = native_pc_layout::kLobbyPopupButtons,
                         .t0 = count > 1U ? native_pc_layout::kLobbyPopupButtons +
                                                native_pc_layout::kLobbyPopupButtonStride
                                          : 0U,
                         .t1 = count == 3U ? native_pc_layout::kLobbyPopupButtons +
                                                 2U * native_pc_layout::kLobbyPopupButtonStride
                                           : 0U});
}

Slus21050NativePcOptions::HookResult
Slus21050NativePcOptions::lobby_frontend_work(guest::R5900State& state, guest::EeMemory& memory) {
    if (lobby_wait_page_ && state.read_gpr_u32(4U) == kFrontendObject) {
        if (memory.read_u32_le(kCurrentScreen) == lobby_target_screen_ &&
            memory.read_u32_le(kCurrentPhase) == 5U &&
            (lobby_expected_page_ == 0U ||
             memory.read_u32_le(native_pc_retail::kCurrentPage) == lobby_expected_page_)) {
            lobby_wait_page_ = false;
            if (lobby_wait_returning_) {
                lobby_returned_ = true;
                lobby_return_page_ = 0U;
                lobby_return_screen_ = 0U;
            }
            return HookResult::NotHooked;
        }
        if (++lobby_wait_frames_ > 600U) {
            std::clog << "Native lobby navigation timeout: returning=" << lobby_wait_returning_
                      << " target-screen=0x" << std::hex << lobby_target_screen_
                      << " current-screen=0x" << memory.read_u32_le(kCurrentScreen)
                      << " expected-page=0x" << lobby_expected_page_ << " current-page=0x"
                      << memory.read_u32_le(native_pc_retail::kCurrentPage) << " phase=" << std::dec
                      << memory.read_u32_le(kCurrentPhase) << '\n';
            lobby_wait_page_ = false;
            lobby_error_ = "The original lobby page did not finish opening. Choose the lobby again.";
        }
        return HookResult::NotHooked;
    }
    if (stage_ != Stage::Idle || lobby_stage_ != LobbyStage::Idle ||
        state.read_gpr_u32(4U) != kFrontendObject || memory.read_u32_le(kCurrentPhase) != 5U) {
        return HookResult::NotHooked;
    }
    if (!lobby_open_mods_ && !lobby_return_requested_ && lobby_prompt_ == lobby_shown_prompt_ &&
        lobby_popup_open_ == lobby_prompt_.has_value()) {
        return HookResult::NotHooked;
    }
    lobby_saved_state_ = std::make_shared<guest::R5900State>(state);
    if (lobby_popup_open_ && lobby_prompt_.has_value() && lobby_shown_prompt_.has_value()) {
        ui::HostPrompt shown = *lobby_shown_prompt_;
        ui::HostPrompt desired = *lobby_prompt_;
        shown.focus_index = desired.focus_index;
        if (shown == desired && !lobby_open_mods_ && !lobby_return_requested_) {
            // Only the focus moved: move the retail cursor and play its sound.
            write_u16(memory, kPopupFocus,
                      static_cast<std::uint16_t>(
                          std::min(desired.focus_index, desired.actions.size() - 1U)));
            lobby_shown_prompt_ = desired;
            lobby_stage_ = LobbyStage::Sound;
            lobby_call(state, GuestCall{.target = kPlaySoundRoutine,
                                        .a0 = kSoundObject,
                                        .a1 = kPopupSound});
            return HookResult::Replaced;
        }
    }
    if (lobby_popup_open_ ||
        ((lobby_prompt_.has_value() || lobby_open_mods_ || lobby_return_requested_) &&
         memory.read_u8(kPopupLiveFlag) != 0U)) {
        lobby_stage_ = LobbyStage::Close;
        lobby_call(state, GuestCall{.target = kPopupCloseRoutine, .a0 = kPopupObject});
        return HookResult::Replaced;
    }
    lobby_next_operation(state, memory);
    return HookResult::Replaced;
}

void Slus21050NativePcOptions::continue_lobby_work(guest::R5900State& state,
                                                   guest::EeMemory& memory) {
    const auto fail = [this, &state](std::string message) {
        lobby_error_ = std::move(message);
        lobby_open_mods_ = false;
        lobby_return_requested_ = false;
        lobby_returning_ = false;
        finish_lobby_work(state);
    };
    switch (lobby_stage_) {
    case LobbyStage::Close:
        lobby_popup_open_ = false;
        lobby_shown_prompt_.reset();
        mods_popup_open_ = false;
        lobby_next_operation(state, memory);
        return;
    case LobbyStage::Popup:
        if (lobby_shown_prompt_.has_value()) {
            write_u16(memory, kPopupFocus,
                      static_cast<std::uint16_t>(std::min(lobby_shown_prompt_->actions.size() - 1U,
                                                          lobby_shown_prompt_->focus_index)));
        }
        lobby_stage_ = LobbyStage::Sound;
        lobby_call(state, GuestCall{.target = kPlaySoundRoutine,
                                    .a0 = kSoundObject,
                                    .a1 = kPopupSound});
        return;
    case LobbyStage::Lookup: {
        if (!(lobby_returning_ || lobby_open_mods_) ||
            (lobby_returning_ && !lobby_return_requested_)) {
            finish_lobby_work(state);
            return;
        }
        lobby_target_screen_ = state.read_gpr_u32(2U);
        const std::uint32_t pool = memory.read_u32_le(kSoundObject);
        if (lobby_target_screen_ == 0U || !memory.contains(lobby_target_screen_, 16U) ||
            pool == 0U || !memory.contains(pool, 32U)) {
            fail("The Mods screen is not available yet. Your active mods have not changed.");
            return;
        }
        const std::uint32_t activate = memory.read_u32_le(pool + 24U);
        const std::uint32_t switch_to = memory.read_u32_le(pool + 28U);
        if (activate == 0U || !memory.contains(activate, 4U) || switch_to == 0U ||
            !memory.contains(switch_to, 4U)) {
            fail("The original menu transition is unavailable.");
            return;
        }
        if (!lobby_returning_ && lobby_return_screen_ == 0U) {
            lobby_return_screen_ = memory.read_u32_le(kCurrentScreen);
            lobby_return_page_ = memory.read_u32_le(native_pc_retail::kCurrentPage);
        }
        const std::uint32_t page = memory.read_u32_le(native_pc_retail::kCurrentPage);
        if (lobby_return_screen_ != 0U &&
            (!memory.contains(lobby_return_screen_, 16U) ||
             (lobby_return_page_ != 0U && !memory.contains(lobby_return_page_, 4U)))) {
            fail("The original lobby page is no longer available. Choose the lobby again.");
            return;
        }
        guest::GuestAddress page_cleanup = 0U;
        if (page != 0U && memory.contains(page, 4U)) {
            const std::uint32_t vtable = memory.read_u32_le(page);
            if (vtable == 0U || !memory.contains(vtable + 12U, 4U)) {
                fail("The current menu page cannot be closed.");
                return;
            }
            page_cleanup = memory.read_u32_le(vtable + 12U);
            if (page_cleanup != 0U && !memory.contains(page_cleanup, 4U)) {
                fail("The current menu page cannot be closed.");
                return;
            }
        } else if (page != 0U) {
            fail("The current menu page cannot be closed.");
            return;
        }
        // The widgets of the current page are retired one by one through their
        // own cleanup routines, then the page itself.
        std::vector<std::uint32_t> visited;
        lobby_retire_calls_.clear();
        lobby_retire_index_ = 0U;
        for (unsigned widget = memory.read_u32_le(kWidgetHead); widget != 0U;
             widget = memory.read_u32_le(widget + 4U)) {
            if (!memory.contains(widget, 16U) || visited.size() > 255U ||
                std::find(visited.begin(), visited.end(), widget) != visited.end()) {
                fail("The current menu widgets cannot be closed.");
                return;
            }
            visited.push_back(widget);
            if (memory.read_u8(widget + 10U) != 0U) {
                const std::uint32_t widget_vtable = memory.read_u32_le(widget);
                if (!memory.contains(widget_vtable + 12U, 4U)) {
                    fail("The current menu widgets cannot be closed.");
                    return;
                }
                const std::uint32_t cleanup = memory.read_u32_le(widget_vtable + 12U);
                if (cleanup == 0U || !memory.contains(cleanup, 4U)) {
                    fail("The current menu widgets cannot be closed.");
                    return;
                }
                lobby_retire_calls_.push_back({.target = cleanup, .a0 = widget});
            }
        }
        lobby_activate_target_ = activate;
        lobby_switch_target_ = switch_to;
        memory.write_u32_le(kCurrentPhase, 6U);
        if (page_cleanup != 0U) {
            lobby_stage_ = LobbyStage::RetirePage;
            lobby_call(state, GuestCall{.target = page_cleanup, .a0 = page});
            return;
        }
        memory.write_u32_le(native_pc_retail::kCurrentPage, 0U);
        lobby_retire_widgets(state, memory);
        return;
    }
    case LobbyStage::RetirePage:
        memory.write_u32_le(native_pc_retail::kCurrentPage, 0U);
        lobby_retire_widgets(state, memory);
        return;
    case LobbyStage::RetireWidget:
        lobby_retire_widgets(state, memory);
        return;
    case LobbyStage::RetirePool:
        lobby_stage_ = LobbyStage::Activate;
        lobby_call(state, GuestCall{.target = lobby_activate_target_,
                                    .a0 = kSoundObject,
                                    .a1 = lobby_target_screen_,
                                    .a2 = lobby_transition_subpage_});
        return;
    case LobbyStage::Activate: {
        lobby_previous_screen_ = memory.read_u32_le(kCurrentScreen);
        memory.write_u32_le(kCurrentScreen, lobby_target_screen_);
        const std::uint32_t object = memory.read_u32_le(kScreenObjectSlot);
        if (object != 0U && memory.contains(object + 32U, 4U)) {
            const std::uint32_t backdrop = memory.read_u32_le(object + 32U);
            memory.write_u32_le(kScreenBackdropSlot, backdrop);
            memory.write_u32_le(lobby_target_screen_ + 8U, backdrop);
        }
        lobby_stage_ = LobbyStage::Switch;
        lobby_call(state, GuestCall{.target = lobby_switch_target_,
                                    .a0 = kSoundObject,
                                    .a1 = lobby_previous_screen_,
                                    .a2 = lobby_transition_subpage_});
        return;
    }
    case LobbyStage::Switch:
        lobby_wait_page_ = true;
        lobby_wait_frames_ = 0U;
        lobby_wait_returning_ = lobby_returning_;
        lobby_expected_page_ = lobby_returning_ ? lobby_return_page_ : native_pc_layout::kModsPage;
        std::clog << "Native lobby navigation opening: returning=" << lobby_returning_
                  << " target-screen=0x" << std::hex << lobby_target_screen_ << " expected-page=0x"
                  << lobby_expected_page_ << std::dec << '\n';
        lobby_return_requested_ = false;
        lobby_returning_ = false;
        lobby_open_mods_ = false;
        finish_lobby_work(state);
        return;
    case LobbyStage::Sound:
        finish_lobby_work(state);
        return;
    case LobbyStage::Idle:
        return;
    default:
        break;
    }
}


namespace {

constexpr guest::GuestAddress kRetirePoolRoutine = 0x003B8900U;
constexpr guest::GuestAddress kRetirePoolObject = 0x01E90438U;
constexpr guest::GuestAddress kPageInitRoutine = 0x00318530U;
constexpr guest::GuestAddress kPageInitFinish = 0x0045C858U;
// The retained list widget the option pages drive.
constexpr guest::GuestAddress kListObject = 0x01E8961CU;
constexpr guest::GuestAddress kListSetValueRoutine = 0x003BC5B0U;
constexpr guest::GuestAddress kListSetLevelRoutine = 0x003BC4C0U;
constexpr guest::GuestAddress kListObjectRefreshRoutine = 0x002853F0U;
constexpr guest::GuestAddress kListObjectRefresh = 0x01E7A800U;
constexpr guest::GuestAddress kListUpRoutine = 0x00319360U;
constexpr guest::GuestAddress kListDownRoutine = 0x00319330U;
constexpr guest::GuestAddress kFrontendMessageRoutine = 0x0030D500U;
constexpr guest::GuestAddress kListInitRoutine = 0x003BE5A0U;
constexpr guest::GuestAddress kTitleRoutine = 0x00319C80U;
constexpr guest::GuestAddress kTitleObject = 0x01E89518U;
constexpr guest::GuestAddress kHeaderRoutine = 0x0031D930U;
constexpr guest::GuestAddress kHeaderObject = 0x01E88A80U;
constexpr guest::GuestAddress kFooterRoutine = 0x0031ECD0U;
constexpr guest::GuestAddress kFooterObject = 0x01E89800U;
constexpr guest::GuestAddress kPageDrawRoutine = 0x0038AD80U;
constexpr guest::GuestAddress kPageDrawObject = 0x01E894FCU;

} // namespace

void Slus21050NativePcOptions::lobby_retire_widgets(guest::R5900State& state,
                                                    guest::EeMemory& memory) {
    while (lobby_retire_index_ < lobby_retire_calls_.size()) {
        const GuestCall call = lobby_retire_calls_[lobby_retire_index_++];
        if (memory.read_u8(call.a0 + 10U) == 0U) {
            continue;
        }
        lobby_stage_ = LobbyStage::RetireWidget;
        lobby_call(state, call);
        return;
    }
    lobby_stage_ = LobbyStage::RetirePool;
    lobby_call(state, GuestCall{.target = kRetirePoolRoutine, .a0 = kRetirePoolObject});
}

void Slus21050NativePcOptions::lobby_activate_target(guest::R5900State& state,
                                                     guest::EeMemory& memory) {
    static_cast<void>(memory);
    lobby_stage_ = LobbyStage::Activate;
    lobby_call(state, GuestCall{.target = lobby_activate_target_,
                                .a0 = kSoundObject,
                                .a1 = lobby_target_screen_,
                                .a2 = lobby_transition_subpage_});
}

void Slus21050NativePcOptions::begin_page_init(guest::R5900State& state, const bool pause_context,
                                               const PageKind kind) {
    page_kind_ = kind;
    init_pause_context_ = pause_context;
    if (kind == PageKind::Settings) {
        settings_pause_ = pause_context;
        settings_sounds_.clear();
        settings_apply_audio_ = false;
        settings_back_ = false;
    }
    finish_pc_ = kPageInitFinish;
    finish_v0_ = 1U;
    queue_size_ = 0U;
    stage_ = Stage::InitWrites;
    issue_call(state, GuestCall{.target = kPageInitRoutine, .a0 = page_address(kind)});
}

void Slus21050NativePcOptions::begin_page_update(guest::R5900State& state,
                                                 guest::EeMemory& memory, const PageKind kind) {
    page_kind_ = kind;
    update_page_ = state.read_gpr_u32(4U);
    finish_pc_ = state.read_gpr_u32(31U);
    finish_v0_ = 0U;
    queue_size_ = 0U;
    if (kind == PageKind::Camera && memory.read_u32_le(native_pc_layout::kCameraMarksLive) == 0U) {
        queue_camera_marks(memory, true, false);
        run_queue(state, memory);
        return;
    }
    if (memory.read_u32_le(kCurrentPhase) != 5U) {
        finish_sequence(state);
        return;
    }
    if (kind == PageKind::Settings) {
        // The page's list widget has no rows; its highlight (the list's two
        // transform nodes) is scaled to nothing so only the host page shows.
        for (const std::uint32_t slot : {0xD4U, 0xD8U}) {
            const std::uint32_t node = memory.read_u32_le(kListObject + slot);
            if (node != 0U && memory.contains(node, 16U)) {
                write_f32(memory, node + 8U, 0.0F);
                write_f32(memory, node + 12U, 0.0F);
            }
        }
        // The host drives this page; carry out what it asked for.
        const auto push = [this](const GuestCall& call) {
            if (queue_size_ < queue_.size()) {
                queue_[queue_size_++] = call;
            }
        };
        // The retail option pages refresh the audio object before each tick
        // (sound 75), and without it the game drops the tick: the refresh runs
        // whenever a sound is played, as on those pages.
        if (settings_apply_audio_ || !settings_sounds_.empty()) {
            settings_apply_audio_ = false;
            push(GuestCall{.target = kListObjectRefreshRoutine, .a0 = kListObjectRefresh, .a1 = 1U});
        }
        for (const std::uint32_t sound : settings_sounds_) {
            push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = sound});
        }
        settings_sounds_.clear();
        if (settings_back_) {
            settings_back_ = false;
            push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = 74U});
            push(GuestCall{.target = kFrontendMessageRoutine, .a0 = kFrontendObject, .a1 = 3U});
        }
        run_queue(state, memory);
        return;
    }
    if (kind == PageKind::Controls) {
        if (controls_capturing_) {
            const std::string_view prompt =
                controls_prompt_.empty()
                    ? std::string_view{"PRESS A CONTROL TO ASSIGN IT.\nESC: CANCEL   BACKSPACE: CLEAR"}
                    : std::string_view{controls_prompt_};
            if (!mods_popup_open_) {
                controls_capture_popup_ = true;
                controls_shown_prompt_ = prompt;
                open_mods_information(state, memory, prompt);
                return;
            }
            if (controls_capture_popup_ && prompt != controls_shown_prompt_) {
                controls_shown_prompt_ = prompt;
                queue_size_ = 0U;
                mods_popup_pages_ = information_pages(prompt);
                mods_popup_page_ = 0U;
                queue_mods_popup(memory, true);
                queue_[queue_size_++] =
                    GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound};
                run_queue(state, memory);
            } else {
                finish_sequence(state);
            }
            return;
        }
        if (mods_popup_open_) {
            if (controls_capture_popup_ || pad_action(memory, 68U) != 0U ||
                pad_action(memory, 92U) != 0U) {
                queue_mods_popup_close();
            }
            controls_capture_popup_ = false;
            run_queue(state, memory);
            return;
        }
        if (!controls_notice_.empty() && controls_notice_ != controls_shown_notice_) {
            controls_shown_notice_ = controls_notice_;
            open_mods_information(state, memory, controls_notice_);
            return;
        }
    }
    if (kind == PageKind::Mods) {
        if (mods_popup_open_) {
            if (pad_action(memory, 68U) != 0U || pad_action(memory, 92U) != 0U) {
                queue_mods_popup_close();
                queue_[queue_size_++] =
                    GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = 74U};
            } else if (!mods_popup_pages_.empty()) {
                auto next = mods_popup_page_;
                if (pad_action(memory, 20U) != 0U && next != 0U) {
                    --next;
                } else if (pad_action(memory, 32U) != 0U && next + 1U < mods_popup_pages_.size()) {
                    ++next;
                }
                if (mods_popup_page_ != next) {
                    mods_popup_page_ = next;
                    queue_mods_popup(memory, true);
                    queue_[queue_size_++] = GuestCall{
                        .target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound};
                }
            }
            run_queue(state, memory);
            return;
        }
        if (memory.read_u8(kPopupLiveFlag) != 0U) {
            finish_sequence(state);
            return;
        }
        if (mods_pending_notice_) {
            const auto notice = std::move(*mods_pending_notice_);
            mods_pending_notice_.reset();
            open_mods_information(state, memory, notice);
            return;
        }
    }
    update_step(state, memory, UpdatePhase::Up);
}

void Slus21050NativePcOptions::update_step(guest::R5900State& state, guest::EeMemory& memory,
                                           const UpdatePhase phase) {
    const std::uint32_t page = update_page_;
    const bool gameplay = page_kind_ == PageKind::Gameplay;
    const bool mods = page_kind_ == PageKind::Mods;
    const bool controls = page_kind_ == PageKind::Controls;
    const bool camera = page_kind_ == PageKind::Camera;
    const std::size_t row_count =
        gameplay ? 3U
        : controls ? std::min<std::size_t>(9U, controls_rows_.size())
        : mods ? mods_rows_ + 1U
        : camera ? 3U
                                         : 9U;
    const guest::GuestAddress values = gameplay ? native_pc_layout::kGameplayValues
                                       : controls ? native_pc_layout::kControlsValues
                                       : mods ? native_pc_layout::kModsValues
                                       : camera
                                           ? native_pc_layout::kCameraValues
                                           : native_pc_layout::kValues;
    const guest::GuestAddress dirty = gameplay ? native_pc_layout::kGameplayDirty
                                      : mods ? native_pc_layout::kModsDirty
                                      : camera
                                          ? native_pc_layout::kCameraDirty
                                          : native_pc_layout::kDirty;
    const auto item_of = [&page](const std::uint32_t row) { return row * 20U + page; };
    const auto push = [this](const GuestCall& call) { queue_[queue_size_++] = call; };
    queue_size_ = 0U;
    if (phase == UpdatePhase::Up &&
        ((controls && controls_stale_) || (mods && mods_values_stale_) ||
         (!gameplay && !mods && !camera && !controls &&
          video_values_stale_))) {
        mods_values_stale_ = false;
        video_values_stale_ = false;
        controls_stale_ = false;
        for (std::size_t row = 0U; row < row_count; ++row) {
            push(GuestCall{kListSetValueRoutine, kListObject, static_cast<std::uint32_t>(row),
                           memory.read_u32_le(item_of(static_cast<std::uint32_t>(row)) + 12U)});
        }
        run_queue(state, memory);
        return;
    }
    if (!gameplay && !mods && !camera && !controls) {
      if (phase == UpdatePhase::Up) {
        const auto row = memory.read_u16_le(page + 4U);
        if ((pad_action(memory, 44U) != 0U && row == 0U && video_window_ != 0U) ||
            (pad_action(memory, 56U) != 0U && row == 8U &&
             video_window_ + kNativeModsPageRowCount < kNativePcVideoRowCount)) {
            video_window_ = pad_action(memory, 44U) != 0U ? video_window_ - 1U : video_window_ + 1U;
            publish_video_rows(memory);
            video_values_stale_ = true;
            update_step(state, memory, UpdatePhase::Up);
            return;
        }
      }
    }
    if (phase == UpdatePhase::Up && controls) {
        const auto row = memory.read_u16_le(page + 4U);
        if ((pad_action(memory, 44U) != 0U && row == 0U && controls_window_ != 0U) ||
            (pad_action(memory, 56U) != 0U && static_cast<std::size_t>(row) + 1U == row_count &&
             controls_window_ + row_count < controls_rows_.size())) {
            controls_window_ =
                pad_action(memory, 44U) != 0U ? controls_window_ - 1U : controls_window_ + 1U;
            publish_controls_rows(memory);
            update_step(state, memory, UpdatePhase::Up);
            return;
        }
    }
    if (phase == UpdatePhase::Up && pad_action(memory, 44U) != 0U) {
        stage_ = Stage::UpResult;
        issue_call(state, GuestCall{.target = kListUpRoutine, .a0 = page + 4U});
        return;
    }
    if (phase != UpdatePhase::Side && pad_action(memory, 56U) != 0U) {
        stage_ = Stage::DownResult;
        issue_call(state, GuestCall{.target = kListDownRoutine, .a0 = page + 4U});
        return;
    }
    const bool left = pad_action(memory, 20U) != 0U;
    const bool right = !left && pad_action(memory, 32U) != 0U;
    if (controls) {
        const std::size_t row = controls_window_ + memory.read_u16_le(page + 4U);
        if (right || left || pad_action(memory, 68U) != 0U) {
            controls_action_ = std::pair<std::size_t, int>{row, left ? -1 : right ? 1 : 0};
            push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound});
        } else if (pad_action(memory, 92U) != 0U) {
            push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = 74U});
            push(GuestCall{
                .target = kFrontendMessageRoutine, .a0 = kFrontendObject, .a1 = 3U});
        }
        run_queue(state, memory);
        return;
    }
    const auto rescan_row = [&] {
        return mods && memory.read_u16_le(page + 4U) == static_cast<std::uint32_t>(mods_rows_);
    };
    if ((right || left) && rescan_row()) {
        finish_sequence(state);
        return;
    }
    if ((right || left) && camera && memory.read_u16_le(page + 4U) == 0U) {
            const std::uint32_t level = memory.read_u32_le(native_pc_layout::kCameraLevel);
            std::uint32_t next = level;
            if (level != 0U && left) {
                next = level - 1U;
            } else if (right && level + 1U <= 50U) {
                next = level + 1U;
            }
            if (next != level) {
                memory.write_u32_le(native_pc_layout::kCameraLevel, next);
                memory.write_u32_le(native_pc_layout::kCameraDirty, 1U);
                push(GuestCall{
                    .target = kListSetLevelRoutine,
                    .a0 = kListObject,
                    .a2 = ((next < 50U ? next : 50U) * 20U + 25U) / 50U});
                push(GuestCall{
                    .target = kListObjectRefreshRoutine, .a0 = kListObjectRefresh, .a1 = 1U});
                push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound});
                if (memory.read_u32_le(native_pc_layout::kCameraMarksLive) != 0U) {
                    queue_camera_marks(memory, false, true);
                }
            }
            run_queue(state, memory);
            return;
    }
    if (right || left) {
        const std::uint32_t row = memory.read_u16_le(page + 4U);
        if (row >= row_count) {
            finish_sequence(state);
            return;
        }
        const auto entry = item_of(row);
        const int count = static_cast<int>(memory.read_u8(entry + 20U));
        if (count == 0) {
            finish_sequence(state);
            return;
        }
        int value = static_cast<int>(memory.read_u32_le(entry + 12U)) + (left ? -1 : 1);
        if (value < 0) {
            value = count - 1;
        } else if (value >= count) {
            value = 0;
        }
        memory.write_u32_le(entry + 12U, static_cast<std::uint32_t>(value));
        const auto logical =
            !gameplay && !mods && !camera
                ? static_cast<std::uint32_t>(video_window_) + row
                : row;
        memory.write_u32_le(values + logical * 4U, static_cast<std::uint32_t>(value));
        memory.write_u32_le(dirty, 1U);
        if (row <= 1U && gameplay) {
            memory.write_u8(row + 0x004F507BU, static_cast<std::uint8_t>(value));
        }
        push(GuestCall{.target = kListSetValueRoutine,
                                          .a0 = kListObject,
                                          .a1 = row,
                                          .a2 = static_cast<std::uint32_t>(value)});
        if (!gameplay && !mods && !camera) {
            if (logical == 1U) {
                const auto& ids = string_pack().aa_value_ids[value];
                for (std::uint32_t i = 0U; i < 5U; ++i) {
                    memory.write_u32_le(video_tables_[2] + i * 4U, ids[i]);
                }
                if (video_window_ <= 2U) {
                    push(GuestCall{
                        .target = kListSetValueRoutine,
                        .a0 = kListObject,
                        .a1 = 2U - static_cast<std::uint32_t>(video_window_),
                        .a2 = memory.read_u32_le(native_pc_layout::kValues + 8U)});
                }
            }
        }
        push(GuestCall{
            .target = kListObjectRefreshRoutine, .a0 = kListObjectRefresh, .a1 = 1U});
        push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound});
    } else if (pad_action(memory, 68U) != 0U) {
        if (rescan_row()) {
            memory.write_u32_le(native_pc_layout::kModsRescan, 1U);
            push(GuestCall{
                .target = kListObjectRefreshRoutine, .a0 = kListObjectRefresh, .a1 = 1U});
            push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = kPopupSound});
        } else if (mods) {
            const auto row = mods_window_ + memory.read_u16_le(page + 4U);
            if (row < mods_information_.size()) {
                open_mods_information(state, memory, mods_information_[row]);
                return;
            }
        } else {
            push(GuestCall{
                .target = kFrontendMessageRoutine, .a0 = kFrontendObject, .a1 = 1U});
        }
    } else if (pad_action(memory, 92U) != 0U) {
        push(GuestCall{.target = kPlaySoundRoutine, .a0 = kSoundObject, .a1 = 74U});
        push(GuestCall{.target = kFrontendMessageRoutine, .a0 = kFrontendObject, .a1 = 3U});
    }
    run_queue(state, memory);
}

void Slus21050NativePcOptions::continue_sequence(guest::R5900State& state,
                                                 guest::EeMemory& memory) {
    switch (stage_) {
    case Stage::InitWrites: {
        const bool gameplay = page_kind_ == PageKind::Gameplay;
        const bool mods = page_kind_ == PageKind::Mods;
        const bool controls = page_kind_ == PageKind::Controls;
        const bool camera = page_kind_ == PageKind::Camera;
        const auto page = page_address(page_kind_);
        const bool settings = page_kind_ == PageKind::Settings;
        const std::size_t rows =
            settings ? 0U
            : gameplay ? 3U
            : controls ? std::min<std::size_t>(9U, controls_rows_.size())
            : mods ? mods_rows_ + 1U
            : camera ? 3U
                     : 9U;
        const guest::GuestAddress values = gameplay ? native_pc_layout::kGameplayValues
                                           : controls ? native_pc_layout::kControlsValues
                                           : mods ? native_pc_layout::kModsValues
                                           : camera ? native_pc_layout::kCameraValues
                                                    : native_pc_layout::kValues;
        write_u16(memory, page + 4U, 0U);
        write_u16(memory, page + 6U, static_cast<std::uint16_t>(rows));
        const auto item_of = [&page](const std::uint32_t row) { return row * 20U + page; };
        std::array<std::uint32_t, 9U> shown{};
        for (std::size_t row = 0U; row < rows; ++row) {
            const auto logical = !gameplay && !mods && !camera && !controls ? video_window_ + row : row;
            shown[row] = memory.read_u32_le(values + static_cast<std::uint32_t>(logical) * 4U);
            const auto item = item_of(static_cast<std::uint32_t>(row));
            if (row == 0U && camera) {
                memory.write_u8(item + 20U,
                                static_cast<std::uint8_t>((std::min(shown[row], 50U) * 20U + 25U) / 50U));
            } else {
                memory.write_u32_le(item + 12U, shown[row]);
            }
        }
        if (mods) {
            mods_values_stale_ = false;
        }
        const auto push = [this](const GuestCall& call) { queue_[queue_size_++] = call; };
        queue_size_ = 0U;
        push(GuestCall{kListInitRoutine, kListObject, page + 4U, page + 8U});
        for (std::size_t row = 0U; row < rows; ++row) {
            const bool slider = row == 0U && camera;
            push(GuestCall{slider ? kListSetLevelRoutine : kListSetValueRoutine, kListObject,
                           static_cast<std::uint32_t>(row),
                           slider ? (std::min(static_cast<std::uint32_t>(shown[row]), 50U) * 20U + 25U) / 50U : shown[row]});
        }
        const bool pause = init_pause_context_;
        push(GuestCall{kTitleRoutine, kTitleObject,
                       settings ? native_pc_retail::kSettingsStringId
                       : gameplay ? string_pack().gameplay_title_id
                       : controls ? string_pack().controls_title_id
                       : mods ? string_pack().mods_title_id
                       : camera ? string_pack().camera_title_id
                                : string_pack().title_id,
                       pause ? 5U : 14U});
        push(GuestCall{kHeaderRoutine, kHeaderObject, pause ? 7U : 0U});
        push(GuestCall{kFooterRoutine, kFooterObject, pause ? 394U : 266U, 15U});
        push(GuestCall{kPageDrawRoutine, kPageDrawObject});
        if (camera) {
            queue_camera_marks_free(memory);
        }
        run_queue(state, memory);
        break;
    }
    case Stage::Queue:
        if (queue_next_ < queue_size_) {
            issue_call(state, resolved(memory, queue_[queue_next_++]));
        } else {
            finish_sequence(state);
        }
        break;
    case Stage::UpResult:
        if (state.read_gpr_u32(2U) != 0U) {
            finish_sequence(state);
        } else {
            update_step(state, memory, UpdatePhase::Down);
        }
        break;
    case Stage::DownResult:
        if (state.read_gpr_u32(2U) != 0U) {
            finish_sequence(state);
        } else {
            update_step(state, memory, UpdatePhase::Side);
        }
        break;
    case Stage::Idle:
        break;
    }
}

std::uint32_t Slus21050NativePcOptions::pause_item_selected(const guest::EeMemory& memory) const {
    if (!memory.contains(native_pc_retail::kPauseList, 88U)) {
        return 0U;
    }
    const std::uint32_t selected = memory.read_u16_le(native_pc_retail::kPauseList + 4U);
    if (selected > 9U) {
        return 0U;
    }
    const auto item = memory.read_u32_le(native_pc_layout::kPauseItems + selected * 8U);
    const auto strings = memory.read_u32_le(memory.read_u32_le(native_pc_retail::kStringTablePointer) + 12U);
    return item == memory.read_u32_le(strings + native_pc_retail::kSettingsStringId * 4U)
               ? native_pc_retail::kSettingsDetail
               : 0U;
}

Slus21050NativePcOptions::HookResult
Slus21050NativePcOptions::extend_pause_list(guest::R5900State& state, guest::EeMemory& memory) {
    const auto item_of = [](const std::uint32_t index) { return native_pc_layout::kPauseItems + index * 8U; };
    const auto retail_item_of = [](const std::uint32_t index) { return native_pc_retail::kPauseList + index * 8U; };
    const auto retail_action_of = [](const std::uint32_t index) { return native_pc_retail::kPauseList + index * 4U; };
    const std::uint32_t list = state.read_gpr_u32(16U);
    pause_quit_from_ = 0xFFFFFFFFU;
    pause_quit_to_ = 0xFFFFFFFFU;
    if (list == native_pc_retail::kPauseList &&
        memory.contains(native_pc_retail::kPauseList, 97U)) {
        const auto flag = memory.read_u8(native_pc_retail::kPauseList + 96U);
        const std::uint32_t retail_count = memory.read_u16_le(native_pc_retail::kPauseList + 6U);
        if (retail_count > 7U) {
            return HookResult::NotHooked;
        }
        if (retail_count == 0U) {
            return HookResult::NotHooked;
        }
        // The retail rows, with AUDIO and CONTROLLERS replaced by one SETTINGS
        // row where AUDIO was; the last row (quit) stays last.
        const auto strings = memory.read_u32_le(memory.read_u32_le(native_pc_retail::kStringTablePointer) + 12U);
        const auto retail_text = [&](const std::uint32_t id) { return memory.read_u32_le(strings + id * 4U); };
        const std::uint32_t audio_text = retail_text(native_pc_retail::kAudioPauseStringId);
        const std::uint32_t controllers_text = retail_text(native_pc_retail::kControllersStringId);
        const std::uint32_t settings_text = retail_text(native_pc_retail::kSettingsStringId);
        std::uint32_t count = 0U;
        bool settings_added = false;
        const auto copy_row = [&](const std::uint32_t row) {
            for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
                memory.write_u8(byte + item_of(count), memory.read_u8(retail_item_of(row) + byte + 8U));
            }
            memory.write_u32_le(native_pc_layout::kPauseActions + count * 4U,
                                flag == 0U ? memory.read_u32_le(retail_action_of(row) + 68U) : 0U);
            ++count;
        };
        const auto add_settings = [&] {
            const auto item = item_of(count);
            memory.write_u32_le(item, settings_text);
            memory.write_u8(item + 4U, 0U);
            memory.write_u8(item + 5U, 0U);
            write_u16(memory, item + 6U, 0U);
            memory.write_u32_le(native_pc_layout::kPauseActions + count * 4U, flag == 0U ? 6U : 0U);
            settings_added = true;
            ++count;
        };
        for (std::uint32_t row = 0U; row + 1U < retail_count; ++row) {
            const auto text = memory.read_u32_le(retail_item_of(row) + 8U);
            if (text == audio_text || text == controllers_text) {
                if (!settings_added) {
                    add_settings();
                }
                continue;
            }
            copy_row(row);
        }
        if (!settings_added) {
            add_settings();
        }
        if (retail_count - 1U != count) {
            pause_quit_from_ = retail_count - 1U;
            pause_quit_to_ = count;
        }
        copy_row(retail_count - 1U);
        write_u16(memory, native_pc_retail::kPauseList + 6U, static_cast<std::uint16_t>(count));
    }
    state.write_gpr_u64(2U, 0x004F0000U);
    state.set_pc(native_pc_retail::kPauseListEnd + 4U);
    return HookResult::Replaced;
}

Slus21050NativePcOptions::HookResult
Slus21050NativePcOptions::open_from_pause(guest::R5900State& state, guest::EeMemory& memory) {
    if (state.pc() == native_pc_retail::kPauseActionSwitch) {
        const std::uint16_t selected = memory.read_u16_le(native_pc_retail::kPauseList + 4U);
        if (selected > 9U) {
            return HookResult::NotHooked;
        }
        state.write_gpr_u64(3U, memory.read_u32_le(native_pc_layout::kPauseActions + selected * 4));
    }
    if (pause_item_selected(memory) != 0U) {
        state.set_pc(native_pc_retail::kPauseActionSwitch + 0xD8U);
        return HookResult::Replaced;
    }
    if (state.pc() == native_pc_retail::kPauseIndexSwitch) {
        if (pause_quit_to_ != 0xFFFFFFFFU && pause_quit_to_ == state.read_gpr_u32(4U)) {
            state.write_gpr_u64(4U, pause_quit_from_);
            state.write_gpr_u64(3U, 3U);
            state.set_pc(native_pc_retail::kPauseIndexSwitch + 4U);
            return HookResult::Replaced;
        }
    }
    return HookResult::NotHooked;
}

Slus21050NativePcOptions::HookResult
Slus21050NativePcOptions::pause_open_call(guest::R5900State& state, guest::EeMemory& memory) {
    const auto selected = pause_item_selected(memory);
    if (selected == 0U) {
        return HookResult::NotHooked;
    }
    const auto target = state.read_gpr_u32(25U);
    state.write_gpr_u64(6U, selected);
    state.write_gpr_u64(31U, state.pc() + 8U);
    state.set_pc(target);
    return HookResult::Replaced;
}

Slus21050NativePcOptions::HookResult
Slus21050NativePcOptions::on_dispatch(guest::R5900State& state, guest::EeMemory& memory) {
    if (!installed_ || state.is_executing_delay_slot()) {
        return HookResult::NotHooked;
    }
    const auto text_address = [](const std::uint32_t id) {
        return native_pc_layout::kStrings + string_pack().offsets[id - native_pc_retail::kStringCount];
    };
    const auto menu_action_of = [](const std::uint32_t index) { return native_pc_retail::kMainMenu + index * 4U; };
    switch (state.pc()) {
    case native_pc_retail::kFrontendDraw:
        return lobby_frontend_work(state, memory);
    case native_pc_layout::kLobbyCallReturn:
        if (lobby_stage_ == LobbyStage::Idle) {
            return HookResult::NotHooked;
        }
        continue_lobby_work(state, memory);
        return HookResult::Replaced;
    case native_pc_retail::kGameplayInit: {
        const auto return_address = state.read_gpr_u32(31U);
        const auto context = state.read_gpr_u32(17U);
        const auto pause = memory.contains(context + 80U, 1U) && memory.read_u8(context + 80U) != 0U;
        const auto strings = memory.read_u32_le(memory.read_u32_le(native_pc_retail::kStringTablePointer) + 12U);
        const auto& pack = string_pack();
        for (std::uint32_t row = 0U; row < 3U; ++row) {
            const auto item = row * 20U + native_pc_layout::kGameplayPage + 8U;
            memory.write_u32_le(item, memory.read_u32_le(strings + (row != 2U ? row + 2451U : pack.fps_label_id) * 4U));
            memory.write_u32_le(item + 4U, row != 2U
                                               ? static_cast<std::uint32_t>(memory.read_u8(5197947U + row) != 0U)
                                               : memory.read_u32_le(native_pc_layout::kGameplayValues + 8U));
            const auto table = row != 2U ? state.read_gpr_u32(28U) - (row == 0U ? 24600U : 24592U)
                                             : native_pc_layout::kCameraCorrectionTable;
            memory.write_u32_le(item + 8U, table);
            memory.write_u8(item + 12U, 2U);
            memory.write_u8(item + 13U, 4U);
            memory.write_u16_le(item + 14U, 0U);
            memory.write_u32_le(item + 16U, 0U);
            memory.write_u32_le(native_pc_layout::kGameplayValues + row * 4U, memory.read_u32_le(item + 4U));
        }
        begin_page_init(state, pause, PageKind::Gameplay);
        finish_pc_ = return_address;
        return HookResult::Replaced;
    }
    case native_pc_layout::kGameplayUpdate:
        begin_page_update(state, memory, PageKind::Gameplay);
        return HookResult::Replaced;
    case native_pc_retail::kPauseItemsArgument:
        state.write_gpr_u64(6U, native_pc_layout::kPauseItems);
        state.write_gpr_u64(31U, native_pc_retail::kPauseItemsArgument + 8U);
        state.set_pc(0x00321470U);
        return HookResult::Replaced;
    case 0x0032C550U: {
        if (mods_popup_open_ || lobby_popup_open_) {
            if (state.read_gpr_u32(4U) == 0x01E898E8U) {
                if (state.read_gpr_u32(31U) == 0x0032CD38U ||
                    state.read_gpr_u32(31U) == 0x0032CEECU) {
                    state.write_gpr_u64(5U, lobby_popup_open_ ? native_pc_layout::kLobbyPopupPosition
                                                              : native_pc_layout::kModsPopupPosition);
                    state.write_gpr_u64(6U, lobby_popup_open_ ? native_pc_layout::kLobbyPopupSize
                                                              : native_pc_layout::kModsPopupSize);
                    state.write_gpr_u64(7U, lobby_popup_open_ ? native_pc_layout::kLobbyPopupCentre
                                                              : native_pc_layout::kModsPopupCentre);
                    write_f32(memory, 0x01E89958U, lobby_popup_open_ ? 18.0F : 20.0F);
                }
            }
        }
        return HookResult::NotHooked;
    }
    case native_pc_retail::kControllersInit: {
        const auto return_address = state.read_gpr_u32(31U);
        const bool pause = state.read_gpr_u32(5U) != 0U;
        controls_window_ = 0U;
        publish_controls_rows(memory);
        begin_page_init(state, pause, PageKind::Controls);
        finish_pc_ = return_address;
        return HookResult::Replaced;
    }
    case native_pc_layout::kControlsUpdate:
        begin_page_update(state, memory, PageKind::Controls);
        return HookResult::Replaced;
    case native_pc_layout::kControlsCleanup:
        if (stage_ != Stage::Idle) {
            return HookResult::NotHooked;
        }
        finish_pc_ = state.read_gpr_u32(31U);
        finish_v0_ = 0U;
        queue_size_ = 0U;
        controls_capturing_ = false;
        queue_mods_popup_close();
        run_queue(state, memory);
        return HookResult::Replaced;
    case native_pc_retail::kVideoInit: {
        const auto return_address = state.read_gpr_u32(31U);
        const auto context = state.read_gpr_u32(17U);
        const auto pause = memory.contains(context + 80U, 1U) && memory.read_u8(context + 80U) != 0U;
        video_window_ = 0U;
        publish_video_rows(memory);
        begin_page_init(state, pause, PageKind::Pc);
        finish_pc_ = return_address;
        return HookResult::Replaced;
    }
    case native_pc_retail::kMainMenuCountSite: {
        const auto menu = state.read_gpr_u32(16U);
        main_menu_extended_ = menu == native_pc_retail::kMainMenu;
        if (menu == native_pc_retail::kMainMenu) {
            state.write_gpr_u64(3U, 6U);
            state.set_pc(native_pc_retail::kMainMenuCountSite + 4U);
        } else {
            return HookResult::NotHooked;
        }
        return HookResult::Replaced;
    }
    case native_pc_retail::kMainMenuItemsArgument: {
        const auto menu = state.read_gpr_u32(16U);
        if (!main_menu_extended_ || menu != native_pc_retail::kMainMenu) {
            return HookResult::NotHooked;
        }
        for (std::uint32_t byte = 0U; byte < 40U; byte += 4U) {
            memory.write_u32_le(native_pc_layout::kMainMenuItems + byte,
                                memory.read_u32_le(native_pc_retail::kMainMenu + 24U + byte));
        }
        memory.write_u32_le(native_pc_layout::kMainMenuItems + 40U, text_address(string_pack().quit_label_id));
        memory.write_u32_le(native_pc_layout::kMainMenuItems + 44U, 0U);
        state.write_gpr_u64(6U, native_pc_layout::kMainMenuItems);
        state.set_pc(native_pc_retail::kMainMenuItemsArgument + 4U);
        return HookResult::Replaced;
    }
    case native_pc_retail::kMainMenuDescriptionsArgument: {
        const auto menu = state.read_gpr_u32(16U);
        if (!main_menu_extended_ || menu != native_pc_retail::kMainMenu) {
            return HookResult::NotHooked;
        }
        for (auto row = 0U; row < 5U; ++row) {
            memory.write_u32_le(native_pc_layout::kMainMenuDescriptions + row * 4U,
                                memory.read_u32_le(menu_action_of(row) + 64U));
        }
        memory.write_u32_le(native_pc_layout::kMainMenuDescriptions + 20U,
                            text_address(string_pack().quit_description_id));
        state.write_gpr_u64(6U, native_pc_layout::kMainMenuDescriptions);
        state.set_pc(native_pc_retail::kMainMenuDescriptionsArgument + 4U);
        return HookResult::Replaced;
    }
    case native_pc_retail::kMainMenuConfirmSite: {
        const auto menu = state.read_gpr_u32(16U);
        if (!main_menu_extended_ || menu != native_pc_retail::kMainMenu) {
            return HookResult::NotHooked;
        }
        if (read_u16(memory, native_pc_retail::kMainMenu + 20U) != 5U) {
            return HookResult::NotHooked;
        }
        quit_requested_ = true;
        state.write_gpr_u64(2U, 1U);
        state.set_pc(native_pc_retail::kMainMenuUpdateReturn);
        return HookResult::Replaced;
    }
    case native_pc_retail::kHubCountSite:
        // SETTINGS, EA GAMES TRAX and (with mods) MODS.
        hub_items_ = mods_page_enabled_ ? 3U : 2U;
        state.write_gpr_u64(2U, hub_items_);
        state.set_pc(native_pc_retail::kHubCountSite + 4U);
        return HookResult::Replaced;
    case native_pc_retail::kHubItemsArgument: {
        const auto hub = state.read_gpr_u32(16U);
        const std::uint32_t items = hub_items_;
        hub_items_ = 0U;
        hub_rows_ = 0U;
        if (items == 0U || hub != native_pc_retail::kHub || !memory.contains(native_pc_retail::kHub + 8U, 40U)) {
            return HookResult::NotHooked;
        }
        const auto strings = memory.read_u32_le(memory.read_u32_le(native_pc_retail::kStringTablePointer) + 12U);
        const auto append = [&memory](const std::uint32_t index, const std::uint32_t text) {
            const guest::GuestAddress item = native_pc_layout::kHubItems + index * 8U;
            memory.write_u32_le(item, text);
            memory.write_u8(item + 4U, 0U);
            memory.write_u8(item + 5U, 0U);
            write_u16(memory, item + 6U, 0U);
        };
        append(0U, memory.read_u32_le(strings + native_pc_retail::kSettingsStringId * 4U));
        // EA GAMES TRAX: the retail row as the hub built it.
        for (std::uint32_t byte = 0U; byte < 8U; byte += 4U) {
            memory.write_u32_le(native_pc_layout::kHubItems + 8U + byte,
                                memory.read_u32_le(native_pc_retail::kHub + 8U + 4U * 8U + byte));
        }
        if (items == 3U) {
            const auto& pack = string_pack();
            append(2U, native_pc_layout::kStrings + pack.offsets[pack.mods_title_id - native_pc_retail::kStringCount]);
        }
        hub_rows_ = items;
        state.write_gpr_u64(6U, native_pc_layout::kHubItems);
        state.set_pc(native_pc_retail::kHubWidgetInitCall);
        return HookResult::Replaced;
    }
    case native_pc_retail::kHubSelectSite: {
        // The selected row of the SETTINGS hub, as the retail row whose table
        // entry opens it (EA GAMES TRAX keeps its own row 4).
        const std::uint32_t row = state.read_gpr_u32(3U);
        if (hub_rows_ == 0U || read_u16(memory, native_pc_retail::kHub + 6U) != hub_rows_ || row >= hub_rows_) {
            return HookResult::NotHooked;
        }
        static constexpr std::array<std::uint32_t, 3> kRetailRow{7U, 4U, 6U};
        state.write_gpr_u64(3U, kRetailRow[row]);
        state.write_gpr_u64(2U, 4U);
        state.set_pc(native_pc_retail::kHubSelectSite + 4U);
        return HookResult::Replaced;
    }
    case native_pc_retail::kDetailSwitchSite: {
        const auto detail = state.read_gpr_u32(8U);
        PageKind kind;
        if (detail == native_pc_retail::kSettingsDetail) {
            kind = PageKind::Settings;
        } else if (detail == 8U) {
            kind = PageKind::Gameplay;
        } else if (detail == 4U) {
            kind = PageKind::Pc;
        } else if (detail == 5U && mods_page_enabled_) {
            kind = PageKind::Mods;
        } else if (detail == 6U) {
            kind = PageKind::Camera;
        } else {
            return HookResult::NotHooked;
        }
        const auto context = state.read_gpr_u32(17U);
        const auto pause = memory.contains(context + 80U, 1U) && memory.read_u8(context + 80U) != 0U;
        if (kind == PageKind::Gameplay) {
            state.write_gpr_u64(31U, native_pc_retail::kDetailSwitchEnd);
            state.set_pc(native_pc_retail::kGameplayInit);
            return on_dispatch(state, memory);
        }
        begin_page_init(state, pause, kind);
        return HookResult::Replaced;
    }
    case native_pc_retail::kPageHeaderLookup: {
        const auto page = memory.read_u32_le(native_pc_retail::kCurrentPage);
        if (page != native_pc_layout::kGameplayPage && page != native_pc_layout::kControlsPage &&
            page != native_pc_layout::kSettingsPage) {
            if (page != native_pc_layout::kPage && page != native_pc_layout::kCameraPage) {
                if (!mods_page_enabled_ || page != native_pc_layout::kModsPage) {
                    return HookResult::NotHooked;
                }
            }
        }
        state.write_gpr_u64(2U, native_pc_retail::kHeaderDriverDetails);
        state.set_pc(state.read_gpr_u32(31U));
        return HookResult::Replaced;
    }
    case native_pc_layout::kUpdateEntry:
        begin_page_update(state, memory, PageKind::Pc);
        return HookResult::Replaced;
    case native_pc_layout::kSettingsUpdate:
        begin_page_update(state, memory, PageKind::Settings);
        return HookResult::Replaced;
    case native_pc_layout::kCameraUpdateEntry:
        begin_page_update(state, memory, PageKind::Camera);
        return HookResult::Replaced;
    case native_pc_layout::kCameraCleanupEntry:
        if (stage_ != Stage::Idle) {
            return HookResult::NotHooked;
        }
        page_kind_ = PageKind::Camera;
        finish_pc_ = state.read_gpr_u32(31U);
        finish_v0_ = 0U;
        queue_size_ = 0U;
        queue_camera_marks_free(memory);
        run_queue(state, memory);
        return HookResult::Replaced;
    case native_pc_layout::kModsUpdateEntry:
        if (!mods_page_enabled_) {
            return HookResult::NotHooked;
        }
        begin_page_update(state, memory, PageKind::Mods);
        return HookResult::Replaced;
    case native_pc_layout::kModsCleanupEntry:
        if (stage_ != Stage::Idle) {
            return HookResult::NotHooked;
        }
        page_kind_ = PageKind::Mods;
        finish_pc_ = state.read_gpr_u32(31U);
        finish_v0_ = 0U;
        queue_size_ = 0U;
        queue_mods_popup_close();
        mods_pending_notice_.reset();
        run_queue(state, memory);
        return HookResult::Replaced;
    case native_pc_layout::kCallReturn:
        if (stage_ == Stage::Idle) {
            return HookResult::NotHooked;
        }
        if (pending_result_to_ != 0U) {
            memory.write_u32_le(pending_result_to_, state.read_gpr_u32(2U));
            pending_result_to_ = 0U;
        }
        continue_sequence(state, memory);
        return HookResult::Replaced;
    case native_pc_retail::kPauseListEnd:
        return extend_pause_list(state, memory);
    case native_pc_retail::kPauseActionSwitch:
    case native_pc_retail::kPauseIndexSwitch:
        return open_from_pause(state, memory);
    case native_pc_retail::kPauseOpenCall:
    case native_pc_retail::kPauseOpenFinish:
        return pause_open_call(state, memory);
    default:
        return HookResult::NotHooked;
    }
}

void Slus21050NativePcOptions::publish_controls_rows(guest::EeMemory& memory) {
    if (controls_rows_.empty()) {
        return;
    }
    const StringPack& pack = string_pack();
    const auto write = [&memory](const std::uint32_t at, std::string_view text) {
        for (std::size_t i = 0U; i < 79U; ++i) {
            const int ch = i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
            write_u16(memory, at + static_cast<std::uint32_t>(i) * 2U,
                      static_cast<std::uint16_t>(ch));
        }
        write_u16(memory, at + 158U, 0U);
    };
    for (std::size_t row = 0U; row < 9U && controls_window_ + row < controls_rows_.size(); ++row) {
        const auto label =
            native_pc_layout::kControlsLabels + static_cast<std::uint32_t>(row) * 160U;
        const auto value =
            native_pc_layout::kControlsText + static_cast<std::uint32_t>(row) * 160U;
        const auto table =
            native_pc_layout::kControlsTables + static_cast<std::uint32_t>(row) * 4U;
        const auto item =
            native_pc_layout::kControlsPage + 8U + static_cast<std::uint32_t>(row) * 20U;
        write(label, controls_rows_[controls_window_ + row].first);
        write(value, controls_rows_[controls_window_ + row].second);
        memory.write_u32_le(native_pc_layout::kStringArray + pack.mods_label_ids[row] * 4U, value);
        memory.write_u32_le(table, pack.mods_label_ids[row]);
        memory.write_u32_le(item, label);
        memory.write_u32_le(item + 4U, 0U);
        memory.write_u32_le(item + 8U, table);
        memory.write_u32_le(item + 12U, 1025U);
        memory.write_u32_le(item + 16U, 0U);
        memory.write_u32_le(native_pc_layout::kControlsValues + static_cast<std::uint32_t>(row) * 4U, 0U);
    }
    controls_stale_ = true;
}

void Slus21050NativePcOptions::service_controls(
    guest::EeMemory& memory, const std::vector<std::pair<std::string, std::string>>& rows,
    bool capturing, std::string_view notice, std::string_view prompt) {
    controls_capturing_ = capturing;
    controls_notice_ = notice;
    controls_prompt_ = prompt;
    if (rows != controls_rows_) {
        controls_rows_ = rows;
        if (installed_) {
            publish_controls_rows(memory);
        }
    }
    if (installed_) {
        for (std::size_t row = 0U; row < 9U; ++row) {
            memory.write_u32_le(native_pc_layout::kStringArray + string_pack().mods_label_ids[row] * 4U,
                                native_pc_layout::kControlsText + static_cast<std::uint32_t>(row) * 160U);
        }
    }
}

std::optional<std::pair<std::size_t, int>> Slus21050NativePcOptions::consume_controls_action() {
    return std::exchange(controls_action_, std::nullopt);
}
}
