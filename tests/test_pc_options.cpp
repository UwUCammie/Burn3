// Unit tests for the PC graphics/performance options: settings parsing and
// validation, the options menu model, the frame-limit and supersampling
// policy, the renderer presentation record, and the native game options.

#include "takedown/config/app_settings.hpp"
#include "takedown/config/settings_store.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/platform/parallel_gs_host_presentation.hpp"
#include "takedown/platform/parallel_gs_stream.hpp"
#include "takedown/presentation/graphics_options.hpp"
#include "takedown/runtime/slus_21050_graphics_options.hpp"
#include "takedown/ui/options_menu.hpp"
#include "takedown/ui/pc_options_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace takedown;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path unique_path(const std::string_view suffix) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("takedown-pc-options-" + std::to_string(ticks) + "-" + std::string(suffix));
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void test_graphics_defaults() {
    const config::GraphicsSettings defaults{};
    expect(!defaults.presentation_fps_limit.has_value(), "frame-rate limit defaults to unlimited");
    expect(defaults.menu_fps_limit == std::optional<std::uint32_t>{240U},
           "menu/video limit defaults to 240");
    expect(defaults.aspect_ratio == config::AspectRatio::Standard && defaults.render_scale == 1U &&
               defaults.anti_aliasing == config::AntiAliasing::Off && defaults.motion_blur &&
               defaults.bloom && defaults.render_distance == config::RenderDistance::Original,
           "picture options default to the original game");
    expect(config::is_valid(defaults), "defaults validate");
}

void test_graphics_validation() {
    config::GraphicsSettings settings{};
    settings.render_scale = 7U;
    settings.menu_fps_limit = 0U;
    settings.aspect_ratio = static_cast<config::AspectRatio>(9);
    settings.anti_aliasing = static_cast<config::AntiAliasing>(9);
    settings.render_distance = static_cast<config::RenderDistance>(9);
    const auto issues = config::validate(settings);
    const auto has = [&](const std::string_view field) {
        return std::any_of(issues.begin(), issues.end(),
                           [&](const config::ValidationIssue& issue) {
                               return issue.field == field;
                           });
    };
    expect(has("graphics.render_scale") && has("graphics.menu_fps_limit") &&
               has("graphics.aspect_ratio") && has("graphics.anti_aliasing") &&
               has("graphics.render_distance"),
           "every invalid new field is reported");
    const auto repaired = config::normalized(settings);
    expect(config::is_valid(repaired), "normalization repairs the new fields");
    expect(repaired.render_scale == 1U && !repaired.menu_fps_limit.has_value(),
           "an invalid scale falls back to 1 and an invalid menu cap to unlimited");
}

void test_settings_file_round_trip() {
    const auto directory = unique_path("store");
    std::filesystem::create_directories(directory);
    const auto path = directory / "settings.ini";

    config::AppSettings settings{};
    settings.graphics.aspect_ratio = config::AspectRatio::Widescreen;
    settings.graphics.render_scale = 2U;
    settings.graphics.anti_aliasing = config::AntiAliasing::Ssaa8x;
    settings.graphics.menu_fps_limit.reset();
    settings.graphics.presentation_fps_limit = 144U;
    settings.graphics.render_distance = config::RenderDistance::Far;
    settings.graphics.motion_blur = false;
    settings.graphics.bloom = false;
    settings.graphics.dithering = false;
    settings.graphics.field_of_view = 115U;
    settings.graphics.fov_correction = false;
    std::error_code error;
    expect(config::save_settings_file(path, settings, error), "settings save");
    const auto loaded = config::load_settings_file(path);
    expect(loaded.diagnostics.empty(), "a saved file loads without diagnostics");
    expect(loaded.settings == settings, "every PC option survives a save/load round trip");

    // A version 1 file written before these keys existed keeps working and
    // gets the new defaults.
    write_text(path, "version = 1\n[graphics]\nvsync = false\nframe_rate = variable\n");
    const auto old = config::load_settings_file(path);
    expect(old.version_supported && old.diagnostics.empty(), "an older file loads cleanly");
    expect(!old.settings.graphics.vsync &&
               old.settings.graphics.menu_fps_limit == std::optional<std::uint32_t>{240U} &&
               old.settings.graphics.aspect_ratio == config::AspectRatio::Standard,
           "an older file keeps its values and gains the new defaults");

    write_text(path, "version = 1\n[graphics]\naspect_ratio = widescreen\nanti_aliasing = 16X\n"
                     "menu_fps_limit = none\nrender_scale = 9\nmotion_blur = maybe\n");
    const auto odd = config::load_settings_file(path);
    expect(odd.settings.graphics.aspect_ratio == config::AspectRatio::Widescreen &&
               odd.settings.graphics.anti_aliasing == config::AntiAliasing::Ssaa16x &&
               !odd.settings.graphics.menu_fps_limit.has_value(),
           "aliases and case-insensitive values are accepted");
    expect(odd.settings.graphics.render_scale == 1U, "an out-of-range scale is repaired");
    expect(odd.settings.graphics.motion_blur && odd.diagnostics.size() == 1U &&
               odd.diagnostics.front().key == "graphics.motion_blur",
           "a malformed value is reported and keeps its default");
    write_text(path, "version = 1\n[graphics]\nrender_scale = 400%\n");
    expect(config::load_settings_file(path).settings.graphics.render_scale == 4U,
           "a render scale may be written as a percentage");
    write_text(path, "version = 1\n[graphics]\nrender_scale = 800%\n");
    expect(config::load_settings_file(path).settings.graphics.render_scale == 1U,
           "a scale beyond the renderer's 400% falls back to 100%");
    std::filesystem::remove_all(directory, error);
}

