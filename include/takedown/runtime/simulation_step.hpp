#pragma once

#include "takedown/runtime/fixed_step_clock.hpp"

namespace takedown::runtime {

// One completed native update. Tick ordinals start at one. This context never
// changes the EE, VBlank, audio, or presentation clock.
struct SimulationStep {
    std::uint64_t simulation_tick{};
    double delta_seconds{};
    double reference_scale{};
    std::uint64_t reference_tick{};
    bool reference_tick_due{};
    // Exact position of an emitted reference boundary within this update.
    // Consumers must split integration here when discrete events affect it;
    // running every event at the end would quantize 60 Hz events at 144 Hz.
    std::uint64_t boundary_numerator{};
    std::uint64_t boundary_denominator{1};
};

// One instance per recovered subsystem cadence. Configuration is immutable:
// changing rates requires an explicit transition at a proven state boundary.
// Call advance once per executed simulation step, never once per present.
class SimulationStepSequence {
  public:
    SimulationStepSequence(FixedStepRate simulation, FixedStepRate reference);
    [[nodiscard]] SimulationStep advance();
    // Starts at an absolute pair of elapsed ordinals. The reference ordinal
    // must equal the exact floor of simulation_tick * reference/simulation;
    // rejecting an inconsistent pair prevents a seeded session from silently
    // falling back to relative reference ticks.
    void seed(std::uint64_t simulation_tick, std::uint64_t reference_tick);
    [[nodiscard]] FixedStepRate simulation_rate() const noexcept { return simulation_; }
    [[nodiscard]] FixedStepRate reference_rate() const noexcept { return reference_; }
    [[nodiscard]] std::uint64_t simulation_tick() const noexcept { return simulation_tick_; }
    [[nodiscard]] std::uint64_t reference_tick() const noexcept { return reference_tick_; }

  private:
    FixedStepRate simulation_;
    FixedStepRate reference_;
    std::uint64_t scale_numerator_{};
    std::uint64_t scale_denominator_{};
    std::uint64_t phase_{};
    std::uint64_t simulation_tick_{};
    std::uint64_t reference_tick_{};
};

} // namespace takedown::runtime
