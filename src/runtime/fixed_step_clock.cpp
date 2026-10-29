#include "takedown/runtime/fixed_step_clock.hpp"

#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

FixedStepRate normalized_rate(std::uint32_t numerator, std::uint32_t denominator) {
    if (numerator == 0U || denominator == 0U) {
        throw std::invalid_argument("simulation rate numerator and denominator must be nonzero");
    }
    const std::uint32_t divisor = std::gcd(numerator, denominator);
    return {numerator / divisor, denominator / divisor};
}

bool is_integral_hz(const long double& rounded, const long double& hz) {
    if (rounded >= 1.0L && rounded <= 4294967295.0L) {
        return std::abs(hz - rounded) <= std::numeric_limits<double>::epsilon() * std::max(1.0L, hz);
    }
    return false;
}

FixedStepRate legacy_rate_from_hz(const double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) {
        throw std::invalid_argument("simulation_hz must be finite and positive");
    }
    if (is_integral_hz(static_cast<long double>(__builtin_round(hz)), static_cast<long double>(hz))) {
        return {static_cast<std::uint32_t>(static_cast<long double>(__builtin_round(hz))), 1U};
    }
    const long double denominator_capacity = 4294967295.0L / static_cast<long double>(hz);
    const auto denominator = denominator_capacity >= 1'000'000.0L
                                 ? 1'000'000U
                                 : static_cast<std::uint32_t>(std::trunc(denominator_capacity));
    if (denominator == 0U) {
        throw std::invalid_argument("simulation_hz exceeds the supported rational rate range");
    }
    const long double scaled = static_cast<long double>(hz) * static_cast<long double>(denominator);
    if (std::round(scaled) < 1.0L || std::round(scaled) > 4294967295.0L) {
        throw std::invalid_argument("simulation_hz cannot be represented by the legacy rate seam");
    }
    return normalized_rate(static_cast<std::uint32_t>(std::round(scaled)), denominator);
}

FixedStepRate resolve_simulation_rate(const FixedStepClockConfig& config) {
    if (config.simulation_hz_numerator != 0U) {
        return normalized_rate(config.simulation_hz_numerator, config.simulation_hz_denominator);
    }
    if (config.simulation_hz_denominator == 0U) {
        throw std::invalid_argument("simulation_hz_denominator must be nonzero");
    }
    return legacy_rate_from_hz(config.simulation_hz);
}

void validate_config(const FixedStepClockConfig& config) {
    if (config.max_catch_up_steps == 0U) {
        throw std::invalid_argument("max_catch_up_steps must be at least one");
    }
    if (!std::isfinite(config.max_presentation_delta_seconds) ||
        config.max_presentation_delta_seconds <= 0.0) {
        throw std::invalid_argument("max_presentation_delta_seconds must be finite and positive");
    }
}

} // namespace

// RECOVERY: only the legacy-hz branch layout differs from the original GIMPLE (basic-block
// order and the long double rounded-value compare folding); behaviour is the same.
FixedStepGuestClock::FixedStepGuestClock(FixedStepClockConfig config)
    : config_(config), simulation_rate_(resolve_simulation_rate(config)) {
    validate_config(config_);
    const auto rational_period_nanoseconds =
        static_cast<std::uint64_t>(simulation_rate_.denominator) * kNanosecondsPerSecond;
    if (simulation_rate_.numerator > rational_period_nanoseconds) {
        throw std::invalid_argument("simulation rate must not exceed one tick per nanosecond");
    }
    config_.simulation_hz = static_cast<double>(simulation_rate_.numerator) /
                            static_cast<double>(simulation_rate_.denominator);
    config_.simulation_hz_numerator = simulation_rate_.numerator;
    config_.simulation_hz_denominator = simulation_rate_.denominator;
    simulation_step_seconds_ = static_cast<double>(simulation_rate_.denominator) /
                               static_cast<double>(simulation_rate_.numerator);
    step_base_nanoseconds_ = rational_period_nanoseconds / simulation_rate_.numerator;
    step_remainder_ =
        static_cast<std::uint32_t>(rational_period_nanoseconds % simulation_rate_.numerator);
    step_phase_ = step_remainder_ == 0U ? 0U : simulation_rate_.numerator - 1U;
}

