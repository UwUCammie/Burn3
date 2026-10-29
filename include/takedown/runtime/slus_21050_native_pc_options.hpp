#pragma once

// PC options inside the game's own menus (docs/NATIVE_PC_OPTIONS_MENU.md).
//
// The retail Options screen is a list of sub-pages (Gameplay, Audio, Video,
// Controllers, EA Games Trax), and the in-race pause list has Audio and
// Controllers. Native display settings extend the original VIDEO entry; CAMERA
// and MODS use the original list widgets. There is no PC OPTIONS category.
// Fonts, frame artwork, highlight, sounds and navigation belong to the game:
//
//   * a host-owned region of low EE RAM (unused by the game) holds the page
//     object (state and visible rows in the game's record layout), its vtable,
//     the value tables (string ids) and UTF-16 strings, as plain data;
//   * the game's string table is extended with those strings, so the game's
//     own text code draws them;
//   * the page's init and per-frame update are native C++ (on_dispatch). They
//     run at PCs the main loop hands to on_dispatch (a few interior PCs of
//     retail menu code and two PCs of the host region that hold no code). The
//     retail routines the page needs (list widget, title, sounds, messages)
//     are still the game's: the native code calls them by setting up the
//     argument registers and a return address that is itself a hooked PC, then
//     continues when the retail routine returns (a native call sequence);
//   * one retail data word (the hub's selection table) is patched; every code
//     word is left alone and only checked against its retail value;
//   * the page keeps its value indices in a mailbox word array; the host
//     reads them back each VBlank and applies them to config::GraphicsSettings.
//
// The same machinery serves a second page, MODS, when the host passes a mods
// state to service(): it is another entry of the Options hub (the module owns
// the whole hub extension, so both entries appear together) whose rows are the
// mods folder, each row's value cycling through "Disabled" and the load-order
// slots "1".."9", plus a REFRESH MOD LIST button. The mods page's own mailbox carries
// the player's edits back to the host's mod manager, which owns the files and
// the load order. Everything the MODS page shows is still the game's list page.
//
// Everything is checked against the retail words before anything is written,
// and a mismatch leaves the game untouched. The state is re-checked every few
// VBlanks because the game can reload its string table.

#include "takedown/config/app_settings.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/ui/host_prompt.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime {

// One row of the page: a label and the list of values the row cycles through.
struct NativePcOptionRow {
    std::string label;
    std::vector<std::string> values;
};

// Rows whose state Slus21050NativePcOptions keeps in its own members. Its
// layout is shared with release objects, so this stays 12; later rows keep
// their state in the module (slus_21050_native_pc_options.cpp).
inline constexpr std::size_t kNativePcOptionRowCount = 12U;
// UPSCALING (FSR 3.1 preset) and FSR SHARPNESS.
inline constexpr std::size_t kNativePcFsrRowCount = 2U;
inline constexpr std::size_t kNativePcVideoRowCount = kNativePcOptionRowCount + kNativePcFsrRowCount;

// The rows in display order: widescreen, render scale, anti-aliasing, frame
// rate limit, menu and video frame limit, render distance, model quality,
// motion blur, dithering, high-resolution UI, display mode, VSync, upscaling
// (FSR 3.1) and FSR sharpness. The original list widget shows nine rows at a
// time, with native scrolling for the complete list.
[[nodiscard]] const std::array<NativePcOptionRow, kNativePcVideoRowCount>&
native_pc_option_rows();
[[nodiscard]] std::string native_pc_aa_value_text(std::uint32_t render_scale, std::size_t index);
// Position of the current setting in the row's value list. A frame limit that
// is not in the list (edited by hand) shows as the nearest listed value.
[[nodiscard]] std::size_t native_pc_option_index(std::size_t row,
                                                 const config::GraphicsSettings& settings);
// Stores value `index` of `row` into `settings`; false when out of range.
[[nodiscard]] bool native_pc_option_apply(std::size_t row, std::size_t index,
                                          config::GraphicsSettings& settings);

// ---- CAMERA page -----------------------------------------------------------

