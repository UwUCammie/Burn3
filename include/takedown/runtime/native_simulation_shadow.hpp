#pragma once

#include "takedown/runtime/native_simulation_state.hpp"
#include "takedown/runtime/native_simulation_trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::runtime {

// The live adapter is a read-only diagnostic seam. Its bounded capacities are
// deliberately compile-time values: an opt-in run can fail closed when the
// evidence no longer fits instead of allocating or silently dropping records.
inline constexpr std::size_t kNativeSimulationShadowTraceCapacity = 1024U;
inline constexpr std::size_t kNativeSimulationShadowOwnerCapacity = 64U;
inline constexpr std::size_t kNativeSimulationShadowStateCapacity =
    kNativeSimulationShadowTraceCapacity;

enum class NativeSimulationShadowFailure : std::uint8_t {
    None,
    CycleRegression,
    CycleOverflow,
    DriverRejected,
    TraceRejected,
    InvalidOwnerState,
    NestedOwner,
    MissingOwnerReturn,
    OwnerCapacity,
    IncompleteOwner,
    TimerMismatch,
    StatePublisherRejected,
    NoOwner,
    Finalized,
};

[[nodiscard]] std::string_view to_string(
    NativeSimulationShadowFailure failure) noexcept;

struct NativeSimulationShadowSample {
    std::uint64_t ee_cycles{};
    std::uint64_t owner_cycle_delta{};
    std::uint32_t shared_tick{};
    RetailReferenceTimerState expected{};
    RetailReferenceTimerState observed{};
    RetailReferenceTimerState shadow{};
    bool matched{};
};

struct NativeSimulationShadowSummary {
    bool enabled{};
    bool seeded{};
    bool finalized{};
    bool passed{};
    NativeSimulationShadowFailure failure{NativeSimulationShadowFailure::None};
    std::uint64_t cycle_samples{};
    std::uint64_t guest_cycle_batches{};
    std::uint64_t owner_entries{};
    std::uint64_t owner_returns{};
    std::uint64_t matches{};
    std::uint64_t mismatches{};
    std::uint64_t dropped_owner_samples{};
    std::uint64_t state_epoch{};
    std::uint64_t state_publications{};
    NativeSimulationStateFailure state_failure{NativeSimulationStateFailure::None};
    std::size_t trace_steps{};
    std::size_t trace_capacity{};
    std::size_t state_capacity{};
};

// A passive emulation-thread shadow for the recovered 30/60 Hz timer owner.
// It never receives a guest CPU or memory reference: callers pass immutable
// scalar snapshots, and every driver advance is sourced from an absolute
// emulated EE-cycle sample. The presentation clock has no entry point here.
class NativeSimulationShadow final {
  public:
    explicit NativeSimulationShadow(
        bool enabled = false, NativeSimulationDriverConfig driver_config = {},
        std::size_t trace_capacity = kNativeSimulationShadowTraceCapacity);

