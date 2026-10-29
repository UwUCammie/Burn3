#include "takedown/runtime/slus_21050_ui_draw_session_feed.hpp"

#include "takedown/runtime/slus_21050_ui_draw_session_observer.hpp"

#include <limits>
#include <optional>
#include <utility>

namespace takedown::runtime::slus_21050 {

std::string_view to_string(const SubmitUiDrawBucketsFeedFailure failure) noexcept {
    switch (failure) {
    case SubmitUiDrawBucketsFeedFailure::None:
        return "none";
    case SubmitUiDrawBucketsFeedFailure::WorkspaceUnavailable:
        return "workspace-unavailable";
    case SubmitUiDrawBucketsFeedFailure::FunctionProfileRejected:
        return "function-profile-rejected";
    case SubmitUiDrawBucketsFeedFailure::CpuStateRejected:
        return "cpu-state-rejected";
    case SubmitUiDrawBucketsFeedFailure::GuestSourceRejected:
        return "guest-source-rejected";
    case SubmitUiDrawBucketsFeedFailure::UnexpectedBoundary:
        return "unexpected-boundary";
    case SubmitUiDrawBucketsFeedFailure::LoopProgressMismatch:
        return "loop-progress-mismatch";
    case SubmitUiDrawBucketsFeedFailure::SetterOrderMismatch:
        return "setter-order-mismatch";
    case SubmitUiDrawBucketsFeedFailure::CallbackBoundaryRejected:
        return "callback-boundary-rejected";
    case SubmitUiDrawBucketsFeedFailure::CallbackReturnMismatch:
        return "callback-return-mismatch";
    case SubmitUiDrawBucketsFeedFailure::TargetSourceRejected:
        return "target-source-rejected";
    case SubmitUiDrawBucketsFeedFailure::TargetMismatch:
        return "target-mismatch";
    case SubmitUiDrawBucketsFeedFailure::LimitExceeded:
        return "limit-exceeded";
    case SubmitUiDrawBucketsFeedFailure::InvocationReturnedBeforeCompletion:
        return "invocation-returned-before-completion";
    case SubmitUiDrawBucketsFeedFailure::ExecutionContextRetired:
        return "execution-context-retired";
    case SubmitUiDrawBucketsFeedFailure::FeedDestroyed:
        return "feed-destroyed";
    case SubmitUiDrawBucketsFeedFailure::InternalObserverFailure:
        return "internal-observer-failure";
    }
    return "unknown";
}

namespace {

using ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation;
using ui_draw_guest_detail::SubmitUiDrawBucketsSessionObserver;
using ui_draw_guest_detail::SubmitUiDrawBucketsSessionWorkspace;
using ui_draw_guest_detail::UiDrawSessionBoundaryKind;
using ui_draw_guest_detail::UiDrawSessionObservationStatus;
using ui_draw_guest_detail::UiDrawSessionSealReason;

constexpr guest::GuestAddress kEntry = SubmitUiDrawBucketsSessionFeed::kSubmitEntryAddress;

const ui_draw_guest_detail::UiDrawSessionCallbackCallObservation*
callback_call(const SubmitUiDrawBucketsSessionObservation& observation) noexcept {
    if (!observation.event.has_value()) {
        return nullptr;
    }
    return std::get_if<ui_draw_guest_detail::UiDrawSessionCallbackCallObservation>(&*observation.event);
}

void saturating_increment(unsigned long long& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

void saturating_add(std::uint64_t& counter, const std::uint64_t amount) noexcept {
    counter = amount <= std::numeric_limits<std::uint64_t>::max() - counter
                  ? counter + amount
                  : std::numeric_limits<std::uint64_t>::max();
}

SubmitUiDrawBucketsFeedFailure failure_for(const UiDrawSessionSealReason reason) noexcept {
    switch (reason) {
    case UiDrawSessionSealReason::WorkspaceUnavailable:
        return SubmitUiDrawBucketsFeedFailure::WorkspaceUnavailable;
    case UiDrawSessionSealReason::CodeProfileRejected:
        return SubmitUiDrawBucketsFeedFailure::FunctionProfileRejected;
    case UiDrawSessionSealReason::EntryMismatch:
        return SubmitUiDrawBucketsFeedFailure::CpuStateRejected;
    case UiDrawSessionSealReason::FrameOutOfRange:
        return SubmitUiDrawBucketsFeedFailure::GuestSourceRejected;
    case UiDrawSessionSealReason::Reason5:
        return SubmitUiDrawBucketsFeedFailure::UnexpectedBoundary;
    case UiDrawSessionSealReason::LoopProgressMismatch:
        return SubmitUiDrawBucketsFeedFailure::LoopProgressMismatch;
    case UiDrawSessionSealReason::SetterRejected:
        return SubmitUiDrawBucketsFeedFailure::SetterOrderMismatch;
    case UiDrawSessionSealReason::CallbackBoundaryRejected:
        return SubmitUiDrawBucketsFeedFailure::CallbackBoundaryRejected;
    case UiDrawSessionSealReason::Reason9:
        return SubmitUiDrawBucketsFeedFailure::CallbackReturnMismatch;
    case UiDrawSessionSealReason::TargetUnavailable:
        return SubmitUiDrawBucketsFeedFailure::TargetSourceRejected;
    case UiDrawSessionSealReason::Reason11:
        return SubmitUiDrawBucketsFeedFailure::TargetMismatch;
    case UiDrawSessionSealReason::CallbackLimitExceeded:
        return SubmitUiDrawBucketsFeedFailure::LimitExceeded;
    case UiDrawSessionSealReason::None:
        return SubmitUiDrawBucketsFeedFailure::None;
    }
    return SubmitUiDrawBucketsFeedFailure::InternalObserverFailure;
}

} // namespace

class SubmitUiDrawBucketsSessionFeed::Impl {
  public:
    Impl(bool& externally_active, const SubmitUiDrawBucketsSessionObservationSink sink,
         void* const sink_context,
         const SubmitUiDrawBucketsSessionTerminationSink termination_sink) noexcept
        : externally_active_(&externally_active), sink_(sink), termination_sink_(termination_sink),
          sink_context_(sink_context) {}

