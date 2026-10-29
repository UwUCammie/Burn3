#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {

// The first bounded timing-consumer trace deliberately owns one exact retail
// function.  These constants are also used by the runner to keep the whole
// function on the scalar interpreter path while the trace is enabled.
inline constexpr guest::GuestAddress kRetailTimerConsumerEntry = 0x0028b160U;
inline constexpr guest::GuestAddress kRetailTimerConsumerReturn = 0x0028b214U;
inline constexpr guest::GuestAddress kRetailTimerConsumerReturnDelay = 0x0028b218U;

enum class RetailTimerConsumerTraceFailure : std::uint8_t {
    None,
    NoCompleteInvocation,
    MalformedEntry,
    NestedEntry,
    MissingReturnDelay,
    ReturnInstructionMismatch,
    SavedReturnAddressMismatch,
    SharedTickInvalid,
    InvalidCounters,
    InvocationOverflow,
    NestedTargetOverflow,
    MemoryRangeInvalid,
    StepOverflow,
    IncompleteInvocation,
    HleBoundaryFailed,
    MemoryWriteOverflow,
    MemoryWritePreimageUnavailable,
    PublicationFailed,
};

// This is intentionally a tracer-local POD.  The runner samples it only at
// scalar/HLE boundaries, so the trace does not depend on the kernel's larger
// execution-context types or add work to the disabled path.
struct RetailTimerConsumerTraceContext {
    std::int32_t thread_id{};
    bool callback_active{};
    std::uint64_t callback_identity{};

    bool operator==(const RetailTimerConsumerTraceContext&) const = default;
};

enum class RetailTimerConsumerHleStatus : std::uint8_t {
    Handled,
    Blocked,
    Unsupported,
    InvalidArguments,
};

enum class RetailTimerConsumerMemoryWriteSource : std::uint8_t {
    OwnerScalar,
    InterleavedScalar,
    SynchronousHle,
    SchedulerSifDma,
    UnclassifiedTemporal,
};

enum class RetailTimerConsumerTraceScope : std::uint8_t {
    WholeRun,
    GuestWorkWindow,
};

enum class RetailTimerConsumerTracePhase : std::uint8_t {
    Immediate,
    AwaitingStart,
    Collecting,
    Completed,
};

[[nodiscard]] std::string_view to_string(RetailTimerConsumerTraceScope scope) noexcept;
[[nodiscard]] std::string_view to_string(RetailTimerConsumerTracePhase phase) noexcept;

[[nodiscard]] std::string_view to_string(RetailTimerConsumerHleStatus status) noexcept;

[[nodiscard]] std::string_view to_string(RetailTimerConsumerTraceFailure failure) noexcept;

struct RetailTimerConsumerTraceSummary {
    bool enabled{};
    bool published{};
    bool passed{};
    bool gate_applicable{};
    bool collection_opened{};
    RetailTimerConsumerTraceScope scope{RetailTimerConsumerTraceScope::WholeRun};
    RetailTimerConsumerTracePhase phase{RetailTimerConsumerTracePhase::Immediate};
    bool memory_write_journal_available{};
    bool memory_delta_complete{};
    std::filesystem::path output_path;
    std::size_t complete_invocations{};
    std::size_t nested_jalr_capacity{};
    std::size_t hle_boundary_capacity{};
    std::uint64_t attempted_entry_count{};
    std::uint64_t attempted_return_count{};
    std::uint64_t attempted_nested_jalr_count{};
    std::uint64_t attempted_hle_boundary_count{};
    std::uint64_t attempted_memory_write_count{};
    std::uint64_t interleaved_scalar_memory_write_events{};
    std::uint64_t synchronous_hle_memory_write_events{};
    std::uint64_t scheduler_sif_dma_memory_write_events{};
    std::uint64_t unclassified_temporal_memory_write_events{};
    std::uint64_t memory_write_preimage_unavailable_events{};
    std::uint64_t memory_write_continuity_gap_bytes{};
    bool memory_write_continuity_available{};
    bool temporal_memory_write_coverage_complete{};
    std::size_t memory_write_events{};
    std::size_t memory_write_capacity{};
    std::size_t nested_jalr_targets{};
    std::size_t hle_boundaries{};
    std::uint64_t observed_scalar_steps{};
    std::uint64_t observed_scalar_cycles{};
    RetailTimerConsumerTraceFailure failure{RetailTimerConsumerTraceFailure::None};

