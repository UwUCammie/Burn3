#pragma once

#include "takedown/guest/types.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace takedown::guest {

class EeMemory;
class R5900State;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050::ui_draw_guest_detail {

struct SubmitUiDrawBucketsSessionObservation;

} // namespace takedown::runtime::slus_21050::ui_draw_guest_detail

namespace takedown::runtime::slus_21050 {

// Stable production-facing failure classification. Observer implementation
// details stay source-private while runtime diagnostics retain a meaningful,
// allocation-free first-failure value.
enum class SubmitUiDrawBucketsFeedFailure : std::uint8_t {
    None,
    WorkspaceUnavailable,
    FunctionProfileRejected,
    CpuStateRejected,
    GuestSourceRejected,
    UnexpectedBoundary,
    LoopProgressMismatch,
    SetterOrderMismatch,
    CallbackBoundaryRejected,
    CallbackReturnMismatch,
    TargetSourceRejected,
    TargetMismatch,
    LimitExceeded,
    InvocationReturnedBeforeCompletion,
    ExecutionContextRetired,
    FeedDestroyed,
    InternalObserverFailure,
};

[[nodiscard]] std::string_view to_string(SubmitUiDrawBucketsFeedFailure failure) noexcept;

struct SubmitUiDrawBucketsSessionFeedDiagnostics {
    std::uint64_t attempted_invocations{};
    std::uint64_t completed_invocations{};
    std::uint64_t sealed_invocations{};
    std::uint64_t overlapping_invocations{};
    std::uint64_t observed_events{};
    std::uint64_t compiled_boundary_clips{};
    std::uint64_t internal_failures{};
    std::uint64_t code_profile_refreshes{};
    SubmitUiDrawBucketsFeedFailure first_failure{SubmitUiDrawBucketsFeedFailure::None};
    std::int32_t active_thread_id{};
    bool active{};
    bool awaiting_sealed_return{};

    bool operator==(const SubmitUiDrawBucketsSessionFeedDiagnostics&) const = default;
};

// Kernel callback delivery can temporarily borrow the same guest thread id.
// The callback identity remains stable across SIF callback suspension/resume,
// even when it resumes over a different current thread. Callback contexts are
// therefore keyed by identity; normal contexts are keyed by guest thread id.
struct SubmitUiDrawBucketsExecutionContext {
    std::int32_t thread_id{};
    bool kernel_callback_active{};
    std::uint64_t kernel_callback_identity{};

    [[nodiscard]] bool operator==(
        const SubmitUiDrawBucketsExecutionContext& other) const noexcept {
        if (kernel_callback_active != other.kernel_callback_active) {
            return false;
        }
        if (!kernel_callback_active) {
            return thread_id == other.thread_id;
        }
        // Production callback contexts always have a nonzero identity. Keep a
        // reflexive fallback for standalone fixtures that omit one.
        if (kernel_callback_identity == 0U || other.kernel_callback_identity == 0U) {
            return thread_id == other.thread_id &&
                   kernel_callback_identity == other.kernel_callback_identity;
        }
        return kernel_callback_identity == other.kernel_callback_identity;
    }
};

enum class SubmitUiDrawBucketsFeedTerminationReason : std::uint8_t {
    Completed,
    ObserverSealed,
    ReturnedBeforeCompletion,
    ExecutionContextRetired,
    FeedDestroyed,
};

// Exactly one termination is emitted for every admitted invocation, after its
// final observation and before the fixed workspace is released. This lets a
// future typed session owner discard partial state on every fail-closed path.
struct SubmitUiDrawBucketsSessionTermination {
    SubmitUiDrawBucketsExecutionContext owner_context{};
    SubmitUiDrawBucketsFeedTerminationReason reason{
        SubmitUiDrawBucketsFeedTerminationReason::Completed};
    SubmitUiDrawBucketsFeedFailure failure{SubmitUiDrawBucketsFeedFailure::None};
    std::uint64_t invocation_ordinal{};
    std::uint64_t observed_events{};

    bool operator==(const SubmitUiDrawBucketsSessionTermination&) const = default;
};

// Both sinks run synchronously on the serialized EE execution thread and must
// neither retain a reference, throw, nor call back into this feed. sink_context
// must remain valid until feed destruction completes because destruction can
// emit a terminal cancellation. A null observation sink keeps the production
// feed as a diagnostics-only passive observer. This source-private observation
// seam exists for differential verification; the renderer-facing session owner
// must use the later stable, game-specific event adapter rather than this raw
// type.
using SubmitUiDrawBucketsSessionObservationSink = void (*)(
    void* context,
    const ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation& observation) noexcept;
using SubmitUiDrawBucketsSessionTerminationSink = void (*)(
    void* context, const SubmitUiDrawBucketsSessionTermination& termination) noexcept;

// Passive production bridge for one physical SubmitUiDrawBuckets invocation at
// a time. Startup allocates its fixed workspace once. The hot path performs no
// dynamic allocation, never mutates guest state, never executes or suppresses
// guest work, and never submits renderer work.
class SubmitUiDrawBucketsSessionFeed final {
  public:
    static constexpr guest::GuestAddress kSubmitEntryAddress = 0x001d75c0U;

