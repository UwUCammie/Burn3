#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/retail_reference_timer.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime {

// Native variable frame rate for SLUS-21050.
//
// Retail simulates in fixed ticks of 1/60 s (races) or 1/30 s (frontend) and
// derives every game clock from `tick count x tick period`. Here each tick
// instead lasts as long as the previous displayed frame really took, so the
// game runs at whatever rate the host and display allow, including a rate that
// changes every frame, at unchanged game speed.
//
// Three retail pieces assume a constant period and are replaced natively: the
// timer class (0x001d3a60), the frame-skip controller (0x00376570) and the
// tick/swap-interval setters. A few gameplay routines that count frames
// instead of time are corrected at their entry.

struct Slus21050TickPlannerStatistics {
    std::uint64_t frames{};
    std::uint64_t ticks{};
    std::uint64_t split_frames{};    // frames simulated as more than one tick
    std::uint64_t unplanned_ticks{}; // ticks outside the per-frame loop (loading, transitions)
    double dropped_seconds{};        // real time not simulated because a frame was too long
    double shortest_tick_seconds{};
    double longest_tick_seconds{};
};

// Turns guest-clock time into simulation ticks. One tick per frame, except
// that a frame longer than the largest step retail ever takes in that mode is
// split into equal ticks, as retail's own frame-skip does.
class Slus21050TickPlanner {
  public:
    static constexpr std::uint32_t kMaximumTicksPerFrame = 8U;
    static constexpr double kMinimumTickSeconds = 1.0 / 1000.0;
    // Retail's PAL build steps 1.2 times longer than NTSC in every mode, so
    // steps up to that are known to be stable.
    static constexpr double kMaximumStepScale = 1.2;

    // Retail tick rate of the current mode: 60 in races, 30 in the frontend.
    void set_retail_rate(std::uint32_t ticks_per_second) noexcept;

    // Once per frame, where retail asks its frame-skip controller. Returns the
    // number of ticks the coming frame simulates. `forced_ticks` is used while
    // the game is paused, when retail always runs its minimum.
    [[nodiscard]] std::uint32_t plan_frame(std::uint64_t guest_cycles,
                                           std::optional<std::uint32_t> forced_ticks = {}) noexcept;
    // At every tick increment. Returns the tick's duration in seconds.
    double begin_tick(std::uint64_t guest_cycles) noexcept;

    [[nodiscard]] std::uint32_t retail_rate() const noexcept { return retail_rate_; }
    [[nodiscard]] double maximum_step_seconds() const noexcept {
        return kMaximumStepScale / static_cast<double>(retail_rate_);
    }
    [[nodiscard]] double tick_seconds() const noexcept { return tick_seconds_; }
    [[nodiscard]] double game_clock_seconds() const noexcept { return game_clock_seconds_; }
    // Ticks in the frame being simulated; 1 outside the per-frame loop.
    [[nodiscard]] std::uint32_t frame_ticks() const noexcept { return frame_ticks_; }
    [[nodiscard]] const Slus21050TickPlannerStatistics& statistics() const noexcept {
        return statistics_;
    }

  private:
    [[nodiscard]] double measure(std::uint64_t guest_cycles) noexcept;

    std::uint32_t retail_rate_{60U};
    std::optional<std::uint64_t> last_measure_cycles_;
    double bank_seconds_{};
    double planned_tick_seconds_{};
    std::uint32_t planned_ticks_remaining_{};
    std::uint32_t frame_ticks_{1U};
    double tick_seconds_{1.0 / 60.0};
    double game_clock_seconds_{};
    Slus21050TickPlannerStatistics statistics_{};
};

// Host-side continuation of one guest timer object: its elapsed time is kept
// as a running sum, because `whole ticks x period` only works for a constant
// period.
struct Slus21050NativeTimerClock {
    double elapsed_seconds{};
    double last_game_clock_seconds{};
};

// One call of the retail timer update (0x001d3a60) with a tick that lasted
// `tick_seconds`. The integer fields follow retail exactly; step and elapsed
// follow the game clock. Returns false, changing nothing, for a state retail
// itself would mishandle (a subdivision count below one).
[[nodiscard]] bool update_slus_21050_native_timer(RetailReferenceTimerState& state,
                                                  Slus21050NativeTimerClock& clock,
                                                  std::uint32_t tick,
                                                  double game_clock_seconds,
                                                  double tick_seconds);

enum class Slus21050FrameRateHookResult : std::uint8_t {
    NotHooked, // not one of this module's addresses, or left to the guest
    Continue,  // state adjusted; the guest code at this address still runs
    Replaced,  // the guest function was performed natively and has returned
};

struct Slus21050FrameRateStatistics {
    std::uint64_t set_tick_rate_calls{};
    std::uint64_t increment_tick_calls{};
    std::uint64_t timer_updates{};
    std::uint64_t timer_updates_left_to_guest{};
    std::uint64_t timer_resets{};
    std::uint64_t frame_skip_computes{};
    std::uint64_t swap_interval_overrides{};
    std::uint64_t per_frame_step_scalings{};
    std::uint64_t follow_camera_corrections{};
    std::uint64_t witness_violations{};
    std::size_t tracked_timers{};
};

class Slus21050VariableFrameRate {
  public:
    // Describes the first retail function that differs from the revision this
    // module was written against; nullopt when all of them match.
    [[nodiscard]] static std::optional<std::string>
    signature_mismatch(const guest::EeMemory& memory);
    // Every address on_dispatch acts at. The runner must reach its dispatch
    // boundary at each of them.
    [[nodiscard]] static std::span<const guest::GuestAddress> hook_addresses() noexcept;

    // Call with the CPU about to execute the instruction at state.pc() and the
    // guest clock at that boundary.
    Slus21050FrameRateHookResult on_dispatch(guest::R5900State& state, guest::EeMemory& memory,
                                             std::uint64_t guest_cycles);
    // Cheap per-VBlank check that nothing put the game back into a
    // one-flip-per-two-VBlanks mode behind this module's back.
    void on_vblank(const guest::EeMemory& memory) noexcept;

    [[nodiscard]] const Slus21050TickPlanner& planner() const noexcept { return planner_; }
    [[nodiscard]] Slus21050FrameRateStatistics statistics() const noexcept;

  private:
    struct TrackedTimer {
        guest::GuestAddress address{};
        Slus21050NativeTimerClock clock{};
    };

    [[nodiscard]] Slus21050NativeTimerClock& timer_clock(guest::GuestAddress address,
                                                        const guest::EeMemory& memory);
    Slus21050FrameRateHookResult update_timer(guest::R5900State& state, guest::EeMemory& memory);

    Slus21050TickPlanner planner_{};
    std::vector<TrackedTimer> timers_;
    Slus21050FrameRateStatistics statistics_{};
};

} // namespace takedown::runtime
