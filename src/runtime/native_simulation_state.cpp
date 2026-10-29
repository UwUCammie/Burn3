#include "takedown/runtime/native_simulation_state.hpp"

#include "takedown/runtime/slus_21050_collision_trace.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>

namespace takedown::runtime {
namespace {

FixedStepRate normalized(FixedStepRate rate) {
    const auto divisor = std::gcd(rate.numerator, rate.denominator);
    return {rate.numerator / divisor, rate.denominator / divisor};
}

[[nodiscard]] bool valid(const FixedStepRate rate) noexcept {
    return rate.numerator != 0U && rate.denominator != 0U;
}

// A reference rate faster than the simulation rate could never be reached.
[[nodiscard]] bool simulation_covers_reference(const FixedStepRate simulation, const FixedStepRate reference) noexcept {
    const auto simulation_scaled = static_cast<std::uint64_t>(simulation.numerator) * reference.denominator;
    const auto reference_scaled = static_cast<std::uint64_t>(reference.numerator) * simulation.denominator;
    return simulation_scaled >= reference_scaled;
}

// The reference ordinal must be the exact floor of simulation_tick * reference / simulation.
[[nodiscard]] bool reference_tick_consistent(const std::uint64_t simulation_tick, const std::uint64_t reference_tick,
                                             const std::uint64_t reference_scaled,
                                             const std::uint64_t simulation_scaled) noexcept {
    const unsigned __int128 expected =
        static_cast<unsigned __int128>(simulation_tick) * reference_scaled / simulation_scaled;
    return expected <= std::numeric_limits<std::uint64_t>::max() && reference_tick == static_cast<std::uint64_t>(expected);
}

// The step length must be the reciprocal of the epoch's simulation rate,
// within a few ulps of the larger operand.
[[nodiscard]] bool step_seconds_match(const double seconds, const FixedStepRate rate) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return false;
    }
    const double expected = static_cast<double>(rate.denominator) / static_cast<double>(rate.numerator);
    const double tolerance =
        std::max({1.0, std::abs(seconds), expected}) * std::numeric_limits<double>::epsilon() * 4.0;
    return std::abs(seconds - expected) <= tolerance;
}

[[nodiscard]] bool valid_alpha(const double alpha) noexcept {
    return std::isfinite(alpha) && alpha >= 0.0 && alpha < 1.0;
}

[[nodiscard]] bool valid_seconds(const double seconds) noexcept {
    if (!std::isfinite(seconds)) {
        return false;
    }
    if (seconds < 0.0) {
        return false;
    }
    return true;
}

// Guest cycles are elapsed in units of 1 / (rate.denominator * 1e9) of a
// cycle-rate tick; the remainder carries the sub-tick position.
[[nodiscard]] std::uint64_t advanced_remainder(const std::uint64_t delta_cycles, const std::uint64_t remainder,
                                               const FixedStepRate rate) noexcept {
    const unsigned __int128 exact =
        static_cast<unsigned __int128>(delta_cycles) * rate.denominator * 1'000'000'000U + remainder;
    return static_cast<std::uint64_t>(exact % rate.numerator);
}

[[nodiscard]] std::uint64_t bits(const double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] bool same_bits(const double left, const double right) noexcept {
    return bits(left) == bits(right);
}

[[nodiscard]] bool same_schedule(const FrameSchedule& left, const FrameSchedule& right) noexcept {
    return left.simulation_steps == right.simulation_steps &&
           same_bits(left.simulation_step_seconds, right.simulation_step_seconds) &&
           same_bits(left.interpolation_alpha, right.interpolation_alpha) &&
           same_bits(left.presentation_delta_seconds, right.presentation_delta_seconds) &&
           left.presentation_delta_nanoseconds == right.presentation_delta_nanoseconds &&
           same_bits(left.discarded_seconds, right.discarded_seconds) &&
           left.backlog_nanoseconds == right.backlog_nanoseconds &&
           left.catch_up_limited == right.catch_up_limited &&
           left.presentation_allowed == right.presentation_allowed;
}

