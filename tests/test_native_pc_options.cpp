// Unit tests for the PC options page inside the game's menus: the row model,
// installation into a synthetic guest image (no game data), and the page's
// native routines (hub entry, options screen switch, page init and update, the
// pause list rows) run through on_dispatch with the retail routines they call
// replaced by recording stubs.

#include "takedown/config/app_settings.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/slus_21050_native_pc_options.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace takedown;
namespace layout = runtime::native_pc_layout;
namespace retail = runtime::native_pc_retail;
using Options = runtime::Slus21050NativePcOptions;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint32_t kTableObject = 0x00600000U;
constexpr std::uint32_t kTableArray = 0x00610000U;
constexpr std::uint32_t kTableStrings = 0x00620000U;
constexpr std::uint32_t kScratch = 0x00700000U;
constexpr std::uint32_t kSentinel = 0x00BFF000U;
constexpr std::uint32_t kPadRecord = 0x004F50C0U + 3400U; // first controller
constexpr std::uint32_t kPhase = 0x01E90424U;

constexpr std::size_t kV0 = 2U;
constexpr std::size_t kV1 = 3U;
constexpr std::size_t kA0 = 4U;
constexpr std::size_t kA1 = 5U;
constexpr std::size_t kA2 = 6U;
constexpr std::size_t kT0 = 8U;
constexpr std::size_t kS0 = 16U;
constexpr std::size_t kS1 = 17U;
constexpr std::size_t kT9 = 25U;
constexpr std::size_t kRa = 31U;

void write_u16(guest::EeMemory& memory, const std::uint32_t address, const std::uint16_t value) {
    memory.write_u8(address, static_cast<std::uint8_t>(value & 0xFFU));
    memory.write_u8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
}

std::uint16_t read_u16(const guest::EeMemory& memory, const std::uint32_t address) {
    return static_cast<std::uint16_t>(memory.read_u8(address) | (memory.read_u8(address + 1U) << 8));
}

std::u16string read_string(const guest::EeMemory& memory, std::uint32_t address) {
    std::u16string text;
    for (;;) {
        const std::uint16_t unit = read_u16(memory, address);
        if (unit == 0U) {
            return text;
        }
        text.push_back(static_cast<char16_t>(unit));
        address += 2U;
    }
}

// Retail words the module checks, and a string table shaped like the game's.
void install_fixture(guest::EeMemory& memory) {
    for (const auto& word : Options::retail_words()) {
        memory.write_u32_le(word.address, word.value);
    }
    memory.write_u32_le(retail::kHubSelectionTable, 0U);
    memory.write_u32_le(retail::kStringTablePointer, kTableObject);
    memory.write_u32_le(kTableObject + 8U, retail::kStringCount);
    memory.write_u32_le(kTableObject + 12U, kTableArray);
    for (std::uint32_t index = 0U; index < retail::kStringCount; ++index) {
        memory.write_u32_le(kTableArray + 4U * index, kTableStrings + 0x20U * index);
    }
    const std::u16string audio = u"AUDIO";
    for (std::size_t index = 0U; index < audio.size(); ++index) {
        write_u16(memory,
                  kTableStrings + 0x20U * retail::kAudioStringId + 2U * static_cast<std::uint32_t>(index),
                  static_cast<std::uint16_t>(audio[index]));
    }
}

struct Call {
    std::uint32_t address;
    std::uint32_t a0, a1, a2;
};

struct Run {
    std::vector<Call> calls;
    std::uint32_t stopped_at{0U};
    bool not_hooked{false};
    guest::R5900State state;
};

using Stub = std::function<std::uint32_t(guest::R5900State&)>;

std::uint32_t constant_zero(guest::R5900State&) { return 0U; }
std::uint32_t constant_one(guest::R5900State&) { return 1U; }

std::map<std::uint32_t, Stub> retail_stubs() {
    std::map<std::uint32_t, Stub> stubs;
    for (const std::uint32_t address :
         {0x318530U, 0x3BE5A0U, 0x3BC5B0U, 0x3BC4C0U, 0x319C80U, 0x31D930U, 0x31ECD0U, 0x38AD80U, 0x2853F0U,
          0x30ED70U, 0x30D500U, 0x1D7560U, 0x2F9690U}) {
        stubs[address] = constant_zero;
    }
    // The UI node pool hands out successive nodes.
    stubs[0x1D7580U] = [next = std::make_shared<std::uint32_t>(0x00700000U)](guest::R5900State&) {
        *next += 0x40U;
        return *next;
    };
    stubs[0x319360U] = constant_one;
    stubs[0x319330U] = constant_one;
    return stubs;
}

// Runs the CPU at `entry` through on_dispatch until it reaches a stop address.
// An address in `stubs` is a retail routine: the call is recorded and returns
// the stub's value to ra.
Run run_native(Options& options, guest::EeMemory& memory, const std::uint32_t entry,
               const std::map<std::uint32_t, Stub>& stubs, const std::vector<std::uint32_t>& stops,
               const std::vector<std::pair<std::size_t, std::uint32_t>>& registers = {}) {
    Run run;
    run.state.set_pc(entry);
    run.state.write_gpr_u64(29U, 0x007F0000U);
    run.state.write_gpr_u64(kRa, kSentinel);
    for (const auto& [index, value] : registers) {
        run.state.write_gpr_u64(index, value);
    }
    for (int step = 0; step < 1000; ++step) {
        const std::uint32_t pc = run.state.pc();
        if (pc == kSentinel) {
            run.stopped_at = kSentinel;
            return run;
        }
        for (const std::uint32_t stop : stops) {
            if (pc == stop) {
                run.stopped_at = stop;
                return run;
            }
        }
        const auto stub = stubs.find(pc);
        if (stub != stubs.end()) {
            run.calls.push_back({pc, run.state.read_gpr_u32(kA0), run.state.read_gpr_u32(kA1),
                                 run.state.read_gpr_u32(kA2)});
            run.state.write_gpr_u64(kV0, stub->second(run.state));
            run.state.set_pc(run.state.read_gpr_u32(kRa));
            continue;
        }
        if (options.on_dispatch(run.state, memory) != Options::HookResult::Replaced) {
            run.not_hooked = true;
            run.stopped_at = pc;
            return run;
        }
    }
    run.stopped_at = 0xBADU;
    return run;
}

std::size_t count_calls(const Run& run, const std::uint32_t address) {
    std::size_t count = 0U;
    for (const Call& call : run.calls) {
        count += call.address == address ? 1U : 0U;
    }
    return count;
}