    // An admitted invocation still live at destruction is cancelled so its owner
    // sees exactly one termination.
    ~Impl() {
        if (session_.has_value()) {
            fail(SubmitUiDrawBucketsFeedFailure::FeedDestroyed);
            finish(SubmitUiDrawBucketsFeedTerminationReason::FeedDestroyed);
        }
    }

    // Records the first failure of the invocation once; later ones are folded in.
    void fail(const SubmitUiDrawBucketsFeedFailure failure) noexcept {
        if (invocation_failure_ != SubmitUiDrawBucketsFeedFailure::None) {
            return;
        }
        invocation_failure_ = failure;
        saturating_increment(diagnostics_.sealed_invocations);
        if (diagnostics_.first_failure == SubmitUiDrawBucketsFeedFailure::None) {
            diagnostics_.first_failure = failure;
        }
    }

    // Emits the single termination for the admitted invocation and releases the workspace.
    void finish(const SubmitUiDrawBucketsFeedTerminationReason reason) noexcept {
        if (termination_sink_ != nullptr) {
            termination_sink_(sink_context_, {owner_context_, reason, invocation_failure_,
                                              current_invocation_ordinal_, last_event_ordinal_});
        }
        session_.reset();
        *externally_active_ = false;
        owner_context_ = {};
        caller_stack_pointer_ = 0U;
        caller_return_address_ = 0U;
        dynamic_callback_target_ = 0U;
        last_event_ordinal_ = 0U;
        awaiting_sealed_return_ = false;
        awaiting_rejected_entry_departure_ = false;
        invocation_failure_ = SubmitUiDrawBucketsFeedFailure::None;
        current_invocation_ordinal_ = 0U;
    }

    [[nodiscard]] bool requires_boundary(const SubmitUiDrawBucketsExecutionContext context,
                                         const guest::GuestAddress candidate_pc) const noexcept {
        if (candidate_pc == kEntry) {
            return true;
        }
        if (!session_.has_value() || !(context == owner_context_)) {
            return false;
        }
        if (awaiting_rejected_entry_departure_) {
            return true;
        }
        if (awaiting_sealed_return_) {
            return candidate_pc == caller_return_address_;
        }
        return session_->requires_instruction_boundary(candidate_pc) ||
               candidate_pc == dynamic_callback_target_ || candidate_pc == caller_return_address_;
    }