void test_frame_rate_policy() {
    config::GraphicsSettings settings{};
    expect(!presentation::effective_frame_rate_limit(settings, false).has_value(),
           "races are unlimited by default");
    expect(presentation::effective_frame_rate_limit(settings, true) ==
               std::optional<std::uint32_t>{240U},
           "menus and videos are capped at 240 by default");
    settings.presentation_fps_limit = 60U;
    expect(presentation::effective_frame_rate_limit(settings, true) ==
               std::optional<std::uint32_t>{60U},
           "the lower of the two caps applies in menus");
    settings.menu_fps_limit.reset();
    expect(presentation::effective_frame_rate_limit(settings, true) ==
               std::optional<std::uint32_t>{60U},
           "an unlimited menu cap leaves the general cap in force");
}

void test_supersampling_policy() {
    using config::AntiAliasing;
    using presentation::SupersamplingPlan;
    using presentation::supersampling_plan;
    expect(supersampling_plan(1U, AntiAliasing::Off) == SupersamplingPlan{1U, 1U},
           "original picture renders one sample");
    expect(supersampling_plan(1U, AntiAliasing::Ssaa2x) == SupersamplingPlan{2U, 1U} &&
               supersampling_plan(1U, AntiAliasing::Ssaa4x) == SupersamplingPlan{4U, 1U} &&
               supersampling_plan(1U, AntiAliasing::Ssaa8x) == SupersamplingPlan{8U, 1U} &&
               supersampling_plan(1U, AntiAliasing::Ssaa16x) == SupersamplingPlan{16U, 1U},
           "every anti-aliasing level at the original size has its own sample count");
    expect(supersampling_plan(2U, AntiAliasing::Off) == SupersamplingPlan{4U, 2U},
           "200% keeps four samples as pixels");
    expect(supersampling_plan(2U, AntiAliasing::Ssaa2x) == SupersamplingPlan{8U, 2U},
           "200% with 2x anti-aliasing averages two samples per pixel");
    expect(supersampling_plan(2U, AntiAliasing::Ssaa4x) == SupersamplingPlan{16U, 2U} &&
               supersampling_plan(2U, AntiAliasing::Ssaa16x) == SupersamplingPlan{16U, 2U},
           "200% with 4x or more averages four samples per pixel");
    expect(presentation::effective_anti_aliasing(2U, AntiAliasing::Ssaa16x) ==
               AntiAliasing::Ssaa4x,
           "the effective level at 200% is reported as 4x");
    expect(supersampling_plan(4U, AntiAliasing::Off) == SupersamplingPlan{16U, 4U} &&
               supersampling_plan(4U, AntiAliasing::Ssaa8x) == SupersamplingPlan{16U, 4U},
           "400% keeps all sixteen samples as pixels");
    expect(supersampling_plan(3U, AntiAliasing::Ssaa2x) == SupersamplingPlan{16U, 3U},
           "300% scans out the sixteen-sample grid at three times the size");
    expect(presentation::effective_anti_aliasing(4U, AntiAliasing::Ssaa8x) == AntiAliasing::Off,
           "no anti-aliasing is left at 400%");
    expect(presentation::render_scale_text(1U) == "100% (640x448)" &&
               presentation::render_scale_text(2U) == "200% (1280x896)" &&
               presentation::render_scale_text(3U) == "300% (1920x1344)" &&
               presentation::render_scale_text(4U) == "400% (2560x1792)" &&
               presentation::render_scale_text(6U) == "600% (3840x2688)",
           "the render scale shows its percentage and resolution");
    expect(supersampling_plan(5U, AntiAliasing::Off).samples == 64U &&
               supersampling_plan(6U, AntiAliasing::Off).samples == 64U &&
               supersampling_plan(6U, AntiAliasing::Off).resolution_scale == 6U,
           "500% and 600% use the 64-sample grid");
}

