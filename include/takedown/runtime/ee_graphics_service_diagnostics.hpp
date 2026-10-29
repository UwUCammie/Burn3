#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace takedown::runtime {

enum class EeGraphicsCallbackRoute : std::uint8_t {
    Unknown,
    JoinPump,
    CompiledBlock,
};

enum class EeGraphicsVif1Event : std::uint8_t {
    NoPending,
    WaitBlocked,
    WaitBlockedDirect,
    WaitBlockedVu1Idle,
    WaitBlockedFlush,
    WaitBlockedFlushAll,
    WaitSatisfied,
    Path1Boundary,
    ZeroTimeTransition,
    TagBegin,
    TteTransfer,
    TteBypass,
    PayloadQuadword,
    PayloadRetire,
    ReadyRelease,
    Completion,
    Fault,
    Count,
};

enum class EeGraphicsGifEvent : std::uint8_t {
    NoPending,
    BlockedExternalWakeBypass,
    ArbiterCall,
    ArbiterReentrant,
    Path1Selected,
    Path2Selected,
    Path3Selected,
    PauseBreak,
    NoEligibleSourceBreak,
    Path1EmptyBreak,
    Path2EmptyBreak,
    Path3EmptyBreak,
    CycleCreditBreak,
    Path3Eop,
    Path3TagBoundary,
    Path3ImtSlice,
    Completion,
    Count,
};

// The route-scoped operation census deliberately has its own fixed schema.
// JoinPumpRegionExit has six values; keeping the count here avoids making this
// runtime diagnostic header depend on the recompiler implementation header.
inline constexpr std::size_t kEeGraphicsJoinPumpRegionExitCount = 6U;

struct EeGraphicsJoinPumpOperationSnapshot {
    // The first four buckets are an exhaustive state classification for each
    // route-scoped VIF1 service entry. A pending wait of any kind other than
    // VU1-idle is intentionally included in wait_other.
    std::uint64_t vif1_wait_state_checks{};
    std::uint64_t vif1_wait_vu1_idle{};
    std::uint64_t vif1_wait_other{};
    std::uint64_t vif1_pending_no_wait{};
    std::uint64_t vif1_no_pending{};

    // A GIF pump entry has one of the two explicitly observed outcomes below.
    // Progress and accepted QW counts are kept separately because one progress
    // call can accept multiple quadwords and producer progress can also enter
    // the arbiter synchronously.
    std::uint64_t gif_arbiter_outcome_checks{};
    std::uint64_t gif_cache_bypasses{};
    std::uint64_t gif_arbiter_calls{};
    std::uint64_t gif_no_eligible_breaks{};
    std::uint64_t gif_progress_calls{};
    std::uint64_t gif_accepted_qwords{};

    std::uint64_t callback_result_checks{};
    std::uint64_t callback_result_clean_returns{};
    std::uint64_t callback_result_yield_any{};
    std::uint64_t callback_result_queued_dmac{};
    std::uint64_t callback_result_intc{};
    std::uint64_t callback_result_sif_boundaries{};

    // Pass/rejection classify the main scheduler guard bundle. A passed
    // bundle can still decline to execute when its clipped capacity is one
    // instruction; join_pump_region_calls remains the execution count.
    std::uint64_t admission_attempts{};
    std::uint64_t admission_passes{};
    std::uint64_t admission_rejections{};
    std::uint64_t scheduler_queries{};
    std::uint64_t deadline_queries{};

    // Saturating diagnostic counters invalidate the sidecar rather than
    // allowing a wrapped value to satisfy an arithmetic identity.
    bool arithmetic_valid{true};
};

struct EeGraphicsServiceDiagnosticsSnapshot {
    std::uint64_t callback_calls{};
    std::uint64_t callback_unknown_calls{};
    std::uint64_t callback_join_pump_calls{};
    std::uint64_t callback_compiled_block_calls{};
    std::uint64_t callback_vif1_requests{};
    std::uint64_t callback_vif1_cycles{};
    std::uint64_t callback_gif_requests{};
    std::uint64_t callback_gif_cycles{};
    std::uint64_t callback_both_producer_advance{};
    std::uint64_t callback_no_producer_advance{};
    std::uint64_t callback_yield_queued_dmac{};
    std::uint64_t callback_yield_intc{};
    std::uint64_t callback_sif_boundaries{};
    std::uint64_t callback_clean_returns{};

