#pragma once

#include "takedown/runtime/fixed_step_clock.hpp"
#include "takedown/runtime/retail_reference_timer.hpp"
#include "takedown/runtime/simulation_step.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime {

// Configuration for the diagnostic native timing adapter. The adapter is
// deliberately disabled by default and is not a boot-entry or dispatcher
// dependency. Enabling it is an explicit choice for an offline parity probe
// or a unit test.
struct NativeSimulationDriverConfig {
    bool enabled{};
    FixedStepClockConfig clock{};
    FixedStepRate reference_rate{60U, 1U};
    float reference_delta_seconds{1.0F / 60.0F};
    // Guest-time input is expressed as cycles per second. The default is the
    // measured EE clock used by the existing audio/guest timing contracts.
    FixedStepRate guest_time_rate{294'912'000U, 1U};
};

struct NativeSimulationDriverFrame {
    FrameSchedule schedule{};
    std::uint32_t reference_ticks_delivered{};
    // Absolute guest-cycle endpoint for a guest-time batch. Presentation-only
    // advances retain the current endpoint and do not fabricate guest time.
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
    // True only for batches produced by advance_guest_cycles().
    bool guest_time_authoritative{};
    bool enabled{};
};

// One authenticated point on the fixed simulation timeline. The point may be
// inside the interval following simulation_tick; pairing it with the guest
// cycle endpoint makes the phase origin explicit instead of implicitly
// assuming that guest cycle zero and simulation tick zero coincide.
struct NativeSimulationPhasePoint {
    std::uint64_t simulation_tick{};
    std::uint64_t simulation_phase_nanoseconds{};
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
};

struct NativeSimulationPhaseOrigin {
    NativeSimulationPhasePoint point{};
};

// Checks the exact rational relationship between an authenticated phase
// origin and a later seed/epoch point. This is deliberately fail-closed: a
// caller that cannot provide a relative origin must start at the all-zero
// point, rather than recovering phase from an unrelated absolute cycle
// remainder. The function is noexcept so state publishers can use the same
// contract in their pre-commit validation path.
[[nodiscard]] bool native_simulation_phase_origin_matches(
    FixedStepRate simulation_rate, FixedStepRate guest_time_rate,
    const NativeSimulationPhaseOrigin& origin,
    const NativeSimulationPhasePoint& point) noexcept;

// A complete diagnostic session origin. Simulation and reference ordinals
// are absolute elapsed ordinals from the fixed-rate epoch; the timer state is
// copied byte-for-byte so a resumed trace cannot accidentally restart its
// reference clock at zero. At an owner timestamp inside the next simulation
// step, reference_tick and the timer state may describe the last committed
// boundary; the read-only projection then exposes the pending boundary at that
// timestamp. An optional host timestamp establishes the first absolute
// advance_to() anchor for the same session.
struct NativeSimulationDriverSeed {
    std::uint64_t simulation_tick{};
    std::uint64_t reference_tick{};
    RetailReferenceTimerState reference_timer_state{};
    // Elapsed integer nanoseconds inside the interval immediately following
    // simulation_tick. This preserves a sampled guest callback's phase relative
    // to its authoritative scheduler boundary.
    std::uint64_t simulation_phase_nanoseconds{};
    // The active guest-time origin and the exact numerator remainder carried
    // while converting cycles to integer nanoseconds. The remainder is in the
    // half-open range [0, guest_time_rate.numerator).
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
    std::optional<std::uint64_t> monotonic_nanoseconds{};
    // Required for any nonzero point. The origin is a copied, authenticated
    // relative point; it is not inferred from absolute guest cycles.
    std::optional<NativeSimulationPhaseOrigin> simulation_phase_origin{};
};

// One ordered, allocation-free sink record. The frame fields are repeated for
// each step because a bounded host-delivery batch may contain several guest
// updates; retaining them beside every update keeps the trace self-contained.
struct NativeSimulationTraceStep {
    SimulationStep step{};
    FrameSchedule frame{};
    RetailReferenceTimerState timer_state{};
    double guest_time_seconds{};
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
    std::uint32_t batch_step_index{};
};

// Driver-owned callbacks are deliberately a small caller-owned interface.
// The driver never allocates for a sink and never calls it while disabled.
// begin_batch() reserves a whole batch so a fixed-capacity trace can reject an
// incomplete append before any driver state is committed. abort_batch() must
// discard the current batch if a checked driver operation throws.
class NativeSimulationStepSink {
  public:
    virtual ~NativeSimulationStepSink() = default;

    [[nodiscard]] virtual bool begin_batch(const NativeSimulationDriverFrame& frame,
                                           std::uint32_t step_count) noexcept = 0;
    // A false record or commit result rejects the whole batch. The driver
    // restores its sequence and timer, calls abort_batch(), and leaves its
    // clock/guest-time origin unchanged.
    [[nodiscard]] virtual bool record_step(
        const NativeSimulationTraceStep& step) noexcept = 0;
    [[nodiscard]] virtual bool end_batch() noexcept = 0;
    virtual void abort_batch() noexcept = 0;
};