FixedStepGuestClock::StepInterval FixedStepGuestClock::next_step_interval(
    const std::uint32_t phase) const noexcept {
    if (step_remainder_ == 0U) {
        return {step_base_nanoseconds_, 0U};
    }
    const auto carry_threshold = simulation_rate_.numerator - step_remainder_;
    if (phase >= carry_threshold) {
        return {step_base_nanoseconds_ + 1U, phase + (step_remainder_ - simulation_rate_.numerator)};
    }
    return {step_base_nanoseconds_, phase + step_remainder_};
}

FrameSchedule FixedStepGuestClock::advance_nanoseconds(const std::uint64_t presentation_delta_nanoseconds) {
    if (std::numeric_limits<std::uint64_t>::max() - accumulator_nanoseconds_ <
        presentation_delta_nanoseconds) {
        throw std::overflow_error("fixed-step active-time backlog overflow");
    }
    auto next_accumulator = accumulator_nanoseconds_ + presentation_delta_nanoseconds;
    auto next_phase = step_phase_;
    auto next_tick = simulation_tick_;
    std::uint32_t steps_to_run = 0U;
    while (steps_to_run < config_.max_catch_up_steps) {
        const StepInterval interval = next_step_interval(next_phase);
        if (interval.nanoseconds > next_accumulator) {
            break;
        }
        if (next_tick == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("fixed-step simulation tick overflow");
        }
        next_accumulator -= interval.nanoseconds;
        next_phase = interval.next_phase;
        ++next_tick;
        ++steps_to_run;
    }

    const StepInterval following_interval = next_step_interval(next_phase);
    const bool catch_up_limited = next_accumulator >= following_interval.nanoseconds;
    accumulator_nanoseconds_ = next_accumulator;
    step_phase_ = next_phase;
    simulation_tick_ = next_tick;

    FrameSchedule schedule;
    schedule.discarded_seconds = 0.0;
    schedule.simulation_steps = steps_to_run;
    schedule.simulation_step_seconds = simulation_step_seconds_;
    schedule.presentation_delta_nanoseconds = presentation_delta_nanoseconds;
    schedule.presentation_delta_seconds =
        static_cast<double>(presentation_delta_nanoseconds) / 1.0e9;
    schedule.backlog_nanoseconds = next_accumulator;
    schedule.catch_up_limited = catch_up_limited;
    schedule.presentation_allowed = next_accumulator < following_interval.nanoseconds;
    const long double raw_alpha = static_cast<long double>(next_accumulator) /
                                  static_cast<long double>(following_interval.nanoseconds);
    schedule.interpolation_alpha =
        std::min(static_cast<double>(raw_alpha), 0.9999999999999999);
    return schedule;
}

FrameSchedule FixedStepGuestClock::advance(const std::chrono::nanoseconds presentation_delta) {
    if (presentation_delta.count() < 0) {
        throw std::invalid_argument("presentation delta must be non-negative");
    }
    return advance_nanoseconds(static_cast<std::uint64_t>(presentation_delta.count()));
}

FrameSchedule FixedStepGuestClock::advance(const double presentation_delta_seconds) {
    if (!std::isfinite(presentation_delta_seconds) || presentation_delta_seconds < 0.0) {
        throw std::invalid_argument("presentation_delta_seconds must be finite and non-negative");
    }
    const long double exact_nanoseconds =
        static_cast<long double>(presentation_delta_seconds) * 1.0e9L +
        compatibility_nanosecond_remainder_;
    const long double rounded_nanoseconds = std::floor(exact_nanoseconds + 0.5L);
    if (rounded_nanoseconds < 0.0L || rounded_nanoseconds >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        throw std::overflow_error("presentation delta is outside the nanosecond clock range");
    }
    const auto integer_nanoseconds = static_cast<std::uint64_t>(rounded_nanoseconds);
    const long double next_remainder = exact_nanoseconds - rounded_nanoseconds;
    FrameSchedule schedule = advance_nanoseconds(integer_nanoseconds);
    compatibility_nanosecond_remainder_ = next_remainder;
    schedule.presentation_delta_seconds = presentation_delta_seconds;
    return schedule;
}