// The CAMERA page's FIELD OF VIEW row is the game's own slider (the Audio
// page's volume bar, 20 cells). Each press steps 2 degrees of the FOV at full
// boost: level 0 is 40, level 25 (the middle) the game's own 90 and level 50
// is 140; the bar shows the level in proportion (native_camera_slider_cells).
// The angles are drawn by the game above the bar (native_camera_slider_marks).
inline constexpr std::uint32_t kNativeCameraFovLevels = 51U;
inline constexpr std::uint32_t kNativeCameraFovMinimum = 40U;
inline constexpr std::uint32_t kNativeCameraFovStep = 2U;
inline constexpr std::uint32_t kNativeCameraFovDefault = 90U;
inline constexpr std::uint32_t kNativeCameraSliderCells = 20U;
// The slider cells lit for a level (0 to 20, rounded).
[[nodiscard]] std::uint32_t native_camera_slider_cells(std::uint32_t level);

// Slider level for the setting (0 means the game's own FOV, level 10); a
// value between two steps shows as the nearest level.
[[nodiscard]] std::uint32_t native_camera_fov_level(const config::GraphicsSettings& settings);
// The setting for a slider level: 0 (the game's own) for the default level.
[[nodiscard]] std::uint32_t native_camera_fov_from_level(std::uint32_t level);
// The angle a level stands for, in degrees (90 for the game's own).
[[nodiscard]] std::uint32_t native_camera_fov_degrees(std::uint32_t level);
// One text mark of the CAMERA page's slider, drawn by the game in its menu
// font: the minimum, default and maximum angles over the start, middle and end
// of the bar, and the current angle above them at the knob. Positions are the menu's 640x448 space (the
// mark's centre), measured from the game's own slider.
struct NativeCameraSliderMark {
    std::string text;
    float x{};
    float y{};
    bool value{false}; // the angle (larger and brighter than the end marks)
};
inline constexpr std::size_t kNativeCameraSliderMarks = 4U;
[[nodiscard]] std::array<NativeCameraSliderMark, kNativeCameraSliderMarks>
native_camera_slider_marks(std::uint32_t level);

// ---- MODS page -------------------------------------------------------------

// One mod as the MODS page shows it.
struct NativeModsPageMod {
    std::string label;      // display name (mod.txt "name", else the folder name)
    bool enabled{false};
    std::uint32_t slot{0};  // 1-based load-order slot among the enabled mods; 0 when disabled
    std::string information; // plain metadata, presented by the original message box
};

// The game's list widget keeps nine rows; the last one is the REFRESH MOD
// LIST button. A longer mod list scrolls: the rows show a window of eight mods
// that follows the selection.
inline constexpr std::size_t kNativeModsPageMaximumMods = 8U;
inline constexpr std::size_t kNativeModsPageRowCount = kNativeModsPageMaximumMods + 1U;

struct NativeModsPageState {
    // Every known mod, in the manager's order. The page shows a window of
    // kNativeModsPageMaximumMods of them at a time.
    std::vector<NativeModsPageMod> mods;
    std::uint64_t notice_serial{};
    std::string notice;
};

// Load-order slots a mod row can show.
inline constexpr std::uint32_t kNativeModsPageMaximumSlot = 32U;
// Value text of a mod row: index 0 is "Disabled", index k (1 to
// kNativeModsPageMaximumSlot) is the load-order slot k. Used by the host to
// describe the same list.
[[nodiscard]] std::string_view native_mods_page_value_text(std::uint32_t index) noexcept;

