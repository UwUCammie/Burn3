#pragma once

#include "takedown/platform/window.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace takedown::runtime {

// Absolute runner boundary used by the finite B3IR input timeline. Guest steps
// select the boundary; EE cycles are an independent replay invariant which
// catches an already-diverged execution before input reaches guest state.
struct GuestInputTimelineBoundary {
    std::size_t committed_steps{};
    std::uint64_t ee_cycles{};

    friend constexpr bool operator==(const GuestInputTimelineBoundary&,
                                     const GuestInputTimelineBoundary&) = default;
};

struct GuestInputTimelineAdvance {
    bool controller_state_changed{};
    bool measurement_start{};
    bool completion{};
};

enum class GuestInputTimelineFormat : std::uint32_t {
    B3IR1 = 1U,
    B3IR2 = 2U,
};

// Writes a new canonical B3IR text file. An S record's decimal mask uses
// bits 0..13 for D-pad up/right/down/left, Start, Select, Cross, Circle,
// Square, Triangle, L1, R1, L2, and R2; signed LX/LY follow it. The observer
// must be called for every retail controller update, beginning with ordinal
// one. Only changed full-state snapshots are written. B3IR 1 retains its
// neutral A-to-C measurement window. B3IR 2 preserves changed states before
// and after A, permits a held state at A and C, and treats C as the final
// controller reset.
class GuestInputTimelineRecorder {
  public:
    explicit GuestInputTimelineRecorder(
        const std::filesystem::path& path,
        GuestInputTimelineFormat format = GuestInputTimelineFormat::B3IR1);
    ~GuestInputTimelineRecorder();

    GuestInputTimelineRecorder(const GuestInputTimelineRecorder&) = delete;
    GuestInputTimelineRecorder& operator=(const GuestInputTimelineRecorder&) = delete;
    GuestInputTimelineRecorder(GuestInputTimelineRecorder&&) noexcept;
    GuestInputTimelineRecorder& operator=(GuestInputTimelineRecorder&&) noexcept;

    // Returns true only when a changed S record was written.
    [[nodiscard]] bool observe_retail_poll(std::uint64_t poll_ordinal,
                                           GuestInputTimelineBoundary boundary,
                                           const takedown::platform::HostControllerState& state);
    void record_measurement_start(GuestInputTimelineBoundary boundary);
    void finish(std::uint64_t poll_count, GuestInputTimelineBoundary boundary);

    // Last full state actually sampled by observe_retail_poll(). Host-only
    // consumers can use this without re-sampling input between retail polls;
    // successful completion exposes B3IR's final neutral reset.
    [[nodiscard]] const takedown::platform::HostControllerState&
    retail_poll_controller_state() const noexcept;
    [[nodiscard]] std::uint64_t observed_poll_count() const noexcept;
    [[nodiscard]] GuestInputTimelineFormat format() const noexcept;
    [[nodiscard]] bool measurement_started() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Loads and strictly validates a complete B3IR 1 or B3IR 2 file before replay
// begins. State starts neutral, changes only at recorded instruction/poll
// boundaries, and becomes neutral again at the final C boundary.
class GuestInputTimelineReplay {
  public:
    // An explicit new diagnostic output path selects poll-preserving replay:
    // S states apply at their original poll ordinals; A/C keep their step
    // positions but record actual cycles and completion poll count. This is
    // output-validation evidence, never strict or identical-work qualification.
    // The new B3IR2 file can subsequently be replayed with the strict default.
    //
    // poll_window_start additionally anchors the measurement window to game
    // time instead of step counts: A fires at the first boundary after that
    // retail poll ordinal, C at the first boundary after the recorded final
    // poll count. The window then spans the same game ticks under any timing
    // model, which a fixed step count cannot once guest idle work changes.
    //
    // time_keyed replays by guest time instead, for runs whose tick rate
    // differs from the recording (variable frame rate): each S state applies
    // at the first retail poll at or after its recorded EE cycle, at most one
    // per poll so no press or release is skipped, and A and C fire at the
    // first boundary at or after their recorded cycles.
    explicit GuestInputTimelineReplay(const std::filesystem::path& path,
        std::optional<std::filesystem::path> poll_diagnostic_output = std::nullopt,
        std::optional<std::uint64_t> poll_window_start = std::nullopt,
        bool time_keyed = false);
    ~GuestInputTimelineReplay();