    explicit SubmitUiDrawBucketsSessionFeed(
        SubmitUiDrawBucketsSessionObservationSink sink = nullptr,
        void* sink_context = nullptr,
        SubmitUiDrawBucketsSessionTerminationSink termination_sink = nullptr,
        bool enabled = true);
    ~SubmitUiDrawBucketsSessionFeed();

    SubmitUiDrawBucketsSessionFeed(const SubmitUiDrawBucketsSessionFeed&) = delete;
    SubmitUiDrawBucketsSessionFeed& operator=(const SubmitUiDrawBucketsSessionFeed&) = delete;
    SubmitUiDrawBucketsSessionFeed(SubmitUiDrawBucketsSessionFeed&&) = delete;
    SubmitUiDrawBucketsSessionFeed& operator=(SubmitUiDrawBucketsSessionFeed&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Call once for each state accepted by requires_pre_instruction(). Guest
    // thread suspension is supported: only the invocation's owning execution
    // context advances the observer.
    void observe_pre_instruction(const guest::R5900State& cpu,
                                 const guest::EeMemory& memory,
                                 SubmitUiDrawBucketsExecutionContext context) noexcept;

    // True when a prospective compiled instruction must first return to the
    // outer runner. This includes the dynamic callback target and original
    // caller return learned from the active invocation.
    [[nodiscard]] bool requires_pre_instruction(
        const SubmitUiDrawBucketsExecutionContext context,
        const guest::GuestAddress candidate_pc) const noexcept {
        return enabled_ && (candidate_pc == kSubmitEntryAddress ||
                            (active_ && requires_active_pre_instruction(context, candidate_pc)));
    }

    // What a compiled run has to stop before while a session is live, so that
    // the runner can observe those instructions: the observer's static boundary
    // table (sorted), and the two learned addresses, the dynamic callback
    // target and the original caller return (0 when not known). Only the
    // runner changes the learned pair, so it stays valid for a whole burst.
    // The phases that need every instruction (a delay slot or entry awaited)
    // are never run compiled: requires_pre_instruction is true for each PC.
    [[nodiscard]] static std::span<const guest::GuestAddress> static_boundaries() noexcept;
    [[nodiscard]] guest::GuestAddress learned_callback_target() const noexcept;
    [[nodiscard]] guest::GuestAddress learned_caller_return() const noexcept;

    // Shortens a prospective sequential compiled bundle so the next required
    // pre-instruction state returns to the runner. The current instruction was
    // already observed and may still use compiled execution.
    [[nodiscard]] std::uint32_t
    clip_compiled_budget(guest::GuestAddress start_pc, std::uint32_t maximum_instructions,
                         SubmitUiDrawBucketsExecutionContext context) noexcept;

    // End a session whose physical execution context can no longer resume.
    // The exact-context form is used for callback completion; the thread form
    // also covers a suspended thread terminated by another guest thread. The
    // all-context form covers a successful SetupThread, which starts a new
    // kernel thread namespace and can reuse thread IDs; every saved owner is
    // therefore conservatively invalidated.
    void retire_execution_context(SubmitUiDrawBucketsExecutionContext context) noexcept;
    void retire_thread(std::int32_t thread_id) noexcept;
    void retire_all_execution_contexts() noexcept;

    // A whole-function replacement for the outer routine would hide all of its
    // internal boundaries. Leaf helpers and callbacks remain eligible because
    // their entry and return states are visible in consecutive runner passes.
    [[nodiscard]] bool
    can_dispatch_atomic_native_function(
        const SubmitUiDrawBucketsExecutionContext context,
        const guest::GuestAddress entry_address) const noexcept {
        return entry_address != kSubmitEntryAddress &&
               (!active_ || can_dispatch_active_native_function(context, entry_address));
    }

    [[nodiscard]] SubmitUiDrawBucketsSessionFeedDiagnostics diagnostics() const noexcept;

  private:
    [[nodiscard]] bool requires_active_pre_instruction(
        SubmitUiDrawBucketsExecutionContext context,
        guest::GuestAddress candidate_pc) const noexcept;
    [[nodiscard]] bool can_dispatch_active_native_function(
        SubmitUiDrawBucketsExecutionContext context,
        guest::GuestAddress entry_address) const noexcept;

    class Impl;
    // A disabled feed never starts a session: every observation seam stays
    // inert and compiled code runs through SubmitUiDrawBuckets unobserved.
    bool enabled_{true};
    bool active_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime::slus_21050