void test_model() {
    const auto& rows = runtime::native_pc_option_rows();
    config::GraphicsSettings settings;
    for (std::size_t row = 0U; row < rows.size(); ++row) {
        expect(!rows[row].label.empty() && rows[row].values.size() >= 2U &&
                   rows[row].values.size() < 255U,
               "every row has a label and values");
        for (std::size_t index = 0U; index < rows[row].values.size(); ++index) {
            config::GraphicsSettings changed = settings;
            expect(runtime::native_pc_option_apply(row, index, changed), "value applies");
            expect(runtime::native_pc_option_index(row, changed) == index,
                   "applied value reads back");
        }
        config::GraphicsSettings changed = settings;
        expect(!runtime::native_pc_option_apply(row, rows[row].values.size(), changed) &&
                   changed == settings,
               "an out-of-range value is refused and changes nothing");
    }
    // Defaults: unlimited frame rate, 240 fps in menus, widescreen off.
    expect(runtime::native_pc_option_index(3U, settings) == 0U, "default frame limit is unlimited");
    expect(rows[3].values[runtime::native_pc_option_index(4U, settings)] == "240 FPS",
           "default menu limit is 240");
    expect(runtime::native_pc_option_index(0U, settings) == 0U, "widescreen starts off");
    settings.presentation_fps_limit = 100U;
    expect(rows[3].values[runtime::native_pc_option_index(3U, settings)] == "90 FPS",
           "a hand-edited limit shows as the nearest listed value");
}

void test_hook_addresses() {
    // The runner marks these PCs; every code site is a retail word the module
    // checks, and the two host-region PCs hold no code.
    const auto addresses = Options::hook_addresses();
    bool region_pcs = false;
    for (const auto address : addresses) {
        bool checked = false;
        for (const auto& word : Options::retail_words()) {
            checked = checked || word.address == address;
        }
        const bool in_region = address >= layout::kRegion &&
                               address < layout::kRegion + layout::kRegionSize;
        expect(checked || in_region, "every hooked PC is a checked retail word or a region PC");
        region_pcs = region_pcs || in_region;
    }
    expect(region_pcs, "the region PCs are hooked");
}

void test_install_and_service() {
    guest::EeMemory memory;
    install_fixture(memory);
    expect(!Options::signature_mismatch(memory).has_value(),
           "the fixture matches the retail words");

    Options options;
    config::GraphicsSettings settings;
    settings.aspect_ratio = config::AspectRatio::Widescreen;
    settings.anti_aliasing = config::AntiAliasing::Ssaa4x;

    // Before the install the module leaves every hooked PC to the guest.
    {
        guest::R5900State state;
        state.set_pc(retail::kHubCountSite);
        expect(options.on_dispatch(state, memory) == Options::HookResult::NotHooked &&
                   state.pc() == retail::kHubCountSite,
               "an idle module does not hook");
    }

    bool installed_now = false;
    for (int vblank = 0; vblank < 40 && !installed_now; ++vblank) {
        installed_now = options.service(memory, settings).installed_now;
    }
    expect(installed_now && options.installed() && !options.unavailable_reason().has_value(),
           "the page installs once the string table is loaded");

    const auto extended = memory.read_u32_le(kTableObject + 12U);
    expect(extended == layout::kStringArray, "the string table array is replaced");
    expect(memory.read_u32_le(kTableObject + 8U) > retail::kStringCount, "the count grows");
    expect(memory.read_u32_le(extended + 4U * retail::kAudioStringId) ==
               kTableStrings + 0x20U * retail::kAudioStringId,
           "retail strings keep their pointers");
    expect(read_string(memory, memory.read_u32_le(extended + 4U * retail::kStringCount)) ==
               u"PC OPTIONS",
           "the first added string is the page title");
    expect(memory.read_u32_le(retail::kHubSelectionTable) == 4U, "entry six opens sub-page 4");
    // No retail code word is written.
    for (const auto& word : Options::retail_words()) {
        expect(memory.read_u32_le(word.address) == word.value, "retail code is untouched");
    }
    expect(memory.read_u32_le(layout::kVtable + 8U) == layout::kUpdateEntry &&
               memory.read_u32_le(layout::kVtable + 12U) == retail::kNoopRoutine,
           "the page vtable points at the native update and the retail no-op");
    expect(!Options::signature_mismatch(memory).has_value(),
           "the patched data word is accepted on a second look");

    // Mailbox and page items hold the current values.
    expect(memory.read_u32_le(layout::kValues + 0U) == 1U &&
               memory.read_u32_le(layout::kValues + 8U) == 2U,
           "the mailbox holds widescreen on and 4x anti-aliasing");

    // A game reload of the string table is repaired.
    memory.write_u32_le(kTableObject + 12U, kTableArray);
    for (int vblank = 0; vblank < 64; ++vblank) {
        (void)options.service(memory, settings);
    }
    expect(memory.read_u32_le(kTableObject + 12U) == layout::kStringArray,
           "a reloaded string table is extended again");

    // Nothing is written while nothing changes.
    settings = config::GraphicsSettings{};
    bool changed = false;
    for (int vblank = 0; vblank < 64; ++vblank) {
        changed = options.service(memory, settings).settings_changed || changed;
    }
    expect(!changed, "no change is reported without a guest edit");
    expect(memory.read_u32_le(layout::kValues + 0U) == 0U, "host settings are republished");

    // An unfamiliar retail word leaves the game untouched.
    for (const auto& victim : {retail::kDetailSwitchSite, retail::kPauseListEnd,
                               retail::kPauseOpenFinish + 4U, 0x00319360U}) {
        guest::EeMemory other;
        install_fixture(other);
        other.write_u32_le(victim, 0x12345678U);
        Options refused;
        config::GraphicsSettings unchanged;
        for (int vblank = 0; vblank < 40; ++vblank) {
            (void)refused.service(other, unchanged);
        }
        expect(!refused.installed() && refused.unavailable_reason().has_value(),
               "a mismatch refuses the install");
        expect(other.read_u32_le(retail::kHubSelectionTable) == 0U &&
                   other.read_u32_le(kTableObject + 12U) == kTableArray,
               "a refused install writes nothing to the game");
    }

    // A string table of another shape is refused after the patience period.
    guest::EeMemory odd;
    install_fixture(odd);
    odd.write_u32_le(kTableObject + 8U, 12U);
    Options waiting;
    config::GraphicsSettings unchanged;
    for (int vblank = 0; vblank < 700; ++vblank) {
        (void)waiting.service(odd, unchanged);
    }
    expect(!waiting.installed() && waiting.unavailable_reason().has_value(),
           "an unexpected string table disables the page");
    expect(odd.read_u32_le(retail::kHubSelectionTable) == 0U, "and leaves the hub alone");
}