// Guest memory map (all inside the reserved region).
namespace native_pc_layout {
inline constexpr guest::GuestAddress kRegion = 0x000C0000U;
inline constexpr guest::GuestAddress kRegionSize = 0x10000U;
inline constexpr guest::GuestAddress kMagic = kRegion + 0x00U;
inline constexpr guest::GuestAddress kVersion = kRegion + 0x04U;
inline constexpr guest::GuestAddress kDirty = kRegion + 0x10U;
inline constexpr guest::GuestAddress kModsDirty = kRegion + 0x14U;
inline constexpr guest::GuestAddress kModsRescan = kRegion + 0x18U;
inline constexpr guest::GuestAddress kCameraDirty = kRegion + 0x1CU;
inline constexpr guest::GuestAddress kValues = kRegion + 0x20U; // one u32 per row
inline constexpr guest::GuestAddress kModsValues = kRegion + 0x60U; // one u32 per mods row
// CAMERA page values, one u32 per row: the FOV slider level, then FOV
// CORRECTION (0 / 1).
inline constexpr guest::GuestAddress kCameraValues = kRegion + 0x90U;
inline constexpr guest::GuestAddress kCameraLevel = kCameraValues;
inline constexpr guest::GuestAddress kVtable = kRegion + 0x100U;
inline constexpr guest::GuestAddress kModsVtable = kRegion + 0x110U;
inline constexpr guest::GuestAddress kCameraVtable = kRegion + 0x120U;
inline constexpr guest::GuestAddress kPage = kRegion + 0x200U;
inline constexpr guest::GuestAddress kModsPage = kRegion + 0x300U;
// PCs that hold no code: the page update entries (in the vtables) and the
// return address of the native call sequence. The main loop hands them to
// Slus21050NativePcOptions::on_dispatch before anything is fetched there.
inline constexpr guest::GuestAddress kUpdateEntry = kRegion + 0x400U;
inline constexpr guest::GuestAddress kCallReturn = kRegion + 0x410U;
inline constexpr guest::GuestAddress kModsUpdateEntry = kRegion + 0x420U;
inline constexpr guest::GuestAddress kCameraUpdateEntry = kRegion + 0x430U;
// The CAMERA page's cleanup (vtable +12): frees its slider marks.
inline constexpr guest::GuestAddress kCameraCleanupEntry = kRegion + 0x440U;
inline constexpr guest::GuestAddress kModsCleanupEntry = kRegion + 0x450U;
// The Options hub's rows (its five retail rows, then PC OPTIONS and MODS), 8
// bytes each, rebuilt every time the hub opens.
inline constexpr guest::GuestAddress kHubItems = kRegion + 0x500U;
// The CAMERA page object and its row label (rewritten with the angle).
inline constexpr guest::GuestAddress kCameraPage = kRegion + 0x600U;
// The main menu's rows (its five retail rows, then QUIT GAME), 8 bytes each,
// and their descriptions (one string pointer each), rebuilt when it opens.
inline constexpr guest::GuestAddress kMainMenuItems = kRegion + 0x700U;
inline constexpr guest::GuestAddress kMainMenuDescriptions = kRegion + 0x740U;
inline constexpr guest::GuestAddress kCameraLabel = kRegion + 0x3800U;
// The slider marks: the game's retained text nodes (one per mark), whether
// they are allocated and in which UI bucket, then the draw inputs the game's
// text builder reads (colours 16-byte aligned for its quadword loads).
inline constexpr guest::GuestAddress kCameraMarks = kRegion + 0x3900U;
inline constexpr guest::GuestAddress kCameraMarkNodes = kCameraMarks + 0x00U; // u32 x4
inline constexpr guest::GuestAddress kCameraMarksLive = kCameraMarks + 0x10U;
inline constexpr guest::GuestAddress kCameraMarksBucket = kCameraMarks + 0x14U;
inline constexpr guest::GuestAddress kCameraMarkValueColor = kCameraMarks + 0x20U;
inline constexpr guest::GuestAddress kCameraMarkEndColor = kCameraMarks + 0x30U;
inline constexpr guest::GuestAddress kCameraMarkAnchor = kCameraMarks + 0x40U;
inline constexpr guest::GuestAddress kCameraMarkPositions = kCameraMarks + 0x48U; // 8 bytes x4
inline constexpr guest::GuestAddress kCameraMarkValueStyle = kCameraMarks + 0x70U;
inline constexpr guest::GuestAddress kCameraMarkEndStyle = kCameraMarks + 0x90U;
inline constexpr guest::GuestAddress kCameraMarkTexts = kCameraMarks + 0xB0U; // 16 bytes x4
// The FOV CORRECTION row's value table (Off, On string ids).
inline constexpr guest::GuestAddress kCameraCorrectionTable = kCameraMarks + 0xF0U;
inline constexpr guest::GuestAddress kTables = kRegion + 0x1000U;
inline constexpr guest::GuestAddress kModsValueTable = kRegion + 0x1400U;
// The REFRESH MOD LIST button's value table: one empty string.
inline constexpr guest::GuestAddress kModsButtonValueTable = kRegion + 0x1500U;
inline constexpr guest::GuestAddress kStrings = kRegion + 0x2000U;
// UTF-16 labels of the MODS page's rows (its mod names are dynamic, so they are
// written here as direct pointers rather than added to the game's string table).
inline constexpr guest::GuestAddress kModsLabels = kRegion + 0x3000U;
inline constexpr std::uint32_t kModsLabelStride = 64U;
inline constexpr guest::GuestAddress kStringArray = kRegion + 0x4000U;
// Stable label addresses for the retained list nodes while VIDEO scrolls.
inline constexpr guest::GuestAddress kVideoLabels = kRegion + 0x9000U;
inline constexpr std::uint32_t kVideoLabelStride = 128U;
inline constexpr guest::GuestAddress kModsPopupText = kRegion + 0x9800U;
inline constexpr std::uint32_t kModsPopupTextUnits = 512U;
// Original UI authored coordinates (640x480), transformed by the game's UI.
inline constexpr guest::GuestAddress kModsPopupPosition = kRegion + 0x9C00U;
inline constexpr guest::GuestAddress kModsPopupSize = kRegion + 0x9C08U;
inline constexpr guest::GuestAddress kModsPopupCentre = kRegion + 0x9C10U;
inline constexpr std::uint32_t kMagicValue = 0x504F4350U; // "PCOP"
inline constexpr guest::GuestAddress kControlsPage = kRegion + 0xA000U;
inline constexpr guest::GuestAddress kControlsVtable = kRegion + 0xA100U;
inline constexpr guest::GuestAddress kControlsLabels = kRegion + 0xA200U;
inline constexpr guest::GuestAddress kControlsText = kRegion + 0xA800U;
inline constexpr guest::GuestAddress kControlsTables = kRegion + 0xAE00U;
inline constexpr guest::GuestAddress kControlsValues = kRegion + 0xAE40U;
inline constexpr guest::GuestAddress kControlsUpdate = kRegion + 0x460U;
inline constexpr guest::GuestAddress kControlsCleanup = kRegion + 0x470U;
inline constexpr guest::GuestAddress kGameplayPage = kRegion + 0xB000U;
inline constexpr guest::GuestAddress kGameplayVtable = kRegion + 0xB100U;
inline constexpr guest::GuestAddress kGameplayValues = kRegion + 0xB120U;
inline constexpr guest::GuestAddress kGameplayDirty = kRegion + 0xB130U;
inline constexpr guest::GuestAddress kPauseItems = kRegion + 0xB200U;
inline constexpr guest::GuestAddress kPauseActions = kRegion + 0xB280U;
inline constexpr guest::GuestAddress kGameplayUpdate = kRegion + 0x480U;
inline constexpr guest::GuestAddress kLobbyCallReturn = kRegion + 0x490U;
inline constexpr guest::GuestAddress kLobbyPopupText = kRegion + 0xC000U;
inline constexpr std::uint32_t kLobbyPopupTextUnits = 1024U;
inline constexpr guest::GuestAddress kLobbyPopupButtons = kRegion + 0xC800U;
inline constexpr std::uint32_t kLobbyPopupButtonStride = 128U;
inline constexpr guest::GuestAddress kLobbyPopupPosition = kRegion + 0xCA00U;
inline constexpr guest::GuestAddress kLobbyPopupSize = kRegion + 0xCA08U;
inline constexpr guest::GuestAddress kLobbyPopupCentre = kRegion + 0xCA10U;
inline constexpr std::uint32_t kVersionValue = 13U;
} // namespace native_pc_layout

