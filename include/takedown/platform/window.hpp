#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "takedown/platform/raw_input_protocol.hpp"

namespace takedown::platform {

// Opt-in environment contract shared by the host-window experiments: a flag is
// enabled only when its variable exists and is EXACTLY "1". "0", "true", "01",
// " 1" and an empty value are all off, so an unset variable is off too. Reads
// no clock and never throws.
[[nodiscard]] bool host_experiment_flag_enabled(const char* name) noexcept;

// Default-on environment contract shared by promoted host features: a flag is
// disabled only when its variable exists and is EXACTLY "0". An unset variable
// and every other value enable the feature. A null or empty variable name is
// invalid and returns false. Reads no clock and never throws.
[[nodiscard]] bool host_default_on_flag_enabled(const char* name) noexcept;

// The compatibility HostWindow swap is redundant only when external
// paraLLEl-GS is the sole renderer. Suppress it by default on that route;
// TAKEDOWN_SKIP_COMPAT_PRESENT=0 restores the swap exactly, while every other
// rendering route keeps presenting regardless of the environment variable.
[[nodiscard]] bool host_compat_present_suppressed(bool parallel_gs_only) noexcept;

// A tightly packed, host-endian RGBA8 image. The renderer treats this as a
// diagnostic presentation surface; it is not a PS2 GS framebuffer abstraction.
struct RgbaFrame {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::span<const std::uint8_t> pixels{};
};

struct HostWindowConfig {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::string title{"Takedown Recomp"};
    // Starts with the on-screen stats overlay visible (the F1 key toggles it
    // at runtime). Host-only; it never reaches guest logic.
    bool stats_overlay_enabled{false};
    // Opt-in host-only timing instrumentation (default off). When off, no
    // steady_clock call is made anywhere in the HostWindow call paths.
    bool timing_enabled{false};
    // An external renderer owns the sole game window. Keep SDL event/gamepad
    // handling and relayed keyboard/mouse input without another window or GPU.
    bool input_only{false};
    bool debug_hotkeys_enabled{true};
    // Controller layouts made in game (gamepad_mappings.txt); empty for none.
    std::filesystem::path gamepad_mappings_path{};
};

// Opt-in host-presentation timing telemetry. Every counter counts actual calls
// made through the public HostWindow entry points; durations come from
// steady_clock around those calls only. `title_nanoseconds` is nested inside
// `present_nanoseconds` when the caller lets present() set the window status,
// so the two totals overlap by design and are reported separately.
// renderer_name/video_driver and the vsync fields are captured from SDL at
// window creation and on each set_vsync(); they are empty/false/zero whenever
// the instrumentation is disabled.
struct HostWindowTiming {
    bool enabled{false};
    std::uint64_t pump_calls{0};
    std::uint64_t pump_nanoseconds{0};
    std::uint64_t present_calls{0};
    std::uint64_t present_nanoseconds{0};
    std::uint64_t title_calls{0};
    std::uint64_t title_nanoseconds{0};
    bool vsync_requested{false};
    bool vsync_applied{false};
    bool vsync_observed{false};
    int vsync_interval{0};
    std::string renderer_name{};
    std::string video_driver{};
};

// Cross-platform input state exposed by the SDL presentation window. The guest
// bridge decides how these host controls map onto a console controller. Stick
// axes use SDL's signed convention: -32768 is left/up, zero is centered, and
// 32767 is right/down.
struct HostControllerState {
    bool dpad_up{};
    bool dpad_right{};
    bool dpad_down{};
    bool dpad_left{};
    bool start{};
    bool select{};
    bool cross{};
    bool circle{};
    bool square{};
    bool triangle{};
    bool left_shoulder{};
    bool right_shoulder{};
    bool left_trigger{};
    bool right_trigger{};
    std::int16_t left_stick_x{};
    std::int16_t left_stick_y{};
    // Normalized button pressures, 0..255, in native binding order: D-pad
    // up/right/down/left, start/select, confirm/throttle, handbrake, brake,
    // camera, left/right shoulder, left/right auxiliary trigger. A digital
    // source is full pressure; analogue sources retain their complete range.
    std::array<std::uint8_t, 14> button_pressure{};
};

[[nodiscard]] HostControllerState merge_host_controller_states(
    const HostControllerState&, const HostControllerState&) noexcept;

struct InputBindings;
struct RawInputState;
struct KeyboardMouseSnapshot;
struct ConnectedController;
struct RawJoystickState;

// Small SDL-backed host presentation surface. It deliberately knows nothing
// about guest memory or GS state: callers provide a selected decoded diagnostic image.
class HostWindow {
  public:
    explicit HostWindow(HostWindowConfig config = {});
    ~HostWindow();

    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;
    HostWindow(HostWindow&&) noexcept;
    HostWindow& operator=(HostWindow&&) noexcept;

