#include "takedown/runtime/native_simulation_driver.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000U;

// First simulation boundary at or after `simulation_tick`, rounded up to a
// whole nanosecond. nullopt means the rate or the product is unrepresentable.
[[nodiscard]] std::optional<std::uint64_t>
simulation_boundary_nanoseconds(FixedStepRate rate, const std::uint64_t simulation_tick) {
    if (rate.numerator == 0U || rate.denominator == 0U) {
        return std::nullopt;
    }
    const auto divisor = std::gcd(rate.numerator, rate.denominator);
    rate.numerator /= divisor;
    rate.denominator /= divisor;
    const auto period_numerator =
        static_cast<std::uint64_t>(rate.denominator) * kNanosecondsPerSecond;
    if (static_cast<std::uint64_t>(rate.numerator) > period_numerator) {
        return std::nullopt;
    }
    if (simulation_tick == 0U) {
        return 0U;
    }
    const auto numerator = static_cast<unsigned __int128>(simulation_tick) * period_numerator;
    const auto denominator = static_cast<unsigned __int128>(rate.numerator);
    const auto boundary = (numerator + denominator - 1U) / denominator;
    if (boundary > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(boundary);
}

// True when the point's phase lies inside the interval that follows its tick.
[[nodiscard]] bool phase_is_inside_step(const FixedStepRate simulation_rate,
                                        const NativeSimulationPhasePoint& point) {
    const auto boundary = simulation_boundary_nanoseconds(simulation_rate, point.simulation_tick);
    if (!boundary || point.simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto next_boundary =
        simulation_boundary_nanoseconds(simulation_rate, point.simulation_tick + 1U);
    if (!next_boundary || *next_boundary <= *boundary) {
        return false;
    }
    return point.simulation_phase_nanoseconds < *next_boundary - *boundary;
}

// Lexicographic order on (tick, phase, cycles).
[[nodiscard]] bool precedes(const NativeSimulationPhasePoint& earlier,
                            const NativeSimulationPhasePoint& later) {
    return earlier.simulation_tick < later.simulation_tick ||
           (earlier.simulation_tick == later.simulation_tick &&
            (earlier.simulation_phase_nanoseconds < later.simulation_phase_nanoseconds ||
             (earlier.simulation_phase_nanoseconds == later.simulation_phase_nanoseconds &&
              earlier.guest_time_cycles < later.guest_time_cycles)));
}

[[nodiscard]] bool timer_state_is_usable(const RetailReferenceTimerState& state) {
    return state.subdivisions > 0 && state.pending_subdivisions > 0 &&
           std::isfinite(state.step_seconds) && state.step_seconds > 0.0F &&
           std::isfinite(state.elapsed_seconds);
}

[[nodiscard]] bool is_zero_point(const NativeSimulationPhasePoint& point) {
    return point.simulation_tick == 0U && point.simulation_phase_nanoseconds == 0U &&
           point.guest_time_cycles == 0U && point.guest_time_cycle_remainder == 0U;
}

} // namespace

// RECOVERY: the original GIMPLE reloads both guest_time_cycle_remainder fields
// after the boundary calls; here GCC reuses the earlier loads.
bool native_simulation_phase_origin_matches(const FixedStepRate simulation_rate,
                                            const FixedStepRate guest_time_rate,
                                            const NativeSimulationPhaseOrigin& origin,
                                            const NativeSimulationPhasePoint& point) noexcept {
    if (guest_time_rate.numerator == 0U || guest_time_rate.denominator == 0U) {
        return false;
    }
    if (origin.point.simulation_tick > point.simulation_tick ||
        origin.point.guest_time_cycles > point.guest_time_cycles) {
        return false;
    }
    if (!precedes(origin.point, point)) {
        return false;
    }
    if (origin.point.guest_time_cycle_remainder >= guest_time_rate.numerator ||
        point.guest_time_cycle_remainder >= guest_time_rate.numerator) {
        return false;
    }
    if (!phase_is_inside_step(simulation_rate, origin.point) ||
        !phase_is_inside_step(simulation_rate, point)) {
        return false;
    }

    const auto cycle_delta = point.guest_time_cycles - origin.point.guest_time_cycles;
    const auto exact_nanoseconds =
        static_cast<unsigned __int128>(cycle_delta) * guest_time_rate.denominator *
            kNanosecondsPerSecond +
        origin.point.guest_time_cycle_remainder;
    const auto guest_rate_numerator = static_cast<unsigned __int128>(guest_time_rate.numerator);
    const auto elapsed_nanoseconds = exact_nanoseconds / guest_rate_numerator;
    const auto expected_remainder = exact_nanoseconds % guest_rate_numerator;
    if (elapsed_nanoseconds > std::numeric_limits<std::uint64_t>::max() ||
        point.guest_time_cycle_remainder != expected_remainder) {
        return false;
    }

    const auto origin_boundary =
        simulation_boundary_nanoseconds(simulation_rate, origin.point.simulation_tick);
    const auto point_boundary =
        simulation_boundary_nanoseconds(simulation_rate, point.simulation_tick);
    if (!origin_boundary || !point_boundary) {
        return false;
    }
    if (*origin_boundary >
        std::numeric_limits<std::uint64_t>::max() - origin.point.simulation_phase_nanoseconds) {
        return false;
    }
    const auto origin_absolute_nanoseconds =
        *origin_boundary + origin.point.simulation_phase_nanoseconds;
    if (std::numeric_limits<std::uint64_t>::max() - origin_absolute_nanoseconds <
        elapsed_nanoseconds) {
        return false;
    }
    const auto point_absolute_nanoseconds =
        static_cast<std::uint64_t>(elapsed_nanoseconds) + origin_absolute_nanoseconds;
    if (*point_boundary >
        std::numeric_limits<std::uint64_t>::max() - point.simulation_phase_nanoseconds) {
        return false;
    }
    const auto expected_point_nanoseconds = *point_boundary + point.simulation_phase_nanoseconds;
    // Integer-nanosecond rounding of the two endpoints may differ by one.
    const auto distance = point_absolute_nanoseconds <= expected_point_nanoseconds
                              ? expected_point_nanoseconds - point_absolute_nanoseconds
                              : point_absolute_nanoseconds - expected_point_nanoseconds;
    if (distance > 1U) {
        return false;
    }
    return true;
}

NativeSimulationDriver::NativeSimulationDriver(NativeSimulationDriverConfig config)
    : config_(config),
      clock_(config_.clock),
      sequence_(clock_.simulation_rate(), config_.reference_rate),
      timer_(0U, config_.reference_delta_seconds) {
    if (config_.guest_time_rate.numerator == 0U || config_.guest_time_rate.denominator == 0U) {
        throw std::invalid_argument("guest-time rate numerator and denominator must be nonzero");
    }
}

void NativeSimulationDriver::seed_session(const NativeSimulationDriverSeed& seed) {
    if (!config_.enabled) {
        return;
    }
    if (seed.reference_tick > std::numeric_limits<std::uint32_t>::max() ||
        seed.reference_timer_state.observed_tick != static_cast<std::uint32_t>(seed.reference_tick)) {
        throw std::invalid_argument(
            "seeded reference ordinal and timer observation must share the retail tick");
    }
    if (seed.guest_time_cycle_remainder >= config_.guest_time_rate.numerator) {
        throw std::invalid_argument("seeded guest-time cycle remainder is outside its rate");
    }
    const NativeSimulationPhasePoint point{
        .simulation_tick = seed.simulation_tick,
        .simulation_phase_nanoseconds = seed.simulation_phase_nanoseconds,
        .guest_time_cycles = seed.guest_time_cycles,
        .guest_time_cycle_remainder = seed.guest_time_cycle_remainder,
    };
    const bool zero_point = is_zero_point(point);
    if (!zero_point && !seed.simulation_phase_origin) {
        throw std::invalid_argument("nonzero simulation seed requires an explicit phase origin");
    }
    if (seed.simulation_phase_origin &&
        !native_simulation_phase_origin_matches(clock_.simulation_rate(), config_.guest_time_rate,
                                                *seed.simulation_phase_origin, point)) {
        throw std::invalid_argument("seeded simulation phase origin is not aligned");
    }

    // Build every replacement first so a rejected seed leaves the driver untouched.
    auto next_clock = clock_;
    auto next_sequence = sequence_;
    if (!next_clock.seed_with_phase(seed.simulation_tick, seed.simulation_phase_nanoseconds)) {
        throw std::invalid_argument(
            "seeded simulation phase is outside the next fixed-step interval");
    }
    next_sequence.seed(seed.simulation_tick, seed.reference_tick);

    auto timer_state = seed.reference_timer_state;
    if (zero_point && timer_state == RetailReferenceTimerState{}) {
        timer_state = RetailReferenceTimer{0U, config_.reference_delta_seconds}.state();
    }
    if (!timer_state_is_usable(timer_state)) {
        throw std::invalid_argument("seeded reference timer state is not finite");
    }
    RetailReferenceTimer next_timer{config_.reference_delta_seconds, timer_state};
    if (seed.monotonic_nanoseconds) {
        next_clock.resync_to(*seed.monotonic_nanoseconds);
    }

    clock_ = next_clock;
    sequence_ = next_sequence;
    timer_ = next_timer;
    last_steps_.clear();
    guest_time_cycles_ = seed.guest_time_cycles;
    guest_time_cycle_remainder_ = seed.guest_time_cycle_remainder;
    time_authority_ = TimeAuthority::Unset;
}

NativeSimulationDriverFrame NativeSimulationDriver::advance(const double presentation_delta_seconds) {
    if (!config_.enabled) {
        return disabled_frame(0U);
    }
    if (time_authority_ == TimeAuthority::GuestCycles) {
        throw std::invalid_argument("presentation time cannot follow guest-cycle authority");
    }
    auto next_clock = clock_;
    const auto frame = execute(next_clock.advance(presentation_delta_seconds));
    clock_ = next_clock;
    time_authority_ = TimeAuthority::Presentation;
    return frame;
}

NativeSimulationDriverFrame
NativeSimulationDriver::disabled_frame(const std::uint64_t presentation_delta_nanoseconds) noexcept {
    const auto seconds = static_cast<double>(presentation_delta_nanoseconds) / 1.0e9;
    last_steps_.clear();
    return {
        .schedule =
            {
                .simulation_steps = 0U,
                .simulation_step_seconds = clock_.simulation_step_seconds(),
                .interpolation_alpha = 0.0,
                .presentation_delta_seconds = seconds,
                .presentation_delta_nanoseconds = presentation_delta_nanoseconds,
                .discarded_seconds = 0.0,
                .backlog_nanoseconds = clock_.accumulator_nanoseconds(),
                .catch_up_limited = false,
                .presentation_allowed = true,
            },
        .reference_ticks_delivered = 0U,
        .guest_time_cycles = guest_time_cycles_,
        .guest_time_cycle_remainder = guest_time_cycle_remainder_,
        .guest_time_authoritative = false,
        .enabled = false,
    };
}

NativeSimulationDriverFrame
NativeSimulationDriver::advance_nanoseconds(const std::uint64_t presentation_delta_nanoseconds) {
    if (!config_.enabled) {
        return disabled_frame(presentation_delta_nanoseconds);
    }
    if (time_authority_ == TimeAuthority::GuestCycles) {
        throw std::invalid_argument("presentation time cannot follow guest-cycle authority");
    }
    auto next_clock = clock_;
    const auto frame = execute(next_clock.advance_nanoseconds(presentation_delta_nanoseconds));
    clock_ = next_clock;
    time_authority_ = TimeAuthority::Presentation;
    return frame;
}

NativeSimulationDriverFrame
NativeSimulationDriver::advance(const std::chrono::nanoseconds presentation_delta) {
    if (!config_.enabled) {
        if (presentation_delta.count() < 0) {
            return disabled_frame(0U);
        }
        return disabled_frame(static_cast<std::uint64_t>(presentation_delta.count()));
    }
    if (time_authority_ == TimeAuthority::GuestCycles) {
        throw std::invalid_argument("presentation time cannot follow guest-cycle authority");
    }
    auto next_clock = clock_;
    const auto frame = execute(next_clock.advance(presentation_delta));
    clock_ = next_clock;
    time_authority_ = TimeAuthority::Presentation;
    return frame;
}

NativeSimulationDriverFrame NativeSimulationDriver::advance_to(const std::uint64_t monotonic_nanoseconds) {
    if (!config_.enabled) {
        return disabled_frame(0U);
    }
    if (time_authority_ == TimeAuthority::GuestCycles) {
        throw std::invalid_argument("presentation time cannot follow guest-cycle authority");
    }
    auto next_clock = clock_;
    const auto frame = execute(next_clock.advance_to(monotonic_nanoseconds));
    clock_ = next_clock;
    time_authority_ = TimeAuthority::Presentation;
    return frame;
}

NativeSimulationDriverFrame
NativeSimulationDriver::advance_guest_cycles(const std::uint64_t guest_cycle_delta) {
    if (!config_.enabled) {
        return disabled_frame(0U);
    }
    if (time_authority_ == TimeAuthority::Presentation) {
        throw std::invalid_argument("guest-cycle time cannot follow presentation authority");
    }
    if (std::numeric_limits<std::uint64_t>::max() - guest_time_cycles_ < guest_cycle_delta) {
        throw std::overflow_error("native guest-time cycle origin overflow");
    }
    const auto rate_numerator = static_cast<unsigned __int128>(config_.guest_time_rate.numerator);
    const auto rate_denominator =
        static_cast<unsigned __int128>(config_.guest_time_rate.denominator);
    const auto exact_nanoseconds =
        static_cast<unsigned __int128>(guest_cycle_delta) * rate_denominator *
            kNanosecondsPerSecond +
        guest_time_cycle_remainder_;
    const auto floor_nanoseconds = exact_nanoseconds / rate_numerator;
    const auto next_remainder = exact_nanoseconds % rate_numerator;
    if (floor_nanoseconds > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("native guest-time nanosecond conversion overflow");
    }
    auto nanoseconds = static_cast<std::uint64_t>(floor_nanoseconds);
    // A nonzero remainder means the endpoint was rounded up to a whole
    // nanosecond; keep the integer timeline consistent between batches.
    const bool previous_endpoint_rounded_up = guest_time_cycle_remainder_ != 0U;
    const bool next_endpoint_rounded_up = next_remainder != 0U;
    if (!previous_endpoint_rounded_up && next_endpoint_rounded_up) {
        if (nanoseconds == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("native guest-time nanosecond conversion overflow");
        }
        ++nanoseconds;
    } else if (previous_endpoint_rounded_up && !next_endpoint_rounded_up) {
        if (nanoseconds == 0U) {
            throw std::overflow_error("native guest-time rounded endpoint regressed");
        }
        --nanoseconds;
    }
    const auto next_cycles = guest_time_cycles_ + guest_cycle_delta;
    auto next_clock = clock_;
    const auto frame = execute(next_clock.advance_nanoseconds(nanoseconds), next_cycles,
                               static_cast<std::uint64_t>(next_remainder), true);
    clock_ = next_clock;
    guest_time_cycles_ = next_cycles;
    guest_time_cycle_remainder_ = static_cast<std::uint64_t>(next_remainder);
    time_authority_ = TimeAuthority::GuestCycles;
    return frame;
}

void NativeSimulationDriver::resync_to(const std::uint64_t monotonic_nanoseconds) noexcept {
    if (config_.enabled) {
        clock_.resync_to(monotonic_nanoseconds);
    }
}

void NativeSimulationDriver::reset() noexcept {
    clock_.reset();
    sequence_ = SimulationStepSequence{clock_.simulation_rate(), config_.reference_rate};
    timer_.reset(0U, config_.reference_delta_seconds);
    last_steps_.clear();
    guest_time_cycles_ = 0U;
    guest_time_cycle_remainder_ = 0U;
    time_authority_ = TimeAuthority::Unset;
}

std::optional<RetailReferenceTimerState>
NativeSimulationDriver::projected_reference_timer_state() const noexcept {
    if (time_authority_ == TimeAuthority::Presentation) {
        return std::nullopt;
    }
    auto projected = timer_;
    auto next_sequence = sequence_;
    const auto simulation_boundary =
        simulation_boundary_nanoseconds(clock_.simulation_rate(), sequence_.simulation_tick());
    if (!simulation_boundary ||
        clock_.accumulator_nanoseconds() >
            std::numeric_limits<std::uint64_t>::max() - *simulation_boundary) {
        return std::nullopt;
    }
    const auto current_absolute_nanoseconds =
        *simulation_boundary + clock_.accumulator_nanoseconds();
    SimulationStep next_step;
    try {
        next_step = next_sequence.advance();
    } catch (...) {
        return std::nullopt;
    }
    std::uint64_t candidate_reference_tick = next_step.reference_tick;
    if (!next_step.reference_tick_due) {
        if (next_step.reference_tick == std::numeric_limits<std::uint64_t>::max()) {
            return projected.state();
        }
        candidate_reference_tick = next_step.reference_tick + 1U;
    }
    const auto reference_boundary =
        simulation_boundary_nanoseconds(config_.reference_rate, candidate_reference_tick);
    if (!reference_boundary) {
        return std::nullopt;
    }
    if (!next_step.reference_tick_due) {
        if (*reference_boundary <= current_absolute_nanoseconds) {
            return std::nullopt;
        }
        return projected.state();
    }
    if (*simulation_boundary > *reference_boundary) {
        return std::nullopt;
    }
    const auto boundary_offset = *reference_boundary - *simulation_boundary;
    if (clock_.accumulator_nanoseconds() < boundary_offset) {
        return projected.state();
    }
    if (next_step.reference_tick > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    try {
        projected.update(static_cast<std::uint32_t>(next_step.reference_tick));
    } catch (...) {
        return std::nullopt;
    }
    {
        const auto following_reference_boundary =
            simulation_boundary_nanoseconds(config_.reference_rate, next_step.reference_tick + 1U);
        if (!following_reference_boundary ||
            *following_reference_boundary <= current_absolute_nanoseconds) {
            return std::nullopt;
        }
    }
    return projected.state();
}

NativeSimulationDriverFrame NativeSimulationDriver::execute(FrameSchedule schedule) {
    return execute(schedule, guest_time_cycles_, guest_time_cycle_remainder_, false);
}

NativeSimulationDriverFrame
NativeSimulationDriver::execute(const FrameSchedule schedule, const std::uint64_t guest_time_cycles,
                                const std::uint64_t guest_time_cycle_remainder,
                                const bool guest_time_authoritative) {
    const NativeSimulationDriverFrame frame{
        .schedule = schedule,
        .guest_time_cycles = guest_time_cycles,
        .guest_time_cycle_remainder = guest_time_cycle_remainder,
        .guest_time_authoritative = guest_time_authoritative,
        .enabled = true,
    };
    const bool has_sink = step_sink_ != nullptr;
    last_steps_.clear();
    if (has_sink && !step_sink_->begin_batch(frame, schedule.simulation_steps)) {
        throw std::overflow_error("native simulation step sink rejected its batch");
    }

    const auto previous_sequence = sequence_;
    const auto previous_timer = timer_;
    std::uint32_t reference_ticks_delivered = 0U;
    try {
        for (std::uint32_t index = 0U; index < schedule.simulation_steps; ++index) {
            const auto step = sequence_.advance();
            last_steps_.push_back(step);
            if (step.reference_tick_due) {
                timer_.update(step);
                if (reference_ticks_delivered == std::numeric_limits<std::uint32_t>::max()) {
                    throw std::overflow_error("native simulation reference delivery overflow");
                }
                ++reference_ticks_delivered;
            }
            if (has_sink) {
                if (!step_sink_->record_step({
                        .step = step,
                        .frame = frame.schedule,
                        .timer_state = timer_.state(),
                        .guest_time_seconds =
                            static_cast<double>(step.simulation_tick) * step.delta_seconds,
                        .guest_time_cycles = guest_time_cycles,
                        .guest_time_cycle_remainder = guest_time_cycle_remainder,
                        .batch_step_index = index,
                    })) {
                    throw std::overflow_error("native simulation step sink rejected a step");
                }
            }
        }
        if (has_sink && !step_sink_->end_batch()) {
            throw std::overflow_error("native simulation step sink rejected commit");
        }
    } catch (...) {
        sequence_ = previous_sequence;
        timer_ = previous_timer;
        last_steps_.clear();
        if (has_sink) {
            step_sink_->abort_batch();
        }
        throw;
    }
    return {
        .schedule = schedule,
        .reference_ticks_delivered = reference_ticks_delivered,
        .guest_time_cycles = guest_time_cycles,
        .guest_time_cycle_remainder = guest_time_cycle_remainder,
        .guest_time_authoritative = guest_time_authoritative,
        .enabled = true,
    };
}

} // namespace takedown::runtime