    [[nodiscard]] bool can_dispatch_native(const SubmitUiDrawBucketsExecutionContext context,
                                           const guest::GuestAddress entry_address) const noexcept {
        if (entry_address == kEntry) {
            return false;
        }
        if (!session_.has_value() || awaiting_sealed_return_ || !(context == owner_context_)) {
            return true;
        }
        return session_->allows_atomic_native_dispatch();
    }

    void retire_owned(const SubmitUiDrawBucketsExecutionContext context) noexcept {
        if (!session_.has_value() || !(context == owner_context_)) {
            return;
        }
        fail(SubmitUiDrawBucketsFeedFailure::ExecutionContextRetired);
        finish(SubmitUiDrawBucketsFeedTerminationReason::ExecutionContextRetired);
    }

    void retire_thread(const std::int32_t thread_id) noexcept {
        if (!session_.has_value() || owner_context_.kernel_callback_active ||
            owner_context_.thread_id != thread_id) {
            return;
        }
        fail(SubmitUiDrawBucketsFeedFailure::ExecutionContextRetired);
        finish(SubmitUiDrawBucketsFeedTerminationReason::ExecutionContextRetired);
    }

    void retire_all() noexcept {
        if (!session_.has_value()) {
            return;
        }
        fail(SubmitUiDrawBucketsFeedFailure::ExecutionContextRetired);
        finish(SubmitUiDrawBucketsFeedTerminationReason::ExecutionContextRetired);
    }

    [[nodiscard]] std::uint32_t clip_budget(const guest::GuestAddress start_pc,
                                            const std::uint32_t maximum_instructions,
                                            const SubmitUiDrawBucketsExecutionContext context) noexcept {
        if (maximum_instructions <= 1U) {
            return maximum_instructions;
        }
        for (std::uint32_t offset = 1U; offset < maximum_instructions; ++offset) {
            const unsigned long long candidate = static_cast<unsigned long long>(start_pc) +
                                                 static_cast<unsigned long long>(offset) * 4U;
            if (candidate > std::numeric_limits<std::uint32_t>::max()) {
                return maximum_instructions;
            }
            if (requires_boundary(context, static_cast<guest::GuestAddress>(candidate))) {
                saturating_increment(diagnostics_.compiled_boundary_clips);
                return offset;
            }
        }
        return maximum_instructions;
    }

    void begin(const guest::R5900State& cpu, const SubmitUiDrawBucketsExecutionContext context) {
        saturating_increment(diagnostics_.attempted_invocations);
        owner_context_ = context;
        caller_stack_pointer_ = cpu.read_gpr_u32(29U);
        caller_return_address_ = cpu.read_gpr_u32(31U);
        dynamic_callback_target_ = 0U;
        last_event_ordinal_ = 0U;
        awaiting_sealed_return_ = false;
        awaiting_rejected_entry_departure_ = false;
        invocation_failure_ = SubmitUiDrawBucketsFeedFailure::None;
        session_.emplace(workspace_);
        *externally_active_ = true;
        current_invocation_ordinal_ = diagnostics_.attempted_invocations;
    }

