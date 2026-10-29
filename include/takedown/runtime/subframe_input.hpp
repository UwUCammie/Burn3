#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "takedown/platform/window.hpp"

namespace takedown::runtime {

// One observed host controller edge, stamped with the guest EE cycle at which
// the host first observed it. The state is a full snapshot: applying the event
// reproduces the complete controller state at that boundary without
// extrapolating anything the host has not actually observed.
struct SubframeInputEvent {
    std::uint64_t arrival_cycle{};
    takedown::platform::HostControllerState state{};
};

// Event-driven sub-frame input precision for the virtual DS2 bridge.
//
// The default bridge samples host controller state exactly when the retail
// transport poll returns, which is hardware-parity: a real SIO2 slot delivers
// the pad's state only when the game reads it. That default cannot represent
// anything that happened between two polls, and during uncapped or paused
// guest execution the host moment of the poll can miss edges entirely.
//
// With this bridge (opt-in via --subframe-input) the host observes controller
// state on every event pump, timestamps each observed change with the guest
// cycle at arrival, and applies it at the first scheduler-safe guest boundary
// at or after that cycle. There is no polling thread and no fixed-rate host
// sampling: events enter the queue only when pump_events() actually observed
// them, and the guest applies them between instructions. A press that arrives
// mid-frame is therefore already in guest pad memory at the next retail poll
// of the same frame, and during fast-forward a real-time tap still lands on
// the guest-frame poll it would have landed on in real time.
//
// Events are never applied before their arrival cycle and never applied from
// a projected future state: the queue only holds states the host has already
// observed.
class SubframeInputBridge {
  public:
    // Diffs the newly observed host state against the last observed state and
    // enqueues one event per observed change, stamped with arrival_cycle.
    // Returns the number of events enqueued (0 when nothing changed).
    std::size_t observe(const takedown::platform::HostControllerState& state,
                        std::uint64_t arrival_cycle);

    // Pops the earliest event whose arrival cycle is at or before
    // current_cycle. Events are strictly ordered by arrival cycle because
    // observe() is called from the single guest-execution thread.
    [[nodiscard]] bool due(std::uint64_t current_cycle, SubframeInputEvent& out);

    // The most recently applied event state, for callers that must reproduce
    // the exact applied sub-frame state at a retail poll boundary instead of
    // re-sampling the host.
    [[nodiscard]] const std::optional<takedown::platform::HostControllerState>&
    last_applied_state() const noexcept {
        return last_applied_state_;
    }

    void apply(const SubframeInputEvent& event);

    [[nodiscard]] std::size_t pending() const noexcept {
        return pending_.size();
    }

    [[nodiscard]] std::uint64_t observed_events() const noexcept {
        return observed_events_;
    }

    [[nodiscard]] std::uint64_t applied_events() const noexcept {
        return applied_events_;
    }

    [[nodiscard]] std::uint64_t dropped_events() const noexcept {
        return dropped_events_;
    }

  private:
    static constexpr std::size_t kMaximumPendingEvents = 1024U;

    [[nodiscard]] static bool states_equal(const takedown::platform::HostControllerState& a,
                                           const takedown::platform::HostControllerState& b) noexcept;

    std::deque<SubframeInputEvent> pending_;
    std::optional<takedown::platform::HostControllerState> last_observed_state_;
    std::optional<takedown::platform::HostControllerState> last_applied_state_;
    std::uint64_t observed_events_{};
    std::uint64_t applied_events_{};
    std::uint64_t dropped_events_{};
};

} // namespace takedown::runtime