// Builds an installed image and returns it with the module and its settings.
struct Installed {
    guest::EeMemory memory;
    Options options;
    config::GraphicsSettings settings;
};

void make_installed(Installed& installed) {
    install_fixture(installed.memory);
    for (int vblank = 0; vblank < 40 && !installed.options.installed(); ++vblank) {
        (void)installed.options.service(installed.memory, installed.settings);
    }
    installed.memory.write_u32_le(kPhase, 5U); // frontend menu phase
}

void test_hub_and_switch() {
    Installed installed;
    make_installed(installed);
    expect(installed.options.installed(), "installed for the hub tests");
    auto& memory = installed.memory;

    // Hub: the count becomes six; the five retail rows are copied to the
    // module's region, the sixth row is appended there, and the list widget is
    // handed that array. The hub object itself is never written past its five
    // rows: the Audio page object starts right after them.
    for (std::uint32_t row = 0U; row < 5U; ++row) {
        memory.write_u32_le(retail::kHub + 8U + 8U * row, 0x00600000U + row);
        memory.write_u32_le(retail::kHub + 12U + 8U * row, 0x0000FFFFU);
    }
    constexpr std::uint32_t kAudioVtable = 0x004A1230U;
    memory.write_u32_le(retail::kHub + 48U, kAudioVtable);
    memory.write_u32_le(retail::kHub + 56U, 0x11223344U);
    const auto count = run_native(installed.options, memory, retail::kHubCountSite, {},
                                  {retail::kHubCountSite + 4U});
    expect(count.stopped_at == retail::kHubCountSite + 4U && count.state.read_gpr_u32(kV0) == 7U,
           "the hub shows seven entries (PC OPTIONS and CAMERA added)");
    const auto item = run_native(installed.options, memory, retail::kHubItemsArgument, {},
                                 {retail::kHubItemsArgument + 4U}, {{kS0, retail::kHub}});
    expect(item.stopped_at == retail::kHubItemsArgument + 4U,
           "the hub init continues at the widget call");
    expect(item.state.read_gpr_u32(kA2) == layout::kHubItems,
           "the widget is handed the module's row array");
    expect(memory.read_u32_le(layout::kHubItems) == 0x00600000U &&
               memory.read_u32_le(layout::kHubItems + 32U) == 0x00600004U,
           "the retail rows are copied in order");
    expect(memory.read_u8(layout::kHubItems + 36U) == 0U &&
               memory.read_u8(layout::kHubItems + 37U) == 0U,
           "the last retail row's flags are cleared as the hub init clears them");
    expect(read_string(memory, memory.read_u32_le(layout::kHubItems + 40U)) == u"PC OPTIONS",
           "the sixth row label is the PC OPTIONS string");
    expect(memory.read_u8(layout::kHubItems + 44U) == 0U &&
               memory.read_u8(layout::kHubItems + 45U) == 0U,
           "the sixth row has no flags");
    expect(memory.read_u32_le(retail::kHub + 48U) == kAudioVtable &&
               memory.read_u32_le(retail::kHub + 56U) == 0x11223344U,
           "the Audio page object after the hub is left untouched");
    // The array is only substituted by a hub init that saw the patched count.
    guest::R5900State again;
    again.set_pc(retail::kHubItemsArgument);
    again.write_gpr_u64(kS0, retail::kHub);
    expect(installed.options.on_dispatch(again, memory) == Options::HookResult::NotHooked,
           "a hub init that did not extend the count keeps its own rows");

    // Detail screen: only sub-page index 4 enters the new page.
    memory.write_u8(kScratch + 80U, 0U);
    const auto enter = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                  retail_stubs(), {retail::kDetailSwitchEnd},
                                  {{kT0, 4U}, {kS1, kScratch}});
    expect(enter.stopped_at == retail::kDetailSwitchEnd && count_calls(enter, 0x318530U) == 1U,
           "sub-page 4 initialises the PC page");
    expect(enter.calls.front().a0 == layout::kPage, "with the page object");
    expect(enter.state.read_gpr_u32(kV0) == 1U, "and reports success like the retail inits");
    const auto other = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                  retail_stubs(), {retail::kDetailSwitchEnd}, {{kT0, 7U}});
    expect(other.not_hooked && other.calls.empty(),
           "any other index is left to the retail branch");
}