    void observe(const guest::R5900State& cpu, const guest::EeMemory& memory,
                 const SubmitUiDrawBucketsExecutionContext context) noexcept {
        const guest::GuestAddress pc = cpu.pc();
        if (!session_.has_value()) {
            if (pc != kEntry) {
                return;
            }
            begin(cpu, context);
        } else if (!(context == owner_context_)) {
            if (pc == kEntry) {
                saturating_increment(diagnostics_.overlapping_invocations);
            }
            return;
        }

        if (awaiting_rejected_entry_departure_) {
            finish(SubmitUiDrawBucketsFeedTerminationReason::ObserverSealed);
            return;
        }

        const bool returned_to_caller = returned_to_caller_frame(cpu);
        if (awaiting_sealed_return_) {
            if (returned_to_caller) {
                finish(SubmitUiDrawBucketsFeedTerminationReason::ObserverSealed);
            }
            return;
        }

        try {
            const SubmitUiDrawBucketsSessionObservation observation =
                session_->observe_instruction_boundary(cpu, memory);
            record_ordinal(observation);
            learn_callback_target(observation);
            if (sink_ != nullptr && observation.status != UiDrawSessionObservationStatus::Unset) {
                sink_(sink_context_, observation);
            }
            if (observation.status == UiDrawSessionObservationStatus::Completed &&
                observation.seal_reason == UiDrawSessionSealReason::None) {
                saturating_increment(diagnostics_.completed_invocations);
                finish(SubmitUiDrawBucketsFeedTerminationReason::Completed);
            } else if (observation.status == UiDrawSessionObservationStatus::Sealed) {
                fail(failure_for(observation.seal_reason));
                if (observation.boundary == UiDrawSessionBoundaryKind::Boundary1 &&
                    !caller_frame_plausible()) {
                    awaiting_rejected_entry_departure_ = true;
                } else {
                    awaiting_sealed_return_ = true;
                    if (returned_to_caller) {
                        finish(SubmitUiDrawBucketsFeedTerminationReason::ObserverSealed);
                    }
                }
                return;
            } else if (returned_to_caller) {
                fail(SubmitUiDrawBucketsFeedFailure::InvocationReturnedBeforeCompletion);
                finish(SubmitUiDrawBucketsFeedTerminationReason::ReturnedBeforeCompletion);
            }
        } catch (...) {
            saturating_increment(diagnostics_.internal_failures);
            fail(SubmitUiDrawBucketsFeedFailure::InternalObserverFailure);
            if (!caller_frame_plausible()) {
                awaiting_rejected_entry_departure_ = true;
                return;
            }
            awaiting_sealed_return_ = true;
            if (returned_to_caller) {
                finish(SubmitUiDrawBucketsFeedTerminationReason::ObserverSealed);
            }
        }
    }

    void record_ordinal(const SubmitUiDrawBucketsSessionObservation& observation) noexcept {
        if (observation.event_ordinal > last_event_ordinal_) {
            saturating_add(diagnostics_.observed_events,
                           observation.event_ordinal - last_event_ordinal_);
            last_event_ordinal_ = observation.event_ordinal;
        }
    }

    [[nodiscard]] bool returned_to_caller_frame(const guest::R5900State& cpu) const noexcept {
        return !cpu.is_executing_delay_slot() && cpu.pc() == caller_return_address_ &&
               cpu.read_gpr_u32(29U) == caller_stack_pointer_ &&
               caller_return_address_ == cpu.read_gpr_u32(31U);
    }

    void learn_callback_target(const SubmitUiDrawBucketsSessionObservation& observation) noexcept {
        if (const auto* const call = callback_call(observation)) {
            dynamic_callback_target_ = call->occurrence.visit.callback_address;
        }
        if (observation.boundary == UiDrawSessionBoundaryKind::Boundary25) {
            dynamic_callback_target_ = 0U;
        }
    }

    // A frame the original caller can plausibly resume from.
    [[nodiscard]] bool caller_frame_plausible() const noexcept {
        return caller_return_address_ != 0U && caller_return_address_ != kEntry &&
               (caller_return_address_ & 3U) == 0U && caller_stack_pointer_ > 303U &&
               (caller_stack_pointer_ & 15U) == 0U;
    }