    NativeSimulationShadow(const NativeSimulationShadow&) = delete;
    NativeSimulationShadow& operator=(const NativeSimulationShadow&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool seeded() const noexcept { return seeded_; }
    [[nodiscard]] bool owner_active() const noexcept { return owner_active_; }
    [[nodiscard]] std::uint64_t state_epoch() const noexcept { return state_epoch_; }
    [[nodiscard]] NativeSimulationStateFailure state_failure() const noexcept {
        return state_publisher_.failure();
    }
    [[nodiscard]] std::uint64_t state_publication_count() const noexcept {
        return state_publisher_.publication_count();
    }

    // Samples the authoritative absolute EE-cycle counter. Calls before the
    // first owner entry establish the origin; later calls feed only the
    // positive delta into NativeSimulationDriver::advance_guest_cycles().
    // Failures are recorded and never escape into the guest loop.
    [[nodiscard]] bool observe_guest_cycles(std::uint64_t emulated_ee_cycles) noexcept;

    // Read-only copies of the state-publication boundary. The publisher's
    // lifecycle remains owned by this shadow.
    [[nodiscard]] bool read_latest_state(
        NativeSimulationStateSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool read_interpolation_state(
        NativeSimulationInterpolationState& state) const noexcept;

    // Accepts only the opaque endpoint minted by the bound vehicle probe.
    // Valid endpoints between fixed-step boundaries are deferred without
    // failing the shadow; malformed, stale, replayed, or wrong-epoch tokens
    // remain fatal to the opt-in diagnostic session.
    [[nodiscard]] NativeSimulationVehiclePublicationResult
    publish_vehicle_snapshot_result(
        const Slus21050VehicleWorldContactPublicationToken& token) noexcept;

    // Convenience form for callers that only need a published/not-published
    // answer. Use the result form when deferred and rejected must differ.
    [[nodiscard]] bool publish_vehicle_snapshot(
        const Slus21050VehicleWorldContactPublicationToken& token) noexcept;

    // Begins an owner observation at 0x001d3a60. The expected post-owner
    // state is computed with the existing RetailReferenceTimer model. The
    // first call establishes an absolute session origin; when the owner
    // boundary lies inside the next simulation step, the seed retains the
    // prior committed state and the driver projection supplies the pending
    // post-update state. Later calls are checked against the cycle-driven
    // shadow at the return PC.
    [[nodiscard]] bool observe_owner_entry(
        std::uint32_t shared_tick, float reference_delta_seconds,
        const RetailReferenceTimerState& before,
        std::uint64_t emulated_ee_cycles) noexcept;

    // Completes the only comparison boundary: the proven JR return dispatch
    // at 0x001d3c04 for the owner entered at 0x001d3a60.
    [[nodiscard]] bool observe_owner_return(
        const RetailReferenceTimerState& observed,
        std::uint64_t emulated_ee_cycles) noexcept;

    // Records an unavailable guest snapshot as a fail-closed diagnostic
    // event. It does not alter driver state or guest state.
    void reject_guest_snapshot() noexcept;

    [[nodiscard]] NativeSimulationShadowSummary finalize() noexcept;
    [[nodiscard]] const NativeSimulationShadowSummary& summary() const noexcept {
        return summary_;
    }
    [[nodiscard]] const NativeSimulationDriver& driver() const noexcept { return driver_; }
    [[nodiscard]] std::span<const NativeSimulationShadowSample> owner_samples() const noexcept {
        return std::span<const NativeSimulationShadowSample>{owner_samples_}.first(
            owner_sample_count_);
    }
    [[nodiscard]] std::span<const NativeSimulationTraceStep> trace_entries() const noexcept {
        return trace_.entries();
    }

  private:
    class StepSink final : public NativeSimulationStepSink {
      public:
        StepSink(NativeSimulationTrace& trace,
                 NativeSimulationStatePublisher& publisher) noexcept
            : trace_(&trace), publisher_(&publisher) {}

        [[nodiscard]] bool begin_batch(const NativeSimulationDriverFrame& frame,
                                       std::uint32_t step_count) noexcept override;
        [[nodiscard]] bool record_step(
            const NativeSimulationTraceStep& step) noexcept override;
        [[nodiscard]] bool end_batch() noexcept override;
        void abort_batch() noexcept override;

      private:
        NativeSimulationTrace* trace_{};
        NativeSimulationStatePublisher* publisher_{};
        bool trace_batch_open_{};
        bool publisher_batch_open_{};
        std::uint32_t expected_steps_{};
        std::uint32_t recorded_steps_{};
    };

    [[nodiscard]] bool seed_from_owner(
        std::uint32_t shared_tick, float reference_delta_seconds,
        const RetailReferenceTimerState& before,
        const RetailReferenceTimerState& expected,
        std::uint64_t emulated_ee_cycles) noexcept;
    [[nodiscard]] bool ensure_cycle_sample(std::uint64_t emulated_ee_cycles) noexcept;
    void fail(NativeSimulationShadowFailure failure) noexcept;

    bool enabled_{};
    bool seeded_{};
    bool owner_active_{};
    bool have_cycle_sample_{};
    bool finalized_{};
    std::uint64_t last_ee_cycles_{};
    std::uint64_t owner_entry_ee_cycles_{};
    std::uint32_t owner_entry_shared_tick_{};
    RetailReferenceTimerState owner_expected_state_{};
    NativeSimulationDriverConfig driver_config_{};
    std::uint64_t state_epoch_{};
    std::array<NativeSimulationTraceStep, kNativeSimulationShadowTraceCapacity>
        trace_storage_{};
    NativeSimulationTrace trace_;
    std::array<NativeSimulationStateSnapshot, kNativeSimulationShadowStateCapacity>
        state_storage_{};
    NativeSimulationStatePublisher state_publisher_;
    StepSink state_step_sink_;
    NativeSimulationDriver driver_;
    std::array<NativeSimulationShadowSample, kNativeSimulationShadowOwnerCapacity>
        owner_samples_{};
    std::size_t owner_sample_count_{};
    NativeSimulationShadowSummary summary_{};
};

} // namespace takedown::runtime