void test_page_init() {
    Installed installed;
    installed.settings.aspect_ratio = config::AspectRatio::Widescreen;
    installed.settings.anti_aliasing = config::AntiAliasing::Ssaa8x;
    installed.settings.presentation_fps_limit = 60U;
    make_installed(installed);
    auto& memory = installed.memory;

    memory.write_u8(kScratch + 80U, 0U);
    const auto init = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                 retail_stubs(), {retail::kDetailSwitchEnd},
                                 {{kT0, 4U}, {kS1, kScratch}});
    expect(init.stopped_at == retail::kDetailSwitchEnd, "the init returns to the screen");
    expect(read_u16(memory, layout::kPage + 4U) == 0U && read_u16(memory, layout::kPage + 6U) == 9U,
           "selection 0 of 9 rows");
    expect(init.calls[0].address == 0x318530U && init.calls[0].a0 == layout::kPage &&
               init.calls[0].a1 == 0U,
           "the page registers itself as the current page, first");
    expect(count_calls(init, 0x3BE5A0U) == 1U, "the list widget is initialised once");
    for (const Call& call : init.calls) {
        if (call.address == 0x3BE5A0U) {
            expect(call.a0 == 0x01E8961CU && call.a1 == layout::kPage + 4U &&
                       call.a2 == layout::kPage + 8U,
                   "with the page's state and items");
        }
    }
    std::vector<std::uint32_t> shown(9U, 99U);
    for (const Call& call : init.calls) {
        if (call.address == 0x3BC5B0U && call.a1 < 9U) {
            shown[call.a1] = call.a2;
        }
    }
    expect(shown[0] == 1U && shown[2] == 3U && shown[3] == 2U,
           "the widget shows widescreen on, 8x supersampling and 60 FPS");
    expect(shown[8] == 1U, "the dithering row shows the default, on");
    // The main menu page frame: title style 14, frame style 0, buttons 266.
    for (const Call& call : init.calls) {
        if (call.address == 0x319C80U) {
            expect(call.a0 == 0x01E89518U && call.a1 == retail::kStringCount && call.a2 == 14U,
                   "the title is the added string in the menu style");
        }
        if (call.address == 0x31D930U) {
            expect(call.a0 == 0x01E88A80U && call.a1 == 0U, "menu frame style");
        }
        if (call.address == 0x31ECD0U) {
            expect(call.a0 == 0x01E89800U && call.a1 == 266U && call.a2 == 15U, "menu buttons");
        }
    }
    expect(init.calls.back().address == 0x38AD80U && init.calls.back().a0 == 0x01E894FCU,
           "the frame is finished last");

    // The title setup asks which top-left header the current page gets: the
    // PC page, once current, gets the Options pages' (index 3); any other page
    // is left to the retail list.
    memory.write_u32_le(retail::kCurrentPage, layout::kPage);
    const auto header = run_native(installed.options, memory, retail::kPageHeaderLookup, {}, {0x00123450U},
                                   {{kRa, 0x00123450U}});
    expect(header.stopped_at == 0x00123450U && header.state.read_gpr_u32(kV0) == 3U,
           "the PC page gets the DRIVER DETAILS header like the other Options pages");
    memory.write_u32_le(retail::kCurrentPage, 0x01E96514U);
    const auto other_page = run_native(installed.options, memory, retail::kPageHeaderLookup, {},
                                       {0x00123450U}, {{kRa, 0x00123450U}});
    expect(other_page.not_hooked, "another page keeps the retail header lookup");

    // Opened from the pause menu the frame uses the pause styles.
    memory.write_u8(kScratch + 80U, 1U);
    const auto paused = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                   retail_stubs(), {retail::kDetailSwitchEnd},
                                   {{kT0, 4U}, {kS1, kScratch}});
    expect(paused.stopped_at == retail::kDetailSwitchEnd, "the pause init returns");
    for (const Call& call : paused.calls) {
        if (call.address == 0x319C80U) {
            expect(call.a1 == retail::kStringCount && call.a2 == 5U, "pause title style 5");
        }
        if (call.address == 0x31D930U) {
            expect(call.a1 == 7U, "pause frame style 7");
        }
        if (call.address == 0x31ECD0U) {
            expect(call.a1 == 394U && call.a2 == 15U, "pause buttons");
        }
    }

    // Item records are the game's list-with-values layout.
    const std::uint32_t item = layout::kPage + 8U + 20U * 3U;
    expect(memory.read_u8(item + 12U) == 12U && memory.read_u8(item + 13U) == 4U &&
               memory.read_u32_le(item + 4U) == 2U,
           "frame limit row: 12 values, cycling list type, current value 60 FPS");
    const std::uint32_t table = memory.read_u32_le(item + 8U);
    const std::uint32_t array = memory.read_u32_le(kTableObject + 12U);
    expect(read_string(memory, memory.read_u32_le(array + 4U * memory.read_u32_le(table))) ==
               u"Unlimited",
           "value tables hold ids of added strings");
    expect(read_string(memory, memory.read_u32_le(item)) == u"FRAME RATE LIMIT",
           "labels are the added strings");
}

void press(guest::EeMemory& memory, const std::uint32_t offset) {
    for (const std::uint32_t slot : {20U, 32U, 44U, 56U, 68U, 92U}) {
        write_u16(memory, kPadRecord + slot, slot == offset ? 1U : 0U);
    }
}

Run run_update(Installed& installed, const std::map<std::uint32_t, Stub>& stubs) {
    return run_native(installed.options, installed.memory, layout::kUpdateEntry, stubs, {},
                      {{kA0, layout::kPage}});
}

void test_page_update() {
    Installed installed;
    make_installed(installed);
    auto& memory = installed.memory;
    const auto stubs = retail_stubs();
    (void)run_native(installed.options, memory, retail::kDetailSwitchSite, stubs,
                     {retail::kDetailSwitchEnd}, {{kT0, 4U}, {kS1, kScratch}});

    // No input: nothing happens.
    press(memory, 0U);
    const auto idle = run_update(installed, stubs);
    expect(idle.stopped_at == kSentinel && idle.calls.empty(), "idle update calls nothing");

    // Right on render scale 1x -> 2x.
    write_u16(memory, layout::kPage + 4U, 1U);
    press(memory, 32U);
    const auto right = run_update(installed, stubs);
    expect(right.stopped_at == kSentinel, "update returns to its caller");
    expect(memory.read_u32_le(layout::kValues + 4U) == 1U && memory.read_u32_le(layout::kDirty) == 1U,
           "the mailbox holds the new value and the dirty flag");
    expect(right.calls.size() == 3U && right.calls[0].address == 0x3BC5B0U &&
               right.calls[0].a0 == 0x01E8961CU && right.calls[0].a1 == 1U && right.calls[0].a2 == 1U,
           "the widget is told the new value");
    expect(right.calls[1].address == 0x2853F0U && right.calls[1].a0 == 0x01E7A800U &&
               right.calls[1].a1 == 1U,
           "the UI feedback is triggered");
    expect(right.calls[2].address == 0x30ED70U && right.calls[2].a0 == 0x01EA2970U &&
               right.calls[2].a1 == 75U,
           "the move sound plays");
    const auto result = installed.options.service(memory, installed.settings);
    expect(result.settings_changed && installed.settings.render_scale == 2U,
           "the host applies render scale 2x");
    expect(memory.read_u32_le(layout::kDirty) == 0U, "and clears the flag");

    // Right goes on through 300% to 400%, then wraps from the last value to
    // the first; left wraps the other way.
    write_u16(memory, layout::kPage + 4U, 1U);
    press(memory, 32U);
    (void)run_update(installed, stubs);
    expect(memory.read_u32_le(layout::kValues + 4U) == 2U, "right from 200% reaches 300%");
    const auto triple = installed.options.service(memory, installed.settings);
    expect(triple.settings_changed && installed.settings.render_scale == 3U,
           "the host applies render scale 3x");
    write_u16(memory, layout::kPage + 4U, 1U);
    press(memory, 32U);
    (void)run_update(installed, stubs);
    const auto quad = installed.options.service(memory, installed.settings);
    expect(quad.settings_changed && installed.settings.render_scale == 4U,
           "the host applies render scale 4x");
    write_u16(memory, layout::kPage + 4U, 1U);
    press(memory, 32U);
    (void)run_update(installed, stubs);
    expect(memory.read_u32_le(layout::kValues + 4U) == 0U, "right from the last value wraps to 0");
    write_u16(memory, layout::kPage + 4U, 2U);
    press(memory, 20U);
    (void)run_update(installed, stubs);
    expect(memory.read_u32_le(layout::kValues + 8U) == 4U, "left from 0 wraps to the last value");
    const auto wrapped = installed.options.service(memory, installed.settings);
    expect(wrapped.settings_changed && installed.settings.anti_aliasing == config::AntiAliasing::Ssaa16x &&
               installed.settings.render_scale == 1U,
           "both edits reach the settings");

    // Frame rate limit rows (12 values) and motion blur.
    write_u16(memory, layout::kPage + 4U, 3U);
    press(memory, 32U);
    (void)run_update(installed, stubs);
    write_u16(memory, layout::kPage + 4U, 7U);
    press(memory, 20U);
    (void)run_update(installed, stubs);
    (void)installed.options.service(memory, installed.settings);
    expect(installed.settings.presentation_fps_limit == std::optional<std::uint32_t>{30U},
           "Unlimited steps to 30 FPS");
    expect(!installed.settings.motion_blur, "motion blur toggles off");

    // List movement, confirm and back use the retail routines.
    press(memory, 44U);
    const auto up = run_update(installed, stubs);
    expect(up.calls.size() == 1U && up.calls[0].address == 0x319360U &&
               up.calls[0].a0 == layout::kPage + 4U,
           "up moves the selection and ends the update");
    press(memory, 56U);
    const auto down = run_update(installed, stubs);
    expect(down.calls.size() == 1U && down.calls[0].address == 0x319330U &&
               down.calls[0].a0 == layout::kPage + 4U,
           "down moves the selection and ends the update");
    press(memory, 68U);
    const auto confirm = run_update(installed, stubs);
    expect(confirm.calls.size() == 1U && confirm.calls[0].address == 0x30D500U &&
               confirm.calls[0].a0 == 0x01E90240U && confirm.calls[0].a1 == 1U,
           "confirm posts message 1");
    press(memory, 92U);
    const auto back = run_update(installed, stubs);
    expect(back.calls.size() == 2U && back.calls[0].address == 0x30ED70U &&
               back.calls[0].a1 == 74U && back.calls[1].address == 0x30D500U &&
               back.calls[1].a1 == 3U,
           "back plays the back sound and leaves the page");

    // At the end of the list up and down do not move; the next input is read.
    auto edge = retail_stubs();
    edge[0x319360U] = constant_zero;
    press(memory, 44U);
    write_u16(memory, kPadRecord + 32U, 1U); // up and right together
    write_u16(memory, layout::kPage + 4U, 1U);
    const auto fallthrough = run_update(installed, edge);
    expect(fallthrough.calls.size() == 4U && fallthrough.calls[0].address == 0x319360U &&
               fallthrough.calls[1].address == 0x3BC5B0U,
           "an up that does not move falls through to the value change");

    // Outside the frontend menu phase the page ignores the pad.
    memory.write_u32_le(kPhase, 2U);
    press(memory, 32U);
    const auto ignored = run_update(installed, stubs);
    expect(ignored.stopped_at == kSentinel && ignored.calls.empty(),
           "the page does nothing outside the menu phase");
}