// Retail addresses this module reads, patches or acts at (SLUS-21050). Each
// code site is a word the module only checks; the one data word is patched.
namespace native_pc_retail {
inline constexpr guest::GuestAddress kFrontendDraw = 0x0030D6E0U;
inline constexpr guest::GuestAddress kStringTablePointer = 0x0051BA44U;
inline constexpr std::uint32_t kStringCount = 4006U;
inline constexpr std::uint32_t kAudioStringId = 408U; // "AUDIO", used to validate the table

// Main menu (BURNOUT 3 WORLD TOUR ... DRIVER DETAILS), page 0x01E94C40: init
// 0x00318340 builds five rows of 8 bytes from page +24 and one description
// pointer per row from +64 (the pause list object follows at +84), so it has
// no room for a sixth row; the list widget (0x00321470) and the description
// widget (0x0031C350) are handed the arrays in a2. Its update reads the
// selection at 0x00318208 once a confirm was pressed and returns through
// 0x0031832C.
inline constexpr guest::GuestAddress kMainMenu = 0x01E94C40U;
inline constexpr guest::GuestAddress kMainMenuInit = 0x00318340U;
inline constexpr guest::GuestAddress kMainMenuCountSite = 0x00318444U;        // addiu v1, zero, 5
inline constexpr guest::GuestAddress kMainMenuItemsArgument = 0x00318454U;    // addiu a2, s0, 24
inline constexpr guest::GuestAddress kMainMenuDescriptionsArgument = 0x0031849CU; // addiu a2, s0, 64
inline constexpr guest::GuestAddress kMainMenuConfirmSite = 0x00318208U;      // lhu v1, 20(s0)
inline constexpr guest::GuestAddress kMainMenuUpdateReturn = 0x0031832CU;     // ld ra, 16(sp)

// Options hub init 0x003F39C0 (items of 8 bytes from +8, count at +6).
inline constexpr guest::GuestAddress kGameplayInit = 0x0045BB10U;
inline constexpr guest::GuestAddress kPauseItemsArgument = 0x00342730U;
inline constexpr guest::GuestAddress kControllersInit = 0x003F3670U;
inline constexpr guest::GuestAddress kVideoInit = 0x003F3E60U;
inline constexpr guest::GuestAddress kHubInit = 0x003F39C0U;       // addiu sp, sp, -32
inline constexpr guest::GuestAddress kHubCountSite = 0x003F39E0U;  // addiu v0, zero, 5
// The hub object (0x01E96474) holds exactly its five retail rows: the Audio
// page object starts right after them (0x01E964A4 = hub + 48), so the added
// rows can never be stored in the hub itself. The hub init hands the list
// widget the row array in a2 (`addiu a2, s0, 8`) and the widget keeps that
// pointer; the module hands it a copy in its own region instead.
inline constexpr guest::GuestAddress kHub = 0x01E96474U;
inline constexpr guest::GuestAddress kHubItemsArgument = 0x003F3A80U; // addiu a2, s0, 8
inline constexpr guest::GuestAddress kHubWidgetInitCall = 0x003F3A84U; // jal 0x00321470
inline constexpr guest::GuestAddress kHubSelectionTable = 0x004A5514U; // entry for item 6 (data)
inline constexpr guest::GuestAddress kHubSelectionTableCamera = 0x004A5518U; // entry for item 7 (data)
inline constexpr guest::GuestAddress kHubSelectionTableMods = 0x004A551CU; // entry for item 8 (data)

// Options detail screen 0x0045C140, sub-page switch.
inline constexpr guest::GuestAddress kDetailScreen = 0x0045C140U;     // addiu sp, sp, -80
inline constexpr guest::GuestAddress kDetailSwitchSite = 0x0045C2E0U; // b 0x0045c858
inline constexpr guest::GuestAddress kDetailSwitchDelay = 0x0045C2E4U; // nop
inline constexpr guest::GuestAddress kDetailSwitchEnd = 0x0045C858U;
inline constexpr guest::GuestAddress kNoopRoutine = 0x0045B910U; // jr ra; nop (Gameplay cleanup)

// Page title setup picks the red top-left header (a string of HEADUS.BIN) from
// the identity of the current page: 0x0031ABC0 compares the page pointer at
// 0x01E90430 with a list of known page objects and returns the header index
// (3 = DRIVER DETAILS for the Options hub and every Options sub-page, 10 = none
// for an unknown page).
inline constexpr guest::GuestAddress kPageHeaderLookup = 0x0031ABC0U; // lui v1, 0x01e9
inline constexpr guest::GuestAddress kCurrentPage = 0x01E90430U;
inline constexpr std::uint32_t kHeaderDriverDetails = 3U;

// In-race pause list: init 0x00342410, update 0x00342090, message handler
// 0x003428D0.
inline constexpr guest::GuestAddress kPauseInit = 0x00342410U;        // addiu sp, sp, -112
inline constexpr guest::GuestAddress kPauseUpdate = 0x00342090U;      // addiu sp, sp, -48
inline constexpr guest::GuestAddress kPauseMessages = 0x003428D0U;    // addiu sp, sp, -32
inline constexpr guest::GuestAddress kPauseListEnd = 0x0034265CU;     // lui v0, 0x004f
inline constexpr guest::GuestAddress kPauseActionSwitch = 0x00342114U; // sltiu at, v1, 6
inline constexpr guest::GuestAddress kPauseIndexSwitch = 0x0034226CU;  // addiu v1, zero, 3
inline constexpr guest::GuestAddress kPauseOpenCall = 0x00342CB4U;     // jalr ra, t9
inline constexpr guest::GuestAddress kPauseOpenFinish = 0x00342D00U;   // jalr ra, t9
inline constexpr guest::GuestAddress kPauseList = 0x01E94C94U;         // the pause list object
} // namespace native_pc_retail

