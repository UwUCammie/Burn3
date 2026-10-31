#include "takedown/runtime/slus_21050_settings_page.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_native_pc_options.hpp"
#include "takedown/ui/controls_editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace takedown::runtime {

namespace S = slus_21050_settings;
using ui::SettingsRow;
using ui::SettingsRowKind;
using ui::SettingsTab;

namespace {

// Row ids: the tab's base plus the row.
constexpr std::uint32_t kEffectsRow = 1U;
constexpr std::uint32_t kRadioRow = 2U;
constexpr std::uint32_t kDjRow = 3U;
constexpr std::uint32_t kOutputRow = 4U;
constexpr std::uint32_t kFovRow = 10U;
constexpr std::uint32_t kFovCorrectionRow = 11U;
constexpr std::uint32_t kPaniniRow = 12U;
constexpr std::uint32_t kTakedownCameraRow = 20U;
constexpr std::uint32_t kImpactTimeRow = 21U;
constexpr std::uint32_t kFpsCounterRow = 22U;
constexpr std::uint32_t kVideoRowBase = 100U;
constexpr std::uint32_t kControlsRowBase = 300U;
// Anti-aliasing: the type (Off, SMAA, FSR), then the chosen type's own rows.
constexpr std::uint32_t kAaTypeRow = 200U;
constexpr std::uint32_t kSmaaRow = 201U;
constexpr std::uint32_t kFsrModeRow = 202U;
constexpr std::uint32_t kSectionBase = 0xFFFF0000U;

// Retail sounds (the frontend sound bank).
constexpr std::uint32_t kChangeSound = 75U;

constexpr std::int16_t kStickThreshold = 16000;

SettingsRow section(const std::uint32_t id, std::string label) {
    SettingsRow row;
    row.id = kSectionBase + id;
    row.kind = SettingsRowKind::Section;
    row.label = std::move(label);
    return row;
}

SettingsRow toggle(const std::uint32_t id, std::string label, const bool on, std::string description) {
    SettingsRow row;
    row.id = id;
    row.kind = SettingsRowKind::Toggle;
    row.label = std::move(label);
    row.values = {"Off", "On"};
    row.value = on ? 1U : 0U;
    row.description = std::move(description);
    return row;
}

SettingsRow choice(const std::uint32_t id, std::string label, std::vector<std::string> values, const std::size_t value,
                   std::string description) {
    SettingsRow row;
    row.id = id;
    row.kind = SettingsRowKind::Choice;
    row.label = std::move(label);
    row.values = std::move(values);
    row.value = std::min(value, row.values.empty() ? 0U : row.values.size() - 1U);
    row.description = std::move(description);
    return row;
}

SettingsRow slider(const std::uint32_t id, std::string label, const std::size_t levels, const std::size_t value,
                   std::string text, std::string description) {
    SettingsRow row;
    row.id = id;
    row.kind = SettingsRowKind::Slider;
    row.label = std::move(label);
    row.levels = levels;
    row.value = std::min(value, levels - 1U);
    row.level_text = std::move(text);
    row.description = std::move(description);
    return row;
}

std::size_t volume_level(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return std::min<std::size_t>(memory.read_u8(address) / S::kVolumeStep, S::kVolumeLevels - 1U);
}

// The PC video rows (native_pc_option_rows order) grouped under headings.
struct VideoGroup {
    const char* heading;
    std::vector<std::size_t> rows;
};
const std::array<VideoGroup, 4>& video_groups() {
    static const std::array<VideoGroup, 4> groups{{
        {"DISPLAY", {10U, 0U, 11U, 9U}},
        {"RENDERING", {1U, 2U, 12U, 13U}},
        {"QUALITY", {5U, 6U, 7U, 8U}},
        {"FRAME RATE", {3U, 4U}},
    }};
    return groups;
}

const char* video_description(const std::size_t row) {
    switch (row) {
    case 0: return "Shows more of the world to the sides on a wide screen.";
    case 1: return "Draws the game at a multiple of its original resolution.";
    case 2: return "x1 smooths edges in the finished picture. x2 smooths every sample before they are "
                   "combined (renders at 200% at least).";
    case 3: return "The highest frame rate in races.";
    case 4: return "The highest frame rate in menus and videos.";
    case 5: return "How far away scenery and traffic are drawn.";
    case 6: return "The detail of cars and scenery at a distance.";
    case 7: return "The game's blur at high speed.";
    case 8: return "The original console's colour dithering.";
    case 9: return "Draws menus and the race HUD at the window's resolution.";
    case 10: return "How the game window fills the screen.";
    case 11: return "Waits for the display before showing each frame.";
    case 12: return "How much smaller FSR draws the game before upscaling it to the window.";
    case 13: return "How much FSR sharpens the picture.";
    default: return "";
    }
}

// The anti-aliasing type. FSR does its own anti-aliasing and the renderer
// leaves SMAA off while it is on, so FSR wins when both are set.
enum class AaType : std::uint8_t { Off, Smaa, Fsr };

AaType aa_type(const config::GraphicsSettings& settings) {
    if (config::fsr_preset(settings) != config::FsrPreset::Off) {
        return AaType::Fsr;
    }
    return settings.anti_aliasing != config::AntiAliasing::Off ? AaType::Smaa : AaType::Off;
}

// SMAA x1 and x2 are kept as the anti-aliasing setting's first two levels
// (the renderer reads them as SMAA; older settings files with the higher
// levels get x1).
std::size_t smaa_level(const config::GraphicsSettings& settings) {
    return settings.anti_aliasing == config::AntiAliasing::SpatialMedium ? 1U : 0U;
}

void set_aa_type(config::GraphicsSettings& settings, const AaType type) {
    switch (type) {
    case AaType::Off:
        settings.anti_aliasing = config::AntiAliasing::Off;
        config::set_fsr_preset(settings, config::FsrPreset::Off);
        break;
    case AaType::Smaa:
        config::set_fsr_preset(settings, config::FsrPreset::Off);
        if (settings.anti_aliasing == config::AntiAliasing::Off) {
            settings.anti_aliasing = config::AntiAliasing::SpatialLow;
        }
        break;
    case AaType::Fsr:
        if (config::fsr_preset(settings) == config::FsrPreset::Off) {
            config::set_fsr_preset(settings, config::FsrPreset::Quality);
        }
        break;
    }
}

bool is_toggle_row(const std::size_t row) { return row == 0U || row == 7U || row == 8U || row == 9U || row == 11U; }

} // namespace