// A diagnostic-only owner for the first native timing migration seam. It
// couples host delivery to fixed simulation steps and feeds the typed retail
// timer only at emitted reference boundaries. It deliberately has no native
// function registration path and is not used by boot-entry.
class NativeSimulationDriver {
  public:
    explicit NativeSimulationDriver(NativeSimulationDriverConfig config = {});

    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

    // Begins a fresh diagnostic session at explicit absolute guest ordinals.
    // The operation is transactional: invalid origins, an incoherent timer
    // observation, or an unrepresentable reference tick leave this driver
    // unchanged. Disabled drivers remain inert, including for invalid seeds.
    void seed_session(const NativeSimulationDriverSeed& seed);
    void seed(const NativeSimulationDriverSeed& seed) { seed_session(seed); }

    // The sink is non-owning and is consulted only by enabled advances. It is
    // intentionally absent from boot-entry and the native dispatcher path.
    void set_step_sink(NativeSimulationStepSink* sink) noexcept { step_sink_ = sink; }
    [[nodiscard]] NativeSimulationStepSink* step_sink() const noexcept { return step_sink_; }

    // Guest-time seam for offline traces. Each input batch is converted with
    // a carried rational remainder, so irregular cycle delivery cannot lose
    // sub-nanosecond active time or derive simulation work from presentation.
    // A session that has accepted presentation time must be reseeded before
    // guest-cycle authority can be selected.
    [[nodiscard]] NativeSimulationDriverFrame advance_guest_cycles(
        std::uint64_t guest_cycle_delta);

    // Each call clears last_steps(). When enabled, every returned step is one
    // authoritative fixed simulation update; the retail timer is invoked only
    // for entries whose reference_tick_due flag is set. Presentation and
    // guest-cycle authority cannot be mixed in one session.
    [[nodiscard]] NativeSimulationDriverFrame advance(double presentation_delta_seconds);
    [[nodiscard]] NativeSimulationDriverFrame advance_nanoseconds(
        std::uint64_t presentation_delta_nanoseconds);
    [[nodiscard]] NativeSimulationDriverFrame advance(
        std::chrono::nanoseconds presentation_delta);
    [[nodiscard]] NativeSimulationDriverFrame advance_to(std::uint64_t monotonic_nanoseconds);

    // Excludes host pause time while retaining fractional active time and any
    // bounded simulation backlog already owned by the fixed-step clock.
    void resync_to(std::uint64_t monotonic_nanoseconds) noexcept;

    // Resets all three owned timing objects to their initial state. This is a
    // diagnostic session boundary, not a live guest reset hook.
    void reset() noexcept;

    [[nodiscard]] const FixedStepGuestClock& clock() const noexcept { return clock_; }
    [[nodiscard]] const SimulationStepSequence& step_sequence() const noexcept {
        return sequence_;
    }
    [[nodiscard]] RetailReferenceTimer& reference_timer() noexcept { return timer_; }
    [[nodiscard]] const RetailReferenceTimer& reference_timer() const noexcept { return timer_; }
    // Read-only timer state at the current authoritative guest-time point.
    // At rates such as 144 Hz, a 60 Hz reference boundary can lie inside the
    // next incomplete simulation step. The returned projection applies that
    // one boundary without mutating the driver; the eventual step retains its
    // exact boundary fraction for split integration and does not double-apply
    // the timer update. A point with more than one pending reference boundary
    // is unrepresentable by this bounded projection and returns nullopt.
    [[nodiscard]] std::optional<RetailReferenceTimerState>
    projected_reference_timer_state() const noexcept;
    [[nodiscard]] std::uint64_t guest_time_cycles() const noexcept {
        return guest_time_cycles_;
    }
    [[nodiscard]] std::uint64_t guest_time_cycle_remainder() const noexcept {
        return guest_time_cycle_remainder_;
    }
    [[nodiscard]] FixedStepRate guest_time_rate() const noexcept {
        return config_.guest_time_rate;
    }

    // The most recent host-delivery batch. The span is invalidated by the
    // next advance/reset call and is intentionally diagnostic-only telemetry.
    [[nodiscard]] std::span<const SimulationStep> last_steps() const noexcept {
        return last_steps_;
    }

  private:
    enum class TimeAuthority : std::uint8_t {
        Unset,
        Presentation,
        GuestCycles,
    };

    [[nodiscard]] NativeSimulationDriverFrame execute(FrameSchedule schedule);
    [[nodiscard]] NativeSimulationDriverFrame execute(FrameSchedule schedule,
                                                       std::uint64_t guest_time_cycles,
                                                       std::uint64_t guest_time_cycle_remainder,
                                                       bool guest_time_authoritative);
    [[nodiscard]] NativeSimulationDriverFrame disabled_frame(
        std::uint64_t presentation_delta_nanoseconds) noexcept;

    NativeSimulationDriverConfig config_{};
    FixedStepGuestClock clock_;
    SimulationStepSequence sequence_;
    RetailReferenceTimer timer_;
    std::vector<SimulationStep> last_steps_;
    NativeSimulationStepSink* step_sink_{};
    std::uint64_t guest_time_cycles_{};
    std::uint64_t guest_time_cycle_remainder_{};
    TimeAuthority time_authority_{TimeAuthority::Unset};
};

} // namespace takedown::runtime