class Slus21050NativePcOptions {
  public:
    struct Result {
        bool installed_now{false};
        bool settings_changed{false};

        // A player edit on the MODS page the host must apply to its mod list:
        // `row` indexes the mods the page was last given (the whole list, not
        // the shown window), `value` is 0 for disabled and k for load-order
        // slot k.
        struct ModEdit {
            std::size_t row{0U};
            std::uint32_t value{0U};
        };
        std::optional<ModEdit> mod_edit;
        // The player pressed REFRESH MOD LIST to re-read the mods folder.
        bool mods_rescan_requested{false};
    };

    enum class HookResult : std::uint8_t {
        NotHooked, // not this module's PC, or the module is idle; the guest runs it
        Replaced,  // the instruction(s) at the PC were performed natively; pc is set
    };

    // A retail instruction word the module acts at or beside, or the first word
    // of a retail routine it calls.
    struct RetailWord {
        guest::GuestAddress address;
        std::uint32_t value;
    };
    // Every such word; all must hold their retail value for the module to start.
    [[nodiscard]] static std::span<const RetailWord> retail_words() noexcept;

    // Describes the first retail word that is not the expected value; nullopt
    // when all match. The string table is only checked once the game has
    // loaded it (see service()).
    [[nodiscard]] static std::optional<std::string>
    signature_mismatch(const guest::EeMemory& memory);