    [[nodiscard]] bool failed() const noexcept {
        return enabled && gate_applicable && !passed;
    }
};

// Bounded, default-off state-machine trace for retail timing consumer
// 0x0028b160. Hooks are intentionally scalar-boundary hooks. The bounded
// temporal write journal covers every public scalar EeMemory write while an
// owner invocation is active. Explicit scalar, synchronous-HLE, and
// scheduler-SIF-DMA leases assign provenance; unexpected paths remain
// retained as unclassified-temporal events. Setup mutations outside an active
// owner invocation remain outside the trace.
class RetailTimerConsumerTrace final : public guest::EeMemoryWriteObserver {
  public:
    static constexpr std::size_t kMaximumInvocations = 16U;
    // The garage run observed more than the original pilot bound of 32 in one
    // invocation.  Keep the witness bounded, but leave enough room for the
    // complete call fan-out before failing closed.
    static constexpr std::size_t kMaximumNestedJalrTargets = 4096U;
    static constexpr std::size_t kMaximumHleBoundaries = 64U;
    // The invocation witness is bounded independently of the scalar step
    // count. Focused callers may choose a smaller per-instance capacity.
    static constexpr std::size_t kMaximumMemoryWriteEvents = 16'384U;
    static constexpr std::uint64_t kMaximumInvocationSteps = 1'000'000U;
    // The audited bounded slices are exact: the saved frame view is
    // [SP-0x20, SP), while the consumer's object witness is the 0x40-byte
    // window at A0+0x7200.
    static constexpr std::size_t kStackRangeBytes = 0x20U;
    static constexpr std::ptrdiff_t kStackRangeOffset = -0x20;
    static constexpr std::size_t kObjectRangeOffset = 0x7200U;
    static constexpr std::size_t kObjectRangeBytes = 0x40U;

    // An empty path creates the disabled, null-observer configuration.  The
    // runner constructs this from TAKEDOWN_RETAIL_TIMER_CONSUMER_TRACE only
    // after validating --interpreter-only.
    // The optional per-instance limit is bounded by the production capacity;
    // focused tests use a smaller value to exercise overflow without replaying
    // thousands of instructions.
    explicit RetailTimerConsumerTrace(
        std::filesystem::path output_path = {},
        std::size_t nested_jalr_limit = kMaximumNestedJalrTargets,
        RetailTimerConsumerTraceScope scope = RetailTimerConsumerTraceScope::WholeRun,
        std::size_t memory_write_limit = kMaximumMemoryWriteEvents);

