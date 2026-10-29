#include "takedown/platform/window.hpp"

#include "takedown/platform/input_bindings.hpp"
#include "takedown/platform/raw_input_protocol.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <utility>
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::platform {

bool host_experiment_flag_enabled(const char* const name) noexcept {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    return value[0] == '1' && value[1] == '\0';
}

bool host_default_on_flag_enabled(const char* const name) noexcept {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] != '0') {
        return true;
    }
    return value[1] != '\0';
}

bool host_compat_present_suppressed(const bool parallel_gs_only) noexcept {
    if (!parallel_gs_only) {
        return false;
    }
    const char* const value = std::getenv("TAKEDOWN_SKIP_COMPAT_PRESENT");
    if (value == nullptr || value[0] != '0') {
        return true;
    }
    return value[1] != '\0';
}

namespace {

std::mutex g_sdl_subsystem_mutex;

// Host timing: counts one call and records steady-clock nanoseconds on destruction.
class ScopedHostTiming {
  public:
    ScopedHostTiming(std::uint64_t* nanoseconds, std::uint64_t* calls) noexcept
        : nanoseconds_(nanoseconds) {
        if (nanoseconds_ != nullptr) {
            ++*calls;
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedHostTiming() {
        if (nanoseconds_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        *nanoseconds_ += static_cast<unsigned long long>(elapsed.count());
    }

    ScopedHostTiming(const ScopedHostTiming&) = delete;
    ScopedHostTiming& operator=(const ScopedHostTiming&) = delete;

  private:
    std::uint64_t* nanoseconds_;
    std::chrono::steady_clock::time_point start_{};
};

[[nodiscard]] std::runtime_error sdl_failure(const char* operation) {
    std::string message(operation);
    const char* error = SDL_GetError();
    if (error != nullptr) {
        if (*error != '\0') {
            message += ": ";
            message += error;
        }
    }
    return std::runtime_error(message);
}

RawGamepadState sample_gamepad(SDL_Gamepad* gamepad) {
    RawGamepadState state{};
    if (gamepad == nullptr || !SDL_GamepadConnected(gamepad)) {
        return state;
    }
    state.connected = true;
    for (std::size_t i = 0; i < kGamepadButtonCount; ++i) {
        state.buttons.set(i, SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i)) != 0);
    }
    for (std::size_t i = 0; i < kGamepadAxisCount; ++i) {
        state.axes[i] = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(i));
    }
    return state;
}

int checked_dimension(std::uint32_t dimension, const char* name) {
    if (static_cast<int>(dimension) <= 0) {
        throw std::invalid_argument(std::string("invalid ") + name + " dimension");
    }
    return static_cast<int>(dimension);
}

} // namespace


namespace {

// Describes one connected device as SDL reports it; slot is 1..4 for a gamepad slot, 0 otherwise.
inline ConnectedController describe_controller(const SDL_JoystickID id, const std::uint32_t slot) {
    ConnectedController controller;
    controller.id = id;
    controller.gamepad = SDL_IsGamepad(id);
    controller.slot = slot;
    const char* name = controller.gamepad ? SDL_GetGamepadNameForID(id) : SDL_GetJoystickNameForID(id);
    controller.name = name != nullptr && *name != '\0' ? name : "Unknown controller";
    std::array<char, 33> guid{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid.data(), static_cast<int>(guid.size()));
    controller.guid = guid.data();
    return controller;
}

} // namespace