    // Every PC on_dispatch acts at. The runner must reach its dispatch
    // boundary at each of them (mark them in the burst hook filter).
    [[nodiscard]] static std::span<const guest::GuestAddress> hook_addresses() noexcept;

    // Call with the CPU about to execute the instruction at state.pc(). Acts
    // only once the page is installed.
    HookResult on_dispatch(guest::R5900State& state, guest::EeMemory& memory);

    // Called once per VBlank with the authoritative settings. Installs the
    // page when the game's string table is loaded, publishes the settings to
    // the page and applies changes the player made on it to `settings`.
    //
    // `mods` is the mods list the MODS page shows; pass nullptr to leave that
    // page out entirely (the hub then keeps the PC OPTIONS entry alone). When
    // it is not null the page is installed and the returned Result carries the
    // player's edits back to the caller, which owns the files and load order.
    Result service(guest::EeMemory& memory, config::GraphicsSettings& settings,
                   const NativeModsPageState* mods = nullptr);

    // Native controls use the same original retained list and bottom message box.
    // While capturing the box shows `prompt`, replaced whenever it changes.
    void service_controls(guest::EeMemory& memory,
        const std::vector<std::pair<std::string,std::string>>& rows, bool capturing,
        std::string_view notice = {}, std::string_view prompt = {});
    [[nodiscard]] std::optional<std::pair<std::size_t,int>> consume_controls_action();

    // The original game message widget draws lobby consent/progress. Semantic
    // input belongs to the session; the original widget owns art, font, focus
    // animation, footer and sound. Work runs only at the idle frontend draw
    // boundary, never from a VBlank in an interrupted guest routine.
    void service_lobby_prompt(const ui::HostPrompt* prompt);
    void request_mods_page(std::uint32_t return_screen = 0, std::uint32_t return_page = 0) noexcept;
    void request_lobby_return() noexcept;
    [[nodiscard]] bool consume_lobby_returned() noexcept;
    void cancel_lobby_navigation() noexcept;
    [[nodiscard]] bool lobby_input_blocked(bool buttons_pressed) noexcept;
    [[nodiscard]] bool lobby_prompt_open() const noexcept { return lobby_popup_open_; }
    [[nodiscard]] std::optional<std::string> consume_lobby_error();

    [[nodiscard]] bool installed() const noexcept { return installed_; }
    // True once after the player chose QUIT GAME on the main menu.
    [[nodiscard]] bool consume_quit_request() noexcept { return std::exchange(quit_requested_, false); }
    // While the MODS page is the game's current page: the focused row of the
    // page (0 to mods_page_rows() - 1 a shown mod, mods_page_rows() the
    // REFRESH MOD LIST button). nullopt on any other page.
    [[nodiscard]] std::optional<std::size_t> mods_page_focus(const guest::EeMemory& memory) const;
    // Mod rows the MODS page shows, and the index of the first of them in the
    // mod list (the window scrolls with the selection).
    [[nodiscard]] std::size_t mods_page_rows() const noexcept { return mods_rows_; }
    [[nodiscard]] std::size_t mods_page_window() const noexcept { return mods_window_; }
    [[nodiscard]] bool mods_information_open() const noexcept { return mods_popup_open_; }
    // Set when the page cannot be added; the game is left untouched.
    [[nodiscard]] const std::optional<std::string>& unavailable_reason() const noexcept {
        return unavailable_;
    }