void make_pause_list(guest::EeMemory& memory, const std::uint32_t count, const bool second_layout,
                     const std::uint32_t selected) {
    const std::uint32_t list = retail::kPauseList;
    for (std::uint32_t offset = 0U; offset < 112U; offset += 4U) {
        memory.write_u32_le(list + offset, 0U);
    }
    write_u16(memory, list + 4U, static_cast<std::uint16_t>(selected));
    write_u16(memory, list + 6U, static_cast<std::uint16_t>(count));
    for (std::uint32_t row = 0U; row < count; ++row) {
        memory.write_u32_le(list + 8U + 8U * row, 0xAAAA0000U + row);
        if (second_layout) {
            memory.write_u32_le(list + 68U + 4U * row, row == 2U ? 3U : 1U);
        }
    }
    memory.write_u8(list + 96U, second_layout ? 0U : 1U);
}

void test_pause_list() {
    Installed installed;
    make_installed(installed);
    auto& memory = installed.memory;
    const std::uint32_t list = retail::kPauseList;
    const std::uint32_t next = retail::kPauseListEnd + 4U;

    // Second layout (action codes in an array): row and action 6 appended.
    make_pause_list(memory, 5U, true, 1U);
    auto extend = run_native(installed.options, memory, retail::kPauseListEnd, {}, {next},
                             {{kS0, list}});
    expect(extend.stopped_at == next && extend.state.read_gpr_u32(kV0) == 0x004F0000U,
           "the list end hook does the instruction it stands in for");
    expect(read_u16(memory, list + 6U) == 7U, "the pause list shows two more rows");
    expect(read_string(memory, memory.read_u32_le(list + 8U + 8U * 5U)) == u"PC OPTIONS",
           "the added row is labelled PC OPTIONS");
    expect(memory.read_u8(list + 8U + 8U * 5U + 4U) == 0U &&
               memory.read_u8(list + 8U + 8U * 5U + 5U) == 0U,
           "the added row is enabled");
    expect(memory.read_u32_le(list + 68U + 4U * 5U) == 6U, "with action code 6");
    expect(read_string(memory, memory.read_u32_le(list + 8U + 8U * 6U)) == u"CAMERA" &&
               memory.read_u32_le(list + 68U + 4U * 6U) == 7U,
           "then CAMERA with action code 7");
    expect(memory.read_u32_le(list + 8U) == 0xAAAA0000U && memory.read_u32_le(list + 68U + 8U) == 3U,
           "existing rows are untouched");

    // First layout (selection decides): no action code is written.
    make_pause_list(memory, 5U, false, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next}, {{kS0, list}});
    expect(read_u16(memory, list + 6U) == 7U && memory.read_u32_le(list + 68U + 4U * 5U) == 0U &&
               memory.read_u32_le(list + 68U + 4U * 6U) == 0U,
           "the first layout gets the rows without action codes");
    make_pause_list(memory, 6U, true, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next}, {{kS0, list}});
    expect(read_u16(memory, list + 6U) == 7U &&
               read_string(memory, memory.read_u32_le(list + 8U + 8U * 6U)) == u"PC OPTIONS",
           "with room for one row only PC OPTIONS is added");

    // A full list is left alone, and so is any other object.
    make_pause_list(memory, 7U, true, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next}, {{kS0, list}});
    expect(read_u16(memory, list + 6U) == 7U, "a full list is not extended");
    make_pause_list(memory, 5U, true, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next},
                     {{kS0, kScratch}});
    expect(read_u16(memory, list + 6U) == 5U, "another object is not touched");

    // Selecting the row: both layouts continue at the Controllers-style open.
    make_pause_list(memory, 5U, true, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next}, {{kS0, list}});
    write_u16(memory, list + 4U, 5U);
    const auto action = run_native(installed.options, memory, retail::kPauseActionSwitch, {},
                                   {0x003421ECU}, {{kV1, 6U}});
    expect(action.stopped_at == 0x003421ECU, "the action switch opens the page for action 6");
    const auto other_action = run_native(installed.options, memory, retail::kPauseActionSwitch, {},
                                         {0x003421ECU}, {{kV1, 5U}});
    expect(other_action.not_hooked, "other actions are left to the retail switch");
    const auto index = run_native(installed.options, memory, retail::kPauseIndexSwitch, {},
                                  {0x003421ECU});
    expect(index.stopped_at == 0x003421ECU, "the selection switch opens the page for the added row");
    write_u16(memory, list + 4U, 2U);
    const auto elsewhere = run_native(installed.options, memory, retail::kPauseIndexSwitch, {},
                                      {0x003421ECU});
    expect(elsewhere.not_hooked, "other rows are left to the retail switch");

    // The open calls pass sub-page 4 only for the added row.
    write_u16(memory, list + 4U, 5U);
    for (const std::uint32_t site : {retail::kPauseOpenCall, retail::kPauseOpenFinish}) {
        const auto open = run_native(installed.options, memory, site, {}, {0x00012340U},
                                     {{kT9, 0x00012340U}, {kA2, 3U}});
        expect(open.stopped_at == 0x00012340U && open.state.read_gpr_u32(kA2) == 4U &&
                   open.state.read_gpr_u32(kRa) == site + 8U,
               "the open call carries sub-page 4 and returns after its delay slot");
    }
    write_u16(memory, list + 4U, 3U);
    const auto controllers = run_native(installed.options, memory, retail::kPauseOpenCall, {},
                                        {0x00012340U}, {{kT9, 0x00012340U}});
    expect(controllers.not_hooked, "Controllers still opens with its own sub-page");

    // The CAMERA row opens sub-page 6.
    make_pause_list(memory, 5U, true, 1U);
    (void)run_native(installed.options, memory, retail::kPauseListEnd, {}, {next}, {{kS0, list}});
    write_u16(memory, list + 4U, 6U);
    const auto camera_action = run_native(installed.options, memory, retail::kPauseActionSwitch, {},
                                          {0x003421ECU}, {{kV1, 7U}});
    expect(camera_action.stopped_at == 0x003421ECU, "the action switch opens the page for action 7");
    const auto camera_open = run_native(installed.options, memory, retail::kPauseOpenCall, {},
                                        {0x00012340U}, {{kT9, 0x00012340U}, {kA2, 3U}});
    expect(camera_open.stopped_at == 0x00012340U && camera_open.state.read_gpr_u32(kA2) == 6U,
           "the CAMERA row's open call carries sub-page 6");
}