// What the step sequence must have produced for this simulation tick.
struct ExpectedStepMetadata {
    std::uint64_t reference_tick{};
    std::uint64_t boundary_numerator{};
    std::uint64_t boundary_denominator{1};
    double reference_scale{};
    bool reference_tick_due{};
    bool valid{};
};

[[nodiscard]] ExpectedStepMetadata expected_step_metadata(const std::uint64_t simulation_tick,
                                                        const FixedStepRate simulation,
                                                        const FixedStepRate reference) {
    if (simulation_tick == 0U) {
        return {};
    }
    auto scale_numerator = static_cast<std::uint64_t>(reference.numerator) * simulation.denominator;
    auto scale_denominator = static_cast<std::uint64_t>(reference.denominator) * simulation.numerator;
    const auto divisor = std::gcd(scale_numerator, scale_denominator);
    scale_numerator /= divisor;
    scale_denominator /= divisor;
    const unsigned __int128 scaled = static_cast<unsigned __int128>(simulation_tick) * scale_numerator;
    const unsigned __int128 expected_reference = scaled / scale_denominator;
    if (expected_reference > std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    const auto previous_phase = static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(scale_numerator) * (simulation_tick - 1U) % scale_denominator);
    const auto threshold = scale_denominator - scale_numerator;
    const bool due = previous_phase >= threshold;
    return ExpectedStepMetadata{
        static_cast<std::uint64_t>(expected_reference),
        due ? scale_denominator - previous_phase : 0U,
        due ? scale_numerator : 1U,
        static_cast<double>(scale_numerator) / static_cast<double>(scale_denominator),
        due,
        true,
    };
}

[[nodiscard]] bool valid_vehicle_state(const NativeSimulationVehicleState& state) noexcept {
    return state.complete && state.role_valid && state.vehicle_object != 0U && state.physics_object != 0U &&
           state.physics_component != 0U && state.vehicle_object == state.owner_vehicle &&
           state.world_node_valid == (state.world_node != 0U) && state.world_node_valid == state.world_triangle_valid;
}

[[nodiscard]] NativeSimulationVehicleState to_vehicle_state(const Slus21050VehicleWorldContactSnapshot& snapshot) {
    NativeSimulationVehicleState state{
        snapshot.sequence,
        snapshot.context.committed_steps,
        snapshot.context.ee_cycles,
        snapshot.context.vblank_starts,
        snapshot.context.thread_id,
        snapshot.vehicle_object,
        snapshot.physics_object,
        snapshot.physics_component,
        snapshot.owner_vehicle,
        snapshot.world_node,
        snapshot.world_triangle,
        snapshot.transform_words,
        snapshot.vector_words,
        snapshot.contact_words,
        snapshot.component_words,
        snapshot.world_node_valid,
        snapshot.world_triangle_valid,
        snapshot.role_valid,
        snapshot.complete,
    };
    return state;
}

} // namespace

std::string_view to_string(const NativeSimulationStateFailure failure) noexcept {
    switch (failure) {
    case NativeSimulationStateFailure::None:
        return "none";
    case NativeSimulationStateFailure::Disabled:
        return "disabled";
    case NativeSimulationStateFailure::InvalidEpoch:
        return "invalid-epoch";
    case NativeSimulationStateFailure::InvalidRate:
        return "invalid-rate";
    case NativeSimulationStateFailure::BatchState:
        return "batch-state";
    case NativeSimulationStateFailure::Capacity:
        return "capacity";
    case NativeSimulationStateFailure::NonAuthoritativeGuestTime:
        return "non-authoritative-guest-time";
    case NativeSimulationStateFailure::InvalidFrame:
        return "invalid-frame";
    case NativeSimulationStateFailure::InvalidStep:
        return "invalid-step";
    case NativeSimulationStateFailure::NonMonotonicSimulationTick:
        return "non-monotonic-simulation-tick";
    case NativeSimulationStateFailure::NonMonotonicReferenceTick:
        return "non-monotonic-reference-tick";
    case NativeSimulationStateFailure::NonMonotonicGuestTime:
        return "non-monotonic-guest-time";
    case NativeSimulationStateFailure::SequenceOverflow:
        return "sequence-overflow";
    case NativeSimulationStateFailure::InvalidPhaseOrigin:
        return "invalid-phase-origin";
    case NativeSimulationStateFailure::InvalidVehicleSnapshot:
        return "invalid-vehicle-snapshot";
    case NativeSimulationStateFailure::InvalidVehicleEpoch:
        return "invalid-vehicle-epoch";
    }
    return "unknown";
}