namespace {

// Shared SDL video/gamepad subsystems: the first live lease starts them and the last one stops them.
int g_sdl_subsystem_leases = 0;
std::uint32_t g_sdl_subsystems_started_here = 0U;

const char* const kBuiltInGamepadMappings[] = {
    "03000000c82d00001d30000000000000,8BitDo Ultimate 2C Wired Controller,a:b0,b:b1,back:b10,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b6,leftstick:b13,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b14,righttrigger:b9,rightx:a3,righty:a4,start:b11,x:b3,y:b4,",
};

// Takes one lease on the shared SDL subsystems; the mutex is held only while the count changes.
class SdlSubsystemLease {
  public:
    SdlSubsystemLease() {
        std::lock_guard<std::mutex> lock(g_sdl_subsystem_mutex);
        if (g_sdl_subsystem_leases == 0) {
            SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            g_sdl_subsystems_started_here = ~SDL_WasInit(8224U) & 8224U;
            if (g_sdl_subsystems_started_here != 0U && !SDL_InitSubSystem(g_sdl_subsystems_started_here)) {
                g_sdl_subsystems_started_here = 0U;
                throw sdl_failure("SDL video/gamepad initialization failed");
            }
            for (const char* mapping : kBuiltInGamepadMappings) {
                SDL_AddGamepadMapping(mapping);
            }
        }
        ++g_sdl_subsystem_leases;
    }

    ~SdlSubsystemLease() {
        std::lock_guard<std::mutex> lock(g_sdl_subsystem_mutex);
        if (g_sdl_subsystem_leases == 0) {
            return;
        }
        --g_sdl_subsystem_leases;
        if (g_sdl_subsystem_leases == 0 && g_sdl_subsystems_started_here != 0U) {
            SDL_QuitSubSystem(g_sdl_subsystems_started_here);
            g_sdl_subsystems_started_here = 0U;
        }
    }

    SdlSubsystemLease(const SdlSubsystemLease&) = delete;
    SdlSubsystemLease& operator=(const SdlSubsystemLease&) = delete;
};

} // namespace


// RECOVERY: best-fit layout; see NOTES.txt for the offsets it was derived from.
struct HostWindow::Impl {
    SdlSubsystemLease subsystems_{};
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Joystick* inspected{};
    int texture_width{};
    int texture_height{};
    int test_offscreen{};
    std::uint32_t inspected_id{};
    HostWindowTiming timing{};
    bool open{true};
    bool stats_overlay{};
    bool vsync{};
    bool timing_enabled{};
    bool input_suppressed{};
    bool keyboard_suppressed{};
    bool debug_hotkeys_enabled{true};
    bool frame_step_toggle_hotkey_down{};
    bool frame_step_toggle_requested{};
    bool frame_advance_hotkey_down{};
    bool frame_advance_requested{};
    bool capture_hotkey_down{};
    bool capture_requested{};
    std::array<SDL_Gamepad*, kMaximumGamepads> gamepads{};
    std::array<std::uint32_t, kMaximumGamepads> gamepad_ids{};
    std::vector<ConnectedController> controllers;
    double display_aspect_ratio{};
    std::array<std::uint8_t, 8> layout_pad_{};
    std::vector<TextInputEvent> text_input;
    RawInputState raw_input{};
    InputBindings bindings{default_input_bindings()};
    GamepadFollow gamepad_follow{};
    KeyboardMouseSnapshot external_input{};
    std::string title;

    ~Impl() {
        if (inspected != nullptr) {
            SDL_CloseJoystick(inspected);
        }
        inspected = nullptr;
        inspected_id = 0U;
        for (std::size_t slot = 0; slot < kMaximumGamepads; ++slot) {
            raw_input.gamepads[slot] = {};
            gamepad_ids[slot] = 0U;
            if (gamepads[slot] != nullptr) {
                SDL_CloseGamepad(gamepads[slot]);
                gamepads[slot] = nullptr;
            }
        }
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }

    void open_best_available_gamepad() {
        int count = 0;
        SDL_JoystickID* const ids = SDL_GetGamepads(&count);
        for (std::size_t player = 0; player < kMaximumGamepads; ++player) {
            if (gamepads[player] != nullptr) {
                continue;
            }
            SDL_JoystickID fallback = 0;
            SDL_JoystickID preferred = 0;
            for (int index = 0; index < count; ++index) {
                const SDL_JoystickID id = ids[index];
                if (!SDL_IsGamepad(id)) {
                    continue;
                }
                const auto used = std::find(gamepad_ids.begin(), gamepad_ids.end(), id);
                if (used != gamepad_ids.end()) {
                    continue;
                }
                if (fallback == 0) {
                    fallback = id;
                }
                const bool preferred_layout = (SDL_GetGamepadVendorForID(id) == 11720 &&
                                               SDL_GetGamepadProductForID(id) == 12317) ||
                                              [&] {
                                                  const char* name = SDL_GetGamepadNameForID(id);
                                                  if (name == nullptr) {
                                                      return false;
                                                  }
                                                  std::string folded(name);
                                                  for (char& ch : folded) {
                                                      if (ch >= 'A' && ch <= 'Z') {
                                                          ch = static_cast<char>(ch + 32);
                                                      }
                                                  }
                                                  return folded.find("8bitdo") != std::string::npos &&
                                                         folded.find("ultimate 2c") != std::string::npos;
                                              }();
                if (preferred_layout) {
                    preferred = id;
                }
            }
            const SDL_JoystickID chosen = preferred != 0 ? preferred : fallback;
            if (chosen != 0 && gamepads[player] == nullptr) {
                SDL_Gamepad* opened = SDL_OpenGamepad(chosen);
                if (opened != nullptr) {
                    gamepads[player] = opened;
                    gamepad_ids[player] = SDL_GetGamepadID(opened);
                    raw_input.gamepads[player] = sample_gamepad(opened);
                }
            }
        }
        SDL_free(ids);
        refresh_controllers();
    }

    void refresh_controllers() {
        std::vector<ConnectedController> next;
        for (std::size_t slot = 0; slot < kMaximumGamepads; ++slot) {
            if (gamepads[slot] != nullptr) {
                next.push_back(describe_controller(gamepad_ids[slot], static_cast<std::uint32_t>(slot + 1U)));
            }
        }
        int count = 0;
        SDL_JoystickID* const joysticks = SDL_GetJoysticks(&count);
        for (int index = 0; index < count; ++index) {
            const SDL_JoystickID id = joysticks[index];
            if (std::find(gamepad_ids.begin(), gamepad_ids.end(), id) == gamepad_ids.end()) {
                next.push_back(describe_controller(id, 0U));
            }
        }
        SDL_free(joysticks);
        controllers = std::move(next);
        if (inspected_id != 0U && std::find(gamepad_ids.begin(), gamepad_ids.end(), inspected_id) == gamepad_ids.end()) {
            if (inspected != nullptr) {
                SDL_CloseJoystick(inspected);
            }
            inspected = nullptr;
            inspected_id = 0U;
        }
    }
};


HostControllerState HostWindow::controller_state(const std::size_t player) const noexcept {
    const Impl* impl = impl_.get();
    if (impl == nullptr || impl->input_suppressed || player > 1U) {
        return {};
    }
    RawInputState raw = raw_input_state();
    if (impl->keyboard_suppressed) {
        raw.keys.reset();
        raw.mouse_buttons = 0U;
    }
    const InputBindings followed = followed_bindings(impl->bindings, impl->gamepad_follow);
    return map_player_input(followed, player, raw);
}

bool HostWindow::controller_connected(const std::size_t player) const noexcept {
    const Impl* impl = impl_.get();
    if (impl == nullptr || player > 1U) {
        return false;
    }
    const InputBindings followed = followed_bindings(impl->bindings, impl->gamepad_follow);
    const PlayerBindings& binding = followed.players[player];
    if (binding.keyboard_and_mouse) {
        return true;
    }
    const std::uint32_t slot = binding.gamepad - 1U;
    return slot <= 3U && impl->raw_input.gamepads[slot].connected;
}

RawInputState HostWindow::raw_input_state() const {
    const Impl* impl = impl_.get();
    if (impl == nullptr) {
        return {};
    }
    RawInputState raw = impl->raw_input;
    raw.keys |= impl->external_input.keys;
    raw.mouse_buttons |= impl->external_input.mouse_buttons;
    return raw;
}

void HostWindow::set_input_suppressed(bool suppressed) {
    if (impl_ != nullptr) {
        impl_->input_suppressed = suppressed;
    }
}

