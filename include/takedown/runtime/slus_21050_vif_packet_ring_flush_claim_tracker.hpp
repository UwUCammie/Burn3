#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_native_functions.hpp"
#include "takedown/runtime/slus_21050_ui_draw_session_feed.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace takedown::guest {

class EeMemory;
class R5900State;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

class UiTextVifTransportClaimRegistry;

enum class VifPacketRingFlushClaimFailure : std::uint8_t {
    None,
    InvalidEntryState,
    DuplicateExecutionContext,
    ContextCapacityExhausted,
    FlushPlanRejected,
    InvalidReturnState,
    FlushPostStateMismatch,
    RelocationRejected,
    ExecutionContextRetired,
    TrackerDestroyed,
    InternalFailure,
};

[[nodiscard]] std::string_view
to_string(VifPacketRingFlushClaimFailure failure) noexcept;

struct VifPacketRingFlushClaimTrackerDiagnostics {
    std::uint64_t gated_entries_without_claims{};
    std::uint64_t plan_attempts{};
    std::uint64_t captured_flushes{};
    std::uint64_t reported_flushes{};
    std::uint64_t completed_flushes{};
    std::uint64_t relocated_claims{};
    std::uint64_t failed_entries{};
    std::uint64_t failed_returns{};
    std::uint64_t failed_post_states{};
    std::uint64_t fail_closed_events{};
    std::uint64_t fail_closed_claims{};
    std::uint64_t retired_contexts{};
    std::uint64_t compiled_boundary_clips{};
    std::size_t active_contexts{};
    std::size_t peak_active_contexts{};
    VifPacketRingFlushClaimFailure first_failure{
        VifPacketRingFlushClaimFailure::None};

    bool operator==(const VifPacketRingFlushClaimTrackerDiagnostics&) const = default;
};

// Observes the verified standalone SLUS-21050 VIF packet-ring flush at its
// dispatcher boundary. The tracker borrows the startup-owned claim registry;
// that registry must outlive it. All suspended-call state is held in a fixed
// array and every hot operation is allocation-free.
//
// observe_post_instruction() must run immediately after scalar or compiled
// execution and before scheduler or DMAC work. Atomic native dispatch uses the
// matching post-native seam. observe_pre_instruction() recognizes the eventual
// return for authentication and defensive cleanup, but that later seam alone
// is deliberately never allowed to publish a late relocation.
class VifPacketRingFlushClaimTracker final {
  public:
    static constexpr guest::GuestAddress kFlushEntryAddress = 0x0021a4f0U;
    static constexpr guest::GuestAddress kFlushCommitAddress = 0x0021a5d4U;
    static constexpr guest::GuestAddress kFlushEndAddress = 0x0021a5e8U;
    static constexpr std::size_t kMaximumSuspendedContexts = 8U;

    explicit VifPacketRingFlushClaimTracker(
        UiTextVifTransportClaimRegistry& registry) noexcept;
    ~VifPacketRingFlushClaimTracker();

    VifPacketRingFlushClaimTracker(const VifPacketRingFlushClaimTracker&) = delete;
    VifPacketRingFlushClaimTracker& operator=(
        const VifPacketRingFlushClaimTracker&) = delete;
    VifPacketRingFlushClaimTracker(VifPacketRingFlushClaimTracker&&) = delete;
    VifPacketRingFlushClaimTracker& operator=(
        VifPacketRingFlushClaimTracker&&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return diagnostics_.active_contexts != 0U;
    }

    void observe_pre_instruction(
        const guest::R5900State& cpu, const guest::EeMemory& memory,
        SubmitUiDrawBucketsExecutionContext context) noexcept;

    // The store at kFlushCommitAddress is the final packet/global mutation.
    // Reporting immediately after it closes the transport gap before a timed
    // VIF1 snapshot can run; executed_pc names that just-retired instruction.
    void observe_post_instruction(
        guest::GuestAddress executed_pc, const guest::R5900State& cpu,
        const guest::EeMemory& memory,
        SubmitUiDrawBucketsExecutionContext context) noexcept;

    // Atomic native execution hides the internal commit PC. Call this
    // immediately after a handled dispatch whose executed_entry_pc is the
    // standalone flush entry and before servicing scheduler-visible work.
    void observe_post_native_execution(
        guest::GuestAddress executed_entry_pc,
        const guest::R5900State& cpu, const guest::EeMemory& memory,
        SubmitUiDrawBucketsExecutionContext context) noexcept;

    [[nodiscard]] bool requires_pre_instruction(
        SubmitUiDrawBucketsExecutionContext context,
        guest::GuestAddress candidate_pc) const noexcept;

    [[nodiscard]] std::uint32_t clip_compiled_budget(
        guest::GuestAddress start_pc, std::uint32_t maximum_instructions,
        SubmitUiDrawBucketsExecutionContext context) noexcept;

    void retire_context(SubmitUiDrawBucketsExecutionContext context) noexcept;
    void retire_thread(std::int32_t thread_id) noexcept;
    void retire_all() noexcept;

    [[nodiscard]] VifPacketRingFlushClaimTrackerDiagnostics
    diagnostics() const noexcept;

  private:
    struct PendingFlush {
        bool active{};
        SubmitUiDrawBucketsExecutionContext context{};
        VifPacketRingFlushPlan plan{};
        guest::GuestAddress canonical_stack_pointer{};
        guest::GuestAddress return_address{};
        std::uint64_t claim_epoch{};
        bool reported{};
    };

    [[nodiscard]] PendingFlush* find_context(
        SubmitUiDrawBucketsExecutionContext context) noexcept;
    [[nodiscard]] const PendingFlush* find_context(
        SubmitUiDrawBucketsExecutionContext context) const noexcept;
    [[nodiscard]] PendingFlush* find_free_slot() noexcept;

    void begin_flush(const guest::R5900State& cpu,
                     const guest::EeMemory& memory,
                     SubmitUiDrawBucketsExecutionContext context) noexcept;
    [[nodiscard]] bool report_committed_flush(
        PendingFlush& pending, const guest::EeMemory& memory) noexcept;
    void finish_return(PendingFlush& pending,
                       const guest::R5900State& cpu) noexcept;
    void deactivate(PendingFlush& pending) noexcept;
    void record_failure(VifPacketRingFlushClaimFailure failure) noexcept;
    void fail_closed(VifPacketRingFlushClaimFailure failure) noexcept;

    UiTextVifTransportClaimRegistry* registry_{};
    std::array<PendingFlush, kMaximumSuspendedContexts> pending_{};
    VifPacketRingFlushClaimTrackerDiagnostics diagnostics_{};
    std::uint64_t claim_epoch_{1U};
};

static_assert(VifPacketRingFlushClaimTracker::kMaximumSuspendedContexts > 1U);

} // namespace takedown::runtime::slus_21050
