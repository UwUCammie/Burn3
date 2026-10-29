#include "takedown/runtime/native_simulation_trace.hpp"

#include <cmath>
#include <limits>

namespace takedown::runtime {

namespace {

// Numerator remainder left after converting the elapsed cycles to whole
// nanoseconds, carrying the previous remainder forward.
std::uint64_t expected_guest_time_remainder(const std::uint64_t elapsed_cycles,
                                            const FixedStepRate rate,
                                            const std::uint64_t previous_remainder) {
    const unsigned __int128 exact =
        static_cast<unsigned __int128>(elapsed_cycles) * rate.denominator * 1'000'000'000U +
        previous_remainder;
    return static_cast<std::uint64_t>(exact % rate.numerator);
}

// The all-zero origin may carry the default timer; any other origin needs a
// timer whose divisions and step times are usable.
bool reference_timer_state_valid(const RetailReferenceTimerState& timer,
                                 const bool zero_origin) {
    if (zero_origin && timer == RetailReferenceTimerState{}) {
        return true;
    }
    return timer.subdivisions > 0 && timer.pending_subdivisions > 0 &&
           std::isfinite(timer.step_seconds) && timer.step_seconds > 0.0F &&
           std::isfinite(timer.elapsed_seconds);
}

} // namespace

std::string_view to_string(const NativeSimulationTraceFailure failure) noexcept {
    switch (failure) {
    case NativeSimulationTraceFailure::None:
        return "none";
    case NativeSimulationTraceFailure::Disabled:
        return "disabled";
    case NativeSimulationTraceFailure::Capacity:
        return "capacity";
    case NativeSimulationTraceFailure::InvalidSeed:
        return "invalid-seed";
    case NativeSimulationTraceFailure::BatchState:
        return "batch-state";
    case NativeSimulationTraceFailure::NonAuthoritativeGuestTime:
        return "non-authoritative-guest-time";
    case NativeSimulationTraceFailure::NonMonotonicSimulationTick:
        return "non-monotonic-simulation-tick";
    case NativeSimulationTraceFailure::NonMonotonicReferenceTick:
        return "non-monotonic-reference-tick";
    case NativeSimulationTraceFailure::NonMonotonicGuestTime:
        return "non-monotonic-guest-time";
    case NativeSimulationTraceFailure::InvalidReferenceBoundary:
        return "invalid-reference-boundary";
    case NativeSimulationTraceFailure::InvalidDelta:
        return "invalid-delta";
    case NativeSimulationTraceFailure::GuestTimeNotFinite:
        return "guest-time-not-finite";
    case NativeSimulationTraceFailure::InvalidPhaseOrigin:
        return "invalid-phase-origin";
    }
    return "unknown";
}

void NativeSimulationTrace::begin_session(const NativeSimulationDriverSeed& seed,
                                          const FixedStepRate simulation_rate) noexcept {
    reset();
    const bool zero_origin = seed.simulation_tick == 0U &&
                             seed.simulation_phase_nanoseconds == 0U &&
                             seed.guest_time_cycles == 0U &&
                             seed.guest_time_cycle_remainder == 0U;
    if (guest_time_rate_.numerator == 0U || guest_time_rate_.denominator == 0U ||
        seed.guest_time_cycle_remainder >= guest_time_rate_.numerator ||
        seed.reference_tick > std::numeric_limits<std::uint32_t>::max() ||
        seed.reference_timer_state.observed_tick !=
            static_cast<std::uint32_t>(seed.reference_tick) ||
        !reference_timer_state_valid(seed.reference_timer_state, zero_origin)) {
        fail(NativeSimulationTraceFailure::InvalidSeed);
        return;
    }
    const NativeSimulationPhasePoint point{
        seed.simulation_tick, seed.simulation_phase_nanoseconds, seed.guest_time_cycles,
        seed.guest_time_cycle_remainder};
    if (!zero_origin) {
        if (!seed.simulation_phase_origin.has_value()) {
            fail(NativeSimulationTraceFailure::InvalidPhaseOrigin);
            return;
        }
    }
    if (seed.simulation_phase_origin.has_value() &&
        !native_simulation_phase_origin_matches(simulation_rate, guest_time_rate_,
                                                *seed.simulation_phase_origin, point)) {
        fail(NativeSimulationTraceFailure::InvalidPhaseOrigin);
        return;
    }
    session_seeded_ = true;
    last_simulation_tick_ = seed.simulation_tick;
    last_reference_tick_ = seed.reference_tick;
    last_guest_time_cycles_ = seed.guest_time_cycles;
    last_guest_time_cycle_remainder_ = seed.guest_time_cycle_remainder;
    session_timer_state_ = seed.reference_timer_state;
}

void NativeSimulationTrace::reset() noexcept {
    size_ = 0U;
    batch_begin_ = 0U;
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
    batch_count_ = 0U;
    batch_previous_simulation_tick_ = 0U;
    batch_previous_reference_tick_ = 0U;
    batch_previous_guest_time_cycles_ = 0U;
    batch_previous_guest_time_cycle_remainder_ = 0U;
    batch_guest_time_cycles_ = 0U;
    batch_guest_time_cycle_remainder_ = 0U;
    batch_previous_have_last_step_ = false;
    last_simulation_tick_ = 0U;
    last_reference_tick_ = 0U;
    last_guest_time_cycles_ = 0U;
    last_guest_time_cycle_remainder_ = 0U;
    session_timer_state_ = {};
    have_last_step_ = false;
    session_seeded_ = false;
    batch_open_ = false;
    failure_ = NativeSimulationTraceFailure::None;
}

bool NativeSimulationTrace::begin_batch(const NativeSimulationDriverFrame& frame,
                                        const std::uint32_t step_count) noexcept {
    if (!enabled()) {
        fail(NativeSimulationTraceFailure::Disabled);
        return false;
    }
    if (batch_open_ || failure_ != NativeSimulationTraceFailure::None) {
        fail(NativeSimulationTraceFailure::BatchState);
        return false;
    }
    if (!frame.guest_time_authoritative) {
        failure_ = NativeSimulationTraceFailure::NonAuthoritativeGuestTime;
        return false;
    }
    if (frame.guest_time_cycle_remainder >= guest_time_rate_.numerator ||
        frame.guest_time_cycles < last_guest_time_cycles_ ||
        frame.guest_time_cycle_remainder !=
            expected_guest_time_remainder(frame.guest_time_cycles - last_guest_time_cycles_,
                                          guest_time_rate_, last_guest_time_cycle_remainder_)) {
        fail(NativeSimulationTraceFailure::NonMonotonicGuestTime);
        return false;
    }
    if (step_count > storage_.size() - size_) {
        fail(NativeSimulationTraceFailure::Capacity);
        return false;
    }
    batch_begin_ = size_;
    batch_expected_ = step_count;
    batch_recorded_ = 0U;
    batch_previous_simulation_tick_ = last_simulation_tick_;
    batch_previous_reference_tick_ = last_reference_tick_;
    batch_previous_guest_time_cycles_ = last_guest_time_cycles_;
    batch_previous_guest_time_cycle_remainder_ = last_guest_time_cycle_remainder_;
    batch_previous_have_last_step_ = have_last_step_;
    batch_guest_time_cycles_ = frame.guest_time_cycles;
    batch_guest_time_cycle_remainder_ = frame.guest_time_cycle_remainder;
    batch_open_ = true;
    return true;
}

bool NativeSimulationTrace::record_step(const NativeSimulationTraceStep& step) noexcept {
    if (!batch_open_ || batch_recorded_ >= batch_expected_ || size_ >= storage_.size()) {
        fail(NativeSimulationTraceFailure::BatchState);
        return false;
    }
    if (!check_step(step)) {
        return false;
    }
    storage_[size_++] = step;
    ++batch_recorded_;
    last_simulation_tick_ = step.step.simulation_tick;
    last_reference_tick_ = step.step.reference_tick;
    have_last_step_ = true;
    return true;
}

bool NativeSimulationTrace::check_step(const NativeSimulationTraceStep& record) noexcept {
    const SimulationStep& step = record.step;
    if (record.batch_step_index != batch_recorded_) {
        fail(NativeSimulationTraceFailure::BatchState);
    }
    if (!std::isfinite(record.guest_time_seconds)) {
        fail(NativeSimulationTraceFailure::GuestTimeNotFinite);
    }
    if (!std::isfinite(step.delta_seconds) || step.delta_seconds <= 0.0) {
        fail(NativeSimulationTraceFailure::InvalidDelta);
    }
    if (record.guest_time_cycles != batch_guest_time_cycles_ ||
        record.guest_time_cycle_remainder != batch_guest_time_cycle_remainder_) {
        fail(NativeSimulationTraceFailure::NonMonotonicGuestTime);
    }
    if (have_last_step_) {
        if (last_simulation_tick_ == std::numeric_limits<std::uint64_t>::max() ||
            step.simulation_tick != last_simulation_tick_ + 1U) {
            fail(NativeSimulationTraceFailure::NonMonotonicSimulationTick);
        }
        const bool advances_reference =
            last_reference_tick_ != std::numeric_limits<std::uint64_t>::max() &&
            step.reference_tick == last_reference_tick_ + 1U;
        if (step.reference_tick < last_reference_tick_ ||
            step.reference_tick >
                last_reference_tick_ +
                    static_cast<std::uint64_t>(last_reference_tick_ !=
                                               std::numeric_limits<std::uint64_t>::max()) ||
            step.reference_tick_due != advances_reference) {
            fail(NativeSimulationTraceFailure::NonMonotonicReferenceTick);
        }
    } else if (session_seeded_) {
        if (last_simulation_tick_ == std::numeric_limits<std::uint64_t>::max() ||
            step.simulation_tick != last_simulation_tick_ + 1U) {
            fail(NativeSimulationTraceFailure::NonMonotonicSimulationTick);
        }
        const bool advances_reference =
            last_reference_tick_ != std::numeric_limits<std::uint64_t>::max() &&
            step.reference_tick == last_reference_tick_ + 1U;
        if (step.reference_tick < last_reference_tick_ ||
            step.reference_tick >
                last_reference_tick_ +
                    static_cast<std::uint64_t>(last_reference_tick_ !=
                                               std::numeric_limits<std::uint64_t>::max()) ||
            step.reference_tick_due != advances_reference) {
            fail(NativeSimulationTraceFailure::NonMonotonicReferenceTick);
        }
    } else if (step.simulation_tick == 0U ||
               (step.reference_tick == 0U && step.reference_tick_due)) {
        fail(NativeSimulationTraceFailure::NonMonotonicSimulationTick);
    }
    if (step.reference_tick_due) {
        if (step.boundary_numerator == 0U || step.boundary_denominator == 0U ||
            step.boundary_numerator > step.boundary_denominator) {
            fail(NativeSimulationTraceFailure::InvalidReferenceBoundary);
        }
    } else if (step.boundary_numerator != 0U || step.boundary_denominator != 1U) {
        fail(NativeSimulationTraceFailure::InvalidReferenceBoundary);
    }
    return failure_ == NativeSimulationTraceFailure::None;
}

bool NativeSimulationTrace::end_batch() noexcept {
    if (!batch_open_ || batch_recorded_ != batch_expected_) {
        fail(NativeSimulationTraceFailure::BatchState);
        return false;
    }
    batch_open_ = false;
    last_guest_time_cycles_ = batch_guest_time_cycles_;
    last_guest_time_cycle_remainder_ = batch_guest_time_cycle_remainder_;
    ++batch_count_;
    return true;
}

void NativeSimulationTrace::abort_batch() noexcept {
    if (!batch_open_) {
        return;
    }
    size_ = batch_begin_;
    last_simulation_tick_ = batch_previous_simulation_tick_;
    last_reference_tick_ = batch_previous_reference_tick_;
    last_guest_time_cycles_ = batch_previous_guest_time_cycles_;
    last_guest_time_cycle_remainder_ = batch_previous_guest_time_cycle_remainder_;
    have_last_step_ = batch_previous_have_last_step_;
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
    batch_open_ = false;
}

void NativeSimulationTrace::fail(const NativeSimulationTraceFailure failure) noexcept {
    if (failure_ == NativeSimulationTraceFailure::None) {
        failure_ = failure;
    }
}

} // namespace takedown::runtime
