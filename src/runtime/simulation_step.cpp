#include "takedown/runtime/simulation_step.hpp"

#include <cstdint>
#include <numeric>
#include <stdexcept>

namespace takedown::runtime {
namespace {

FixedStepRate normalize(FixedStepRate rate) {
    if (rate.numerator == 0U || rate.denominator == 0U) {
        throw std::invalid_argument("simulation and reference rates must be positive");
    }
    const std::uint32_t divisor = std::gcd(rate.numerator, rate.denominator);
    return {rate.numerator / divisor, rate.denominator / divisor};
}

} // namespace

SimulationStepSequence::SimulationStepSequence(FixedStepRate simulation, FixedStepRate reference)
    : simulation_(normalize(simulation)), reference_(normalize(reference)) {
    scale_numerator_ = static_cast<std::uint64_t>(reference_.numerator) * simulation_.denominator;
    scale_denominator_ = static_cast<std::uint64_t>(reference_.denominator) * simulation_.numerator;
    if (scale_numerator_ > scale_denominator_) {
        throw std::invalid_argument("simulation rate must be at least the reference rate");
    }
    const std::uint64_t divisor = std::gcd(scale_numerator_, scale_denominator_);
    scale_numerator_ /= divisor;
    scale_denominator_ /= divisor;
}

SimulationStep SimulationStepSequence::advance() {
    const auto threshold = scale_denominator_ - scale_numerator_;
    const bool due = phase_ >= threshold;
    if (simulation_tick_ == UINT64_MAX || (due && reference_tick_ == UINT64_MAX)) {
        throw std::overflow_error("simulation step ordinal overflow");
    }
    SimulationStep step;
    step.simulation_tick = simulation_tick_ + 1U;
    step.delta_seconds = static_cast<double>(simulation_.denominator) / static_cast<double>(simulation_.numerator);
    step.reference_scale = static_cast<double>(scale_numerator_) / static_cast<double>(scale_denominator_);
    step.reference_tick = reference_tick_ + (due ? 1U : 0U);
    step.reference_tick_due = due;
    step.boundary_numerator = due ? scale_denominator_ - phase_ : 0U;
    step.boundary_denominator = due ? scale_numerator_ : 1U;
    phase_ = due ? phase_ + (scale_numerator_ - scale_denominator_) : phase_ + scale_numerator_;
    simulation_tick_ = step.simulation_tick;
    reference_tick_ = step.reference_tick;
    return step;
}

void SimulationStepSequence::seed(const std::uint64_t simulation_tick, const std::uint64_t reference_tick) {
    const unsigned __int128 scaled = static_cast<unsigned __int128>(simulation_tick) * scale_numerator_;
    const unsigned __int128 expected_reference = scaled / scale_denominator_;
    if (expected_reference > UINT64_MAX) {
        throw std::overflow_error("seeded reference tick ordinal overflow");
    }
    if (static_cast<std::uint64_t>(expected_reference) != reference_tick) {
        throw std::invalid_argument("seeded simulation/reference ordinals are inconsistent");
    }
    simulation_tick_ = simulation_tick;
    reference_tick_ = reference_tick;
    phase_ = static_cast<std::uint64_t>(scaled % scale_denominator_);
}

} // namespace takedown::runtime