void HostWindow::set_keyboard_suppressed(bool suppressed) {
    if (impl_ != nullptr) {
        impl_->keyboard_suppressed = suppressed;
    }
}

void HostWindow::set_external_input(const KeyboardMouseSnapshot& input) {
    if (impl_ != nullptr) {
        impl_->external_input = input;
    }
}

bool HostWindow::consume_capture_request() noexcept {
    if (impl_ == nullptr || !impl_->capture_requested) {
        return false;
    }
    impl_->capture_requested = false;
    return true;
}

bool HostWindow::consume_frame_step_toggle_request() noexcept {
    if (impl_ == nullptr || !impl_->frame_step_toggle_requested) {
        return false;
    }
    impl_->frame_step_toggle_requested = false;
    return true;
}

bool HostWindow::consume_frame_advance_request() noexcept {
    if (impl_ == nullptr || !impl_->frame_advance_requested) {
        return false;
    }
    impl_->frame_advance_requested = false;
    return true;
}

bool HostWindow::stats_overlay_enabled() const noexcept {
    return impl_ != nullptr && impl_->stats_overlay;
}

void HostWindow::set_stats_overlay(bool enabled) noexcept {
    if (impl_ != nullptr) {
        impl_->stats_overlay = enabled;
    }
}

bool HostWindow::vsync_enabled() const noexcept {
    return impl_ != nullptr && impl_->vsync;
}

void HostWindow::set_display_aspect_ratio(const double ratio) noexcept {
    if (impl_ != nullptr) {
        impl_->display_aspect_ratio = ratio > 0.0 ? ratio : 0.0;
    }
}

const std::vector<ConnectedController>& HostWindow::controllers() const {
    static const std::vector<ConnectedController> none;
    return impl_ != nullptr ? impl_->controllers : none;
}


namespace {

// Applies the render vsync request and reports whether SDL accepted it.
bool apply_render_vsync(SDL_Renderer* renderer, const int interval) noexcept {
    return SDL_SetRenderVSync(renderer, interval);
}

} // namespace

bool HostWindow::set_vsync(const bool enabled) {
    Impl* impl = impl_.get();
    if (impl == nullptr) {
        return false;
    }
    impl->vsync = enabled;
    if (impl->renderer == nullptr) {
        return false;
    }
    const int interval = enabled ? 1 : 0;
    const bool applied = apply_render_vsync(impl->renderer, interval);
    if (impl->timing_enabled) {
        impl->timing.vsync_requested = enabled;
        impl->timing.vsync_applied = applied;
        int observed = 0;
        impl->timing.vsync_observed = SDL_GetRenderVSync(impl->renderer, &observed);
        impl->timing.vsync_interval = impl->timing.vsync_observed ? observed : 0;
    }
    return applied;
}

bool HostWindow::set_fullscreen(const bool enabled) {
    Impl* impl = impl_.get();
    if (impl == nullptr || impl->window == nullptr || impl->test_offscreen != 0) {
        return false;
    }
    return SDL_SetWindowFullscreen(impl->window, enabled);
}

bool HostWindow::set_window_size(const std::uint32_t width, const std::uint32_t height) {
    Impl* impl = impl_.get();
    if (impl == nullptr || impl->window == nullptr || ((width == 0U) | (height == 0U)) ||
        ((static_cast<signed int>(width) < 0) | (static_cast<signed int>(height) < 0))) {
        return false;
    }
    return SDL_SetWindowSize(impl->window, static_cast<signed int>(width), static_cast<signed int>(height));
}


