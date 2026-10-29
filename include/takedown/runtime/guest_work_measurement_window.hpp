#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// Immutable counters sampled at one existing EE instruction boundary. Audio
// FNV is intentionally retained only as a cumulative endpoint value: FNV-1a
// has no valid subtraction operation for an interval hash.
struct GuestWorkMeasurementSnapshot {
    std::size_t committed_steps{};
    std::uint64_t ee_cycles{};
    std::uint64_t vblank_starts{};
    std::uint64_t audio_frames{};
    std::uint64_t audio_fnv1a64{};
    std::uint64_t monotonic_nanoseconds{};
};

struct GuestWorkMeasurementActualCounts {
    std::size_t committed_steps{};
    std::uint64_t ee_cycles{};
    std::uint64_t vblank_starts{};
    std::uint64_t audio_frames{};
    std::uint64_t monotonic_nanoseconds{};
};

enum class GuestWorkMeasurementArmResult : std::uint8_t {
    Armed,
    IgnoredDisabled,
    RejectedAlreadyArmed,
};

enum class GuestWorkMeasurementStartResult : std::uint8_t {
    Started,
    RejectedNotArmed,
    RejectedAlreadyStarted,
    RejectedStepLimitOverflow,
    RejectedConfiguredStepLimit,
};

enum class GuestWorkMeasurementCompletionResult : std::uint8_t {
    Completed,
    RejectedNotStarted,
    RejectedAlreadyCompleted,
    BeforeStopStep,
    MissedStopStep,
};

[[nodiscard]] std::string_view to_string(GuestWorkMeasurementStartResult result) noexcept;
[[nodiscard]] std::string_view to_string(GuestWorkMeasurementCompletionResult result) noexcept;

// Host-only measurement state. A FIFO request can arm it once; it starts only
// when main reaches an existing instruction boundary. Its stop step is then
// used as the runner's ordinary instruction budget, preserving existing native
// and compiled-block budget clipping.
class GuestWorkMeasurementWindow {
  public:
    explicit GuestWorkMeasurementWindow(std::size_t guest_step_budget);

    [[nodiscard]] GuestWorkMeasurementArmResult arm() noexcept;
    [[nodiscard]] GuestWorkMeasurementStartResult start_at_instruction_boundary(
        const GuestWorkMeasurementSnapshot& snapshot,
        std::size_t configured_step_limit) noexcept;
    [[nodiscard]] GuestWorkMeasurementCompletionResult complete_at_instruction_boundary(
        const GuestWorkMeasurementSnapshot& snapshot) noexcept;
    // A window anchored to game-side events (controller poll ordinals) rather
    // than a step count ends at whatever boundary the runner reaches its final
    // event. The step budget then only bounds the wait for that event.
    [[nodiscard]] bool stop_at_instruction_boundary(std::size_t committed_steps) noexcept;

    [[nodiscard]] bool awaiting_arm() const noexcept;
    [[nodiscard]] bool armed() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] bool rejected() const noexcept;
    [[nodiscard]] std::size_t guest_step_budget() const noexcept;
    [[nodiscard]] std::optional<std::size_t> stop_step() const noexcept;
    [[nodiscard]] const std::optional<GuestWorkMeasurementSnapshot>& start_snapshot() const noexcept;
    [[nodiscard]] const std::optional<GuestWorkMeasurementSnapshot>& end_snapshot() const noexcept;
    [[nodiscard]] std::optional<GuestWorkMeasurementActualCounts> actual_counts() const noexcept;

  private:
    enum class State : std::uint8_t {
        AwaitingArm,
        Armed,
        Started,
        Completed,
        Rejected,
    };

    std::size_t guest_step_budget_{};
    State state_{State::AwaitingArm};
    std::optional<std::size_t> stop_step_;
    std::optional<GuestWorkMeasurementSnapshot> start_snapshot_;
    std::optional<GuestWorkMeasurementSnapshot> end_snapshot_;
};

// Owns optional measurement state without changing the disabled hot path.
// The runner records an input edge, begins at its next instruction boundary,
// and clips its pre-existing maximum step budget to the exact absolute stop.
class GuestWorkMeasurementController {
  public:
    explicit GuestWorkMeasurementController(std::optional<std::size_t> guest_step_budget);

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] bool rejected() const noexcept;
    [[nodiscard]] std::optional<std::size_t> active_step_limit() const noexcept;
    [[nodiscard]] std::uint64_t arm_edges_observed() const noexcept;
    [[nodiscard]] const GuestWorkMeasurementWindow* window() const noexcept;

    [[nodiscard]] GuestWorkMeasurementArmResult observe_input_edge() noexcept;
    [[nodiscard]] GuestWorkMeasurementStartResult start_at_instruction_boundary(
        const GuestWorkMeasurementSnapshot& snapshot,
        std::size_t configured_step_limit) noexcept;
    [[nodiscard]] GuestWorkMeasurementCompletionResult complete_at_instruction_boundary(
        const GuestWorkMeasurementSnapshot& snapshot) noexcept;

  private:
    std::optional<GuestWorkMeasurementWindow> window_;
    std::optional<std::size_t> active_step_limit_;
    std::uint64_t arm_edges_observed_{};
};

} // namespace takedown::runtime
