#include "takedown/runtime/native_simulation_shadow.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace takedown::runtime {
namespace {

// Session epochs are unique per process so a stale publication token can
// never be mistaken for one minted by a later session.
std::atomic<std::uint64_t> g_shadow_epoch_generation{1};

NativeSimulationDriverConfig shadow_driver_config(const bool enabled,
                                                  NativeSimulationDriverConfig config) {
    config.enabled = enabled;
    if (config.clock.max_catch_up_steps == 0U) {
        config.clock.max_catch_up_steps = 8U;
    }
    return config;
}

// FNV-1a over the little-endian bytes of one 64-bit word.
void mix(std::uint64_t& hash, const std::uint64_t value) {
    for (unsigned shift = 0U; shift != 64U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= 1099511628211ULL;
    }
}

// False when the counter is saturated; the caller then fails closed.
[[nodiscard]] bool checked_increment(std::uint64_t& counter) noexcept {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++counter;
    return true;
}

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;

// Calibration of a guest-cycle sample against the fixed simulation timeline.
struct SimulationPhaseCalibration {
    std::uint64_t simulation_tick{};
    std::uint64_t origin_simulation_tick{};
    std::uint64_t phase_nanoseconds{};
    std::uint64_t origin_guest_time_cycles{};
    std::uint64_t origin_guest_time_cycle_remainder{};
};

// Exact guest-cycle remainder of the cycles-to-nanoseconds conversion.
[[nodiscard]] std::uint64_t guest_cycle_remainder(const FixedStepRate guest,
                                                  const std::uint64_t cycles) {
    const auto exact =
        static_cast<unsigned __int128>(cycles) * guest.denominator * kNanosecondsPerSecond;
    return static_cast<std::uint64_t>(exact % guest.numerator);
}

// Integer-nanosecond rounding of the sample and of its step-aligned origin
// can disagree by one when exactly one of them lands on a whole nanosecond.
[[nodiscard]] bool rounds_phase_up(const std::uint64_t sample_remainder,
                                   const std::uint64_t origin_remainder) {
    return sample_remainder != 0U && origin_remainder == 0U;
}

[[nodiscard]] bool rounds_phase_down(const std::uint64_t sample_remainder,
                                     const std::uint64_t origin_remainder) {
    return sample_remainder == 0U && origin_remainder != 0U;
}

// Mixes the owner entry and the seed into a non-zero session epoch. The
// generation keeps epochs distinct when two sessions start from equal input.
[[nodiscard]] std::uint64_t session_epoch_for(const std::uint32_t shared_tick,
                                              const float reference_delta_seconds,
                                              const std::uint64_t emulated_ee_cycles,
                                              const NativeSimulationDriverSeed& seed,
                                              const std::uint64_t generation) {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, shared_tick);
    mix(hash, std::bit_cast<std::uint32_t>(reference_delta_seconds));
    mix(hash, emulated_ee_cycles);
    mix(hash, seed.simulation_tick);
    mix(hash, seed.reference_tick);
    mix(hash, seed.simulation_phase_nanoseconds);
    mix(hash, seed.guest_time_cycles);
    mix(hash, seed.guest_time_cycle_remainder);
    const auto& timer = seed.reference_timer_state;
    mix(hash, timer.observed_tick);
    mix(hash, static_cast<std::uint32_t>(timer.consumed_tick_offset));
    mix(hash, static_cast<std::uint32_t>(timer.paused_ticks));
    mix(hash, static_cast<std::uint32_t>(timer.whole_ticks));
    mix(hash, static_cast<std::uint32_t>(timer.subdivision_remainder));
    mix(hash, timer.baseline_tick);
    mix(hash, static_cast<std::uint32_t>(timer.subdivisions));
    mix(hash, std::bit_cast<std::uint32_t>(timer.step_seconds));
    mix(hash, std::bit_cast<std::uint32_t>(timer.elapsed_seconds));
    mix(hash, static_cast<std::uint32_t>(timer.pending_subdivisions));
    mix(hash, timer.accumulating ? 1U : 0U);
    mix(hash, generation);
    return std::max<std::uint64_t>(hash, 1U);
}

