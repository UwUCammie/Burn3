#pragma once

#include <cstddef>

namespace takedown::runtime {

enum class FrontendReplayInput {
    Neutral,
    DpadDown,
    DpadUp,
    Cross,
};

// Coordinates the synthetic controller-gate press with Burnout's own
// frontend polling cadence. Registration alone is not enough: the frontend
// and controller manager can run in either order, so a one-update edge may be
// cleared before the frontend asks for it. A completed neutral poll provides
// the handshake; the following controller update may then emit one raw DS2
// pulse for the retail decoder to turn into an action edge. If another
// controller update clears that edge before the frontend consumes it, the
// next completed neutral poll re-arms the same handshake.
class FrontendGatePulseHandshake {
  public:
    enum class Phase {
        WaitingForNeutralPoll,
        Armed,
        PulseInFlight,
        WaitingForAcceptance,
        Complete,
    };

    [[nodiscard]] constexpr bool observe_gate_poll(const bool registered,
                                                   const bool action_edge,
                                                   const bool accepted) noexcept {
        if (accepted) {
            phase_ = Phase::Complete;
            return false;
        }
        if ((phase_ == Phase::WaitingForNeutralPoll ||
             phase_ == Phase::WaitingForAcceptance) &&
            registered && !action_edge) {
            phase_ = Phase::Armed;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool pulse_ready() const noexcept {
        return phase_ == Phase::Armed;
    }

    [[nodiscard]] constexpr bool begin_pulse() noexcept {
        if (!pulse_ready()) {
            return false;
        }
        phase_ = Phase::PulseInFlight;
        return true;
    }

    [[nodiscard]] constexpr bool finish_pulse_update() noexcept {
        if (phase_ != Phase::PulseInFlight) {
            return false;
        }
        phase_ = Phase::WaitingForAcceptance;
        return true;
    }

    [[nodiscard]] constexpr Phase phase() const noexcept { return phase_; }

  private:
    Phase phase_{Phase::WaitingForNeutralPoll};
};

// Deterministic controller-update schedule used by opt-in headless frontend
// diagnostics. Pulse widths are expressed in retail controller updates so a
// caller can prove whether an action needs to span an update boundary.
struct FrontendInputReplaySchedule {
    std::size_t down_pulse_update{};
    std::size_t down_pulse_updates{1U};
    std::size_t up_pulse_update{};
    std::size_t up_pulse_updates{};
    std::size_t initial_cross_pulse_update{};
    std::size_t repeated_cross_first_update{};
    std::size_t repeated_cross_period{};
    std::size_t cross_pulse_updates{1U};

    [[nodiscard]] constexpr FrontendReplayInput
    input_for_update(const std::size_t update) const noexcept {
        if (update >= down_pulse_update && update - down_pulse_update < down_pulse_updates) {
            return FrontendReplayInput::DpadDown;
        }
        if (up_pulse_updates != 0U && update >= up_pulse_update &&
            update - up_pulse_update < up_pulse_updates) {
            return FrontendReplayInput::DpadUp;
        }
        if ((update >= initial_cross_pulse_update &&
             update - initial_cross_pulse_update < cross_pulse_updates) ||
            (repeated_cross_period != 0U && update >= repeated_cross_first_update &&
             (update - repeated_cross_first_update) % repeated_cross_period <
                 cross_pulse_updates)) {
            return FrontendReplayInput::Cross;
        }
        return FrontendReplayInput::Neutral;
    }
};

} // namespace takedown::runtime