bool NativeSimulationStatePublisher::begin_epoch(const NativeSimulationStateEpoch& epoch) noexcept {
    if (!enabled()) {
        fail(NativeSimulationStateFailure::Disabled);
        return false;
    }
    if (batch_open_ || epoch.session_epoch == 0U) {
        fail(NativeSimulationStateFailure::InvalidEpoch);
        return false;
    }
    if (!valid(epoch.simulation_rate) || !valid(epoch.reference_rate) || !valid(epoch.guest_time_rate) ||
        !simulation_covers_reference(epoch.simulation_rate, epoch.reference_rate)) {
        fail(NativeSimulationStateFailure::InvalidRate);
        return false;
    }
    const auto simulation_scaled =
        static_cast<std::uint64_t>(epoch.simulation_rate.numerator) * epoch.reference_rate.denominator;
    const auto reference_scaled =
        static_cast<std::uint64_t>(epoch.reference_rate.numerator) * epoch.simulation_rate.denominator;
    if (!std::isfinite(epoch.reference_delta_seconds) || epoch.reference_delta_seconds <= 0.0F ||
        epoch.reference_tick > std::numeric_limits<std::uint32_t>::max() ||
        epoch.reference_timer_state.observed_tick != static_cast<std::uint32_t>(epoch.reference_tick) ||
        !reference_tick_consistent(epoch.simulation_tick, epoch.reference_tick, reference_scaled, simulation_scaled) ||
        epoch.guest_time_cycle_remainder >= epoch.guest_time_rate.numerator) {
        fail(NativeSimulationStateFailure::InvalidEpoch);
        return false;
    }
    const NativeSimulationPhasePoint point{epoch.simulation_tick, epoch.simulation_phase_nanoseconds,
                                           epoch.guest_time_cycles, epoch.guest_time_cycle_remainder};
    const bool origin_point = point.simulation_tick == 0U && point.simulation_phase_nanoseconds == 0U &&
                              point.guest_time_cycles == 0U && point.guest_time_cycle_remainder == 0U;
    if ((!origin_point && !epoch.simulation_phase_origin.has_value()) ||
        (epoch.simulation_phase_origin.has_value() &&
         !native_simulation_phase_origin_matches(epoch.simulation_rate, epoch.guest_time_rate,
                                                 *epoch.simulation_phase_origin, point))) {
        fail(NativeSimulationStateFailure::InvalidPhaseOrigin);
        return false;
    }

    NativeSimulationStateEpoch next = epoch;
    next.simulation_rate = normalized(next.simulation_rate);
    next.reference_rate = normalized(next.reference_rate);
    {
        const std::lock_guard<std::mutex> lock(publication_mutex_);
        previous_ = {};
        current_ = {};
        publication_count_ = 0U;
        retained_count_ = 0U;
        presentation_ = {};
        have_presentation_ = false;
        last_vehicle_source_sequence_ = 0U;
        have_vehicle_source_sequence_ = false;
    }
    epoch_ = next;
    batch_frame_ = {};
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
    last_simulation_tick_ = next.simulation_tick;
    last_reference_tick_ = next.reference_tick;
    last_guest_time_cycles_ = next.guest_time_cycles;
    last_guest_time_cycle_remainder_ = next.guest_time_cycle_remainder;
    last_timer_state_ = next.reference_timer_state;
    batch_previous_simulation_tick_ = 0U;
    batch_previous_reference_tick_ = 0U;
    batch_previous_guest_time_cycles_ = 0U;
    batch_previous_guest_time_cycle_remainder_ = 0U;
    batch_previous_timer_state_ = {};
    epoch_active_.store(true, std::memory_order_release);
    failure_.store(NativeSimulationStateFailure::None, std::memory_order_release);
    return true;
}