    // Returns false after the user requests a close, or presses Escape.
    [[nodiscard]] bool pump_events();

    // Latest merged keyboard/gamepad controller state observed by pump_events().
    // Arrow keys are the D-pad; Z/X/A/S are Cross/Circle/Square/Triangle;
    // Q/E are L1/R1; 1/3 are L2/R2; Enter is Start and Backspace is Select.
    // SDL's standard gamepad layout maps South/East/West/North to
    // Cross/Circle/Square/Triangle. The left stick and analogue triggers are
    // sampled with conservative dead zones.
    [[nodiscard]] HostControllerState controller_state(std::size_t player = 0) const noexcept;
    [[nodiscard]] bool controller_connected(std::size_t player) const noexcept;
    [[nodiscard]] RawInputState raw_input_state() const;
    void set_input_suppressed(bool suppressed);
    void set_keyboard_suppressed(bool suppressed);
    void set_input_bindings(const InputBindings& bindings);
    void set_external_input(const KeyboardMouseSnapshot& snapshot);
    [[nodiscard]] std::vector<TextInputEvent> consume_text_input();

    // Every connected controller, gamepads in slot order first. Refreshed by
    // pump_events() when a device connects, disconnects or gains a layout.
    [[nodiscard]] const std::vector<ConnectedController>& controllers() const;
    // The device's own inputs, unmapped. Opens a joystick SDL has no layout
    // for while it is being read; a disconnected id reads as not connected.
    [[nodiscard]] RawJoystickState joystick_state(std::uint32_t id);
    // Gives SDL a layout mapping ("GUID,name,mapping"); a connected device
    // with that GUID becomes (or is remapped as) a gamepad.
    [[nodiscard]] bool add_gamepad_mapping(const std::string& mapping, std::string& error);

    // Sets a concise status in the native window title. The base title supplied
    // in HostWindowConfig is retained ahead of the status.
    void set_status(std::string_view status);

    // Uploads and presents a letterboxed RGBA image when provided. With no new
    // image it reuses the most recently uploaded texture (or just the diagnostic
    // background before the first frame), avoiding duplicate texture uploads.
    void present(std::optional<RgbaFrame> frame, std::string_view status);

    // Live presentation/window settings (host-only; never visible to guest
    // logic). These exist so stored AppSettings can be applied at runtime.
    // Each returns false when the backend rejected the change (for example
    // SDL_SetRenderVSync on a renderer without vsync support); the caller is
    // expected to report that honestly instead of assuming success.
    [[nodiscard]] bool set_vsync(bool enabled);
    [[nodiscard]] bool set_fullscreen(bool enabled);
    [[nodiscard]] bool set_window_size(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] bool vsync_enabled() const noexcept;
    // Width/height ratio the presented image is shown at (4:3 or 16:9),
    // independent of its pixel size; zero shows the pixels square.
    void set_display_aspect_ratio(double ratio) noexcept;

    // Opt-in timing telemetry for pump_events()/present()/set_status(). All
    // fields stay zero and no clock is read unless timing_enabled was set in
    // HostWindowConfig. This describes host work only; it never observes or
    // changes guest timing.
    [[nodiscard]] HostWindowTiming timing() const;

    // Host on-screen stats overlay visibility. The F1 key toggles it as events
    // are pumped; --stats-overlay starts the window with it on. The overlay is
    // composited by the caller (takedown/presentation/stats_overlay.hpp), not
    // by this class; this state only records whether it should be drawn.
    [[nodiscard]] bool stats_overlay_enabled() const noexcept;
    void set_stats_overlay(bool enabled) noexcept;

    // Consumes one host-only capture checkpoint requested with F8. A physical
    // key press produces at most one request even when SDL key repeat is
    // enabled; subsequent calls return false until F8 is released and pressed
    // again. The hotkey is never exposed to the guest controller mapping.
    [[nodiscard]] bool consume_capture_request() noexcept;

    // Consumes one host-only frame-step mode toggle requested with F6. A held
    // key produces only one request, which remains latched until consumed. F6
    // is never exposed to the guest controller mapping.
    [[nodiscard]] bool consume_frame_step_toggle_request() noexcept;

    // Consumes one host-only single-frame advance requested with F7. A held
    // key produces only one request, which remains latched until consumed. F7
    // is never exposed to the guest controller mapping.
    [[nodiscard]] bool consume_frame_advance_request() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::platform