  private:
    // One retail routine the page calls: target and its arguments. `a0_from`
    // makes the call read a0 from guest memory when it is made (a node an
    // earlier call allocated); `result_to` stores v0 when it returns.
    struct GuestCall {
        guest::GuestAddress target{};
        std::uint32_t a0{};
        std::uint32_t a1{};
        std::uint32_t a2{};
        std::uint32_t a3{};
        std::uint32_t t0{};
        std::uint32_t t1{};
        std::uint32_t t2{};
        std::uint32_t t3{};
        std::uint32_t f12_bits{};
        guest::GuestAddress a0_from{};
        guest::GuestAddress result_to{};
    };
    // What a returning retail routine continues with.
    enum class Stage : std::uint8_t {
        Idle,
        InitWrites, // page registered: write the page state, then queue the widget calls
        Queue,      // run the queued calls, then finish
        UpResult,   // list-up returned: finish if the selection moved, else go on
        DownResult, // list-down returned: finish if the selection moved, else go on
    };
    // The page update's input checks, in the retail order.
    enum class UpdatePhase : std::uint8_t { Up, Down, Side };
    // Which of the two pages the current sequence belongs to.
    enum class PageKind : std::uint8_t { Pc, Mods, Camera, Controls, Gameplay };

    [[nodiscard]] bool install(guest::EeMemory& memory);
    [[nodiscard]] bool patch_string_table(guest::EeMemory& memory, bool first_time);
    void publish(guest::EeMemory& memory, const config::GraphicsSettings& settings);
    void publish_controls_rows(guest::EeMemory& memory);
    void publish_video_rows(guest::EeMemory& memory);
    // Writes the MODS page's rows (labels, value indices, counts) from `mods`.
    // Writes the rows of the shown window; `focus` moves the selection (else
    // it stays on the row it is on).
    void publish_mods(guest::EeMemory& memory, const NativeModsPageState& mods,
                      std::optional<std::uint32_t> focus = std::nullopt);
    // Scrolls the window when the selection reaches its first or last mod row
    // and more mods lie beyond it.
    void scroll_mods(guest::EeMemory& memory, const NativeModsPageState& mods);
    // Writes the CAMERA page's slider level and label when they changed.
    void publish_camera(guest::EeMemory& memory, const config::GraphicsSettings& settings);
    [[nodiscard]] guest::GuestAddress page_address(PageKind kind) const noexcept;

    void issue_call(guest::R5900State& state, const GuestCall& call);
    // Queues the calls that (re)build the CAMERA page's slider marks: frees
    // marks still allocated when `allocate`, allocates them again, then fills
    // every mark (or only the angle) with the game's text builder.
    void queue_camera_marks(guest::EeMemory& memory, bool allocate, bool value_only);
    void queue_camera_marks_free(guest::EeMemory& memory);
    void queue_mods_popup(guest::EeMemory& memory, bool replace);
    void queue_mods_popup_close();
    void open_mods_information(guest::R5900State& state, guest::EeMemory& memory, std::string_view text);
    void run_queue(guest::R5900State& state, const guest::EeMemory& memory);
    // `call` with a0 read from guest memory when it names a source.
    [[nodiscard]] static GuestCall resolved(const guest::EeMemory& memory, GuestCall call);
    void finish_sequence(guest::R5900State& state);
    void begin_page_init(guest::R5900State& state, bool pause_context, PageKind kind);
    void begin_page_update(guest::R5900State& state, guest::EeMemory& memory, PageKind kind);
    void update_step(guest::R5900State& state, guest::EeMemory& memory, UpdatePhase phase);
    void continue_sequence(guest::R5900State& state, guest::EeMemory& memory);
    // The options sub-page (4 PC OPTIONS, 6 CAMERA) of the pause list's
    // highlighted row when it is one of the added rows, else 0.
    [[nodiscard]] std::uint32_t pause_item_selected(const guest::EeMemory& memory) const;
    HookResult extend_pause_list(guest::R5900State& state, guest::EeMemory& memory);
    HookResult open_from_pause(guest::R5900State& state, guest::EeMemory& memory);
    HookResult pause_open_call(guest::R5900State& state, guest::EeMemory& memory);
    HookResult lobby_frontend_work(guest::R5900State& state, guest::EeMemory& memory);
    void continue_lobby_work(guest::R5900State& state, guest::EeMemory& memory);
    void lobby_next_operation(guest::R5900State& state, guest::EeMemory& memory);
    void lobby_retire_widgets(guest::R5900State& state, guest::EeMemory& memory);
    void lobby_activate_target(guest::R5900State& state, guest::EeMemory& memory);
    void lobby_call(guest::R5900State& state, GuestCall call);
    void finish_lobby_work(guest::R5900State& state);