void NativeSimulationStatePublisher::reset() noexcept {
    batch_frame_ = {};
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
    last_simulation_tick_ = 0U;
    last_reference_tick_ = 0U;
    last_guest_time_cycles_ = 0U;
    last_guest_time_cycle_remainder_ = 0U;
    last_timer_state_ = {};
    batch_previous_simulation_tick_ = 0U;
    batch_previous_reference_tick_ = 0U;
    batch_previous_guest_time_cycles_ = 0U;
    batch_previous_guest_time_cycle_remainder_ = 0U;
    batch_previous_timer_state_ = {};
    last_vehicle_source_sequence_ = 0U;
    have_vehicle_source_sequence_ = false;
    epoch_ = {};
    epoch_active_.store(false, std::memory_order_release);
    batch_open_ = false;
    {
        const std::lock_guard<std::mutex> lock(publication_mutex_);
        previous_ = {};
        current_ = {};
        publication_count_ = 0U;
        retained_count_ = 0U;
        presentation_ = {};
        have_presentation_ = false;
    }
    failure_.store(NativeSimulationStateFailure::None, std::memory_order_release);
}

std::uint64_t NativeSimulationStatePublisher::publication_count() const noexcept {
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    return publication_count_;
}

bool NativeSimulationStatePublisher::read_latest(NativeSimulationStateSnapshot& snapshot) const noexcept {
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    if (retained_count_ == 0U) {
        return false;
    }
    snapshot = current_;
    return true;
}

bool NativeSimulationStatePublisher::read_pair(NativeSimulationStatePair& pair) const noexcept {
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    if (retained_count_ < 2U || previous_.session_epoch != current_.session_epoch ||
        previous_.simulation_rate.numerator != current_.simulation_rate.numerator ||
        previous_.simulation_rate.denominator != current_.simulation_rate.denominator ||
        previous_.reference_rate.numerator != current_.reference_rate.numerator ||
        previous_.reference_rate.denominator != current_.reference_rate.denominator ||
        previous_.authoritative != current_.authoritative ||
        previous_.publication_sequence == std::numeric_limits<std::uint64_t>::max() ||
        current_.publication_sequence != previous_.publication_sequence + 1U) {
        return false;
    }
    pair = NativeSimulationStatePair{previous_, current_};
    return true;
}

bool NativeSimulationStatePublisher::read_presentation_state(NativeSimulationPresentationState& state) const noexcept {
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    if (!have_presentation_) {
        return false;
    }
    state = presentation_;
    return true;
}

bool NativeSimulationStatePublisher::read_interpolation_state(NativeSimulationInterpolationState& state) const noexcept {
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    if (retained_count_ < 2U || !have_presentation_ || previous_.session_epoch != current_.session_epoch ||
        presentation_.session_epoch != current_.session_epoch ||
        presentation_.associated_publication_sequence != current_.publication_sequence ||
        previous_.publication_sequence == std::numeric_limits<std::uint64_t>::max() ||
        current_.publication_sequence != previous_.publication_sequence + 1U) {
        return false;
    }
    state = NativeSimulationInterpolationState{.pair = {previous_, current_}, .presentation = presentation_};
    return true;
}