// ---- MODS page -------------------------------------------------------------

runtime::NativeModsPageState mods_state(
    std::vector<std::pair<std::string, std::uint32_t>> mods) {
    runtime::NativeModsPageState state;
    for (auto& [label, value] : mods) {
        runtime::NativeModsPageMod mod;
        mod.label = label;
        mod.enabled = value != 0U;
        mod.slot = value;
        state.mods.push_back(std::move(mod));
    }
    return state;
}

void make_installed_with_mods(Installed& installed, const runtime::NativeModsPageState& mods) {
    install_fixture(installed.memory);
    for (int vblank = 0; vblank < 40 && !installed.options.installed(); ++vblank) {
        (void)installed.options.service(installed.memory, installed.settings, &mods);
    }
    installed.memory.write_u32_le(kPhase, 5U); // frontend menu phase
}

// Without a mods state the MODS page is not part of the install: the hub has
// seven entries and neither the page nor its update entry is hooked.
void test_mods_page_absent() {
    Installed installed;
    make_installed(installed);
    auto& memory = installed.memory;
    expect(memory.read_u32_le(retail::kHubSelectionTableMods) == 0U,
           "with no mods the eighth hub entry is left alone");
    const auto count = run_native(installed.options, memory, retail::kHubCountSite, {},
                                  {retail::kHubCountSite + 4U});
    expect(count.state.read_gpr_u32(kV0) == 7U, "the hub keeps seven entries");
    const auto sub = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                retail_stubs(), {retail::kDetailSwitchEnd}, {{kT0, 5U}});
    expect(sub.not_hooked, "sub-page 5 is left to the retail branch");
    guest::R5900State state;
    state.set_pc(layout::kModsUpdateEntry);
    expect(installed.options.on_dispatch(state, memory) == Options::HookResult::NotHooked,
           "the MODS update entry is not hooked");
}