ui::SettingsPageContent Slus21050SettingsPage::content(const guest::EeMemory& memory,
                                                       const config::GraphicsSettings& settings,
                                                       const SettingsPageControls& controls) {
    ui::SettingsPageContent page;
    auto& audio = page.tabs[static_cast<std::size_t>(SettingsTab::Audio)];
    const auto effects = volume_level(memory, S::kEffectsVolume);
    const auto radio = volume_level(memory, S::kRadioVolume);
    audio.push_back(slider(kEffectsRow, "EFFECTS VOLUME", S::kVolumeLevels, effects,
                           std::to_string(effects * S::kVolumeStep) + "%", "Engines, crashes and other sound effects."));
    audio.push_back(slider(kRadioRow, "RADIO VOLUME", S::kVolumeLevels, radio,
                           std::to_string(radio * S::kVolumeStep) + "%", "The music."));
    audio.push_back(toggle(kDjRow, "DJ", memory.read_u8(S::kDj) != 0U, "The radio DJ between songs."));
    audio.push_back(choice(kOutputRow, "AUDIO OUTPUT", {"Mono", "Stereo", "Dolby Pro Logic II"},
                           memory.read_u8(S::kAudioOutput), "How the sound is mixed for your speakers."));

    auto& camera = page.tabs[static_cast<std::size_t>(SettingsTab::Camera)];
    const auto fov = native_camera_fov_level(settings);
    camera.push_back(slider(kFovRow, "FIELD OF VIEW", kNativeCameraFovLevels, fov,
                            std::to_string(native_camera_fov_degrees(fov)),
                            "How much of the road the race camera shows (90 is the original)."));
    camera.push_back(toggle(kFovCorrectionRow, "FOV CORRECTION", settings.fov_correction,
                            "Keeps the vertical view the same on a wide screen."));
    camera.push_back(toggle(kPaniniRow, "PANINI PROJECTION", settings.panini,
                            "Less stretching at the edges of a wide field of view."));

    auto& controls_tab = page.tabs[static_cast<std::size_t>(SettingsTab::Controls)];
    for (std::size_t index = 0U; index < controls.rows.size(); ++index) {
        SettingsRow row;
        row.id = kControlsRowBase + static_cast<std::uint32_t>(index);
        row.kind = SettingsRowKind::Action;
        row.label = controls.rows[index].first;
        row.values = {controls.rows[index].second};
        const bool action = index >= ui::ControlsEditor::kFirstControlRow &&
                            index < ui::ControlsEditor::kFirstControlRow + platform::kPadControlCount;
        row.description = !controls.notice.empty() ? controls.notice
                          : action ? "Confirm adds a control to this action, or removes one that is already bound. "
                                     "An action can have several."
                          : index == ui::ControlsEditor::kPlayStation2LayoutRow
                              ? "Controller buttons as on PlayStation 2: Cross accelerates, Square brakes, R1 boosts, "
                                "R2 Crashbreaker. Keyboard bindings are kept."
                          : index == ui::ControlsEditor::kXboxLayoutRow
                              ? "Controller buttons as on Xbox: RT accelerates, LT brakes, A boosts, B Crashbreaker; "
                                "A confirms and B goes back in menus. Keyboard bindings are kept."
                              : std::string{};
        controls_tab.push_back(std::move(row));
    }
    if (controls.capturing) {
        page.popup = controls.prompt.empty() ? std::string{"PRESS A CONTROL TO ASSIGN IT.\nESC: CANCEL   BACKSPACE: CLEAR"}
                                             : controls.prompt;
    }

    auto& gameplay = page.tabs[static_cast<std::size_t>(SettingsTab::Gameplay)];
    gameplay.push_back(toggle(kTakedownCameraRow, "TAKEDOWN CAMERA", memory.read_u8(S::kTakedownCamera) != 0U,
                              "Shows each takedown from a crash camera."));
    gameplay.push_back(choice(kImpactTimeRow, "CRASH MODE IMPACT TIME", {"Manual", "Automatic"},
                              memory.read_u8(S::kImpactTime) != 0U ? 1U : 0U,
                              "Whether Impact Time starts by itself in Crash events."));
    gameplay.push_back(toggle(kFpsCounterRow, "FPS COUNTER", settings.fps_counter,
                              "Shows the frame rate in a corner of the screen."));

    auto& video = page.tabs[static_cast<std::size_t>(SettingsTab::Video)];
    const auto& rows = native_pc_option_rows();
    for (std::size_t group = 0U; group < video_groups().size(); ++group) {
        video.push_back(section(static_cast<std::uint32_t>(group), video_groups()[group].heading));
        for (const std::size_t r : video_groups()[group].rows) {
            if (r == 2U) {
                const AaType type = aa_type(settings);
                video.push_back(choice(kAaTypeRow, "ANTI-ALIASING", {"Off", "SMAA", "FSR"},
                                       static_cast<std::size_t>(type),
                                       "SMAA smooths jagged edges. AMD FSR also upscales and smooths edges over "
                                       "several frames."));
                if (type == AaType::Smaa) {
                    video.push_back(choice(kSmaaRow, "SMAA", {"x1", "x2"}, smaa_level(settings),
                                           video_description(2U)));
                }
                continue;
            }
            if (r == 12U || r == 13U) {
                if (aa_type(settings) != AaType::Fsr) {
                    continue;
                }
                if (r == 12U) {
                    std::vector<std::string> modes;
                    for (std::size_t i = 1U; i < rows[12].values.size(); ++i) {
                        std::string mode = rows[12].values[i];
                        if (mode.rfind("FSR ", 0) == 0) {
                            mode.erase(0, 4);
                        }
                        modes.push_back(std::move(mode));
                    }
                    video.push_back(choice(kFsrModeRow, "FSR MODE", std::move(modes),
                                           static_cast<std::size_t>(config::fsr_preset(settings)) - 1U,
                                           video_description(12U)));
                    continue;
                }
            }
            const std::uint32_t id = kVideoRowBase + static_cast<std::uint32_t>(r);
            const std::size_t value = native_pc_option_index(r, settings);
            std::string label = rows[r].label;
            if (r == 9U) {
                label = "HIGH-RESOLUTION UI";
            }
            if (is_toggle_row(r)) {
                video.push_back(toggle(id, std::move(label), value != 0U, video_description(r)));
            } else if (r == 13U) {
                SettingsRow row = slider(id, std::move(label), rows[r].values.size(), value, rows[r].values[value],
                                         video_description(r));
                video.push_back(std::move(row));
            } else {
                video.push_back(choice(id, std::move(label), rows[r].values, value, video_description(r)));
            }
        }
    }
    return page;
}

