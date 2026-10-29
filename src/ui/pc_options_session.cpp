#include "takedown/ui/pc_options_session.hpp"

#include "takedown/config/settings_store.hpp"
#include "takedown/platform/parallel_gs_stream.hpp"
#include "takedown/presentation/graphics_options.hpp"

#include <string>
#include <system_error>
#include <utility>

namespace takedown::ui {

namespace {

platform::ParallelGsOverlayStyle overlay_style(const OptionsMenuLineStyle style) noexcept {
    switch (style) {
    case OptionsMenuLineStyle::Title:
        return platform::ParallelGsOverlayStyle::Title;
    case OptionsMenuLineStyle::Focused:
        return platform::ParallelGsOverlayStyle::Focused;
    case OptionsMenuLineStyle::Unavailable:
        return platform::ParallelGsOverlayStyle::Unavailable;
    case OptionsMenuLineStyle::Hint:
        return platform::ParallelGsOverlayStyle::Hint;
    case OptionsMenuLineStyle::Body:
    default:
        return platform::ParallelGsOverlayStyle::Body;
    }
}

bool any_button(const OptionsMenuButtons& buttons) noexcept {
    return buttons.up || buttons.down || buttons.left || buttons.right || buttons.confirm ||
           buttons.cancel || buttons.select || buttons.start;
}

} // namespace

PcOptionsSession::PcOptionsSession(PcOptionsSessionConfig config)
    : saved_settings_(config::normalized(std::move(config.settings))),
      settings_path_(std::move(config.settings_path)), enabled_(config.enabled) {
    menu_.settings = saved_settings_.graphics;
    menu_.environment = config.environment;
}

PcOptionsUpdate PcOptionsSession::update(const OptionsMenuButtons& buttons,
                                         const bool toggle_requested) {
    PcOptionsUpdate result{};
    if (!enabled_) {
        return result;
    }

    const OptionsMenuInput input = edges_.update(buttons);

    if (!open_) {
        if (release_pending_ && !any_button(buttons)) {
            release_pending_ = false;
        }
        if (toggle_requested || edges_.shortcut_pressed()) {
            open_ = true;
            release_pending_ = false;
            result.opened = true;
        }
        return result;
    }

    if (toggle_requested || edges_.shortcut_pressed()) {
        return close();
    }

    const config::GraphicsSettings before = menu_.settings;
    switch (apply_options_menu_input(menu_, input)) {
    case OptionsMenuEvent::Changed:
        result.settings_changed = !(menu_.settings == before);
        break;
    case OptionsMenuEvent::Closed:
        return close();
    case OptionsMenuEvent::None:
        break;
    }
    return result;
}

PcOptionsUpdate PcOptionsSession::close() {
    PcOptionsUpdate result = persist();
    open_ = false;
    release_pending_ = true;
    result.closed = true;
    return result;
}

PcOptionsUpdate PcOptionsSession::remember_game_audio_levels(const std::uint32_t music,
                                                             const std::uint32_t effects) {
    PcOptionsUpdate result{};
    const config::AudioSettings& audio = saved_settings_.audio;
    if (audio.game_music_level == music && audio.game_effects_level == effects) {
        return result;
    }

    config::AppSettings next = saved_settings_;
    next.audio.game_music_level = music;
    next.audio.game_effects_level = effects;
    if (settings_path_) {
        std::error_code error;
        if (!config::save_settings_file(*settings_path_, next, error)) {
            result.save_error = "could not save " + settings_path_->string() + ": " + error.message();
            return result;
        }
    }
    saved_settings_ = std::move(next);
    return result;
}

PcOptionsUpdate PcOptionsSession::apply_graphics(const config::GraphicsSettings& settings) {
    PcOptionsUpdate result{};
    if (menu_.settings == settings) {
        return result;
    }
    menu_.settings = settings;
    result = persist();
    result.settings_changed = true;
    return result;
}

PcOptionsUpdate PcOptionsSession::persist() {
    PcOptionsUpdate result{};
    if (menu_.settings == saved_settings_.graphics) {
        return result;
    }

    config::AppSettings next = saved_settings_;
    next.graphics = menu_.settings;
    if (settings_path_) {
        std::error_code error;
        if (!config::save_settings_file(*settings_path_, next, error)) {
            result.save_error = "could not save " + settings_path_->string() + ": " + error.message();
            return result;
        }
    }
    saved_settings_ = std::move(next);
    return result;
}

platform::ParallelGsHostPresentation PcOptionsSession::host_presentation() const {
    platform::ParallelGsHostPresentation presentation{};
    const config::GraphicsSettings& graphics = menu_.settings;
    presentation.widescreen = graphics.aspect_ratio == config::AspectRatio::Widescreen;
    presentation.vsync = graphics.vsync;
    presentation.display_mode = static_cast<platform::ParallelGsDisplayMode>(graphics.display_mode);
    const auto plan = presentation::supersampling_plan(graphics.render_scale, graphics.anti_aliasing);
    presentation.samples = plan.samples;
    presentation.spatial_aa = plan.spatial_aa;
    presentation.resolution_scale = plan.resolution_scale;
    presentation.dithering = graphics.dithering;
    presentation.high_resolution_ui = graphics.high_resolution_ui;
    presentation.fps_counter = graphics.fps_counter;
    // FSR travels beside the record's struct (whose layout release objects
    // share); the stream writer picks it up with this presentation.
    platform::ParallelGsUpscaling upscaling{};
    upscaling.fsr_preset = static_cast<std::uint8_t>(config::fsr_preset(graphics));
    upscaling.sharpness = static_cast<std::uint8_t>(config::fsr_sharpness(graphics) * 10U);
    platform::set_parallel_gs_upscaling(upscaling);
    if (open_) {
        for (const OptionsMenuLine& line : options_menu_lines(menu_)) {
            presentation.overlay.push_back({overlay_style(line.style), line.text});
        }
    }
    return presentation;
}

runtime::Slus21050GraphicsOptions::Settings PcOptionsSession::game_options() const noexcept {
    return {.widescreen = menu_.settings.aspect_ratio == config::AspectRatio::Widescreen,
            .motion_blur = menu_.settings.motion_blur,
            .bloom = menu_.settings.bloom};
}

} // namespace takedown::ui