// CAMERA page: the hub's seventh row opens it, its one row is the game's
// slider, left and right step the FOV, and the host turns the level into the
// setting.
void test_camera_page() {
    Installed installed;
    make_installed(installed);
    auto& memory = installed.memory;
    using runtime::native_camera_fov_from_level;
    expect(runtime::native_camera_fov_level(installed.settings) == 25U &&
               native_camera_fov_from_level(25U) == 0U && native_camera_fov_from_level(26U) == 92U &&
               runtime::native_camera_fov_degrees(0U) == 40U &&
               runtime::native_camera_fov_degrees(50U) == 140U,
           "level 25 is the game's own 90 degrees, each step 2 degrees, 40 to 140");
    expect(runtime::native_camera_slider_cells(0U) == 0U &&
               runtime::native_camera_slider_cells(25U) == 10U &&
               runtime::native_camera_slider_cells(26U) == 10U &&
               runtime::native_camera_slider_cells(50U) == 20U,
           "the 20-cell bar shows the level in proportion");
    memory.write_u8(kScratch + 80U, 0U);
    const auto init = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                 retail_stubs(), {retail::kDetailSwitchEnd},
                                 {{kT0, 6U}, {kS1, kScratch}});
    expect(init.stopped_at == retail::kDetailSwitchEnd, "sub-page 6 enters the CAMERA page");
    expect(read_u16(memory, layout::kCameraPage + 6U) == 2U &&
               read_u16(memory, layout::kCameraPage + 4U) == 0U &&
               memory.read_u8(layout::kCameraPage + 8U + 13U) == 7U &&
               memory.read_u8(layout::kCameraPage + 8U + 12U) == 10U,
           "the page opens on its slider, the first row, at the default level");
    expect(read_string(memory, memory.read_u32_le(layout::kCameraPage + 28U)) == u"FOV CORRECTION" &&
               memory.read_u8(layout::kCameraPage + 28U + 12U) == 2U &&
               memory.read_u32_le(layout::kCameraPage + 28U + 4U) == 1U,
           "the second row is FOV CORRECTION, Off / On, on by default");
    expect(read_string(memory, memory.read_u32_le(layout::kCameraPage + 8U)) == u"FIELD OF VIEW",
           "the row label names the setting");
    expect(count_calls(init, 0x3BC4C0U) == 1U, "the slider level is shown with the slider routine");
    expect(count_calls(init, 0x1D7580U) == 0U, "the init leaves the marks to the first update");
    const auto first = run_native(installed.options, memory, layout::kCameraUpdateEntry, retail_stubs(),
                                  {kSentinel}, {{kA0, layout::kCameraPage}, {kRa, kSentinel}});
    std::vector<std::uint32_t> built;
    for (const auto& call : first.calls) {
        if (call.address == 0x2F9690U) {
            built.push_back(call.a0);
        }
    }
    expect(count_calls(first, 0x1D7580U) == 4U && built.size() == 4U &&
               memory.read_u32_le(layout::kCameraMarksLive) == 1U &&
               built[0] == memory.read_u32_le(layout::kCameraMarkNodes) && built[0] != 0U &&
               built[3] == memory.read_u32_le(layout::kCameraMarkNodes + 12U),
           "the slider marks are four game text nodes, each filled by the text builder");
    expect(read_string(memory, layout::kCameraMarkTexts) == u"90" &&
               read_string(memory, layout::kCameraMarkTexts + 16U) == u"40" &&
               read_string(memory, layout::kCameraMarkTexts + 32U) == u"90" &&
               read_string(memory, layout::kCameraMarkTexts + 48U) == u"140",
           "the marks read the angle, then the minimum, default and maximum angles");

    auto stubs = retail_stubs();
    write_u16(memory, layout::kCameraPage + 4U, 0U);
    press(memory, 32U);
    const auto step = run_native(installed.options, memory, layout::kCameraUpdateEntry, stubs,
                                 {kSentinel}, {{kA0, layout::kCameraPage}, {kRa, kSentinel}});
    expect(memory.read_u32_le(layout::kCameraLevel) == 26U, "right steps the slider");
    expect(count_calls(step, 0x2F9690U) == 1U && count_calls(step, 0x1D7580U) == 0U &&
               read_string(memory, layout::kCameraMarkTexts) == u"92",
           "a step rebuilds only the angle mark");
    const auto result = installed.options.service(memory, installed.settings);
    expect(result.settings_changed && installed.settings.field_of_view == 92U,
           "the host applies 92 degrees");

    write_u16(memory, layout::kCameraPage + 4U, 1U);
    press(memory, 32U);
    const auto toggle = run_native(installed.options, memory, layout::kCameraUpdateEntry, stubs,
                                   {kSentinel}, {{kA0, layout::kCameraPage}, {kRa, kSentinel}});
    const auto toggled = installed.options.service(memory, installed.settings);
    expect(count_calls(toggle, 0x3BC5B0U) == 1U && count_calls(toggle, 0x2F9690U) == 0U &&
               toggled.settings_changed && !installed.settings.fov_correction &&
               installed.settings.field_of_view == 92U,
           "a side press on FOV CORRECTION turns the camera correction off");

    const auto marks = runtime::native_camera_slider_marks(26U);
    expect(marks[0].text == "92" && marks[0].value && marks[1].x < marks[2].x &&
               marks[2].x < marks[3].x && marks[0].y < marks[1].y,
           "the angle sits above the minimum, default and maximum, which run left to right");

    const auto cleanup = run_native(installed.options, memory, layout::kCameraCleanupEntry, stubs,
                                    {kSentinel}, {{kA0, layout::kCameraPage}, {kRa, kSentinel}});
    expect(cleanup.stopped_at == kSentinel && count_calls(cleanup, 0x1D7560U) == 4U &&
               memory.read_u32_le(layout::kCameraMarksLive) == 0U &&
               memory.read_u32_le(layout::kCameraMarkNodes) == 0U,
           "leaving the page frees the four marks");
    const auto again = run_native(installed.options, memory, layout::kCameraCleanupEntry, stubs,
                                  {kSentinel}, {{kA0, layout::kCameraPage}, {kRa, kSentinel}});
    expect(again.stopped_at == kSentinel && again.calls.empty(),
           "a second cleanup has nothing to free");
}

void test_mods_install() {
    Installed installed;
    const auto mods = mods_state({{"Alpha", 1U}, {"Beta", 0U}, {"Gamma", 2U}});
    make_installed_with_mods(installed, mods);
    expect(installed.options.installed(), "installed with the MODS page");
    auto& memory = installed.memory;

    expect(memory.read_u32_le(retail::kHubSelectionTable) == 4U &&
               memory.read_u32_le(retail::kHubSelectionTableCamera) == 6U &&
               memory.read_u32_le(retail::kHubSelectionTableMods) == 5U,
           "entries six to eight open sub-pages 4, 6 and 5");
    const auto count = run_native(installed.options, memory, retail::kHubCountSite, {},
                                  {retail::kHubCountSite + 4U});
    expect(count.state.read_gpr_u32(kV0) == 8U, "the hub shows eight entries");
    const auto item = run_native(installed.options, memory, retail::kHubItemsArgument, {},
                                 {retail::kHubItemsArgument + 4U}, {{kS0, retail::kHub}});
    expect(item.state.read_gpr_u32(kA2) == layout::kHubItems &&
               read_string(memory, memory.read_u32_le(layout::kHubItems + 40U)) ==
                   u"PC OPTIONS" &&
               read_string(memory, memory.read_u32_le(layout::kHubItems + 48U)) == u"CAMERA" &&
               read_string(memory, memory.read_u32_le(layout::kHubItems + 56U)) == u"MODS",
           "the added hub rows are PC OPTIONS, CAMERA and MODS");

    expect(memory.read_u32_le(layout::kModsVtable + 8U) == layout::kModsUpdateEntry &&
               memory.read_u32_le(layout::kModsPage) == layout::kModsVtable,
           "the MODS page has its own vtable and update entry");
    expect(read_u16(memory, layout::kModsPage + 6U) == 4U, "three mods and the RESCAN row");
    expect(read_string(memory, memory.read_u32_le(layout::kModsPage + 8U)) == u"Alpha" &&
               read_string(memory, memory.read_u32_le(layout::kModsPage + 8U + 20U * 3U)) ==
                   u"RESCAN MODS",
           "the rows are the mods in order and the RESCAN action last");
    expect(memory.read_u32_le(layout::kModsValues + 0U) == 1U &&
               memory.read_u32_le(layout::kModsValues + 4U) == 0U &&
               memory.read_u32_le(layout::kModsValues + 8U) == 2U,
           "the mailbox holds each mod's load-order value");
    const std::uint32_t item0 = layout::kModsPage + 8U;
    expect(memory.read_u8(item0 + 12U) == 4U && memory.read_u8(item0 + 13U) == 4U,
           "two enabled mods offer Disabled plus three slots, as a cycling list");
    const std::uint32_t table = memory.read_u32_le(item0 + 8U);
    const std::uint32_t array = memory.read_u32_le(kTableObject + 12U);
    expect(read_string(memory, memory.read_u32_le(array + 4U * memory.read_u32_le(table))) ==
               u"Disabled",
           "a mod row's first value is Disabled");
}