    GuestInputTimelineReplay(const GuestInputTimelineReplay&) = delete;
    GuestInputTimelineReplay& operator=(const GuestInputTimelineReplay&) = delete;
    GuestInputTimelineReplay(GuestInputTimelineReplay&&) noexcept;
    GuestInputTimelineReplay& operator=(GuestInputTimelineReplay&&) noexcept;

    // Call once at the top of every outer EE loop. A due S record installs its
    // full held state before the corresponding retail poll. A due A record asks
    // the runner to arm measurement at this exact boundary. Passing either the
    // recorded step or cycle fails closed.
    [[nodiscard]] GuestInputTimelineAdvance
    apply_instruction_boundary(GuestInputTimelineBoundary boundary);

    // Call at every retail controller update, beginning with ordinal one. This
    // proves that a state installed at the top of the loop reached the exact
    // recorded poll rather than an earlier or later update.
    void observe_retail_poll(std::uint64_t poll_ordinal, GuestInputTimelineBoundary boundary);

    // Returns the absolute limit to give both native preflight and compiled
    // dispatch. The returned limit never crosses the next S, A, or C boundary.
    // apply_instruction_boundary() and any due retail-poll validation must run
    // before asking for a limit at the current step.
    [[nodiscard]] std::size_t clip_step_limit(std::size_t current_step,
                                              std::size_t configured_limit) const;
    [[nodiscard]] std::optional<std::size_t> next_boundary_step() const noexcept;
    [[nodiscard]] GuestInputTimelineBoundary completion_boundary() const noexcept;

    // Queries for a runner that wants to skip apply_instruction_boundary()
    // calls which provably do nothing (the scalar burst). Replay state changes
    // only inside apply_instruction_boundary(), observe_retail_poll() and
    // validate_finish(), so both answers hold until the next such call.
    //
    // True when a later apply_instruction_boundary() call could act, fail or
    // change replay state for a reason clip_step_limit() does not report: a
    // poll-window A or C marker waiting for the first boundary after its
    // retail poll, a controller snapshot awaiting validation, or a held state
    // the next call would reset.
    [[nodiscard]] bool instruction_boundary_action_pending() const noexcept;
    // Strict step-anchored replay fails closed when a step before the next
    // recorded boundary reaches that boundary's EE cycle, and time-keyed
    // replay fires its next marker there. Returns that cycle, or nullopt
    // when neither applies (poll-preserving or poll-window replay, a
    // validated finish, or no boundary left).
    [[nodiscard]] std::optional<std::uint64_t> next_boundary_cycle_limit() const noexcept;

    // Accepts only the exact final C record after every earlier event and poll
    // has been consumed. This also handles the usual runner shape where the
    // outer step limit exits before one more top-of-loop call can consume C.
    void validate_finish(std::uint64_t poll_count, GuestInputTimelineBoundary boundary);

    [[nodiscard]] const takedown::platform::HostControllerState& controller_state() const noexcept;
    // Last installed state whose exact S/poll identity has been validated.
    // This deliberately lags controller_state() while a due S is pending.
    [[nodiscard]] const takedown::platform::HostControllerState&
    retail_poll_controller_state() const noexcept;
    [[nodiscard]] std::uint64_t observed_poll_count() const noexcept;
    [[nodiscard]] GuestInputTimelineFormat format() const noexcept;
    [[nodiscard]] bool measurement_start_applied() const noexcept;
    [[nodiscard]] bool completion_applied() const noexcept;
    [[nodiscard]] bool finish_validated() const noexcept;
    [[nodiscard]] bool poll_diagnostic() const noexcept;
    // True when the window is anchored to game events (retail polls or guest
    // time) rather than recorded step counts.
    [[nodiscard]] bool poll_window() const noexcept;
    [[nodiscard]] bool time_keyed() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime
