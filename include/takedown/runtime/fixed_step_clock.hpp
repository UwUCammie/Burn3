#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace takedown::runtime {

// Controls the guest simulation clock. Presentation can run at any frequency;
// only these fixed-size guest steps are exposed to gameplay code.
struct FixedStepRate {
    std::uint32_t numerator{60};
    std::uint32_t denominator{1};
};

struct FixedStepClockConfig {
    // Compatibility input for existing callers. Integral values resolve to an
    // exact N/1 rate. Use the rational fields for rates such as 60000/1001.
    double simulation_hz{60.0};
    std::uint32_t max_catch_up_steps{8};

    // Retained for source/configuration compatibility. Active elapsed time is
    // no longer clipped at this threshold; exclude a pause explicitly with
    // resync_to().
    double max_presentation_delta_seconds{0.25};

    // A nonzero numerator overrides simulation_hz. The constructor reduces
    // the pair and publishes the normalized values through config().
    std::uint32_t simulation_hz_numerator{};
    std::uint32_t simulation_hz_denominator{1};
};

struct FrameSchedule {
    // Run gameplay exactly this many times using simulation_step_seconds.
    std::uint32_t simulation_steps{};
    double simulation_step_seconds{};

    // Blend the last two simulated states by this value when presenting.
    // Always in the half-open range [0, 1).
    double interpolation_alpha{};

    // The wall-clock delta supplied for this presentation frame.
    double presentation_delta_seconds{};
    std::uint64_t presentation_delta_nanoseconds{};

    // Compatibility field. Normal advances never discard authoritative time;
    // pauses are excluded by resynchronizing the absolute host-time anchor.
    double discarded_seconds{};

    // Exact active wall time still queued after this bounded batch. When a
    // complete simulation step remains, presentation should be skipped while
    // zero-delta advances drain the backlog.
    std::uint64_t backlog_nanoseconds{};
    bool catch_up_limited{};
    bool presentation_allowed{true};
};

class FixedStepGuestClock {
  public:
    explicit FixedStepGuestClock(FixedStepClockConfig config = {});

    // Advances presentation time and returns deterministic simulation work.
    // The caller owns both loops: run simulation_steps fixed updates, then
    // render once using interpolation_alpha.
    [[nodiscard]] FrameSchedule advance(double presentation_delta_seconds);

    // Exact scheduling seam. The elapsed duration is active (unpaused) host
    // time and is retained even when this call reaches the catch-up cap.
    [[nodiscard]] FrameSchedule advance_nanoseconds(std::uint64_t presentation_delta_nanoseconds);
    [[nodiscard]] FrameSchedule advance(std::chrono::nanoseconds presentation_delta);

    // Absolute monotonic-time seam. The first call establishes an anchor.
    // Call resync_to() on resume so paused wall time is not added. Resyncing
    // preserves simulation ticks, fractional phase, and overload backlog.
    [[nodiscard]] FrameSchedule advance_to(std::uint64_t monotonic_nanoseconds);
    void resync_to(std::uint64_t monotonic_nanoseconds) noexcept;

    template <class Rep, class Period>
    [[nodiscard]] FrameSchedule advance(std::chrono::duration<Rep, Period> presentation_delta) {
        return advance(std::chrono::duration<double>(presentation_delta).count());
    }

    // Starts a diagnostic session at an absolute simulation boundary. The
    // supplied tick is already elapsed guest simulation work; no active time
    // is invented and the next step retains the exact rational phase that
    // would have followed that many steps from the zero origin.
    void seed(std::uint64_t simulation_tick) noexcept;

    // Seeds a point inside the interval following simulation_tick. The phase
    // must be strictly smaller than that interval; rejection is transactional.
    // This is used when an authoritative guest-cycle timestamp is sampled
    // after its discrete scheduler boundary rather than exactly on it.
    [[nodiscard]] bool seed_with_phase(
        std::uint64_t simulation_tick,
        std::uint64_t accumulator_nanoseconds) noexcept;

    void reset() noexcept;

    [[nodiscard]] const FixedStepClockConfig& config() const noexcept;
    [[nodiscard]] FixedStepRate simulation_rate() const noexcept;
    [[nodiscard]] double simulation_step_seconds() const noexcept;
    [[nodiscard]] double accumulator_seconds() const noexcept;
    [[nodiscard]] std::uint64_t accumulator_nanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t simulation_tick() const noexcept;
    [[nodiscard]] double simulation_time_seconds() const noexcept;

  private:
    struct StepInterval {
        std::uint64_t nanoseconds{};
        std::uint32_t next_phase{};
    };

    [[nodiscard]] StepInterval next_step_interval(std::uint32_t phase) const noexcept;

    FixedStepClockConfig config_;
    FixedStepRate simulation_rate_;
    double simulation_step_seconds_{};
    std::uint64_t step_base_nanoseconds_{};
    std::uint32_t step_remainder_{};
    std::uint32_t step_phase_{};
    std::uint64_t accumulator_nanoseconds_{};
    std::uint64_t simulation_tick_{};
    std::optional<std::uint64_t> monotonic_anchor_nanoseconds_;
    long double compatibility_nanosecond_remainder_{};
};

// Short name for call sites where the guest-clock context is already clear.
using FixedStepClock = FixedStepGuestClock;

} // namespace takedown::runtime