ui::OptionsMenu menu_at(const ui::OptionsMenuItem item) {
    ui::OptionsMenu menu;
    const auto items = ui::options_menu_items();
    menu.focus = static_cast<std::size_t>(std::find(items.begin(), items.end(), item) -
                                          items.begin());
    return menu;
}

void test_menu_navigation() {
    ui::OptionsMenu menu;
    const auto count = ui::options_menu_items().size();
    expect(ui::apply_options_menu_input(menu, {.move_up = true}) == ui::OptionsMenuEvent::None &&
               menu.focus == count - 1U,
           "focus wraps from the first row to the last");
    expect(ui::apply_options_menu_input(menu, {.move_down = true}) ==
                   ui::OptionsMenuEvent::None &&
               menu.focus == 0U,
           "focus wraps from the last row to the first");
    expect(ui::apply_options_menu_input(menu, {.cancel = true}) == ui::OptionsMenuEvent::Closed,
           "cancel closes the menu");
    auto close = menu_at(ui::OptionsMenuItem::Close);
    expect(ui::apply_options_menu_input(close, {.confirm = true}) ==
               ui::OptionsMenuEvent::Closed,
           "the Close row closes the menu");
}

void test_menu_values() {
    auto aspect = menu_at(ui::OptionsMenuItem::AspectRatio);
    expect(ui::apply_options_menu_input(aspect, {.move_right = true}) ==
                   ui::OptionsMenuEvent::Changed &&
               aspect.settings.aspect_ratio == config::AspectRatio::Widescreen,
           "Right selects widescreen");
    expect(ui::apply_options_menu_input(aspect, {.move_right = true}) ==
                   ui::OptionsMenuEvent::Changed &&
               aspect.settings.aspect_ratio == config::AspectRatio::Standard,
           "values wrap around");

    auto limit = menu_at(ui::OptionsMenuItem::FrameRateLimit);
    static_cast<void>(ui::apply_options_menu_input(limit, {.move_right = true}));
    expect(limit.settings.presentation_fps_limit == std::optional<std::uint32_t>{30U},
           "from Unlimited, Right steps to the lowest cap");
    static_cast<void>(ui::apply_options_menu_input(limit, {.move_left = true}));
    expect(!limit.settings.presentation_fps_limit.has_value(),
           "Left from the lowest cap returns to Unlimited");
    static_cast<void>(ui::apply_options_menu_input(limit, {.move_left = true}));
    expect(limit.settings.presentation_fps_limit == std::optional<std::uint32_t>{500U},
           "Left from Unlimited wraps to the highest cap");
    limit.settings.presentation_fps_limit = 100U;
    static_cast<void>(ui::apply_options_menu_input(limit, {.move_right = true}));
    expect(limit.settings.presentation_fps_limit == std::optional<std::uint32_t>{120U},
           "a hand-edited cap steps to the next listed one");

    auto blur = menu_at(ui::OptionsMenuItem::MotionBlur);
    expect(ui::apply_options_menu_input(blur, {.confirm = true}) ==
                   ui::OptionsMenuEvent::Changed &&
               !blur.settings.motion_blur,
           "Confirm changes a setting forward");

    auto defaults = menu_at(ui::OptionsMenuItem::RestoreDefaults);
    defaults.settings.bloom = false;
    defaults.settings.resolution = {1280U, 720U};
    expect(ui::apply_options_menu_input(defaults, {.confirm = true}) ==
                   ui::OptionsMenuEvent::Changed &&
               defaults.settings.bloom && defaults.settings.resolution.width == 1280U,
           "Restore defaults resets menu options and keeps the window size");
    expect(ui::apply_options_menu_input(defaults, {.confirm = true}) ==
               ui::OptionsMenuEvent::None,
           "restoring defaults twice changes nothing");
}