void HostWindow::present(std::optional<RgbaFrame> frame, std::string_view status) {
    Impl* impl = impl_.get();
    const ScopedHostTiming timing_scope(impl != nullptr && impl->timing_enabled ? &impl->timing.present_nanoseconds : nullptr,
                                        impl != nullptr ? &impl->timing.present_calls : nullptr);
    if (impl == nullptr || !impl->open || impl->renderer == nullptr) {
        return;
    }
    set_status(status);
    if (frame.has_value()) {
        if (frame->width == 0U || frame->height == 0U) {
            throw std::invalid_argument("RGBA frame dimensions must be non-zero");
        }
        if (frame->width > 536870911U || static_cast<signed int>(frame->height) < 0) {
            throw std::invalid_argument("RGBA frame dimensions exceed SDL limits");
        }
        const std::uint64_t byte_count = static_cast<unsigned long long>(frame->width) *
                                         static_cast<unsigned long long>(frame->height) * 4U;
        if (byte_count != frame->pixels.size()) {
            throw std::invalid_argument("RGBA frame byte count does not match its dimensions");
        }
        const int width = static_cast<int>(frame->width);
        const int height = static_cast<int>(frame->height);
        if (impl->texture == nullptr || width != impl->texture_width || height != impl->texture_height) {
            if (impl->texture != nullptr) {
                SDL_DestroyTexture(impl->texture);
                impl->texture = nullptr;
            }
            impl->texture = SDL_CreateTexture(impl->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                              width, height);
            if (impl->texture == nullptr) {
                throw sdl_failure("could not create RGBA presentation texture");
            }
            if (!SDL_SetTextureBlendMode(impl->texture, SDL_BLENDMODE_NONE)) {
                throw sdl_failure("could not configure RGBA presentation texture");
            }
            impl->texture_width = width;
            impl->texture_height = height;
        }
        const int pitch = width * 4;
        if (!SDL_UpdateTexture(impl->texture, nullptr, frame->pixels.data(), pitch)) {
            throw sdl_failure("could not upload RGBA diagnostic frame");
        }
    }

    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(impl->renderer, &output_width, &output_height)) {
        throw sdl_failure("could not query SDL render output size");
    }
    if (output_width <= 0 || output_height <= 0) {
        return;
    }
    if (!SDL_SetRenderDrawColor(impl->renderer, 9, 15, 24, 255)) {
        throw sdl_failure("could not set diagnostic draw color");
    }
    if (!SDL_RenderClear(impl->renderer)) {
        throw sdl_failure("could not clear diagnostic surface");
    }
    const float width = static_cast<float>(output_width);
    const float height = static_cast<float>(output_height);
    const SDL_FRect panel{width * 7.5e-2F, height * 7.5e-2F, width * 8.5e-1F, height * 8.5e-1F};
    if (!SDL_SetRenderDrawColor(impl->renderer, 18, 31, 45, 255)) {
        throw sdl_failure("could not set diagnostic draw color");
    }
    if (!SDL_RenderFillRect(impl->renderer, &panel)) {
        throw sdl_failure("could not draw diagnostic panel");
    }
    if (impl->texture != nullptr) {
        const float available_width = static_cast<float>(output_width);
        const float available_height = static_cast<float>(output_height);
        const float source_height = static_cast<float>(impl->texture_height);
        const float source_width = impl->display_aspect_ratio > 0.0
                                       ? source_height * static_cast<float>(impl->display_aspect_ratio)
                                       : static_cast<float>(impl->texture_width);
        const float scale = (available_height / source_height) < (available_width / source_width)
                                ? available_height / source_height
                                : available_width / source_width;
        const float width = source_width * scale;
        const float height = source_height * scale;
        const SDL_FRect destination{(available_width - width) * 5.0e-1F, (available_height - height) * 5.0e-1F,
                                    width, height};
        if (!SDL_RenderTexture(impl->renderer, impl->texture, nullptr, &destination)) {
            throw sdl_failure("could not render RGBA diagnostic frame");
        }
    }
    if (!SDL_RenderPresent(impl->renderer)) {
        throw sdl_failure("could not present SDL window");
    }
}


void HostWindow::set_status(std::string_view status) {
    Impl* impl = impl_.get();
    const ScopedHostTiming timing_scope(impl != nullptr && impl->timing_enabled ? &impl->timing.title_nanoseconds : nullptr,
                                        impl != nullptr ? &impl->timing.title_calls : nullptr);
    if (impl == nullptr || !impl->open || impl->window == nullptr) {
        return;
    }
    std::string title = impl->title;
    if (!status.empty()) {
        title += " - ";
        title += status;
    }
    if (!SDL_SetWindowTitle(impl->window, title.c_str())) {
        throw sdl_failure("could not update SDL window title");
    }
}


