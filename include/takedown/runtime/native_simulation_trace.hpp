#pragma once

#include "takedown/runtime/native_simulation_driver.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::runtime {

enum class NativeSimulationTraceFailure : std::uint8_t {
    None,
    Disabled,
    Capacity,
    InvalidSeed,
    BatchState,
    NonAuthoritativeGuestTime,
    NonMonotonicSimulationTick,
    NonMonotonicReferenceTick,
    NonMonotonicGuestTime,
    InvalidReferenceBoundary,
    InvalidDelta,
    GuestTimeNotFinite,
    InvalidPhaseOrigin,
};

[[nodiscard]] std::string_view to_string(NativeSimulationTraceFailure failure) noexcept;

// Fixed-capacity, caller-owned sink for ordered native simulation evidence.
// The trace stores no heap-owned state: a caller can place the records in an
// array sized for one bounded guest-time slice and fail closed at capacity.
class NativeSimulationTrace final : public NativeSimulationStepSink {
  public:
    NativeSimulationTrace() noexcept = default;
    explicit NativeSimulationTrace(
        std::span<NativeSimulationTraceStep> storage,
        FixedStepRate guest_time_rate = {294'912'000U, 1U}) noexcept
        : storage_(storage), guest_time_rate_(guest_time_rate) {}

    NativeSimulationTrace(const NativeSimulationTrace&) = delete;
    NativeSimulationTrace& operator=(const NativeSimulationTrace&) = delete;

    // Starts a fresh trace session and clears previously recorded batches.
    // The seed is retained as the continuity origin, so the first record must
    // follow its absolute ordinals instead of restarting at one.
    void begin_session(const NativeSimulationDriverSeed& seed,
                       FixedStepRate simulation_rate) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return !storage_.empty(); }
    [[nodiscard]] bool valid() const noexcept {
        return enabled() && failure_ == NativeSimulationTraceFailure::None && !batch_open_;
    }
    [[nodiscard]] bool overflowed() const noexcept {
        return failure_ == NativeSimulationTraceFailure::Capacity;
    }
    [[nodiscard]] NativeSimulationTraceFailure failure() const noexcept { return failure_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::span<const NativeSimulationTraceStep> entries() const noexcept {
        return storage_.first(size_);
    }
    [[nodiscard]] bool session_seeded() const noexcept { return session_seeded_; }
    [[nodiscard]] const RetailReferenceTimerState& session_timer_state() const noexcept {
        return session_timer_state_;
    }
    [[nodiscard]] std::uint64_t batch_count() const noexcept { return batch_count_; }

    // NativeSimulationStepSink implementation. Driver batches are reserved
    // before execution and either committed in full or removed on abort.
    [[nodiscard]] bool begin_batch(const NativeSimulationDriverFrame& frame,
                                   std::uint32_t step_count) noexcept override;
    [[nodiscard]] bool record_step(
        const NativeSimulationTraceStep& step) noexcept override;
    [[nodiscard]] bool end_batch() noexcept override;
    void abort_batch() noexcept override;

  private:
    void fail(NativeSimulationTraceFailure failure) noexcept;
    [[nodiscard]] bool check_step(const NativeSimulationTraceStep& step) noexcept;

    std::span<NativeSimulationTraceStep> storage_{};
    std::size_t size_{};
    std::size_t batch_begin_{};
    std::uint32_t batch_expected_{};
    std::uint32_t batch_recorded_{};
    std::uint64_t batch_count_{};
    std::uint64_t batch_previous_simulation_tick_{};
    std::uint64_t batch_previous_reference_tick_{};
    std::uint64_t batch_previous_guest_time_cycles_{};
    std::uint64_t batch_previous_guest_time_cycle_remainder_{};
    std::uint64_t batch_guest_time_cycles_{};
    std::uint64_t batch_guest_time_cycle_remainder_{};
    bool batch_previous_have_last_step_{};
    std::uint64_t last_simulation_tick_{};
    std::uint64_t last_reference_tick_{};
    std::uint64_t last_guest_time_cycles_{};
    std::uint64_t last_guest_time_cycle_remainder_{};
    FixedStepRate guest_time_rate_{294'912'000U, 1U};
    RetailReferenceTimerState session_timer_state_{};
    bool have_last_step_{};
    bool session_seeded_{};
    bool batch_open_{};
    NativeSimulationTraceFailure failure_{NativeSimulationTraceFailure::None};
};

} // namespace takedown::runtime