void test_menu_lines() {
    ui::OptionsMenu menu;
    menu.environment.supersampling_available = false;
    const auto lines = ui::options_menu_lines(menu);
    expect(lines.size() == ui::options_menu_items().size() + 2U,
           "title, one line per row and a hint");
    expect(lines[1].style == ui::OptionsMenuLineStyle::Focused &&
               lines[1].text.find("< 4:3 >") != std::string::npos,
           "the focused row shows its value between arrows");
    bool marked_unavailable = false;
    bool distance_and_quality_live = true;
    bool console_terms = false;
    for (const auto& line : lines) {
        if (line.text.find("Render resolution") != std::string::npos) {
            marked_unavailable = line.style == ui::OptionsMenuLineStyle::Unavailable &&
                                 line.text.find("Vulkan renderer only") != std::string::npos;
        }
        if (line.text.find("Render distance") != std::string::npos ||
            line.text.find("Model quality") != std::string::npos) {
            distance_and_quality_live =
                distance_and_quality_live &&
                line.style != ui::OptionsMenuLineStyle::Unavailable &&
                line.text.find("not available yet") == std::string::npos;
        }
        for (const std::string_view term : {"memory card", "PS2", "DualShock", "Cross", "Circle"}) {
            console_terms = console_terms || line.text.find(term) != std::string::npos;
        }
    }
    expect(marked_unavailable, "settings that do not work yet are labelled");
    expect(distance_and_quality_live, "render distance and model quality are live settings");
    expect(!console_terms, "menu text uses PC wording");
}

void test_menu_edges() {
    ui::OptionsMenuEdgeTracker tracker;
    static_cast<void>(tracker.update({.select = true, .start = true}));
    expect(!tracker.shortcut_pressed(), "a shortcut held before priming does not open the menu");
    static_cast<void>(tracker.update({}));
    static_cast<void>(tracker.update({.select = true}));
    expect(!tracker.shortcut_pressed(), "Select alone is not the shortcut");
    static_cast<void>(tracker.update({.select = true, .start = true}));
    expect(tracker.shortcut_pressed(), "Select then Start opens the menu");
    static_cast<void>(tracker.update({.select = true, .start = true}));
    expect(!tracker.shortcut_pressed(), "holding the shortcut opens it once");
    const auto edges = tracker.update({.down = true, .confirm = true});
    expect(edges.move_down && edges.confirm, "button presses become edges");
    const auto held = tracker.update({.down = true, .confirm = true});
    expect(!held.move_down && !held.confirm, "held buttons do not repeat");
}

void test_menu_composite() {
    constexpr std::uint32_t kWidth = 640U;
    constexpr std::uint32_t kHeight = 448U;
    std::vector<std::uint8_t> rgba(kWidth * kHeight * 4U, 0x80U);
    ui::composite_options_menu(rgba, kWidth, kHeight, ui::OptionsMenu{});
    expect(rgba[0] == 0x80U, "pixels outside the panel are untouched");
    expect(std::any_of(rgba.begin(), rgba.end(), [](std::uint8_t value) { return value == 0x20U; }),
           "the panel darkens the picture behind it");
    std::vector<std::uint8_t> tiny(4U * 4U * 4U, 0U);
    ui::composite_options_menu(tiny, 4U, 4U, ui::OptionsMenu{});
    ui::composite_options_menu(tiny, 64U, 64U, ui::OptionsMenu{});
    expect(true, "drawing clips to small or mismatched buffers");
}