// open_best_available_gamepad: defined inside Impl (p02) because of the private nesting.

HostWindow::HostWindow(HostWindowConfig config) : impl_(new Impl()) {
    Impl* impl = impl_.get();
    const int width = checked_dimension(config.width, "window width");
    const int height = checked_dimension(config.height, "window height");
    if (config.title.empty()) {
        throw std::invalid_argument("window title must not be empty");
    }
    impl->title = config.title;
    impl->timing_enabled = config.timing_enabled;
    impl->stats_overlay = config.stats_overlay_enabled;
    impl->debug_hotkeys_enabled = config.debug_hotkeys_enabled;
    if (!config.gamepad_mappings_path.empty()) {
        for (const std::string& mapping : load_gamepad_mappings(config.gamepad_mappings_path)) {
            SDL_AddGamepadMapping(mapping.c_str());
        }
    }
    if (config.input_only) {
        impl->open_best_available_gamepad();
        return;
    }
    const char* const offscreen = std::getenv("TAKEDOWN_TEST_OFFSCREEN");
    impl->test_offscreen = offscreen != nullptr && offscreen[0] == '1' && offscreen[1] == '\0';
    impl->window = SDL_CreateWindow(impl->title.c_str(), width, height,
                                    impl->test_offscreen ? SDL_WindowFlags(8232) : SDL_WindowFlags(8224));
    if (impl->window == nullptr) {
        throw sdl_failure("could not create SDL window");
    }
    SDL_StartTextInput(impl->window);
    if (impl->test_offscreen != 0) {
        SDL_SetWindowFocusable(impl->window, false);
        SDL_SetWindowPosition(impl->window, -32000, -32000);
        SDL_ShowWindow(impl->window);
    }
    impl->renderer = SDL_CreateRenderer(impl->window, nullptr);
    if (impl->renderer == nullptr) {
        SDL_DestroyWindow(impl->window);
        impl->window = nullptr;
        throw sdl_failure("could not create SDL renderer");
    }
    impl->timing_enabled = config.timing_enabled;
    if (const char* renderer_name = SDL_GetRendererName(impl->renderer)) {
        impl->timing.renderer_name = renderer_name;
    }
    if (const char* driver = SDL_GetCurrentVideoDriver()) {
        impl->timing.video_driver = driver;
    }
    int interval = 0;
    if (SDL_GetRenderVSync(impl->renderer, &interval)) {
        impl->timing.vsync_observed = true;
        impl->timing.vsync_interval = interval;
    }
    impl->stats_overlay = config.stats_overlay_enabled;
    impl->open_best_available_gamepad();
}

HostWindow::~HostWindow() = default;

HostWindow::HostWindow(HostWindow&& other) noexcept : impl_(std::move(other.impl_)) {}

HostWindow& HostWindow::operator=(HostWindow&& other) noexcept {
    impl_.reset(other.impl_.release());
    return *this;
}


std::vector<TextInputEvent> HostWindow::consume_text_input() {
    if (impl_ != nullptr) {
        return std::exchange(impl_->text_input, std::vector<TextInputEvent>{});
    }
    return {};
}


namespace {

// The SDL error for a rejected layout, or a fixed message when SDL reports none.
const char* mapping_failure_message() noexcept {
    const char* error = SDL_GetError();
    return error != nullptr && *error != '\0' ? error : "SDL rejected the layout";
}

} // namespace

bool HostWindow::add_gamepad_mapping(const std::string& mapping, std::string& error) {
    if (impl_ == nullptr) {
        return false;
    }
    if (SDL_AddGamepadMapping(mapping.c_str()) < 0) {
        error = mapping_failure_message();
        return false;
    }
    impl_->refresh_controllers();
    return true;
}


void HostWindow::set_input_bindings(const InputBindings& bindings) {
    if (impl_ != nullptr) {
        impl_->bindings = bindings;
    }
}