    SubmitUiDrawBucketsSessionWorkspace workspace_;
    std::optional<SubmitUiDrawBucketsSessionObserver> session_;
    bool* externally_active_{};
    SubmitUiDrawBucketsSessionObservationSink sink_{};
    SubmitUiDrawBucketsSessionTerminationSink termination_sink_{};
    void* sink_context_{};
    SubmitUiDrawBucketsSessionFeedDiagnostics diagnostics_{};
    SubmitUiDrawBucketsExecutionContext owner_context_{};
    guest::GuestAddress caller_stack_pointer_{};
    guest::GuestAddress caller_return_address_{};
    guest::GuestAddress dynamic_callback_target_{};
    std::uint64_t last_event_ordinal_{};
    std::uint64_t current_invocation_ordinal_{};
    bool awaiting_sealed_return_{};
    bool awaiting_rejected_entry_departure_{};
    SubmitUiDrawBucketsFeedFailure invocation_failure_{SubmitUiDrawBucketsFeedFailure::None};
};

SubmitUiDrawBucketsSessionFeed::SubmitUiDrawBucketsSessionFeed(
    const SubmitUiDrawBucketsSessionObservationSink sink, void* const sink_context,
    const SubmitUiDrawBucketsSessionTerminationSink termination_sink, const bool enabled)
    : enabled_(enabled),
      impl_(std::make_unique<Impl>(active_, sink, sink_context, termination_sink)) {}

SubmitUiDrawBucketsSessionFeed::~SubmitUiDrawBucketsSessionFeed() = default;

// RECOVERY: matches the original except that GCC reloads observation.event_ordinal after the
// observed_events update and keeps the callback payload address as an unfolded storage reference.
void SubmitUiDrawBucketsSessionFeed::observe_pre_instruction(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    Impl* const impl = impl_.get();
    impl->observe(cpu, memory, context);
}

bool SubmitUiDrawBucketsSessionFeed::requires_active_pre_instruction(
    const SubmitUiDrawBucketsExecutionContext context,
    const guest::GuestAddress candidate_pc) const noexcept {
    const Impl* const impl = impl_.get();
    return impl->requires_boundary(context, candidate_pc);
}

std::span<const guest::GuestAddress> SubmitUiDrawBucketsSessionFeed::static_boundaries() noexcept {
    return ui_draw_guest_detail::submit_ui_draw_buckets_session_static_boundaries();
}

guest::GuestAddress SubmitUiDrawBucketsSessionFeed::learned_callback_target() const noexcept {
    const Impl* const impl = impl_.get();
    return impl->session_.has_value() ? impl->dynamic_callback_target_ : 0U;
}

guest::GuestAddress SubmitUiDrawBucketsSessionFeed::learned_caller_return() const noexcept {
    const Impl* const impl = impl_.get();
    return impl->session_.has_value() ? impl->caller_return_address_ : 0U;
}

std::uint32_t SubmitUiDrawBucketsSessionFeed::clip_compiled_budget(
    const guest::GuestAddress start_pc, const std::uint32_t maximum_instructions,
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    Impl* const impl = impl_.get();
    return impl->clip_budget(start_pc, maximum_instructions, context);
}

void SubmitUiDrawBucketsSessionFeed::retire_execution_context(
    const SubmitUiDrawBucketsExecutionContext context) noexcept {
    Impl* const impl = impl_.get();
    impl->retire_owned(context);
}

void SubmitUiDrawBucketsSessionFeed::retire_thread(const std::int32_t thread_id) noexcept {
    Impl* const impl = impl_.get();
    impl->retire_thread(thread_id);
}

void SubmitUiDrawBucketsSessionFeed::retire_all_execution_contexts() noexcept {
    Impl* const impl = impl_.get();
    impl->retire_all();
}

bool SubmitUiDrawBucketsSessionFeed::can_dispatch_active_native_function(
    const SubmitUiDrawBucketsExecutionContext context,
    const guest::GuestAddress entry_address) const noexcept {
    const Impl* const impl = impl_.get();
    return impl->can_dispatch_native(context, entry_address);
}

SubmitUiDrawBucketsSessionFeedDiagnostics SubmitUiDrawBucketsSessionFeed::diagnostics() const noexcept {
    const Impl* const impl = impl_.get();
    SubmitUiDrawBucketsSessionFeedDiagnostics result = impl->diagnostics_;
    result.code_profile_refreshes = impl->workspace_.code_profile_refresh_count();
    result.active = impl->session_.has_value();
    result.awaiting_sealed_return = impl->awaiting_sealed_return_;
    result.active_thread_id = result.active ? impl->owner_context_.thread_id : 0;
    return result;
}

} // namespace takedown::runtime::slus_21050