void test_host_presentation_codec() {
    platform::ParallelGsHostPresentation presentation;
    presentation.widescreen = true;
    presentation.resolution_scale = 3U;
    presentation.dithering = false;
    presentation.vsync = false;
    presentation.samples = 16U;
    presentation.overlay = {{platform::ParallelGsOverlayStyle::Title, "Graphics"},
                            {platform::ParallelGsOverlayStyle::Focused, "> Aspect ratio"}};
    const auto payload = platform::encode_parallel_gs_host_presentation(presentation);
    platform::ParallelGsHostPresentation decoded;
    expect(platform::decode_parallel_gs_host_presentation(payload.data(), payload.size(),
                                                          decoded) &&
               decoded == presentation,
           "the presentation record round-trips");
    auto truncated = payload;
    truncated.pop_back();
    expect(!platform::decode_parallel_gs_host_presentation(truncated.data(), truncated.size(),
                                                           decoded),
           "a truncated record is rejected");
    auto bad_samples = payload;
    bad_samples[2] = 3U;
    expect(!platform::decode_parallel_gs_host_presentation(bad_samples.data(),
                                                           bad_samples.size(), decoded),
           "an invalid sample count is rejected");
    platform::ParallelGsHostPresentation long_text;
    long_text.overlay.assign(100U, {platform::ParallelGsOverlayStyle::Body, std::string(400U, 'x')});
    const auto clipped = platform::encode_parallel_gs_host_presentation(long_text);
    expect(platform::decode_parallel_gs_host_presentation(clipped.data(), clipped.size(),
                                                          decoded) &&
               decoded.overlay.size() == platform::kParallelGsHostPresentationMaximumLines &&
               decoded.overlay.front().text.size() ==
                   platform::kParallelGsHostPresentationMaximumLineBytes,
           "oversized overlays are clipped, never rejected");
}

void test_stream_host_presentation() {
    const auto plain_path = unique_path("plain.raw");
    const auto host_path = unique_path("host.raw");
    const auto plain_digest = unique_path("plain.digest");
    const auto host_digest = unique_path("host.digest");
    {
        platform::ParallelGsRawStreamWriter plain(plain_path);
        plain.record_digest(plain_digest);
        plain.write_vblank(0U, {});
        plain.write_vblank(1U, {});
        plain.close();
    }
    std::uint64_t encoded = 0U;
    {
        platform::ParallelGsRawStreamWriter host(host_path);
        host.record_digest(host_digest);
        host.set_host_presentation({}); // the renderer's default: nothing to send
        host.write_vblank(0U, {});
        platform::ParallelGsHostPresentation widescreen;
        widescreen.widescreen = true;
        host.set_host_presentation(widescreen);
        host.set_host_presentation(widescreen);
        host.write_vblank(1U, {});
        host.set_host_presentation(widescreen); // unchanged: nothing more
        host.close();
        encoded = host.statistics().encoded_bytes;
    }
    const auto plain = read_bytes(plain_path);
    const auto host = read_bytes(host_path);
    platform::ParallelGsHostPresentation widescreen;
    widescreen.widescreen = true;
    const auto record = platform::encode_parallel_gs_host_presentation(widescreen);
    expect(host.size() == plain.size() + 5U + record.size() && encoded == host.size(),
           "one host record is written, only for a changed state");
    const auto vblank_bytes = plain.size() / 2U;
    expect(host.size() > vblank_bytes && host[vblank_bytes] == 5U &&
               std::equal(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(vblank_bytes),
                          host.begin()),
           "the record precedes the VBlank after the change");
    expect(read_bytes(plain_digest) == read_bytes(host_digest),
           "host records do not change the guest stream digest");
    std::error_code error;
    for (const auto& path : {plain_path, host_path, plain_digest, host_digest}) {
        std::filesystem::remove(path, error);
    }
}