HostWindowTiming HostWindow::timing() const {
    if (impl_ == nullptr) {
        return {};
    }
    HostWindowTiming snapshot = impl_->timing;
    snapshot.enabled = impl_->timing_enabled;
    return snapshot;
}


RawJoystickState HostWindow::joystick_state(const std::uint32_t id) {
    Impl* impl = impl_.get();
    if (impl == nullptr) {
        return {};
    }
    SDL_Joystick* joystick = nullptr;
    for (std::size_t slot = 0; slot < kMaximumGamepads; ++slot) {
        if (impl->gamepads[slot] != nullptr && impl->gamepad_ids[slot] == id) {
            joystick = SDL_GetGamepadJoystick(impl->gamepads[slot]);
        }
        if (joystick != nullptr) {
            break;
        }
    }
    if (joystick == nullptr) {
        if (impl->inspected_id != id) {
            if (impl->inspected != nullptr) {
                SDL_CloseJoystick(impl->inspected);
            }
            impl->inspected = nullptr;
            impl->inspected_id = 0U;
            if (id != 0U) {
                impl->inspected = SDL_OpenJoystick(id);
                if (impl->inspected != nullptr) {
                    impl->inspected_id = id;
                }
            }
        }
        joystick = impl->inspected;
    }
    if (joystick == nullptr || !SDL_JoystickConnected(joystick)) {
        return {};
    }
    RawJoystickState state;
    state.connected = true;
    state.buttons.assign(static_cast<std::size_t>(std::max(SDL_GetNumJoystickButtons(joystick), 0)), false);
    for (std::size_t i = 0; i < state.buttons.size(); ++i) {
        state.buttons[i] = SDL_GetJoystickButton(joystick, static_cast<int>(i));
    }
    state.axes.resize(static_cast<std::size_t>(std::max(SDL_GetNumJoystickAxes(joystick), 0)));
    for (std::size_t i = 0; i < state.axes.size(); ++i) {
        state.axes[i] = SDL_GetJoystickAxis(joystick, static_cast<int>(i));
    }
    state.hats.resize(static_cast<std::size_t>(std::max(SDL_GetNumJoystickHats(joystick), 0)));
    for (std::size_t i = 0; i < state.hats.size(); ++i) {
        state.hats[i] = SDL_GetJoystickHat(joystick, static_cast<int>(i));
    }
    return state;
}


bool HostWindow::pump_events() {
    Impl* impl = impl_.get();
    const ScopedHostTiming timing_scope(impl != nullptr && impl->timing_enabled ? &impl->timing.pump_nanoseconds : nullptr,
                                        impl != nullptr ? &impl->timing.pump_calls : nullptr);
    if (impl != nullptr && impl->open) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == 256 || event.type == 528) {
                impl->open = false;
                continue;
            }
            if (event.type == 768 || event.type == 769) {
                if (impl->debug_hotkeys_enabled) {
                    if (event.type == 768 && event.key.repeat == 0 && event.key.key == 1073741882) {
                        impl->stats_overlay = !impl->stats_overlay;
                    }
                    if (event.key.key == 1073741887) {
                        if (event.type == 768 && event.key.repeat == 0 && !impl->frame_step_toggle_hotkey_down) {
                            impl->frame_step_toggle_requested = true;
                            impl->frame_step_toggle_hotkey_down = true;
                        } else if (event.type == 769) {
                            impl->frame_step_toggle_hotkey_down = false;
                        }
                    }
                    if (event.key.key == 1073741888) {
                        if (event.type == 768 && event.key.repeat == 0 && !impl->frame_advance_hotkey_down) {
                            impl->frame_advance_requested = true;
                            impl->frame_advance_hotkey_down = true;
                        } else if (event.type == 769) {
                            impl->frame_advance_hotkey_down = false;
                        }
                    }
                    if (event.key.key == 1073741889) {
                        if (event.type == 768 && event.key.repeat == 0 && !impl->capture_hotkey_down) {
                            impl->capture_requested = true;
                            impl->capture_hotkey_down = true;
                        } else if (event.type == 769) {
                            impl->capture_hotkey_down = false;
                        }
                    }
                }
                std::uint32_t code = event.key.scancode;
                if (code == 0U) {
                    code = SDL_GetScancodeFromKey(event.key.key, nullptr);
                }
                if (code - 1U <= 510U) {
                    if (event.type == 768) {
                        impl->raw_input.keys.set(code);
                    } else {
                        impl->raw_input.keys.reset(code);
                    }
                }
                if (event.type == 768 && event.key.repeat == 0 && event.key.key == 8) {
                    impl->text_input.push_back(TextInputEvent{TextInputKind::Backspace, {}});
                }
                if (event.type == 768 && event.key.repeat == 0 && event.key.key == 13) {
                    impl->text_input.push_back(TextInputEvent{TextInputKind::Submit, {}});
                }
                if (event.type == 768 && event.key.repeat == 0 && event.key.key == 27) {
                    impl->text_input.push_back(TextInputEvent{TextInputKind::Cancel, {}});
                }
                continue;
            }
            if (event.type == 771) {
                if (event.text.text != nullptr && impl->text_input.size() < 256U) {
                    std::string text(event.text.text, std::min<std::size_t>(std::strlen(event.text.text), 256U));
                    impl->text_input.push_back(TextInputEvent{TextInputKind::Text, std::move(text)});
                }
                continue;
            }
            if (event.type == 527) {
                impl->raw_input.mouse_buttons = 0U;
                impl->frame_step_toggle_hotkey_down = false;
                impl->frame_advance_hotkey_down = false;
                impl->capture_hotkey_down = false;
                continue;
            }
            if (event.type == 1025 || event.type == 1026) {
                const std::uint32_t button = event.button.button;
                if (button >= 1U && button <= 7U) {
                    const auto mask = static_cast<std::uint8_t>(1U << (button - 1U));
                    if (event.type == 1025) {
                        impl->raw_input.mouse_buttons = static_cast<std::uint8_t>(impl->raw_input.mouse_buttons | mask);
                    } else {
                        impl->raw_input.mouse_buttons = static_cast<std::uint8_t>(impl->raw_input.mouse_buttons & ~mask);
                    }
                }
                continue;
            }
            if (event.type == 1619) {
                impl->open_best_available_gamepad();
                continue;
            }
            if (event.type == 1620) {
                for (std::size_t player = 0; player < kMaximumGamepads; ++player) {
                    if (event.gdevice.which == impl->gamepad_ids[player]) {
                        if (impl->gamepads[player] != nullptr) {
                            SDL_CloseGamepad(impl->gamepads[player]);
                            impl->gamepads[player] = nullptr;
                        }
                    }
                }
                impl->open_best_available_gamepad();
                continue;
            }
            if (event.type == 1542) {
                if (event.jdevice.which == impl->inspected_id) {
                    if (impl->inspected != nullptr) {
                        SDL_CloseJoystick(impl->inspected);
                    }
                    impl->inspected = nullptr;
                    impl->inspected_id = 0U;
                }
                impl->refresh_controllers();
                continue;
            }
        }
        bool disconnected = false;
        for (std::size_t player = 0; player < kMaximumGamepads; ++player) {
            SDL_Gamepad* gamepad = impl->gamepads[player];
            if (gamepad != nullptr && !SDL_GamepadConnected(gamepad)) {
                SDL_CloseGamepad(gamepad);
                impl->gamepads[player] = nullptr;
                disconnected = true;
            }
        }
        if (disconnected) {
            impl->open_best_available_gamepad();
        }
        for (std::size_t player = 0; player < kMaximumGamepads; ++player) {
            impl->raw_input.gamepads[player] = sample_gamepad(impl->gamepads[player]);
        }
        update_gamepad_follow(impl->gamepad_follow, impl->bindings, impl->raw_input, SDL_GetTicks());
    }
    return impl != nullptr && impl->open;
}


} // namespace takedown::platform