SettingsPageResult Slus21050SettingsPage::update(guest::EeMemory& memory, Slus21050NativePcOptions& page,
                                                 config::GraphicsSettings& settings,
                                                 const SettingsPageControls& controls,
                                                 const SettingsPageInput& input) {
    SettingsPageResult result;
    const bool active = page.settings_page_active(memory);
    if (active && !active_) {
        menu_.open(last_tab_);
        last_poll_.reset();
    }
    active_ = active;
    result.active = active;
    if (!active) {
        return result;
    }

    const auto& pad = input.pad;
    ui::SettingsInput in;
    in.up = pad.dpad_up || pad.left_stick_y < -kStickThreshold;
    in.down = pad.dpad_down || pad.left_stick_y > kStickThreshold;
    in.left = pad.dpad_left || pad.left_stick_x < -kStickThreshold;
    in.right = pad.dpad_right || pad.left_stick_x > kStickThreshold;
    in.confirm = pad.cross;
    in.back = pad.triangle;
    in.previous_tab = pad.left_shoulder;
    in.next_tab = pad.right_shoulder;
    if (input.pointer.has_value()) {
        const std::array<float, 2> p{(*input.pointer)[0] * platform::kParallelGsHostUiPictureWidth,
                                     (*input.pointer)[1] * platform::kParallelGsHostUiPictureHeight};
        in.pointer_moved = !last_pointer_.has_value() || *last_pointer_ != p;
        in.pointer = p;
    }
    last_pointer_ = in.pointer;
    const bool left_button = (input.mouse_buttons & 1U) != 0U;
    in.click = left_button && !left_button_held_ && in.pointer.has_value();
    in.click_held = left_button;
    left_button_held_ = left_button;
    in.wheel = input.wheel;
    // Real time since the previous poll: hold repeat and the opening animation
    // run at the same speed at any frame rate.
    const auto now = std::chrono::steady_clock::now();
    in.seconds = last_poll_.has_value()
                     ? std::clamp(std::chrono::duration<float>(now - *last_poll_).count(), 0.0f, 0.1f)
                     : input.seconds;
    last_poll_ = now;

    const ui::SettingsPageContent rows = content(memory, settings, controls);
    const ui::SettingsFrame frame = menu_.update(rows, in);
    last_tab_ = menu_.tab();

    bool audio_changed = false;
    for (const ui::SettingsEdit& edit : frame.edits) {
        const auto value = static_cast<std::uint8_t>(edit.value);
        switch (edit.id) {
        case kEffectsRow:
            memory.write_u8(S::kEffectsVolume, static_cast<std::uint8_t>(value * S::kVolumeStep));
            audio_changed = true;
            break;
        case kRadioRow:
            memory.write_u8(S::kRadioVolume, static_cast<std::uint8_t>(value * S::kVolumeStep));
            audio_changed = true;
            break;
        case kDjRow:
            memory.write_u8(S::kDj, value);
            audio_changed = true;
            break;
        case kOutputRow:
            memory.write_u8(S::kAudioOutput, value);
            audio_changed = true;
            break;
        case kTakedownCameraRow:
            memory.write_u8(S::kTakedownCamera, value);
            break;
        case kImpactTimeRow:
            memory.write_u8(S::kImpactTime, value);
            break;
        case kFpsCounterRow:
            settings.fps_counter = value != 0U;
            result.graphics_changed = true;
            break;
        case kFovRow:
            settings.field_of_view = native_camera_fov_from_level(static_cast<std::uint32_t>(edit.value));
            result.graphics_changed = true;
            break;
        case kFovCorrectionRow:
            settings.fov_correction = value != 0U;
            result.graphics_changed = true;
            break;
        case kPaniniRow:
            settings.panini = value != 0U;
            result.graphics_changed = true;
            break;
        case kAaTypeRow:
            set_aa_type(settings, static_cast<AaType>(std::min<std::uint8_t>(value, 2U)));
            result.graphics_changed = true;
            break;
        case kSmaaRow:
            settings.anti_aliasing = value != 0U ? config::AntiAliasing::SpatialMedium : config::AntiAliasing::SpatialLow;
            result.graphics_changed = true;
            break;
        case kFsrModeRow:
            config::set_fsr_preset(settings, static_cast<config::FsrPreset>(
                                                 std::min<std::uint32_t>(value + 1U, 5U)));
            result.graphics_changed = true;
            break;
        default:
            if (edit.id >= kControlsRowBase && edit.action) {
                result.controls_action = std::pair<std::size_t, int>{edit.id - kControlsRowBase, edit.direction};
            } else if (edit.id >= kVideoRowBase && edit.id < kVideoRowBase + kNativePcVideoRowCount) {
                result.graphics_changed |= native_pc_option_apply(edit.id - kVideoRowBase, edit.value, settings);
            }
            break;
        }
    }
    if (audio_changed) {
        page.settings_apply_audio();
    }
    switch (frame.sound) {
    case ui::SettingsSound::Change:
    case ui::SettingsSound::Tab:
    case ui::SettingsSound::Move: // the retail lists play the same tick when the focus moves
        page.settings_play_sound(kChangeSound);
        break;
    case ui::SettingsSound::None:
    case ui::SettingsSound::Back: // played with Back
    case ui::SettingsSound::Blocked:
        break;
    }
    if (frame.close) {
        page.settings_back();
    }
    return result;
}

platform::ParallelGsHostUi Slus21050SettingsPage::host_ui(const std::string& ui_directory) const {
    if (!active_) {
        return {};
    }
    return menu_.host_ui(ui_directory, ui_directory);
}

} // namespace takedown::runtime
