#pragma once

#include "takedown/platform/window.hpp"
#include "takedown/platform/raw_input_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace takedown::platform {

// Input packets travel in a second pipe (POSIX FIFO or Windows named pipe) from the focused
// parallel-gs-stream window back to the recomp runner. Every packet is a full
// state snapshot rather than a single key transition, so a reader that starts
// late or drains several packets at once still converges on the exact held
// controller state. Press/release edges for the host debugger hotkeys are
// retained while draining.
//
// Packet encoding (little-endian, byte packed):
//   u8('B'), u8('3'), u8('I'), u8('N'), u32(button mask)
inline constexpr std::size_t kParallelGsInputPacketBytes = 8U;

enum class ParallelGsInputButton : std::uint8_t {
    DpadUp = 0U,
    DpadRight = 1U,
    DpadDown = 2U,
    DpadLeft = 3U,
    Start = 4U,
    Select = 5U,
    Cross = 6U,
    Circle = 7U,
    Square = 8U,
    Triangle = 9U,
    LeftShoulder = 10U,
    RightShoulder = 11U,
    LeftTrigger = 12U,
    RightTrigger = 13U,
    FrameStepToggle = 16U,
    FrameAdvance = 17U,
    Capture = 18U,
    Quit = 19U,
    // Explicit host-to-runner measurement arm request. This previously unused
    // B3IN bit is not a keyboard mapping and never enters guest controller state.
    GuestWorkMeasurementStart = 20U,
    // Reserved: the renderer window used to send F10 here for the host-drawn
    // PC options menu, which is now a page of the game's own Options menu. The
    // bit is ignored and never reaches guest controller input.
    OptionsMenu = 21U,
};

[[nodiscard]] constexpr std::uint32_t parallel_gs_input_button_mask(
    const ParallelGsInputButton button) noexcept {
    return std::uint32_t{1U} << static_cast<std::uint8_t>(button);
}

// Nonblocking reader for an existing input FIFO or Windows named pipe. The FIFO is never created,
// removed, truncated, or replaced by this class. poll() may be called at the
// same high-frequency host-event cadence as HostWindow::pump_events().
class ParallelGsInputReader {
  public:
    explicit ParallelGsInputReader(const std::filesystem::path& fifo_path);
    ~ParallelGsInputReader();

    ParallelGsInputReader(const ParallelGsInputReader&) = delete;
    ParallelGsInputReader& operator=(const ParallelGsInputReader&) = delete;
    ParallelGsInputReader(ParallelGsInputReader&&) = delete;
    ParallelGsInputReader& operator=(ParallelGsInputReader&&) = delete;

    void poll();

    [[nodiscard]] HostControllerState controller_state() const noexcept;
    [[nodiscard]] bool has_raw_input() const noexcept;
    [[nodiscard]] KeyboardMouseSnapshot raw_input() const noexcept;
    // The mouse pointer over the game picture (B3MP), and the wheel steps
    // received since the last call.
    [[nodiscard]] PointerSnapshot pointer() const noexcept;
    [[nodiscard]] int consume_wheel() noexcept;
    [[nodiscard]] std::vector<TextInputEvent> consume_text_input();
    [[nodiscard]] bool consume_capture_request() noexcept;
    [[nodiscard]] bool consume_frame_step_toggle_request() noexcept;
    [[nodiscard]] bool consume_frame_advance_request() noexcept;
    // Consumes one explicit B3IN GuestWorkMeasurementStart edge. It is host
    // measurement control only, not guest controller input or a keyboard hotkey.
    [[nodiscard]] bool consume_guest_work_measurement_start_request() noexcept;
    [[nodiscard]] bool consume_close_request() noexcept;
    [[nodiscard]] std::uint64_t packet_count() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::platform