void test_mods_page_init() {
    Installed installed;
    const auto mods = mods_state({{"Alpha", 1U}, {"Beta", 0U}});
    make_installed_with_mods(installed, mods);
    auto& memory = installed.memory;

    memory.write_u8(kScratch + 80U, 0U);
    const auto init = run_native(installed.options, memory, retail::kDetailSwitchSite,
                                 retail_stubs(), {retail::kDetailSwitchEnd},
                                 {{kT0, 5U}, {kS1, kScratch}});
    expect(init.stopped_at == retail::kDetailSwitchEnd && count_calls(init, 0x318530U) == 1U &&
               init.calls.front().a0 == layout::kModsPage,
           "sub-page 5 initialises the MODS page");
    expect(read_u16(memory, layout::kModsPage + 4U) == 0U &&
               read_u16(memory, layout::kModsPage + 6U) == 3U,
           "selection 0 of two mods and the RESCAN row");
    for (const Call& call : init.calls) {
        if (call.address == 0x3BE5A0U) {
            expect(call.a0 == 0x01E8961CU && call.a1 == layout::kModsPage + 4U &&
                       call.a2 == layout::kModsPage + 8U,
                   "the list widget is initialised with the MODS page's state");
        }
        if (call.address == 0x319C80U) {
            expect(call.a2 == 14U, "the MODS page uses the menu title style");
            const std::uint32_t array = memory.read_u32_le(kTableObject + 12U);
            expect(read_string(memory, memory.read_u32_le(array + 4U * call.a1)) == u"MODS",
                   "the title is the added MODS string");
        }
    }
    expect(init.calls.back().address == 0x38AD80U && init.calls.back().a0 == 0x01E894FCU,
           "the frame is finished last");
    // The MODS page is a sub-page of the Options hub, so it gets its header.
    memory.write_u32_le(retail::kCurrentPage, layout::kModsPage);
    const auto header = run_native(installed.options, memory, retail::kPageHeaderLookup, {},
                                   {0x00123450U}, {{kRa, 0x00123450U}});
    expect(header.stopped_at == 0x00123450U && header.state.read_gpr_u32(kV0) == 3U,
           "the MODS page gets the DRIVER DETAILS header");
}

void test_mods_page_update() {
    Installed installed;
    const auto mods = mods_state({{"Alpha", 1U}, {"Beta", 0U}});
    make_installed_with_mods(installed, mods);
    auto& memory = installed.memory;
    const auto stubs = retail_stubs();
    (void)run_native(installed.options, memory, retail::kDetailSwitchSite, stubs,
                     {retail::kDetailSwitchEnd}, {{kT0, 5U}, {kS1, kScratch}});

    // Right on the disabled second mod enables it at the first slot.
    write_u16(memory, layout::kModsPage + 4U, 1U);
    press(memory, 32U);
    const auto right = run_native(installed.options, memory, layout::kModsUpdateEntry, stubs, {},
                                  {{kA0, layout::kModsPage}});
    expect(right.stopped_at == kSentinel, "the MODS update returns to its caller");
    expect(memory.read_u32_le(layout::kModsValues + 4U) == 1U &&
               memory.read_u32_le(layout::kModsDirty) == 1U,
           "the mods mailbox holds the new slot and the dirty flag");
    expect(right.calls.size() == 3U && right.calls[0].address == 0x3BC5B0U &&
               right.calls[0].a1 == 1U && right.calls[0].a2 == 1U,
           "the widget, the feedback and the move sound run as on the PC page");
    const auto applied = installed.options.service(memory, installed.settings, &mods);
    expect(applied.mod_edit.has_value() && applied.mod_edit->row == 1U &&
               applied.mod_edit->value == 1U,
           "the host is told to enable the second mod at slot 1");
    expect(memory.read_u32_le(layout::kModsDirty) == 0U, "and the flag is cleared");

    // Left on the first mod disables it.
    write_u16(memory, layout::kModsPage + 4U, 0U);
    press(memory, 20U);
    (void)run_native(installed.options, memory, layout::kModsUpdateEntry, stubs, {},
                     {{kA0, layout::kModsPage}});
    expect(memory.read_u32_le(layout::kModsValues + 0U) == 0U, "left disables the mod");
    const auto disabled = installed.options.service(memory, installed.settings, &mods);
    expect(disabled.mod_edit.has_value() && disabled.mod_edit->row == 0U &&
               disabled.mod_edit->value == 0U,
           "the host is told to disable it");

    // The RESCAN row acts on a side press and on confirm instead of cycling.
    write_u16(memory, layout::kModsPage + 4U, 2U); // the row after the two mods
    press(memory, 32U);
    (void)run_native(installed.options, memory, layout::kModsUpdateEntry, stubs, {},
                     {{kA0, layout::kModsPage}});
    expect(memory.read_u32_le(layout::kModsRescan) == 1U, "right on RESCAN asks for a rescan");
    const auto rescanned = installed.options.service(memory, installed.settings, &mods);
    expect(rescanned.mods_rescan_requested && memory.read_u32_le(layout::kModsRescan) == 0U,
           "the host is asked once");
    press(memory, 68U);
    (void)run_native(installed.options, memory, layout::kModsUpdateEntry, stubs, {},
                     {{kA0, layout::kModsPage}});
    expect(memory.read_u32_le(layout::kModsRescan) == 1U, "confirm on RESCAN asks too");
    (void)installed.options.service(memory, installed.settings, &mods);
}

} // namespace

int main() {
    test_model();
    test_hook_addresses();
    test_install_and_service();
    test_hub_and_switch();
    test_page_init();
    test_page_update();
    test_pause_list();
    test_mods_page_absent();
    test_mods_install();
    test_camera_page();
    test_mods_page_init();
    test_mods_page_update();
    if (failures != 0) {
        std::cerr << failures << " native PC options test(s) failed\n";
        return 1;
    }
    std::cout << "native PC options tests passed\n";
    return 0;
}