NativeSimulationVehiclePublicationResult NativeSimulationStatePublisher::publish_vehicle_snapshot_result(
    const Slus21050VehicleWorldContactPublicationToken& token) noexcept {
    if (!enabled()) {
        fail(NativeSimulationStateFailure::Disabled);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    if (!epoch_active() || batch_open_ || failure() != NativeSimulationStateFailure::None) {
        fail(NativeSimulationStateFailure::BatchState);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    const NativeSimulationVehicleState vehicle_state = to_vehicle_state(token.snapshot_);
    if (!valid_vehicle_state(vehicle_state)) {
        fail(NativeSimulationStateFailure::InvalidVehicleSnapshot);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    const std::lock_guard<std::mutex> lock(publication_mutex_);
    if (token.session_epoch_ != epoch_.session_epoch) {
        fail(NativeSimulationStateFailure::InvalidVehicleEpoch);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    if (have_vehicle_source_sequence_ && vehicle_state.source_sequence <= last_vehicle_source_sequence_) {
        fail(NativeSimulationStateFailure::InvalidVehicleSnapshot);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    if (retained_count_ == 0U || !current_.authoritative) {
        return NativeSimulationVehiclePublicationResult::Deferred;
    }
    if (epoch_.session_epoch != current_.session_epoch) {
        fail(NativeSimulationStateFailure::InvalidVehicleEpoch);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    if (vehicle_state.guest_time_cycles > current_.guest_time_cycles) {
        return NativeSimulationVehiclePublicationResult::Deferred;
    }
    if (vehicle_state.guest_time_cycles < current_.guest_time_cycles) {
        fail(NativeSimulationStateFailure::InvalidVehicleSnapshot);
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    current_.vehicle_state = vehicle_state;
    last_vehicle_source_sequence_ = vehicle_state.source_sequence;
    have_vehicle_source_sequence_ = true;
    return NativeSimulationVehiclePublicationResult::Published;
}

bool NativeSimulationStatePublisher::publish_vehicle_snapshot(
    const Slus21050VehicleWorldContactPublicationToken& token) noexcept {
    return publish_vehicle_snapshot_result(token) == NativeSimulationVehiclePublicationResult::Published;
}

bool NativeSimulationStatePublisher::begin_batch(const NativeSimulationDriverFrame& frame,
                                                 const std::uint32_t step_count) noexcept {
    if (!enabled()) {
        fail(NativeSimulationStateFailure::Disabled);
        return false;
    }
    if (!epoch_active() || batch_open_ || failure() != NativeSimulationStateFailure::None) {
        fail(NativeSimulationStateFailure::BatchState);
        return false;
    }
    if (!frame.enabled || step_count != frame.schedule.simulation_steps ||
        !step_seconds_match(frame.schedule.simulation_step_seconds, epoch_.simulation_rate) ||
        !valid_alpha(frame.schedule.interpolation_alpha) ||
        !valid_seconds(frame.schedule.presentation_delta_seconds) ||
        !valid_seconds(frame.schedule.discarded_seconds) ||
        frame.schedule.presentation_allowed == frame.schedule.catch_up_limited ||
        frame.guest_time_cycle_remainder >= epoch_.guest_time_rate.numerator) {
        fail(NativeSimulationStateFailure::InvalidFrame);
        return false;
    }
    if (!frame.guest_time_authoritative) {
        fail(NativeSimulationStateFailure::NonAuthoritativeGuestTime);
        return false;
    }
    if (frame.guest_time_cycles < last_guest_time_cycles_ ||
        frame.guest_time_cycle_remainder !=
            advanced_remainder(frame.guest_time_cycles - last_guest_time_cycles_, last_guest_time_cycle_remainder_,
                               epoch_.guest_time_rate)) {
        fail(NativeSimulationStateFailure::NonMonotonicGuestTime);
        return false;
    }
    if (step_count > batch_storage_.size()) {
        fail(NativeSimulationStateFailure::Capacity);
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(publication_mutex_);
        if (step_count > std::numeric_limits<std::uint64_t>::max() - publication_count_) {
            fail(NativeSimulationStateFailure::SequenceOverflow);
            return false;
        }
    }
    batch_frame_ = frame;
    batch_previous_simulation_tick_ = last_simulation_tick_;
    batch_previous_reference_tick_ = last_reference_tick_;
    batch_previous_guest_time_cycles_ = last_guest_time_cycles_;
    batch_previous_guest_time_cycle_remainder_ = last_guest_time_cycle_remainder_;
    batch_previous_timer_state_ = last_timer_state_;
    batch_expected_ = step_count;
    batch_recorded_ = 0U;
    batch_open_ = true;
    return true;
}

bool NativeSimulationStatePublisher::record_step(const NativeSimulationTraceStep& record) noexcept {
    if (!batch_open_ || batch_recorded_ >= batch_expected_) {
        fail(NativeSimulationStateFailure::BatchState);
        return false;
    }
    if (!check_step(record)) {
        return false;
    }
    batch_storage_[batch_recorded_] = NativeSimulationStateSnapshot{
        .session_epoch = epoch_.session_epoch,
        .simulation_rate = epoch_.simulation_rate,
        .reference_rate = epoch_.reference_rate,
        .step = record.step,
        .timer_state = record.timer_state,
        .guest_time_cycles = record.guest_time_cycles,
        .guest_time_cycle_remainder = record.guest_time_cycle_remainder,
        .authoritative = true,
    };
    last_simulation_tick_ = record.step.simulation_tick;
    last_reference_tick_ = record.step.reference_tick;
    last_timer_state_ = record.timer_state;
    ++batch_recorded_;
    return true;
}

bool NativeSimulationStatePublisher::end_batch() noexcept {
    if (!batch_open_ || batch_recorded_ != batch_expected_) {
        fail(NativeSimulationStateFailure::BatchState);
        return false;
    }
    if (failure() != NativeSimulationStateFailure::None) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(publication_mutex_);
        const auto next_count = publication_count_ + batch_recorded_;
        if (batch_recorded_ == 1U) {
            NativeSimulationStateSnapshot snapshot = batch_storage_[0];
            if (retained_count_ == 0U) {
                snapshot.publication_sequence = next_count;
                current_ = snapshot;
                retained_count_ = 1U;
            } else {
                previous_ = current_;
                snapshot.publication_sequence = next_count;
                current_ = snapshot;
                retained_count_ = 2U;
            }
        } else if (batch_recorded_ > 1U) {
            previous_ = batch_storage_[batch_recorded_ - 2U];
            current_ = batch_storage_[batch_recorded_ - 1U];
            previous_.publication_sequence = next_count - 1U;
            current_.publication_sequence = next_count;
            retained_count_ = 2U;
        }
        publication_count_ = next_count;
        presentation_ = NativeSimulationPresentationState{
            .session_epoch = epoch_.session_epoch,
            .associated_publication_sequence = next_count,
            .active_time_backlog_nanoseconds = batch_frame_.schedule.backlog_nanoseconds,
            .presentation_delta_nanoseconds = batch_frame_.schedule.presentation_delta_nanoseconds,
            .interpolation_alpha = batch_frame_.schedule.interpolation_alpha,
            .catch_up_limited = batch_frame_.schedule.catch_up_limited,
            .presentation_allowed = batch_frame_.schedule.presentation_allowed,
            .authoritative = batch_frame_.guest_time_authoritative,
        };
        have_presentation_ = true;
    }
    last_guest_time_cycles_ = batch_frame_.guest_time_cycles;
    last_guest_time_cycle_remainder_ = batch_frame_.guest_time_cycle_remainder;
    batch_open_ = false;
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
    return true;
}

void NativeSimulationStatePublisher::abort_batch() noexcept {
    if (!batch_open_) {
        return;
    }
    last_simulation_tick_ = batch_previous_simulation_tick_;
    last_reference_tick_ = batch_previous_reference_tick_;
    last_guest_time_cycles_ = batch_previous_guest_time_cycles_;
    last_guest_time_cycle_remainder_ = batch_previous_guest_time_cycle_remainder_;
    last_timer_state_ = batch_previous_timer_state_;
    batch_open_ = false;
    batch_expected_ = 0U;
    batch_recorded_ = 0U;
}

void NativeSimulationStatePublisher::fail(const NativeSimulationStateFailure failure) noexcept {
    auto expected = NativeSimulationStateFailure::None;
    failure_.compare_exchange_strong(expected, failure, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool NativeSimulationStatePublisher::check_step(const NativeSimulationTraceStep& record) noexcept {
    const SimulationStep& step = record.step;
    const ExpectedStepMetadata expected =
        expected_step_metadata(step.simulation_tick, epoch_.simulation_rate, epoch_.reference_rate);
    const double expected_guest_seconds = static_cast<double>(step.simulation_tick) * step.delta_seconds;
    RetailReferenceTimerState expected_timer_state = last_timer_state_;
    try {
        if (step.reference_tick_due) {
            RetailReferenceTimer expected_timer(epoch_.reference_delta_seconds, last_timer_state_);
            expected_timer.update(step);
            expected_timer_state = expected_timer.state();
        }
    } catch (...) {
        fail(NativeSimulationStateFailure::InvalidStep);
        return false;
    }
    if (record.batch_step_index != batch_recorded_ || !same_schedule(record.frame, batch_frame_.schedule) ||
        record.guest_time_cycles != batch_frame_.guest_time_cycles ||
        record.guest_time_cycle_remainder != batch_frame_.guest_time_cycle_remainder ||
        record.guest_time_cycle_remainder >= epoch_.guest_time_rate.numerator ||
        !std::isfinite(record.guest_time_seconds) || bits(record.guest_time_seconds) != bits(expected_guest_seconds) ||
        !step_seconds_match(step.delta_seconds, epoch_.simulation_rate) ||
        !valid_alpha(record.frame.interpolation_alpha) || !expected.valid ||
        step.reference_tick != expected.reference_tick || step.reference_tick_due != expected.reference_tick_due ||
        step.boundary_numerator != expected.boundary_numerator ||
        step.boundary_denominator != expected.boundary_denominator ||
        !same_bits(step.reference_scale, expected.reference_scale) ||
        !(record.timer_state == expected_timer_state)) {
        fail(NativeSimulationStateFailure::InvalidStep);
        return false;
    }
    if (last_simulation_tick_ == std::numeric_limits<std::uint64_t>::max() ||
        step.simulation_tick != last_simulation_tick_ + 1U) {
        fail(NativeSimulationStateFailure::NonMonotonicSimulationTick);
        return false;
    }
    const bool advances_reference = last_reference_tick_ != std::numeric_limits<std::uint64_t>::max() &&
                                    step.reference_tick == last_reference_tick_ + 1U;
    if (step.reference_tick < last_reference_tick_ ||
        step.reference_tick > last_reference_tick_ + static_cast<std::uint64_t>(last_reference_tick_ != std::numeric_limits<std::uint64_t>::max()) ||
        step.reference_tick_due != advances_reference) {
        fail(NativeSimulationStateFailure::NonMonotonicReferenceTick);
        return false;
    }
    if ((step.reference_tick_due &&
         (step.boundary_numerator == 0U || step.boundary_denominator == 0U ||
          step.boundary_numerator > step.boundary_denominator)) ||
        (!step.reference_tick_due && (step.boundary_numerator != 0U || step.boundary_denominator != 1U))) {
        fail(NativeSimulationStateFailure::InvalidStep);
        return false;
    }
    if (step.reference_tick > std::numeric_limits<std::uint32_t>::max() ||
        record.timer_state.observed_tick != static_cast<std::uint32_t>(step.reference_tick)) {
        fail(NativeSimulationStateFailure::InvalidStep);
        return false;
    }
    return true;
}

} // namespace takedown::runtime