    RetailTimerConsumerTrace(const RetailTimerConsumerTrace&) = delete;
    RetailTimerConsumerTrace& operator=(const RetailTimerConsumerTrace&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool collecting() const noexcept {
        return enabled_ && (scope_ == RetailTimerConsumerTraceScope::WholeRun ||
                            phase_ == RetailTimerConsumerTracePhase::Collecting);
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    // Main uses the explicit capture methods below. This query remains useful
    // to focused callers that only want owner-scalar writes.
    [[nodiscard]] bool observes_memory_writes(
        const RetailTimerConsumerTraceContext& context) const noexcept;
    [[nodiscard]] bool begin_temporal_memory_write_coverage() noexcept;
    void end_temporal_memory_write_coverage() noexcept;
    [[nodiscard]] bool begin_scalar_memory_write_capture(
        const RetailTimerConsumerTraceContext& context) noexcept;
    void end_scalar_memory_write_capture() noexcept;
    [[nodiscard]] bool begin_hle_memory_write_capture(
        const RetailTimerConsumerTraceContext& context) noexcept;
    void end_hle_memory_write_capture() noexcept;
    [[nodiscard]] bool begin_scheduler_sif_dma_memory_write_capture() noexcept;
    void end_scheduler_sif_dma_memory_write_capture() noexcept;
    [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
        return output_path_;
    }

    [[nodiscard]] bool begin_guest_work_collection(std::uint64_t guest_steps,
                                                   std::uint64_t ee_cycles,
                                                   std::uint64_t stop_step) noexcept;
    [[nodiscard]] bool complete_guest_work_collection(std::uint64_t guest_steps,
                                                      std::uint64_t ee_cycles) noexcept;

    // Called immediately before the scalar instruction at state.pc(). The
    // guest counters are the pre-instruction committed step/cycle boundary.
    void observe_pre_instruction(const guest::R5900State& state,
                                 const guest::EeMemory& memory,
                                 const RetailTimerConsumerTraceContext& context,
                                 std::uint64_t guest_steps,
                                 std::uint64_t ee_cycles) noexcept;

    // Compatibility wrapper for focused callers that do not model a kernel
    // context.  Production runner hooks use the context-bearing overload.
    void observe_pre_instruction(const guest::R5900State& state,
                                 const guest::EeMemory& memory,
                                 std::uint64_t guest_steps,
                                 std::uint64_t ee_cycles) noexcept;

    // Called immediately after the scalar instruction.  The caller supplies
    // the post-instruction counters for a successful step and the unchanged
    // counters for a failed step.
    void observe_post_instruction(const guest::R5900State& state,
                                  const guest::EeMemory& memory,
                                  const guest::EeStepResult& result,
                                  const RetailTimerConsumerTraceContext& context,
                                  std::uint64_t guest_steps,
                                  std::uint64_t ee_cycles) noexcept;

    void observe_post_instruction(const guest::R5900State& state,
                                  const guest::EeMemory& memory,
                                  const guest::EeStepResult& result,
                                  std::uint64_t guest_steps,
                                  std::uint64_t ee_cycles) noexcept;

    // Resolves the pending GuestSystemCall boundary after the kernel HLE has
    // run.  The runner supplies the actual status/number and post-HLE CPU and
    // context, after committing the syscall's guest accounting.
    void observe_hle_boundary(const guest::R5900State& state,
                              const guest::EeMemory& memory,
                              RetailTimerConsumerHleStatus status,
                              std::uint32_t syscall_number,
                              const RetailTimerConsumerTraceContext& context,
                              std::uint64_t guest_steps,
                              std::uint64_t ee_cycles) noexcept;

    // Finalizes the run and atomically publishes the bounded artifact.  It is
    // idempotent, allowing the runner to report the summary on every stop
    // path.  A failed/incomplete state machine is still published with a
    // failure status, never as a misleading complete trace.
    [[nodiscard]] RetailTimerConsumerTraceSummary finalize() noexcept;

    [[nodiscard]] RetailTimerConsumerTraceSummary summary() const noexcept;

    void on_ee_memory_write(const guest::EeMemoryWriteEvent& event) noexcept override;

  public:
    struct RegisterFrame {
        std::uint64_t a0{};
        std::uint64_t a1{};
        std::uint64_t a2{};
        std::uint64_t a3{};
        std::uint64_t gp{};
        std::uint64_t sp{};
    };

    struct NestedJalrTarget {
        guest::GuestAddress pc{};
        guest::GuestAddress target{};
        std::uint64_t guest_steps{};
        std::uint64_t ee_cycles{};
    };

    struct HleBoundary {
        RetailTimerConsumerHleStatus status{RetailTimerConsumerHleStatus::Unsupported};
        std::uint32_t syscall_number{};
        std::uint64_t pre_guest_steps{};
        std::uint64_t pre_ee_cycles{};
        std::uint64_t guest_steps{};
        std::uint64_t ee_cycles{};
        std::uint32_t post_pc{};
        std::uint64_t post_ra{};
        RetailTimerConsumerTraceContext owner_context{};
        RetailTimerConsumerTraceContext pre_context{};
        RetailTimerConsumerTraceContext post_context{};
        bool suspended{};
    };

    struct Invocation {
        guest::R5900State entry_state{};
        guest::R5900State return_state{};
        RegisterFrame entry_registers{};
        RegisterFrame return_registers{};
        std::uint64_t entry_ra{};
        std::uint64_t saved_ra{};
        std::uint64_t return_ra{};
        std::uint64_t entry_guest_steps{};
        std::uint64_t return_guest_steps{};
        std::uint64_t entry_ee_cycles{};
        std::uint64_t return_ee_cycles{};
        std::uint64_t entry_ordinal{};
        std::uint64_t return_ordinal{};
        std::uint64_t owner_event_count{};
        RetailTimerConsumerTraceContext owner_context{};
        std::uint32_t object_base{};
        std::uint32_t object_range_base{};
        std::uint32_t entry_stack_base{};
        std::uint32_t return_stack_base{};
        std::optional<std::uint32_t> entry_shared_tick;
        std::optional<std::uint32_t> return_shared_tick;
        std::array<std::uint8_t, kStackRangeBytes> entry_stack{};
        std::array<std::uint8_t, kStackRangeBytes> return_stack{};
        std::array<std::uint8_t, kObjectRangeBytes> entry_object{};
        std::array<std::uint8_t, kObjectRangeBytes> return_object{};
        std::array<NestedJalrTarget, kMaximumNestedJalrTargets> nested_jalr{};
        std::size_t nested_jalr_count{};
        std::array<HleBoundary, kMaximumHleBoundaries> hle_boundaries{};
        std::size_t hle_boundary_count{};
        std::array<guest::EeMemoryWriteEvent, kMaximumMemoryWriteEvents>
            memory_writes{};
        std::array<RetailTimerConsumerMemoryWriteSource, kMaximumMemoryWriteEvents>
            memory_write_sources{};
        std::size_t memory_write_count{};
        std::uint64_t attempted_memory_write_count{};
        std::uint32_t return_jr_word{};
        std::uint32_t return_delay_word{};
        bool return_jr_seen{};
    };

  private:
    void fail(RetailTimerConsumerTraceFailure failure) noexcept;
    void begin_invocation(const guest::R5900State& state,
                          const guest::EeMemory& memory,
                          std::uint64_t guest_steps,
                          std::uint64_t ee_cycles) noexcept;
    void observe_nested_jalr(const guest::R5900State& state,
                             const guest::EeMemory& memory,
                             std::uint64_t guest_steps,
                             std::uint64_t ee_cycles) noexcept;
    void observe_return_jr(const guest::R5900State& state,
                           const guest::EeMemory& memory,
                           std::uint64_t guest_steps,
                           std::uint64_t ee_cycles) noexcept;
    void complete_invocation(const guest::R5900State& state,
                             const guest::EeMemory& memory,
                             const guest::EeStepResult& result,
                             std::uint64_t guest_steps,
                             std::uint64_t ee_cycles) noexcept;
    void record_hle_boundary(const guest::R5900State& state,
                             RetailTimerConsumerHleStatus status,
                             std::uint32_t syscall_number,
                             const RetailTimerConsumerTraceContext& post_context,
                             std::uint64_t guest_steps,
                             std::uint64_t ee_cycles,
                             bool suspended) noexcept;

    [[nodiscard]] bool capture_range(const guest::EeMemory& memory,
                                     std::uint32_t base,
                                     std::ptrdiff_t offset,
                                     std::span<std::uint8_t> destination) noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    read_shared_tick(const guest::R5900State& state,
                     const guest::EeMemory& memory) const noexcept;
    [[nodiscard]] bool decode_at(const guest::EeMemory& memory,
                                 guest::GuestAddress pc,
                                 guest::DecodedR5900Instruction& instruction) const noexcept;
    [[nodiscard]] bool counters_valid(std::uint64_t guest_steps,
                                      std::uint64_t ee_cycles) const noexcept;
    [[nodiscard]] bool context_matches_owner(
        const RetailTimerConsumerTraceContext& context) const noexcept;
    [[nodiscard]] bool record_owner_event() noexcept;
    [[nodiscard]] bool write_artifact() noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    memory_write_continuity_gap_bytes() const noexcept;
    void preserve_failed_memory_write_invocation() noexcept;

    std::filesystem::path output_path_;
    std::size_t nested_jalr_limit_{kMaximumNestedJalrTargets};
    std::size_t memory_write_limit_{kMaximumMemoryWriteEvents};
    bool enabled_{};
    RetailTimerConsumerTraceScope scope_{RetailTimerConsumerTraceScope::WholeRun};
    RetailTimerConsumerTracePhase phase_{RetailTimerConsumerTracePhase::Immediate};
    bool collection_opened_{};
    std::uint64_t collection_start_guest_steps_{};
    std::uint64_t collection_start_ee_cycles_{};
    std::uint64_t collection_stop_step_{};
    std::uint64_t collection_end_guest_steps_{};
    std::uint64_t collection_end_ee_cycles_{};
    bool active_{};
    bool temporal_memory_write_coverage_opened_{};
    bool temporal_memory_write_coverage_active_{};
    bool temporal_memory_write_coverage_closed_{};
    bool finalized_{};
    bool published_{};
    RetailTimerConsumerTraceFailure failure_{RetailTimerConsumerTraceFailure::None};
    std::uint64_t last_guest_steps_{};
    std::uint64_t last_ee_cycles_{};
    bool counters_seen_{};
    std::uint64_t next_entry_ordinal_{};
    std::uint64_t next_return_ordinal_{};
    std::uint64_t attempted_entry_count_{};
    std::uint64_t attempted_return_count_{};
    std::uint64_t attempted_nested_jalr_count_{};
    std::uint64_t attempted_hle_boundary_count_{};
    std::uint64_t attempted_memory_write_count_{};
    std::uint64_t interleaved_scalar_memory_write_events_{};
    std::uint64_t synchronous_hle_memory_write_events_{};
    std::uint64_t scheduler_sif_dma_memory_write_events_{};
    std::uint64_t unclassified_temporal_memory_write_events_{};
    std::uint64_t memory_write_preimage_unavailable_events_{};
    std::uint64_t observed_scalar_steps_{};
    std::uint64_t observed_scalar_cycles_{};
    std::unique_ptr<Invocation> current_;
    // A memory-journal overflow fails closed but retains the partial
    // invocation so its bounded prefix remains serializable evidence.
    std::unique_ptr<Invocation> failed_current_;
    std::vector<Invocation> invocations_;
    RetailTimerConsumerTraceContext owner_context_{};
    RetailTimerConsumerTraceContext pending_hle_context_{};
    std::uint32_t pending_hle_syscall_number_{};
    std::uint64_t pending_hle_guest_steps_{};
    std::uint64_t pending_hle_ee_cycles_{};
    bool pending_hle_boundary_{};
    std::optional<RetailTimerConsumerMemoryWriteSource> active_memory_write_source_{};
    bool suspended_{};
    // Keep the most recent owner boundary independently of the active record
    // so a fail-closed HLE status still publishes the syscall/context witness
    // after its invocation storage is discarded.
    bool last_hle_boundary_seen_{};
    RetailTimerConsumerHleStatus last_hle_status_{RetailTimerConsumerHleStatus::Unsupported};
    std::uint32_t last_hle_syscall_number_{};
    std::uint64_t last_hle_pre_guest_steps_{};
    std::uint64_t last_hle_pre_ee_cycles_{};
    std::uint64_t last_hle_guest_steps_{};
    std::uint64_t last_hle_ee_cycles_{};
    std::uint32_t last_hle_post_pc_{};
    std::uint64_t last_hle_post_ra_{};
    RetailTimerConsumerTraceContext last_hle_owner_context_{};
    RetailTimerConsumerTraceContext last_hle_pre_context_{};
    RetailTimerConsumerTraceContext last_hle_post_context_{};
    bool last_hle_suspended_{};
};

} // namespace takedown::runtime