    std::vector<std::pair<std::string,std::string>> controls_rows_;
    std::size_t controls_window_{};
    bool controls_stale_{};
    bool controls_capturing_{};
    bool controls_capture_popup_{};
    std::string controls_notice_;
    std::string controls_shown_notice_;
    std::string controls_prompt_;
    std::string controls_shown_prompt_;
    std::optional<std::pair<std::size_t,int>> controls_action_;
    enum class LobbyStage : std::uint8_t { Idle, Close, Popup, Sound, Lookup, RetirePage, RetireWidget, RetirePool, Activate, Switch };
    LobbyStage lobby_stage_{LobbyStage::Idle};
    std::optional<ui::HostPrompt> lobby_prompt_, lobby_shown_prompt_;
    std::shared_ptr<guest::R5900State> lobby_saved_state_;
    bool lobby_popup_open_{}, lobby_open_mods_{}, lobby_return_requested_{}, lobby_returning_{}, lobby_returned_{}, lobby_release_wait_{};
    std::uint32_t lobby_target_screen_{}, lobby_previous_screen_{};
    std::uint32_t lobby_return_screen_{}, lobby_transition_subpage_{5U};
    std::uint32_t lobby_return_page_{}, lobby_expected_page_{}, lobby_activate_target_{}, lobby_switch_target_{};
    std::vector<GuestCall> lobby_retire_calls_;
    std::size_t lobby_retire_index_{};
    bool lobby_wait_page_{}, lobby_wait_returning_{};
    unsigned lobby_wait_frames_{};
    std::optional<std::string> lobby_error_;
    bool installed_{false};
    std::optional<std::string> unavailable_;
    std::uint32_t vblanks_{0U};
    std::uint32_t last_table_object_{0U};
    std::array<std::uint32_t, kNativePcOptionRowCount> published_{};
    bool published_valid_{false};
    std::size_t video_window_{};
    bool video_values_stale_{};
    std::array<guest::GuestAddress, kNativePcOptionRowCount> video_tables_{};

    // MODS page: whether it is part of the install, the rows it was last given
    // and the signature used to notice a change.
    bool mods_page_enabled_{false};
    std::size_t mods_rows_{0U};
    std::size_t mods_window_{0U};
    std::optional<std::size_t> mods_last_focus_;
    bool mods_values_stale_{false};
    std::uint64_t mods_signature_{0U};
    bool mods_signature_valid_{false};
    std::vector<std::uint32_t> published_mods_;
    std::vector<std::string> mods_information_;
    std::vector<std::u16string> mods_popup_pages_;
    std::size_t mods_popup_page_{};
    bool mods_popup_open_{};
    std::uint64_t mods_notice_serial_{};
    std::optional<std::string> mods_pending_notice_;
    // FOV level, FOV CORRECTION, PANINI PROJECTION as last published.
    std::optional<std::array<std::uint32_t, 3>> published_camera_;

    // Native call sequence in progress (init, update, or their tail calls).
    Stage stage_{Stage::Idle};
    PageKind page_kind_{PageKind::Pc};
    std::array<GuestCall, 24> queue_{};
    guest::GuestAddress pending_result_to_{};
    std::size_t queue_size_{0U};
    std::size_t queue_next_{0U};
    guest::GuestAddress finish_pc_{};
    std::uint32_t finish_v0_{};
    std::uint32_t update_page_{};
    bool init_pause_context_{false};
    // Entries the hub init owes after its patched count (0 when none).
    std::uint32_t hub_items_{0U};
    // The main menu init's count was extended, so its rows must be too.
    bool main_menu_extended_{false};
    bool quit_requested_{false};
    // The pause list's QUIT row: its retail index and where it was moved to
    // (below the added rows); 0xFFFFFFFF when it was not moved.
    std::uint32_t pause_quit_from_{0xFFFFFFFFU};
    std::uint32_t pause_quit_to_{0xFFFFFFFFU};
};

} // namespace takedown::runtime
