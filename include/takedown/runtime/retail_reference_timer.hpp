#pragma once

#include <bit>
#include <cstdint>

namespace takedown::runtime {

struct SimulationStep;

// Typed model of the retail timer object updated by SLUS-21050 0x001d3a60.
// The field order mirrors guest offsets +0x00 through +0x28 so differential
// captures can compare the model without inventing semantic projections.
struct RetailReferenceTimerState {
    std::uint32_t observed_tick{};        // +0x00
    std::int32_t consumed_tick_offset{};  // +0x04
    std::int32_t paused_ticks{};          // +0x08
    std::int32_t whole_ticks{};           // +0x0c
    std::int32_t subdivision_remainder{}; // +0x10
    std::uint32_t baseline_tick{};        // +0x14
    std::int32_t subdivisions{1};         // +0x18
    float step_seconds{};                 // +0x1c
    float elapsed_seconds{};              // +0x20
    std::int32_t pending_subdivisions{1}; // +0x24
    bool accumulating{};                  // +0x28

    [[nodiscard]] bool operator==(const RetailReferenceTimerState& other) const noexcept {
        return observed_tick == other.observed_tick &&
               consumed_tick_offset == other.consumed_tick_offset &&
               paused_ticks == other.paused_ticks && whole_ticks == other.whole_ticks &&
               subdivision_remainder == other.subdivision_remainder &&
               baseline_tick == other.baseline_tick && subdivisions == other.subdivisions &&
               std::bit_cast<std::uint32_t>(step_seconds) ==
                   std::bit_cast<std::uint32_t>(other.step_seconds) &&
               std::bit_cast<std::uint32_t>(elapsed_seconds) ==
                   std::bit_cast<std::uint32_t>(other.elapsed_seconds) &&
               pending_subdivisions == other.pending_subdivisions &&
               accumulating == other.accumulating;
    }
};

// This is a parity model and native migration seam; constructing it does not
// replace the live guest object. Feed only the recovered shared reference-tick
// ordinal. Higher-rate continuous simulation still receives its smaller dt
// from SimulationStepSequence and advances this object only at reference ticks.
class RetailReferenceTimer {
  public:
    RetailReferenceTimer(std::uint32_t shared_tick, float reference_delta_seconds);
    RetailReferenceTimer(float reference_delta_seconds, RetailReferenceTimerState state);

    void reset(std::uint32_t shared_tick, float reference_delta_seconds);
    void update(std::uint32_t shared_tick);
    void update(std::uint32_t shared_tick, float reference_delta_seconds);
    // Connects the authored retail timer to a higher-rate native sequence.
    // Continuous simulation steps between reference boundaries must not
    // advance or rewrite this discrete 30/60 Hz state.
    void update(const SimulationStep& step);
    void set_accumulating(std::uint32_t shared_tick, bool accumulating);
    void set_pending_subdivisions(std::int32_t subdivisions);

    [[nodiscard]] const RetailReferenceTimerState& state() const noexcept { return state_; }
    [[nodiscard]] float reference_delta_seconds() const noexcept {
        return reference_delta_seconds_;
    }

  private:
    float reference_delta_seconds_{};
    RetailReferenceTimerState state_{};
};

} // namespace takedown::runtime