FrameSchedule FixedStepGuestClock::advance_to(const std::uint64_t monotonic_nanoseconds) {
    if (!monotonic_anchor_nanoseconds_) {
        FrameSchedule first = advance_nanoseconds(0U);
        monotonic_anchor_nanoseconds_ = monotonic_nanoseconds;
        return first;
    }
    if (*monotonic_anchor_nanoseconds_ > monotonic_nanoseconds) {
        throw std::invalid_argument("fixed-step monotonic time moved backwards");
    }
    const auto elapsed = monotonic_nanoseconds - *monotonic_anchor_nanoseconds_;
    FrameSchedule schedule = advance_nanoseconds(elapsed);
    monotonic_anchor_nanoseconds_ = monotonic_nanoseconds;
    return schedule;
}

void FixedStepGuestClock::resync_to(const std::uint64_t monotonic_nanoseconds) noexcept {
    monotonic_anchor_nanoseconds_ = monotonic_nanoseconds;
}

void FixedStepGuestClock::seed(const std::uint64_t simulation_tick) noexcept {
    static_cast<void>(seed_with_phase(simulation_tick, 0U));
}

bool FixedStepGuestClock::seed_with_phase(
    const std::uint64_t simulation_tick,
    const std::uint64_t accumulator_nanoseconds) noexcept {
    const unsigned long long numerator = simulation_rate_.numerator;
    const unsigned long long remainder = step_remainder_;
    const unsigned long long tick_remainder = simulation_tick % numerator;
    const unsigned long long phase_remainder = (remainder * tick_remainder) % numerator;
    const auto next_phase =
        static_cast<std::uint32_t>((numerator + phase_remainder - 1U) % numerator);
    if (accumulator_nanoseconds >= next_step_interval(next_phase).nanoseconds) {
        return false;
    }
    step_phase_ = next_phase;
    accumulator_nanoseconds_ = accumulator_nanoseconds;
    simulation_tick_ = simulation_tick;
    monotonic_anchor_nanoseconds_.reset();
    compatibility_nanosecond_remainder_ = 0.0L;
    return true;
}

void FixedStepGuestClock::reset() noexcept {
    step_phase_ = step_remainder_ == 0U ? 0U : simulation_rate_.numerator - 1U;
    accumulator_nanoseconds_ = 0U;
    simulation_tick_ = 0U;
    monotonic_anchor_nanoseconds_.reset();
    compatibility_nanosecond_remainder_ = 0.0L;
}

const FixedStepClockConfig& FixedStepGuestClock::config() const noexcept {
    return config_;
}

FixedStepRate FixedStepGuestClock::simulation_rate() const noexcept {
    return simulation_rate_;
}

double FixedStepGuestClock::simulation_step_seconds() const noexcept {
    return simulation_step_seconds_;
}

double FixedStepGuestClock::accumulator_seconds() const noexcept {
    return static_cast<double>(accumulator_nanoseconds_) / 1.0e9;
}

std::uint64_t FixedStepGuestClock::accumulator_nanoseconds() const noexcept {
    return accumulator_nanoseconds_;
}

std::uint64_t FixedStepGuestClock::simulation_tick() const noexcept {
    return simulation_tick_;
}

double FixedStepGuestClock::simulation_time_seconds() const noexcept {
    const auto ticks = static_cast<long double>(simulation_tick_);
    const long double seconds = static_cast<long double>(simulation_rate_.denominator) * ticks /
                                static_cast<long double>(simulation_rate_.numerator);
    return static_cast<double>(seconds);
}

} // namespace takedown::runtime