// floor(reference_tick * simulation / reference); nullopt when the rates are
// degenerate, the reference is faster than the simulation, or on overflow.
[[nodiscard]] std::optional<std::uint64_t>
simulation_tick_for_reference(const SimulationStepSequence& sequence,
                              const std::uint32_t reference_tick) {
    const auto simulation = sequence.simulation_rate();
    const auto reference = sequence.reference_rate();
    const auto scale_numerator =
        static_cast<std::uint64_t>(reference.numerator) * simulation.denominator;
    const auto scale_denominator =
        static_cast<std::uint64_t>(reference.denominator) * simulation.numerator;
    if (scale_numerator == 0U || scale_denominator == 0U || scale_numerator > scale_denominator) {
        return std::nullopt;
    }
    const auto scaled = static_cast<unsigned __int128>(reference_tick) * scale_denominator;
    const auto quotient = scaled / scale_numerator;
    if (quotient > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(quotient);
}

// floor(simulation_tick * reference / simulation).
[[nodiscard]] std::optional<std::uint64_t>
reference_tick_for_simulation(const SimulationStepSequence& sequence,
                              const std::uint64_t simulation_tick) {
    const auto simulation = sequence.simulation_rate();
    const auto reference = sequence.reference_rate();
    const auto scale_numerator =
        static_cast<std::uint64_t>(reference.numerator) * simulation.denominator;
    const auto scale_denominator =
        static_cast<std::uint64_t>(reference.denominator) * simulation.numerator;
    if (scale_numerator == 0U || scale_denominator == 0U || scale_numerator > scale_denominator) {
        return std::nullopt;
    }
    const auto scaled = static_cast<unsigned __int128>(simulation_tick) * scale_numerator;
    const auto quotient = scaled / scale_denominator;
    if (quotient > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(quotient);
}

// The retail timer only runs at 60 Hz or 30 Hz; the bit pattern is compared
// so the mapping is exact.
[[nodiscard]] std::optional<FixedStepRate> reference_rate_for_delta(const float delta_seconds) {
    if (std::bit_cast<std::uint32_t>(delta_seconds) == std::bit_cast<std::uint32_t>(1.0F / 60.0F)) {
        return FixedStepRate{60U, 1U};
    }
    if (std::bit_cast<std::uint32_t>(delta_seconds) == std::bit_cast<std::uint32_t>(1.0F / 30.0F)) {
        return FixedStepRate{30U, 1U};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SimulationPhaseCalibration>
calibrate_phase(const NativeSimulationDriver& driver, const std::uint64_t simulation_tick,
                const std::uint64_t emulated_ee_cycles) {
    const auto guest = driver.guest_time_rate();
    const auto simulation = driver.clock().simulation_rate();
    const auto cycles_per_step_numerator =
        static_cast<unsigned __int128>(guest.numerator) * simulation.denominator;
    const auto cycles_per_step_denominator =
        static_cast<unsigned __int128>(guest.denominator) * simulation.numerator;
    if (cycles_per_step_denominator == 0U ||
        cycles_per_step_numerator % cycles_per_step_denominator != 0U) {
        return std::nullopt;
    }
    const auto cycles_per_step = cycles_per_step_numerator / cycles_per_step_denominator;
    if (cycles_per_step == 0U || cycles_per_step > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    const auto cycles_per_step_u64 = static_cast<std::uint64_t>(cycles_per_step);
    const auto phase_cycles = emulated_ee_cycles % cycles_per_step_u64;
    const auto phase_origin_cycles = emulated_ee_cycles - phase_cycles;
    const auto numerator = static_cast<unsigned __int128>(guest.numerator);
    const auto exact_phase_nanoseconds =
        guest_cycle_remainder(guest, phase_origin_cycles) +
        static_cast<unsigned __int128>(guest.denominator) * phase_cycles * kNanosecondsPerSecond;
    auto phase_nanoseconds = exact_phase_nanoseconds / numerator;
    if (rounds_phase_up(guest_cycle_remainder(guest, emulated_ee_cycles),
                        guest_cycle_remainder(guest, phase_origin_cycles))) {
        phase_nanoseconds += 1U;
    } else if (rounds_phase_down(guest_cycle_remainder(guest, emulated_ee_cycles),
                                 guest_cycle_remainder(guest, phase_origin_cycles))) {
        if (numerator > exact_phase_nanoseconds) {
            return std::nullopt;
        }
        phase_nanoseconds -= 1U;
    }
    if (phase_nanoseconds > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto origin_cycles = phase_origin_cycles;
    auto origin_simulation_tick = simulation_tick;
    if (phase_cycles == 0U && simulation_tick != 0U) {
        if (cycles_per_step_u64 > phase_origin_cycles) {
            return std::nullopt;
        }
        origin_cycles -= cycles_per_step_u64;
        origin_simulation_tick -= 1U;
    }
    return SimulationPhaseCalibration{
        simulation_tick,
        origin_simulation_tick,
        static_cast<std::uint64_t>(phase_nanoseconds),
        origin_cycles,
        guest_cycle_remainder(guest, origin_cycles),
    };
}

} // namespace

std::string_view to_string(const NativeSimulationShadowFailure failure) noexcept {
    switch (failure) {
    case NativeSimulationShadowFailure::None:
        return "none";
    case NativeSimulationShadowFailure::CycleRegression:
        return "cycle-regression";
    case NativeSimulationShadowFailure::CycleOverflow:
        return "cycle-overflow";
    case NativeSimulationShadowFailure::DriverRejected:
        return "driver-rejected";
    case NativeSimulationShadowFailure::TraceRejected:
        return "trace-rejected";
    case NativeSimulationShadowFailure::InvalidOwnerState:
        return "invalid-owner-state";
    case NativeSimulationShadowFailure::NestedOwner:
        return "nested-owner";
    case NativeSimulationShadowFailure::MissingOwnerReturn:
        return "missing-owner-return";
    case NativeSimulationShadowFailure::OwnerCapacity:
        return "owner-capacity";
    case NativeSimulationShadowFailure::IncompleteOwner:
        return "incomplete-owner";
    case NativeSimulationShadowFailure::TimerMismatch:
        return "timer-mismatch";
    case NativeSimulationShadowFailure::StatePublisherRejected:
        return "state-publisher-rejected";
    case NativeSimulationShadowFailure::NoOwner:
        return "no-owner";
    case NativeSimulationShadowFailure::Finalized:
        return "finalized";
    }
    return "unknown";
}

NativeSimulationShadow::NativeSimulationShadow(const bool enabled,
                                               NativeSimulationDriverConfig driver_config,
                                               const std::size_t trace_capacity)
    : enabled_(enabled),
      driver_config_(shadow_driver_config(enabled, driver_config)),
      trace_(std::span<NativeSimulationTraceStep>{
          trace_storage_.data(), trace_capacity < kNativeSimulationShadowTraceCapacity
                                       ? trace_capacity
                                       : kNativeSimulationShadowTraceCapacity}),
      state_publisher_(enabled ? std::span<NativeSimulationStateSnapshot>{state_storage_}
                               : std::span<NativeSimulationStateSnapshot>{}),
      state_step_sink_(trace_, state_publisher_),
      driver_(driver_config_) {
    summary_.enabled = enabled_;
    summary_.trace_capacity = trace_.capacity();
    summary_.state_capacity = state_publisher_.capacity();
    if (enabled_) {
        driver_.set_step_sink(&state_step_sink_);
    }
}

bool NativeSimulationShadow::StepSink::begin_batch(const NativeSimulationDriverFrame& frame,
                                                   const std::uint32_t step_count) noexcept {
    trace_batch_open_ = false;
    publisher_batch_open_ = false;
    expected_steps_ = step_count;
    recorded_steps_ = 0U;
    if (!trace_->begin_batch(frame, step_count)) {
        trace_->abort_batch();
        publisher_->abort_batch();
        return false;
    }
    trace_batch_open_ = true;
    if (!publisher_->begin_batch(frame, step_count)) {
        trace_->abort_batch();
        publisher_->abort_batch();
        trace_batch_open_ = false;
        return false;
    }
    publisher_batch_open_ = true;
    return true;
}

bool NativeSimulationShadow::StepSink::record_step(
    const NativeSimulationTraceStep& step) noexcept {
    if (!trace_batch_open_ || !publisher_batch_open_ || recorded_steps_ >= expected_steps_) {
        abort_batch();
        return false;
    }
    if (!trace_->record_step(step) || !publisher_->record_step(step)) {
        abort_batch();
        return false;
    }
    ++recorded_steps_;
    return true;
}

void NativeSimulationShadow::StepSink::abort_batch() noexcept {
    if (trace_batch_open_) {
        trace_->abort_batch();
    }
    if (publisher_batch_open_) {
        publisher_->abort_batch();
    }
    trace_batch_open_ = false;
    publisher_batch_open_ = false;
    expected_steps_ = 0U;
    recorded_steps_ = 0U;
}

bool NativeSimulationShadow::StepSink::end_batch() noexcept {
    if (!trace_batch_open_ || !publisher_batch_open_ || recorded_steps_ != expected_steps_ ||
        trace_->failure() != NativeSimulationTraceFailure::None ||
        publisher_->failure() != NativeSimulationStateFailure::None) {
        abort_batch();
        return false;
    }
    const bool trace_ok = trace_->end_batch();
    if (!trace_ok) {
        abort_batch();
        return false;
    }
    trace_batch_open_ = false;
    const bool publisher_ok = publisher_->end_batch();
    if (!publisher_ok) {
        abort_batch();
        return false;
    }
    publisher_batch_open_ = false;
    expected_steps_ = 0U;
    recorded_steps_ = 0U;
    return true;
}

bool NativeSimulationShadow::observe_guest_cycles(const std::uint64_t emulated_ee_cycles) noexcept {
    if (!enabled_) {
        return false;
    }
    if (finalized_) {
        fail(NativeSimulationShadowFailure::Finalized);
        return false;
    }
    if (summary_.failure != NativeSimulationShadowFailure::None) {
        return false;
    }
    if (!checked_increment(summary_.cycle_samples)) {
        fail(NativeSimulationShadowFailure::CycleOverflow);
        return false;
    }
    if (!have_cycle_sample_) {
        have_cycle_sample_ = true;
        last_ee_cycles_ = emulated_ee_cycles;
        return true;
    }
    if (emulated_ee_cycles < last_ee_cycles_) {
        fail(NativeSimulationShadowFailure::CycleRegression);
        return false;
    }
    const auto delta = emulated_ee_cycles - last_ee_cycles_;
    last_ee_cycles_ = emulated_ee_cycles;
    if (!seeded_ || delta == 0U) {
        return true;
    }
    if (!checked_increment(summary_.guest_cycle_batches)) {
        fail(NativeSimulationShadowFailure::CycleOverflow);
        return false;
    }
    try {
        (void)driver_.advance_guest_cycles(delta);
    } catch (...) {
        fail(NativeSimulationShadowFailure::DriverRejected);
        return false;
    }
    if (!trace_.valid()) {
        fail(NativeSimulationShadowFailure::TraceRejected);
        return false;
    }
    if (state_publisher_.failure() != NativeSimulationStateFailure::None) {
        fail(NativeSimulationShadowFailure::StatePublisherRejected);
        return false;
    }
    summary_.trace_steps = trace_.size();
    return true;
}

bool NativeSimulationShadow::read_latest_state(
    NativeSimulationStateSnapshot& snapshot) const noexcept {
    return enabled_ && state_publisher_.read_latest(snapshot);
}

bool NativeSimulationShadow::read_interpolation_state(
    NativeSimulationInterpolationState& state) const noexcept {
    return enabled_ && state_publisher_.read_interpolation_state(state);
}

NativeSimulationVehiclePublicationResult NativeSimulationShadow::publish_vehicle_snapshot_result(
    const Slus21050VehicleWorldContactPublicationToken& token) noexcept {
    if (!enabled_ || !seeded_ || finalized_ ||
        summary_.failure != NativeSimulationShadowFailure::None) {
        if (enabled_ && !finalized_ && summary_.failure == NativeSimulationShadowFailure::None) {
            fail(NativeSimulationShadowFailure::StatePublisherRejected);
        }
        return NativeSimulationVehiclePublicationResult::Rejected;
    }
    const auto result = state_publisher_.publish_vehicle_snapshot_result(token);
    if (result == NativeSimulationVehiclePublicationResult::Rejected) {
        fail(NativeSimulationShadowFailure::StatePublisherRejected);
    }
    return result;
}

bool NativeSimulationShadow::publish_vehicle_snapshot(
    const Slus21050VehicleWorldContactPublicationToken& token) noexcept {
    return publish_vehicle_snapshot_result(token) ==
           NativeSimulationVehiclePublicationResult::Published;
}

bool NativeSimulationShadow::ensure_cycle_sample(const std::uint64_t emulated_ee_cycles) noexcept {
    if (!have_cycle_sample_ || last_ee_cycles_ != emulated_ee_cycles) {
        return observe_guest_cycles(emulated_ee_cycles);
    }
    return summary_.failure == NativeSimulationShadowFailure::None;
}

// RECOVERY: matches the original GIMPLE except for statement order around the
// guest-cycle remainders (calibrate_phase and the seed's phase origin); the
// original evaluated those remainders once, earlier.
bool NativeSimulationShadow::seed_from_owner(const std::uint32_t shared_tick,
                                             const float reference_delta_seconds,
                                             const RetailReferenceTimerState& before,
                                             const RetailReferenceTimerState& expected,
                                             const std::uint64_t emulated_ee_cycles) noexcept {
    const auto reference_rate = reference_rate_for_delta(reference_delta_seconds);
    if (!reference_rate) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    auto next_config = driver_config_;
    next_config.reference_rate = *reference_rate;
    next_config.reference_delta_seconds = reference_delta_seconds;
    NativeSimulationDriver next_driver{next_config};
    next_driver.set_step_sink(&state_step_sink_);

    if (expected.observed_tick != shared_tick) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    const auto guest_time_rate = next_driver.guest_time_rate();
    const auto simulation_tick = simulation_tick_for_reference(next_driver.step_sequence(), expected.observed_tick);
    if (!simulation_tick) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    const auto calibration = calibrate_phase(next_driver, *simulation_tick, emulated_ee_cycles);
    if (!calibration) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    const auto boundary_reference_tick = reference_tick_for_simulation(next_driver.step_sequence(),
                                                              calibration->simulation_tick);
    if (!boundary_reference_tick) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }

    // The owner may be inside the next simulation step: then the seed
    // describes the last committed reference boundary.
    RetailReferenceTimerState seed_timer_state{};
    std::uint64_t seed_reference_tick = 0U;
    if (expected.observed_tick == *boundary_reference_tick) {
        seed_reference_tick = expected.observed_tick;
        seed_timer_state = expected;
    } else if (*boundary_reference_tick != std::numeric_limits<std::uint64_t>::max() &&
               expected.observed_tick == *boundary_reference_tick + 1U &&
               *boundary_reference_tick == before.observed_tick) {
        seed_reference_tick = before.observed_tick;
        seed_timer_state = before;
    } else {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }

    const NativeSimulationPhasePoint phase_origin_point{
        calibration->origin_simulation_tick, 0U, calibration->origin_guest_time_cycles,
        calibration->origin_guest_time_cycle_remainder};
    // A seed at the zero point carries no origin; anything else keeps the
    // calibrated step-aligned origin.
    const std::optional<NativeSimulationPhaseOrigin> phase_origin =
        (calibration->simulation_tick == 0U && calibration->phase_nanoseconds == 0U &&
         emulated_ee_cycles == 0U &&
         guest_cycle_remainder(guest_time_rate, emulated_ee_cycles) == 0U)
            ? std::optional<NativeSimulationPhaseOrigin>{}
            : std::optional<NativeSimulationPhaseOrigin>{
                  NativeSimulationPhaseOrigin{phase_origin_point}};
    NativeSimulationDriverSeed seed{
        calibration->simulation_tick,
        seed_reference_tick,
        seed_timer_state,
        calibration->phase_nanoseconds,
        emulated_ee_cycles,
        guest_cycle_remainder(guest_time_rate, emulated_ee_cycles),
    };
    seed.simulation_phase_origin = phase_origin;
    trace_.begin_session(seed, next_driver.clock().simulation_rate());
    if (!trace_.valid() && trace_.failure() != NativeSimulationTraceFailure::None) {
        fail(NativeSimulationShadowFailure::TraceRejected);
        return false;
    }
    try {
        next_driver.seed_session(seed);
    } catch (...) {
        fail(NativeSimulationShadowFailure::DriverRejected);
        return false;
    }
    if (!trace_.valid()) {
        fail(NativeSimulationShadowFailure::TraceRejected);
        return false;
    }

    NativeSimulationStateEpoch state_epoch{};
    state_epoch.session_epoch = session_epoch_for(
        shared_tick, reference_delta_seconds, emulated_ee_cycles, seed,
        g_shadow_epoch_generation.fetch_add(1U, std::memory_order_relaxed));
    state_epoch.simulation_rate = next_driver.clock().simulation_rate();
    state_epoch.reference_rate = next_driver.step_sequence().reference_rate();
    state_epoch.guest_time_rate = next_driver.guest_time_rate();
    state_epoch.reference_delta_seconds = reference_delta_seconds;
    state_epoch.simulation_tick = seed.simulation_tick;
    state_epoch.reference_tick = seed.reference_tick;
    state_epoch.guest_time_cycles = seed.guest_time_cycles;
    state_epoch.guest_time_cycle_remainder = seed.guest_time_cycle_remainder;
    state_epoch.reference_timer_state = seed.reference_timer_state;
    state_epoch.simulation_phase_nanoseconds = seed.simulation_phase_nanoseconds;
    state_epoch.simulation_phase_origin = seed.simulation_phase_origin;
    if (state_epoch.session_epoch == 0U || !state_publisher_.begin_epoch(state_epoch)) {
        trace_.reset();
        fail(NativeSimulationShadowFailure::StatePublisherRejected);
        return false;
    }
    state_epoch_ = state_epoch.session_epoch;
    driver_config_ = next_config;
    driver_ = std::move(next_driver);
    seeded_ = true;
    summary_.seeded = true;
    summary_.state_epoch = state_epoch_;
    return true;
}

bool NativeSimulationShadow::observe_owner_entry(const std::uint32_t shared_tick,
                                                 const float reference_delta_seconds,
                                                 const RetailReferenceTimerState& before,
                                                 const std::uint64_t emulated_ee_cycles) noexcept {
    if (!enabled_) {
        return false;
    }
    if (finalized_) {
        fail(NativeSimulationShadowFailure::Finalized);
        return false;
    }
    if (summary_.failure != NativeSimulationShadowFailure::None) {
        return false;
    }
    if (owner_active_) {
        fail(NativeSimulationShadowFailure::NestedOwner);
        return false;
    }
    if (!ensure_cycle_sample(emulated_ee_cycles)) {
        return false;
    }
    if (!checked_increment(summary_.owner_entries)) {
        fail(NativeSimulationShadowFailure::OwnerCapacity);
        return false;
    }
    if (!std::isfinite(reference_delta_seconds) || reference_delta_seconds <= 0.0F) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    try {
        RetailReferenceTimer expected_timer{reference_delta_seconds, before};
        expected_timer.update(shared_tick);
        owner_expected_state_ = expected_timer.state();
        if (!seeded_) {
            if (!seed_from_owner(shared_tick, reference_delta_seconds, before,
                                 owner_expected_state_, emulated_ee_cycles)) {
                return false;
            }
        } else if (std::bit_cast<std::uint32_t>(reference_delta_seconds) !=
                   std::bit_cast<std::uint32_t>(driver_config_.reference_delta_seconds)) {
            fail(NativeSimulationShadowFailure::InvalidOwnerState);
            return false;
        }
    } catch (...) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    owner_entry_ee_cycles_ = emulated_ee_cycles;
    owner_entry_shared_tick_ = shared_tick;
    owner_active_ = true;
    return true;
}

bool NativeSimulationShadow::observe_owner_return(const RetailReferenceTimerState& observed,
                                                  const std::uint64_t emulated_ee_cycles) noexcept {
    if (!enabled_) {
        return false;
    }
    if (finalized_) {
        fail(NativeSimulationShadowFailure::Finalized);
        return false;
    }
    if (summary_.failure != NativeSimulationShadowFailure::None) {
        return false;
    }
    if (!owner_active_) {
        fail(NativeSimulationShadowFailure::MissingOwnerReturn);
        return false;
    }
    if (!ensure_cycle_sample(emulated_ee_cycles) || emulated_ee_cycles < owner_entry_ee_cycles_) {
        if (summary_.failure == NativeSimulationShadowFailure::None) {
            fail(NativeSimulationShadowFailure::CycleRegression);
        }
        owner_active_ = false;
        return false;
    }
    if (owner_sample_count_ >= owner_samples_.size()) {
        ++summary_.dropped_owner_samples;
        fail(NativeSimulationShadowFailure::OwnerCapacity);
        return false;
    }
    const auto projected_shadow = driver_.projected_reference_timer_state();
    if (!projected_shadow) {
        owner_active_ = false;
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
        return false;
    }
    const RetailReferenceTimerState shadow = *projected_shadow;
    const bool matched = observed == owner_expected_state_ && shadow == owner_expected_state_;
    NativeSimulationShadowSample sample{
        .ee_cycles = emulated_ee_cycles,
        .owner_cycle_delta = emulated_ee_cycles - owner_entry_ee_cycles_,
        .shared_tick = owner_entry_shared_tick_,
        .expected = owner_expected_state_,
        .observed = observed,
        .shadow = shadow,
        .matched = matched,
    };
    owner_samples_[owner_sample_count_++] = sample;
    owner_active_ = false;
    if (!checked_increment(summary_.owner_returns)) {
        fail(NativeSimulationShadowFailure::OwnerCapacity);
        return false;
    }
    if (matched) {
        if (!checked_increment(summary_.matches)) {
            fail(NativeSimulationShadowFailure::OwnerCapacity);
            return false;
        }
        return true;
    }
    if (!checked_increment(summary_.mismatches)) {
        fail(NativeSimulationShadowFailure::OwnerCapacity);
        return false;
    }
    fail(NativeSimulationShadowFailure::TimerMismatch);
    return false;
}

void NativeSimulationShadow::reject_guest_snapshot() noexcept {
    if (enabled_ && !finalized_) {
        fail(NativeSimulationShadowFailure::InvalidOwnerState);
    }
}

void NativeSimulationShadow::fail(const NativeSimulationShadowFailure failure) noexcept {
    if (summary_.failure == NativeSimulationShadowFailure::None) {
        summary_.failure = failure;
    }
    if (const auto state_failure = state_publisher_.failure();
        state_failure != NativeSimulationStateFailure::None) {
        summary_.state_failure = state_failure;
    }
    owner_active_ = false;
}

NativeSimulationShadowSummary NativeSimulationShadow::finalize() noexcept {
    if (!enabled_) {
        return summary_;
    }
    if (!finalized_) {
        finalized_ = true;
        summary_.finalized = true;
        summary_.trace_steps = trace_.size();
        summary_.state_epoch = state_epoch_;
        summary_.state_publications = state_publisher_.publication_count();
        summary_.state_failure = state_publisher_.failure();
        if (owner_active_ && summary_.failure == NativeSimulationShadowFailure::None) {
            fail(NativeSimulationShadowFailure::IncompleteOwner);
        }
        if (!seeded_ && summary_.failure == NativeSimulationShadowFailure::None) {
            fail(NativeSimulationShadowFailure::NoOwner);
        }
        if (summary_.state_failure != NativeSimulationStateFailure::None &&
            summary_.failure == NativeSimulationShadowFailure::None) {
            fail(NativeSimulationShadowFailure::StatePublisherRejected);
        }
    }
    summary_.passed = summary_.failure == NativeSimulationShadowFailure::None && seeded_ &&
                      summary_.owner_entries != 0U &&
                      summary_.owner_entries == summary_.owner_returns &&
                      summary_.owner_returns == summary_.matches && summary_.mismatches == 0U &&
                      summary_.guest_cycle_batches != 0U && trace_.size() != 0U && trace_.valid() &&
                      state_epoch_ != 0U && state_publisher_.epoch_active() &&
                      state_publisher_.failure() == NativeSimulationStateFailure::None;
    return summary_;
}

} // namespace takedown::runtime