void test_native_game_options() {
    guest::EeMemory memory;
    memory.write_u32_le(0x004e0a38U, 0x3f99999aU);
    memory.write_u32_le(0x004e0c7cU, 0x3fcf5c29U);
    memory.write_u32_le(0x004e0c3cU, 0x3faaaaabU);
    memory.write_u32_le(0x004e0a94U, 0x3f8aaaabU);
    memory.write_u8(0x00665f2aU, 1U);
    memory.write_u8(0x00665f28U, 1U);
    expect(!runtime::Slus21050GraphicsOptions::signature_mismatch(memory).has_value(),
           "the retail data matches");

    runtime::Slus21050GraphicsOptions options;
    options.on_vblank(memory);
    expect(options.statistics().writes == 0U && options.statistics().restores == 0U,
           "retail settings never write guest memory");

    options.configure({.widescreen = true, .motion_blur = false, .bloom = true});
    options.on_vblank(memory);
    expect(memory.read_u8(0x00665ecdU) == 1U && memory.read_u32_le(0x004e0a38U) == 0x3faaaaabU &&
               memory.read_u32_le(0x004e0c7cU) == 0x3fe38e39U &&
               memory.read_u8(0x00665f2aU) == 0U && memory.read_u8(0x00665f28U) == 1U,
           "widescreen and motion blur off are written");
    const auto writes = options.statistics().writes;
    options.on_vblank(memory);
    expect(options.statistics().writes == writes, "values already in place are not rewritten");
    memory.write_u8(0x00665f2aU, 1U); // the game resets its flag
    options.on_vblank(memory);
    expect(memory.read_u8(0x00665f2aU) == 0U, "a value the game resets is enforced again");

    options.configure({});
    options.on_vblank(memory);
    expect(memory.read_u8(0x00665ecdU) == 0U && memory.read_u32_le(0x004e0a38U) == 0x3f99999aU &&
               memory.read_u8(0x00665f2aU) == 1U,
           "returning to retail restores the retail values once");
    const auto restores = options.statistics().restores;
    memory.write_u8(0x00665f2aU, 0U);
    options.on_vblank(memory);
    expect(options.statistics().restores == restores && memory.read_u8(0x00665f2aU) == 0U,
           "after restoring, the game's own values are left alone");

    memory.write_u32_le(0x004e0a38U, 0x12345678U);
    expect(runtime::Slus21050GraphicsOptions::signature_mismatch(memory).has_value(),
           "a different revision is detected");
}

void test_session() {
    const auto directory = unique_path("session");
    const auto path = directory / "settings.ini";
    ui::PcOptionsSessionConfig config;
    config.settings.audio.music_volume = 0.5F;
    config.settings_path = path;
    ui::PcOptionsSession session{config};

    expect(!session.update({}, false).opened && !session.guest_input_blocked(),
           "the menu starts closed and the game gets input");
    expect(session.host_presentation() == platform::ParallelGsHostPresentation{},
           "default settings match the renderer's own defaults");
    expect(session.update({}, true).opened && session.guest_input_blocked(),
           "the hotkey opens the menu and blocks game input");
    expect(!session.host_presentation().overlay.empty(), "an open menu is sent as an overlay");

    const auto changed = session.update({.right = true}, false);
    expect(changed.settings_changed &&
               session.graphics().aspect_ratio == config::AspectRatio::Widescreen &&
               session.host_presentation().widescreen && session.game_options().widescreen,
           "a change applies live to the renderer record and the game options");
    expect(!std::filesystem::exists(path), "nothing is saved while the menu is open");

    static_cast<void>(session.update({}, false));
    const auto closed = session.update({.cancel = true}, false);
    expect(closed.closed && !closed.save_error.has_value() && !session.menu_open(),
           "Cancel closes the menu and saves");
    expect(session.guest_input_blocked(), "game input stays blocked while Cancel is held");
    static_cast<void>(session.update({}, false));
    expect(!session.guest_input_blocked(), "releasing every button returns input to the game");
    const auto loaded = config::load_settings_file(path);
    expect(loaded.settings.graphics.aspect_ratio == config::AspectRatio::Widescreen &&
               loaded.settings.audio.music_volume == 0.5F,
           "the saved file holds the change and keeps the other sections");

    static_cast<void>(session.update({.select = true}, false));
    expect(session.update({.select = true, .start = true}, false).opened,
           "Back/Select with Start opens the menu from a controller");
    expect(session.update({}, true).closed, "the hotkey closes it again");

    ui::PcOptionsSessionConfig disabled_config;
    disabled_config.enabled = false;
    ui::PcOptionsSession disabled{disabled_config};
    expect(!disabled.update({}, true).opened && !disabled.guest_input_blocked(),
           "a disabled session never opens");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    test_graphics_defaults();
    test_graphics_validation();
    test_settings_file_round_trip();
    test_frame_rate_policy();
    test_supersampling_policy();
    test_menu_navigation();
    test_menu_values();
    test_menu_lines();
    test_menu_edges();
    test_menu_composite();
    test_host_presentation_codec();
    test_stream_host_presentation();
    test_native_game_options();
    test_session();
    if (failures != 0) {
        std::cerr << failures << " PC options test(s) failed\n";
        return 1;
    }
    std::cout << "PC options tests passed\n";
    return 0;
}