    std::uint64_t vif1_advance_calls{};
    std::uint64_t vif1_advance_cycles{};
    std::uint64_t vif1_callback_advance_calls{};
    std::uint64_t vif1_outer_advance_calls{};
    std::uint64_t vif1_disabled_calls{};
    std::uint64_t vif1_zero_cycle_calls{};
    std::array<std::uint64_t, static_cast<std::size_t>(EeGraphicsVif1Event::Count)>
        vif1_events{};

    std::uint64_t gif_advance_calls{};
    std::uint64_t gif_advance_cycles{};
    std::uint64_t gif_callback_advance_calls{};
    std::uint64_t gif_outer_advance_calls{};
    std::uint64_t gif_disabled_calls{};
    std::uint64_t gif_zero_cycle_calls{};
    std::array<std::uint64_t, static_cast<std::size_t>(EeGraphicsGifEvent::Count)> gif_events{};
};

// Exact-window event counters for active graphics servicing. This object has
// no host clock and performs no I/O while the guest runs. The normal runner
// leaves the pointer in EeMemory null; the profile option installs it only for
// the A-to-C measurement window.
class EeGraphicsServiceDiagnostics {
  public:
    void start_window() noexcept;
    void stop_window() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    void begin_callback(EeGraphicsCallbackRoute route = EeGraphicsCallbackRoute::Unknown) noexcept;
    void end_callback() noexcept;

    void record_callback_advance(bool vif1, std::uint64_t vif1_cycles,
                                 bool gif, std::uint64_t gif_cycles) noexcept;
    void record_callback_result(bool clean_return, bool queued_dmac,
                                bool intc, bool sif_boundary) noexcept;

    void record_vif1_advance(std::uint64_t ee_cycles, bool timing_enabled) noexcept;
    void record_gif_advance(std::uint64_t ee_cycles, bool timing_enabled) noexcept;
    // The caller supplies the state observed at the start of one VIF1 service
    // entry. This is intentionally separate from event rows: event rows count
    // transitions, while this fixed split counts the route's entry state.
    void record_vif1_route_state(bool no_pending, bool wait_active,
                                 bool wait_vu1_idle) noexcept;
    void record_gif_progress() noexcept;
    void record_gif_accepted_qword() noexcept;
    void record_vif1_event(EeGraphicsVif1Event event) noexcept;
    void record_gif_event(EeGraphicsGifEvent event) noexcept;

    // Main admission/query counters are recorded only while this exact-window
    // object is active. They remain independent of the callback event rows.
    void record_join_pump_admission(bool admitted) noexcept;
    void record_join_pump_scheduler_query() noexcept;
    void record_join_pump_deadline_query() noexcept;

    [[nodiscard]] EeGraphicsJoinPumpOperationSnapshot
    join_pump_operation_snapshot() const noexcept {
        return join_pump_operations_;
    }

    [[nodiscard]] EeGraphicsServiceDiagnosticsSnapshot snapshot() const noexcept {
        return counters_;
    }

  private:
    void increment_join_pump(std::uint64_t& counter) noexcept;
    [[nodiscard]] bool join_pump_route_active() const noexcept;

    EeGraphicsServiceDiagnosticsSnapshot counters_{};
    std::uint32_t callback_depth_{};
    // Nested callback wrappers retain their own route. A small fixed stack
    // avoids allocation while ensuring CompiledBlock work nested below a
    // JoinPump callback is not attributed to JoinPump.
    static constexpr std::size_t kMaximumCallbackRouteDepth = 8U;
    std::array<EeGraphicsCallbackRoute, kMaximumCallbackRouteDepth>
        callback_route_stack_{};
    std::size_t callback_route_depth_{};
    EeGraphicsJoinPumpOperationSnapshot join_pump_operations_{};
    bool active_{};
};

[[nodiscard]] const char* to_string(EeGraphicsVif1Event event) noexcept;
[[nodiscard]] const char* to_string(EeGraphicsGifEvent event) noexcept;

void prepare_empty_ee_graphics_service_profile_directory(
    const std::filesystem::path& directory);
void write_ee_graphics_service_profile_artifacts(
    const std::filesystem::path& directory,
    const EeGraphicsServiceDiagnostics& diagnostics);

// Emits the route-scoped census as a separate artifact. The established
// graphics-service TSV files intentionally retain their existing schemas.
void write_join_pump_operation_profile_artifact(
    const std::filesystem::path& directory,
    const EeGraphicsServiceDiagnostics& diagnostics,
    std::uint64_t join_pump_region_calls,
    std::uint64_t join_pump_region_retired_instructions,
    std::uint64_t join_pump_region_zero_retired_rejections,
    const std::array<std::uint64_t, kEeGraphicsJoinPumpRegionExitCount>&
        join_pump_region_exits);

} // namespace takedown::runtime
