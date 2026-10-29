#include "takedown/runtime/slus_21050_ui_draw_packet_session.hpp"

#include "takedown/runtime/slus_21050_stream_environment_guest.hpp"
#include "takedown/runtime/slus_21050_text_draw_guest.hpp"
#include "takedown/runtime/slus_21050_ui_draw_session_observer.hpp"
#include "takedown/runtime/slus_21050_ui_text_vif_transport.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace takedown::runtime::slus_21050 {

std::string_view to_string(const SubmitUiDrawBucketsPacketSessionFailure failure) noexcept {
    switch (failure) {
    case SubmitUiDrawBucketsPacketSessionFailure::None:
        return "none";
    case SubmitUiDrawBucketsPacketSessionFailure::InvalidObservation:
        return "invalid-observation";
    case SubmitUiDrawBucketsPacketSessionFailure::EventOrdinalMismatch:
        return "event-ordinal-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::InvalidFirstObservation:
        return "invalid-first-observation";
    case SubmitUiDrawBucketsPacketSessionFailure::PacketSessionUnavailable:
        return "packet-session-unavailable";
    case SubmitUiDrawBucketsPacketSessionFailure::EventSequenceMismatch:
        return "event-sequence-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::CallbackSourceMismatch:
        return "callback-source-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::PayloadProfileLifecycleMismatch:
        return "payload-profile-lifecycle-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::CallbackBeginRejected:
        return "callback-begin-rejected";
    case SubmitUiDrawBucketsPacketSessionFailure::CallbackEntryMismatch:
        return "callback-entry-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::CallbackReturnMismatch:
        return "callback-return-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::CompletionMismatch:
        return "completion-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::TerminalMismatch:
        return "terminal-mismatch";
    case SubmitUiDrawBucketsPacketSessionFailure::InternalFailure:
        return "internal-failure";
    }
    return "unknown";
}

std::string_view to_string(const SubmitUiDrawBucketsTextPayloadProfileAvailability availability) noexcept {
    switch (availability) {
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::NotApplicable:
        return "not-applicable";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::FreshlyAdmitted:
        return "freshly-admitted";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::ReusedFromCache:
        return "reused-from-cache";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::AvailableForInvocation:
        return "available-for-invocation";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::CodePageCaptureRejected:
        return "code-page-capture-rejected";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::FunctionFingerprintRejected:
        return "function-fingerprint-rejected";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::ChangedDuringAdmission:
        return "changed-during-admission";
    case SubmitUiDrawBucketsTextPayloadProfileAvailability::InvalidatedDuringInvocation:
        return "invalidated-during-invocation";
    }
    return "unknown";
}

namespace {

void saturating_increment(unsigned long long& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

std::optional<SubmitUiDrawBucketsTextPayloadProfileAvailability>
to_packet_session_availability(
    const ui_draw_guest_detail::UiTextCallbackPayloadCodeProfileAvailability availability) noexcept {
    using Source = ui_draw_guest_detail::UiTextCallbackPayloadCodeProfileAvailability;
    using Target = SubmitUiDrawBucketsTextPayloadProfileAvailability;
    switch (availability) {
    case Source::NotApplicable:
        return Target::NotApplicable;
    case Source::FreshlyAdmitted:
        return Target::FreshlyAdmitted;
    case Source::ReusedFromCache:
        return Target::ReusedFromCache;
    case Source::AvailableForInvocation:
        return Target::AvailableForInvocation;
    case Source::CodePageCaptureRejected:
        return Target::CodePageCaptureRejected;
    case Source::FunctionFingerprintRejected:
        return Target::FunctionFingerprintRejected;
    case Source::ChangedDuringAdmission:
        return Target::ChangedDuringAdmission;
    case Source::InvalidatedDuringInvocation:
        return Target::InvalidatedDuringInvocation;
    }
    return std::nullopt;
}

bool is_next_event_ordinal(const std::uint64_t last,
                           const ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation& observation) noexcept {
    if (last != std::numeric_limits<std::uint64_t>::max()) {
        const unsigned long long expected_ordinal = last + 1U;
        if (expected_ordinal != observation.event_ordinal) {
            return false;
        }
        return true;
    }
    return false;
}

// The first observation of an invocation is either the submit entry or a seal at the same boundary.
bool is_valid_first_observation(
    const ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation& observation) noexcept {
    using ui_draw_guest_detail::UiDrawSessionBoundaryKind;
    using ui_draw_guest_detail::UiDrawSessionObservationStatus;
    const bool first_event = observation.status == UiDrawSessionObservationStatus::Observed &&
                             observation.boundary == UiDrawSessionBoundaryKind::Boundary1 &&
                             observation.event.has_value() &&
                             std::holds_alternative<ui_draw_guest_detail::UiDrawSessionSubmitEntryObservation>(
                                 *observation.event);
    return (observation.status == UiDrawSessionObservationStatus::Sealed &&
            observation.boundary == UiDrawSessionBoundaryKind::Boundary1) ||
           first_event;
}

bool is_next_feed_termination(const std::uint64_t last_invocation,
                              const SubmitUiDrawBucketsSessionTermination& termination,
                              const std::uint64_t last_event) noexcept {
    if (last_invocation != std::numeric_limits<std::uint64_t>::max()) {
        const unsigned long long expected_invocation_ordinal = last_invocation + 1U;
        if (expected_invocation_ordinal != termination.invocation_ordinal) {
            return false;
        }
        if (termination.observed_events != last_event) {
            return false;
        }
        return true;
    }
    return false;
}

bool event_matches_boundary(const ui_draw_guest_detail::UiDrawSessionObservationEvent& event,
                            const ui_draw_guest_detail::UiDrawSessionBoundaryKind boundary) noexcept {
    using Boundary = ui_draw_guest_detail::UiDrawSessionBoundaryKind;
    switch (event.index()) {
    case 0:
        return boundary == Boundary::Boundary1;
    case 1:
        return boundary == Boundary::Boundary2;
    case 2:
        return boundary == Boundary::Boundary9;
    case 3:
        return boundary == Boundary::Boundary12;
    case 4:
        return boundary == Boundary::Boundary13;
    case 5:
        return boundary == Boundary::Boundary18;
    case 6:
        return boundary == Boundary::Boundary21;
    case 7:
        return boundary == Boundary::Boundary23;
    case 8:
        return boundary == Boundary::Boundary25;
    case 9:
        return boundary == Boundary::Boundary26;
    case 10:
        return boundary == Boundary::Boundary30;
    case 11:
        if (boundary == Boundary::Boundary34) {
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

bool is_unstarted_feed_termination(const SubmitUiDrawBucketsSessionTermination& termination,
                                   const unsigned long long expected_invocation_ordinal) noexcept {
    using Reason = SubmitUiDrawBucketsFeedTerminationReason;
    if (termination.observed_events != 0U || expected_invocation_ordinal == 0U ||
        termination.invocation_ordinal != expected_invocation_ordinal ||
        termination.failure != SubmitUiDrawBucketsFeedFailure::InternalObserverFailure) {
        return false;
    }
    return termination.reason == Reason::ObserverSealed || termination.reason == Reason::ExecutionContextRetired ||
           termination.reason == Reason::FeedDestroyed;
}

SubmitUiDrawBucketsFeedFailure failure_for(const ui_draw_guest_detail::UiDrawSessionSealReason reason) noexcept {
    using Reason = ui_draw_guest_detail::UiDrawSessionSealReason;
    using Failure = SubmitUiDrawBucketsFeedFailure;
    switch (reason) {
    case Reason::WorkspaceUnavailable:
        return Failure::WorkspaceUnavailable;
    case Reason::CodeProfileRejected:
        return Failure::FunctionProfileRejected;
    case Reason::EntryMismatch:
        return Failure::CpuStateRejected;
    case Reason::FrameOutOfRange:
        return Failure::GuestSourceRejected;
    case Reason::Reason5:
        return Failure::UnexpectedBoundary;
    case Reason::LoopProgressMismatch:
        return Failure::LoopProgressMismatch;
    case Reason::SetterRejected:
        return Failure::SetterOrderMismatch;
    case Reason::CallbackBoundaryRejected:
        return Failure::CallbackBoundaryRejected;
    case Reason::Reason9:
        return Failure::CallbackReturnMismatch;
    case Reason::TargetUnavailable:
        return Failure::TargetSourceRejected;
    case Reason::Reason11:
        return Failure::TargetMismatch;
    case Reason::CallbackLimitExceeded:
        return Failure::LimitExceeded;
    case Reason::None:
        return Failure::None;
    }
    return Failure::InternalObserverFailure;
}

// The failure a feed termination must carry for the seal reason (if any) that the feed reported.
bool termination_failure_matches(
    const std::optional<ui_draw_guest_detail::UiDrawSessionSealReason>& seal_reason,
    const SubmitUiDrawBucketsSessionTermination& termination) noexcept {
    using Reason = SubmitUiDrawBucketsFeedTerminationReason;
    using Failure = SubmitUiDrawBucketsFeedFailure;
    if (seal_reason.has_value()) {
        return (termination.reason == Reason::ObserverSealed || termination.reason == Reason::ExecutionContextRetired ||
                termination.reason == Reason::FeedDestroyed) &&
               termination.failure == failure_for(*seal_reason);
    }
    switch (termination.reason) {
    case Reason::Completed:
        return termination.failure == Failure::None;
    case Reason::ObserverSealed:
        return termination.failure == Failure::InternalObserverFailure;
    case Reason::ReturnedBeforeCompletion:
        return termination.failure == Failure::InvocationReturnedBeforeCompletion;
    case Reason::ExecutionContextRetired:
        return termination.failure == Failure::ExecutionContextRetired ||
               termination.failure == Failure::InternalObserverFailure;
    case Reason::FeedDestroyed:
        return termination.failure == Failure::FeedDestroyed ||
               termination.failure == Failure::InternalObserverFailure;
    }
    return false;
}

} // namespace

class SubmitUiDrawBucketsPacketSessionAdapter::Impl {
  public:
    struct PendingCallback {
        text_draw_guest_detail::UiTextCallbackBoundaryOccurrence source;
        guest::GuestAddress node{};
        guest::GuestAddress render_record{};
        std::optional<SubmitUiDrawBucketsTextCallbackIdentity> text{};
        std::optional<UiDrawCallbackEntryAbi> entry{};
        bool entered{};
        text_draw_guest_detail::UiTextCallbackEntryVisitMatchStatus match_status{2};
    };

    Impl() : owned_transport_(std::in_place), transport_(&*owned_transport_) {}
    explicit Impl(UiTextVifTransportClaimRegistry& transport) : transport_(&transport) {}

    // Plain-text transport hooks.
    [[nodiscard]] bool try_reserve_plain_transport(
        const SubmitPlainTextLinesTransportInvocation& invocation,
        const VifPacketProducerTicketReservationPass& packet,
        SubmitPlainTextLinesTransportClaimToken& token);
    [[nodiscard]] bool can_publish_plain_transport(SubmitPlainTextLinesTransportClaimToken token);
    [[nodiscard]] bool publish_plain_transport(const guest::EeMemory& memory,
                                               SubmitPlainTextLinesTransportClaimToken token,
                                               const StreamEnvironmentPacketGuestResult& result);
    void cancel_plain_transport(SubmitPlainTextLinesTransportClaimToken token);
    void fail(SubmitUiDrawBucketsPacketSessionFailure failure) noexcept;
    void record_failure(const SubmitUiDrawBucketsPacketSessionFailure failure) noexcept {
        saturating_increment(failed_invocations_);
        if (first_failure_ == SubmitUiDrawBucketsPacketSessionFailure::None) {
            first_failure_ = failure;
        }
    }
    void reset_invocation_state(const bool active) noexcept {
        active_ = active;
        packet_interval_open_ = false;
        submit_seen_ = false;
        retained_count_seen_ = false;
        begin_seen_ = false;
        end_seen_ = false;
        summary_seen_ = false;
        completed_observation_ = false;
        feed_seal_reason_.reset();
        processing_sealed_ = false;
        current_failure_ = SubmitUiDrawBucketsPacketSessionFailure::None;
        last_event_ordinal_ = 0U;
        current_source_ordinal_ = 0U;
        current_bucket_count_ = 0U;
        current_active_bucket_count_ = 0U;
        current_setter_count_ = 0U;
        current_callback_count_ = 0U;
        submit_owner_ = 0U;
        caller_stack_pointer_ = 0U;
        caller_return_address_ = 0U;
        frame_stack_pointer_ = 0U;
        bucket_array_ = 0U;
        current_bucket_ = 0U;
        current_bucket_flags_ = 0U;
        expected_callback_node_ = 0U;
        retained_bucket_count_ = 0U;
        next_callback_node_index_ = 0U;
        bucket_array_seen_ = false;
        awaiting_bucket_ = false;
        terminal_loop_seen_ = false;
        current_bucket_open_ = false;
        current_bucket_active_ = false;
        current_bucket_head_seen_ = false;
        active_target_.reset();
        pending_callback_.reset();
        callback_lease_.reset();
        pending_payload_profile_.reset();
    }
    void begin_invocation() noexcept {
        packet_session_.emplace();
        reset_invocation_state(true);
        saturating_increment(started_invocations_);
        if (!packet_session_->valid()) {
            fail(SubmitUiDrawBucketsPacketSessionFailure::PacketSessionUnavailable);
        }
    }
    void consume_event(const ui_draw_guest_detail::UiDrawSessionObservationEvent& variant, const bool completed) noexcept {
        using Failure = SubmitUiDrawBucketsPacketSessionFailure;
        Impl* const impl = this;
        switch (variant.index()) {
        case 0: {
            const auto& event = *std::get_if<0>(&variant);
            if (impl->submit_seen_ || impl->last_event_ordinal_ != 1U) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->submit_seen_ = true;
            impl->submit_owner_ = event.owner;
            impl->caller_stack_pointer_ = event.caller_stack_pointer;
            impl->caller_return_address_ = event.caller_return_address;
            break;
        }
        case 1: {
            const auto& event = *std::get_if<1>(&variant);
            if (!impl->submit_seen_ || impl->retained_count_seen_ || event.owner != impl->submit_owner_) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->retained_count_seen_ = true;
            impl->retained_bucket_count_ = event.retained_bucket_count;
            break;
        }
        case 2: {
            const auto& event = *std::get_if<2>(&variant);
            if (!impl->submit_seen_ || !impl->retained_count_seen_ || impl->begin_seen_ || impl->end_seen_ ||
                event.owner != impl->submit_owner_ ||
                event.retained_bucket_count != impl->retained_bucket_count_ ||
                impl->caller_stack_pointer_ < 304U ||
                event.frame_stack_pointer != impl->caller_stack_pointer_ - 304U) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->begin_seen_ = true;
            impl->packet_interval_open_ = true;
            impl->frame_stack_pointer_ = event.frame_stack_pointer;
            impl->active_target_ = event.target;
            break;
        }
        case 3: {
            const auto& event = *std::get_if<3>(&variant);
            const bool bucket_in_progress =
                impl->current_bucket_open_ && impl->current_bucket_active_ &&
                (!impl->current_bucket_head_seen_ || impl->expected_callback_node_ != 0U);
            if (!impl->packet_interval_open_ || impl->pending_callback_.has_value() || impl->awaiting_bucket_ ||
                impl->terminal_loop_seen_ || bucket_in_progress ||
                event.processed_bucket_count != impl->current_bucket_count_ ||
                event.processed_bucket_count > impl->retained_bucket_count_ ||
                event.remaining_bucket_count !=
                    impl->retained_bucket_count_ - static_cast<std::uint32_t>(event.processed_bucket_count) ||
                event.terminal != (event.processed_bucket_count == impl->retained_bucket_count_)) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            if (!impl->bucket_array_seen_) {
                impl->bucket_array_ = event.bucket_array;
                impl->bucket_array_seen_ = true;
            } else if (event.bucket_array != impl->bucket_array_) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->current_bucket_open_ = false;
            impl->current_bucket_active_ = false;
            impl->current_bucket_head_seen_ = false;
            impl->expected_callback_node_ = 0U;
            impl->next_callback_node_index_ = 0U;
            impl->awaiting_bucket_ = !event.terminal;
            impl->terminal_loop_seen_ = event.terminal;
            break;
        }
        case 4: {
            const auto& event = *std::get_if<4>(&variant);
            const unsigned long long expected_bucket =
                static_cast<unsigned long long>(impl->bucket_array_) + event.bucket_index * 16U;
            const bool expected_active = (event.flags & 1U) == 0U && event.first_node_head != 0U;
            if (!impl->packet_interval_open_ || !impl->awaiting_bucket_ || impl->terminal_loop_seen_ ||
                impl->pending_callback_.has_value() || !impl->bucket_array_seen_ ||
                impl->current_bucket_open_ || event.bucket_index != impl->current_bucket_count_ ||
                expected_bucket > std::numeric_limits<std::uint32_t>::max() ||
                event.bucket != static_cast<std::uint32_t>(expected_bucket) ||
                event.active != expected_active) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->awaiting_bucket_ = false;
            impl->current_bucket_open_ = true;
            impl->current_bucket_active_ = event.active;
            impl->current_bucket_head_seen_ = false;
            impl->current_bucket_ = event.bucket;
            impl->current_bucket_flags_ = event.flags;
            impl->expected_callback_node_ = 0U;
            impl->next_callback_node_index_ = 0U;
            saturating_increment(impl->current_bucket_count_);
            if (event.active) {
                saturating_increment(impl->current_active_bucket_count_);
            }
            break;
        }
        case 5: {
            const auto& event = *std::get_if<5>(&variant);
            if (!impl->packet_interval_open_ || impl->awaiting_bucket_ || impl->terminal_loop_seen_ ||
                impl->pending_callback_.has_value() || !impl->current_bucket_open_ ||
                !impl->current_bucket_active_ || impl->current_bucket_head_seen_ ||
                impl->current_bucket_count_ == 0U ||
                event.bucket_index != impl->current_bucket_count_ - 1U ||
                event.bucket != impl->current_bucket_) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            saturating_increment(impl->current_setter_count_);
            break;
        }
        case 6: {
            const auto& event = *std::get_if<6>(&variant);
            if (!impl->packet_interval_open_ || impl->awaiting_bucket_ || impl->terminal_loop_seen_ ||
                impl->pending_callback_.has_value() || !impl->current_bucket_open_ ||
                !impl->current_bucket_active_ || impl->current_bucket_head_seen_ ||
                impl->current_bucket_count_ == 0U ||
                event.bucket_index != impl->current_bucket_count_ - 1U ||
                event.bucket != impl->current_bucket_ || event.flags != impl->current_bucket_flags_) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->current_bucket_head_seen_ = true;
            impl->expected_callback_node_ = event.node_head;
            impl->next_callback_node_index_ = 0U;
            break;
        }
        case 7: {
            const auto& event = *std::get_if<7>(&variant);
            impl->consume_callback_call(event);
            break;
        }
        case 8: {
            const auto& event = *std::get_if<8>(&variant);
            if (!impl->pending_callback_.has_value() || impl->pending_callback_->entered ||
                !(event.occurrence == impl->pending_callback_->source) ||
                event.entry.callback_address != impl->pending_callback_->source.visit.callback_address ||
                event.entry.descriptor != impl->pending_callback_->source.visit.descriptor ||
                event.entry.render_record != impl->pending_callback_->render_record) {
                impl->fail(Failure::CallbackEntryMismatch);
                break;
            }
            const text_draw_guest_detail::UiTextCallbackEntryVisitMatchStatus expected_status =
                static_cast<text_draw_guest_detail::UiTextCallbackEntryVisitMatchStatus>(
                    impl->pending_callback_->text.has_value() ? 0U : 1U);
            if (event.match_status != expected_status ||
                impl->pending_callback_->text.has_value() != impl->callback_lease_.has_value()) {
                impl->fail(Failure::CallbackEntryMismatch);
                break;
            }
            impl->pending_callback_->entry = event.entry;
            impl->pending_callback_->match_status = event.match_status;
            impl->pending_callback_->entered = true;
            if (impl->pending_callback_->text.has_value()) {
                impl->last_text_callback_ = impl->pending_callback_->text;
                saturating_increment(impl->text_callback_entries_);
                if (impl->pending_callback_->text->effect != UiTextEffectKind::Plain) {
                    impl->callback_lease_.reset();
                }
            } else {
                saturating_increment(impl->compatibility_callback_entries_);
            }
            break;
        }
        case 9: {
            const auto& event = *std::get_if<9>(&variant);
            impl->callback_lease_.reset();
            if (!impl->pending_callback_.has_value() || !impl->pending_callback_->entered ||
                !impl->pending_callback_->entry.has_value() ||
                !(event.occurrence == impl->pending_callback_->source) ||
                !(event.entry == *impl->pending_callback_->entry) ||
                event.match_status != impl->pending_callback_->match_status) {
                impl->fail(Failure::CallbackReturnMismatch);
                break;
            }
            if (impl->pending_callback_->text.has_value()) {
                saturating_increment(impl->text_callback_returns_);
            }
            impl->expected_callback_node_ = event.next_node;
            saturating_increment(impl->next_callback_node_index_);
            impl->pending_callback_.reset();
            break;
        }
        case 10: {
            const auto& event = *std::get_if<10>(&variant);
            const unsigned long long expected_final_bucket =
                static_cast<unsigned long long>(impl->bucket_array_) +
                static_cast<unsigned long long>(impl->retained_bucket_count_) * 16U;
            if (!impl->packet_interval_open_ || !impl->begin_seen_ || impl->end_seen_ ||
                !impl->bucket_array_seen_ || !impl->terminal_loop_seen_ || impl->awaiting_bucket_ ||
                impl->pending_callback_.has_value() || !impl->active_target_.has_value() ||
                expected_final_bucket > std::numeric_limits<std::uint32_t>::max() ||
                event.final_bucket != static_cast<std::uint32_t>(expected_final_bucket) ||
                event.terminal_count != std::numeric_limits<std::uint32_t>::max() ||
                event.target.target != impl->active_target_->target ||
                !(event.target.view == impl->active_target_->view)) {
                impl->fail(Failure::EventSequenceMismatch);
                break;
            }
            impl->end_seen_ = true;
            impl->packet_interval_open_ = false;
            break;
        }
        case 11: {
            const auto& event = *std::get_if<11>(&variant);
            if (!completed || !impl->end_seen_ || impl->summary_seen_ || !impl->bucket_array_seen_ ||
                !impl->terminal_loop_seen_ || impl->awaiting_bucket_ || impl->pending_callback_.has_value() ||
                !impl->active_target_.has_value() || event.owner != impl->submit_owner_ ||
                event.caller_stack_pointer != impl->caller_stack_pointer_ ||
                event.caller_return_address != impl->caller_return_address_ ||
                event.retained_bucket_count != impl->retained_bucket_count_ ||
                event.bucket_array != impl->bucket_array_ ||
                event.observed_bucket_count != impl->current_bucket_count_ ||
                event.active_bucket_count != impl->current_active_bucket_count_ ||
                event.setter_count != impl->current_setter_count_ ||
                event.callback_count != impl->current_callback_count_ ||
                !(event.target == *impl->active_target_)) {
                impl->fail(Failure::CompletionMismatch);
                break;
            }
            impl->summary_seen_ = true;
            break;
        }
        default:
            break;
        }
    }
    void seal_session(const ui_draw_guest_detail::UiDrawSessionSealReason reason) noexcept {
        feed_seal_reason_ = reason;
        processing_sealed_ = true;
        callback_lease_.reset();
        pending_callback_.reset();
    }
    void consume_callback_call(const ui_draw_guest_detail::UiDrawSessionCallbackCallObservation& call);
    [[nodiscard]] bool accept_payload_profile(
        const bool has_effect,
        const SubmitUiDrawBucketsTextPayloadProfileAvailability profile) noexcept {
        using Availability = SubmitUiDrawBucketsTextPayloadProfileAvailability;
        if (!has_effect) {
            return profile == Availability::NotApplicable;
        }
        if (profile == Availability::NotApplicable) {
            return false;
        }
        if (!pending_payload_profile_.has_value()) {
            switch (profile) {
            case Availability::FreshlyAdmitted:
            case Availability::ReusedFromCache:
            case Availability::CodePageCaptureRejected:
            case Availability::FunctionFingerprintRejected:
            case Availability::ChangedDuringAdmission:
                pending_payload_profile_ = profile;
                return true;
            case Availability::NotApplicable:
            case Availability::AvailableForInvocation:
            case Availability::InvalidatedDuringInvocation:
                return false;
            }
            return false;
        }
        switch (*pending_payload_profile_) {
        case Availability::FreshlyAdmitted:
        case Availability::ReusedFromCache:
        case Availability::AvailableForInvocation:
            if (profile == Availability::AvailableForInvocation ||
                profile == Availability::InvalidatedDuringInvocation) {
                pending_payload_profile_ = profile;
                return true;
            }
            return false;
        case Availability::CodePageCaptureRejected:
        case Availability::FunctionFingerprintRejected:
        case Availability::ChangedDuringAdmission:
        case Availability::InvalidatedDuringInvocation:
            return profile == *pending_payload_profile_;
        default:
            return false;
        }
    }


    void expire_unclaimed_plain_text_entry(const guest::GuestAddress entry_address) noexcept {
        if (entry_address != 0x002f9140U || !pending_callback_.has_value() || !pending_callback_->entered ||
            !pending_callback_->entry.has_value() ||
            pending_callback_->entry->callback_address != 0x002f9140U ||
            !pending_callback_->text.has_value() ||
            pending_callback_->text->effect != UiTextEffectKind::Plain) {
            return;
        }
        callback_lease_.reset();
    }

    std::optional<UiTextVifTransportClaimRegistry> owned_transport_;
    UiTextVifTransportClaimRegistry* transport_{};
    std::optional<text_draw_guest_detail::UiTextPacketSession> packet_session_;
    std::optional<text_draw_guest_detail::UiTextCallbackLease> callback_lease_;
    std::optional<PendingCallback> pending_callback_;
    std::optional<ActiveUiRenderTargetGuestBinding> active_target_;
    std::optional<SubmitUiDrawBucketsTextCallbackIdentity> last_text_callback_;
    std::optional<SubmitUiDrawBucketsSessionTermination> last_termination_;
    std::optional<SubmitUiDrawBucketsTextPayloadProfileAvailability> pending_payload_profile_;
    std::uint64_t started_invocations_{};
    std::uint64_t terminated_invocations_{};
    std::uint64_t completed_invocations_{};
    std::uint64_t abandoned_invocations_{};
    std::uint64_t failed_invocations_{};
    std::uint64_t callback_calls_{};
    std::uint64_t text_callback_entries_{};
    std::uint64_t text_callback_returns_{};
    std::uint64_t compatibility_callback_entries_{};
    std::uint64_t payload_profile_available_callbacks_{};
    std::uint64_t payload_profile_unavailable_callbacks_{};
    std::uint64_t plain_transport_reservation_attempts_{};
    std::uint64_t plain_transport_reservations_{};
    std::uint64_t plain_transport_reservation_rejections_{};
    std::uint64_t plain_transport_publications_{};
    std::uint64_t plain_transport_publication_rejections_{};
    std::uint64_t plain_transport_cancellations_{};
    std::uint64_t plain_transport_reported_writes_{};
    std::uint64_t terminal_session_releases_{};
    std::uint64_t last_event_ordinal_{};
    std::uint64_t last_feed_invocation_ordinal_{};
    std::uint64_t current_source_ordinal_{};
    std::uint64_t current_bucket_count_{};
    std::uint64_t current_active_bucket_count_{};
    std::uint64_t current_setter_count_{};
    std::uint64_t current_callback_count_{};
    guest::GuestAddress submit_owner_{};
    guest::GuestAddress caller_stack_pointer_{};
    guest::GuestAddress caller_return_address_{};
    guest::GuestAddress frame_stack_pointer_{};
    guest::GuestAddress bucket_array_{};
    guest::GuestAddress current_bucket_{};
    std::uint32_t current_bucket_flags_{};
    guest::GuestAddress expected_callback_node_{};
    std::uint32_t retained_bucket_count_{};
    std::uint64_t next_callback_node_index_{};
    SubmitUiDrawBucketsPacketSessionFailure first_failure_{SubmitUiDrawBucketsPacketSessionFailure::None};
    SubmitUiDrawBucketsPacketSessionFailure current_failure_{
        SubmitUiDrawBucketsPacketSessionFailure::None};
    bool active_{};
    bool packet_interval_open_{};
    bool submit_seen_{};
    bool retained_count_seen_{};
    bool bucket_array_seen_{};
    bool awaiting_bucket_{};
    bool terminal_loop_seen_{};
    bool current_bucket_open_{};
    bool current_bucket_active_{};
    bool current_bucket_head_seen_{};
    bool begin_seen_{};
    bool end_seen_{};
    bool summary_seen_{};
    bool completed_observation_{};
    std::optional<ui_draw_guest_detail::UiDrawSessionSealReason> feed_seal_reason_;
    bool processing_sealed_{};
};

// RECOVERY: matches the original except that GCC zero-fills the whole pass occurrence in one store (the
// original zero-initialised it as a unit) instead of storing line_slot/pass_slot/role separately.
bool SubmitUiDrawBucketsPacketSessionAdapter::Impl::try_reserve_plain_transport(
    const SubmitPlainTextLinesTransportInvocation& invocation,
    const VifPacketProducerTicketReservationPass& packet,
    SubmitPlainTextLinesTransportClaimToken& token) {
    token = {};
    saturating_increment(plain_transport_reservation_attempts_);
    if (!active_ || processing_sealed_ || !packet_interval_open_ || !pending_callback_.has_value() ||
        !pending_callback_->entered || !pending_callback_->entry.has_value() ||
        !pending_callback_->text.has_value() || !callback_lease_.has_value() || !callback_lease_->active() ||
        pending_callback_->match_status != 0U || pending_callback_->text->effect != UiTextEffectKind::Plain ||
        invocation.entry_address != 0x002f9140U ||
        pending_callback_->entry->callback_address != 0x002f9140U ||
        invocation.render_record != pending_callback_->entry->render_record ||
        invocation.descriptor != pending_callback_->entry->descriptor) {
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    if (!make_stream_environment_packet_ticket_reservation_request(
             VifPacketProducerTicketOwner{.opaque_value = 1U}, packet)
             .has_value()) {
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    const text_draw_guest_detail::UiTextCallbackOccurrence offered = callback_lease_->callback();
    const SubmitUiDrawBucketsTextCallbackIdentity& text = *pending_callback_->text;
    if (offered.packet_session_epoch.value != text.packet_session_epoch ||
        offered.callback_ordinal.value != text.callback_ordinal || offered.key != text.key ||
        offered.bucket_index != text.bucket_index || offered.node_index != text.node_index ||
        offered.effect != text.effect) {
        callback_lease_.reset();
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    if (!is_submit_ui_draw_buckets_text_payload_profile_available(text.payload_profile)) {
        callback_lease_.reset();
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    const text_draw_guest_detail::UiTextSinglePassOwnerIssueResult owner_issue =
        callback_lease_->issue_single_owner();
    callback_lease_.reset();
    if (owner_issue.status != 0U || !owner_issue.callback.has_value() ||
        owner_issue.owner.opaque_value == 0U || !(*owner_issue.callback == offered)) {
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    const auto packet_request =
        make_stream_environment_packet_ticket_reservation_request(owner_issue.owner, packet);
    if (!packet_request.has_value()) {
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    const std::array<UiTextVifTransportPassClaimRequest, 1> requests{UiTextVifTransportPassClaimRequest{
        .pass = {.callback = {.packet_session_epoch = text.packet_session_epoch,
                              .callback_ordinal = text.callback_ordinal,
                              .source_ordinal = text.source_ordinal,
                              .key = text.key,
                              .bucket_index = text.bucket_index,
                              .node_index = text.node_index,
                              .effect = text.effect}},
        .packet = *packet_request}};
    std::array<UiTextVifTransportClaimId, 1> claim_ids{};
    const UiTextVifTransportReserveResult reserved = transport_->try_reserve(requests, claim_ids);
    if (reserved.status != UiTextVifTransportReserveStatus::Reserved) {
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    if (reserved.reservation.pass_count != 1U || reserved.reservation.value == 0U ||
        claim_ids[0].value == 0U) {
        static_cast<void>(transport_->cancel_callback(reserved.reservation));
        saturating_increment(plain_transport_reservation_rejections_);
        return false;
    }
    token = SubmitPlainTextLinesTransportClaimToken{reserved.reservation.value, claim_ids[0].value, 1U};
    saturating_increment(plain_transport_reservations_);
    return true;
}

bool SubmitUiDrawBucketsPacketSessionAdapter::Impl::can_publish_plain_transport(const SubmitPlainTextLinesTransportClaimToken token) {
    const std::optional<UiTextVifTransportClaim> claim =
        token.valid() ? transport_->find(UiTextVifTransportClaimId{token.claim_value})
                      : std::optional<UiTextVifTransportClaim>{};
    if (!claim.has_value() || claim->reservation.value != token.reservation_value ||
        claim->reservation.pass_count != token.pass_count ||
        claim->state != UiTextVifTransportClaimState::Reserved) {
        saturating_increment(plain_transport_publication_rejections_);
        return false;
    }
    const UiTextVifTransportPreflightResult preflight =
        transport_->can_publish(UiTextVifTransportClaimId{token.claim_value});
    if (preflight.status != UiTextVifTransportPreflightStatus::Ready) {
        saturating_increment(plain_transport_publication_rejections_);
        return false;
    }
    return true;
}

bool SubmitUiDrawBucketsPacketSessionAdapter::Impl::publish_plain_transport(const guest::EeMemory& memory,
                                           const SubmitPlainTextLinesTransportClaimToken token,
                                           const StreamEnvironmentPacketGuestResult& result) {
    const std::optional<UiTextVifTransportClaim> claim =
        token.valid() ? transport_->find(UiTextVifTransportClaimId{token.claim_value})
                      : std::optional<UiTextVifTransportClaim>{};
    if (!claim.has_value() || claim->reservation.value != token.reservation_value ||
        claim->reservation.pass_count != token.pass_count ||
        claim->state != UiTextVifTransportClaimState::Reserved) {
        saturating_increment(plain_transport_publication_rejections_);
        return false;
    }
    const UiTextVifTransportPublishResult published =
        transport_->publish_reserved_after_stream_commit(
            memory, UiTextVifTransportClaimId{token.claim_value}, result);
    if (published.status != UiTextVifTransportPublishStatus::Published) {
        saturating_increment(plain_transport_publication_rejections_);
        return false;
    }
    saturating_increment(plain_transport_publications_);
    return true;
}

void SubmitUiDrawBucketsPacketSessionAdapter::Impl::cancel_plain_transport(const SubmitPlainTextLinesTransportClaimToken token) {
    const std::optional<UiTextVifTransportClaim> claim =
        token.valid() ? transport_->find(UiTextVifTransportClaimId{token.claim_value})
                      : std::optional<UiTextVifTransportClaim>{};
    if (!claim.has_value() || claim->reservation.value != token.reservation_value ||
        claim->reservation.pass_count != token.pass_count ||
        claim->state != UiTextVifTransportClaimState::Reserved) {
        return;
    }
    const UiTextVifTransportCancelResult cancelled = transport_->cancel_callback(
        UiTextVifTransportCallbackReservation{token.reservation_value, token.pass_count});
    if (cancelled.status == UiTextVifTransportCancelStatus::Cancelled) {
        saturating_increment(plain_transport_cancellations_);
    }
}

void SubmitUiDrawBucketsPacketSessionAdapter::Impl::fail(const SubmitUiDrawBucketsPacketSessionFailure failure) noexcept {
    if (current_failure_ == SubmitUiDrawBucketsPacketSessionFailure::None) {
        current_failure_ = failure;
        record_failure(failure);
    }
    processing_sealed_ = true;
    callback_lease_.reset();
    pending_callback_.reset();
}

// RECOVERY: only the branch probability of the begin_callback status test differs (50/50 in the original).
void SubmitUiDrawBucketsPacketSessionAdapter::Impl::consume_callback_call(
    const ui_draw_guest_detail::UiDrawSessionCallbackCallObservation& call) {
    const auto& occurrence = call.occurrence;
    const auto& visit = occurrence.visit;
    const unsigned long long expected_descriptor = static_cast<unsigned long long>(call.node) + 40U;
    const unsigned long long expected_render_record =
        static_cast<unsigned long long>(frame_stack_pointer_) + 176U;
    const unsigned long long expected_source =
        current_source_ordinal_ == std::numeric_limits<std::uint64_t>::max() ? 0U
                                                                              : current_source_ordinal_ + 1U;
    const std::optional<SubmitUiDrawBucketsTextPayloadProfileAvailability> payload_profile =
        to_packet_session_availability(call.payload_code_profile);
    if (!packet_interval_open_ || awaiting_bucket_ || terminal_loop_seen_ || pending_callback_.has_value() ||
        callback_lease_.has_value() || !current_bucket_open_ || !current_bucket_active_ ||
        !current_bucket_head_seen_ || expected_callback_node_ == 0U || call.node != expected_callback_node_ ||
        current_bucket_count_ == 0U || visit.bucket_index != current_bucket_count_ - 1U ||
        visit.node_index != next_callback_node_index_ || (call.node & 15U) != 8U ||
        expected_descriptor > std::numeric_limits<std::uint32_t>::max() ||
        visit.descriptor != call.node + 40U ||
        expected_render_record > std::numeric_limits<std::uint32_t>::max() ||
        call.render_record != frame_stack_pointer_ + 176U || expected_source == 0U ||
        occurrence.source_ordinal != expected_source ||
        visit.key.value != static_cast<std::uint64_t>(visit.descriptor) ||
        visit.text_effect != decode_ui_text_effect_callback(visit.callback_address)) {
        fail(SubmitUiDrawBucketsPacketSessionFailure::CallbackSourceMismatch);
        return;
    }
    if (!payload_profile.has_value() || !accept_payload_profile(visit.text_effect.has_value(), *payload_profile)) {
        fail(SubmitUiDrawBucketsPacketSessionFailure::PayloadProfileLifecycleMismatch);
        return;
    }
    PendingCallback pending;
    pending.source = occurrence;
    pending.node = call.node;
    pending.render_record = call.render_record;
    if (visit.text_effect.has_value()) {
        text_draw_guest_detail::UiTextCallbackBeginResult issued =
            packet_session_->begin_callback({.key = visit.key,
                                             .bucket_index = visit.bucket_index,
                                             .node_index = visit.node_index,
                                             .effect = *visit.text_effect});
        if (issued.status == 0U && issued.lease.has_value()) {
            const text_draw_guest_detail::UiTextCallbackOccurrence& granted = issued.lease->callback();
            pending.text = SubmitUiDrawBucketsTextCallbackIdentity{
                .packet_session_epoch = granted.packet_session_epoch.value,
                .callback_ordinal = granted.callback_ordinal.value,
                .source_ordinal = occurrence.source_ordinal,
                .key = granted.key,
                .bucket_index = granted.bucket_index,
                .node_index = granted.node_index,
                .effect = granted.effect,
                .payload_profile = *payload_profile};
            if (is_submit_ui_draw_buckets_text_payload_profile_available(*payload_profile)) {
                saturating_increment(payload_profile_available_callbacks_);
            } else {
                saturating_increment(payload_profile_unavailable_callbacks_);
            }
            callback_lease_.emplace(std::move(*issued.lease));
        } else {
            fail(SubmitUiDrawBucketsPacketSessionFailure::CallbackBeginRejected);
            return;
        }
    }
    pending_callback_.emplace(pending);
    current_source_ordinal_ = occurrence.source_ordinal;
    saturating_increment(current_callback_count_);
    saturating_increment(callback_calls_);
}

// RECOVERY: matches the original except for one forwarding block after the sealed-at-start early return.
void SubmitUiDrawBucketsPacketSessionAdapter::observation_sink(
    void* const context, const ui_draw_guest_detail::SubmitUiDrawBucketsSessionObservation& observation) noexcept {
    using Failure = SubmitUiDrawBucketsPacketSessionFailure;
    using ui_draw_guest_detail::UiDrawSessionBoundaryKind;
    using ui_draw_guest_detail::UiDrawSessionObservationStatus;
    using ui_draw_guest_detail::UiDrawSessionSealReason;
    if (context == nullptr) {
        return;
    }
    Impl* const impl = static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_.get();
    if (observation.status == UiDrawSessionObservationStatus::Unset) {
        if (!impl->active_) {
            impl->begin_invocation();
        }
        impl->fail(Failure::InvalidObservation);
        return;
    }
    if (!impl->active_) {
        impl->begin_invocation();
        if (observation.event_ordinal == 0U && observation.status == UiDrawSessionObservationStatus::Sealed &&
            observation.boundary == UiDrawSessionBoundaryKind::Boundary0 &&
            observation.seal_reason == UiDrawSessionSealReason::WorkspaceUnavailable &&
            !observation.event.has_value()) {
            impl->seal_session(UiDrawSessionSealReason::WorkspaceUnavailable);
            return;
        }
    }
    if (!is_next_event_ordinal(impl->last_event_ordinal_, observation)) {
        impl->fail(Failure::EventOrdinalMismatch);
        impl->last_event_ordinal_ = observation.event_ordinal;
        return;
    }
    impl->last_event_ordinal_ = observation.event_ordinal;
    if (impl->processing_sealed_) {
        impl->fail(Failure::EventSequenceMismatch);
        return;
    }
    const bool completed = observation.status == UiDrawSessionObservationStatus::Completed;
    if ((observation.status != UiDrawSessionObservationStatus::Sealed &&
         observation.status != UiDrawSessionObservationStatus::Observed &&
          observation.status != UiDrawSessionObservationStatus::Completed) ||
        (observation.status == UiDrawSessionObservationStatus::Sealed
             ? observation.seal_reason == UiDrawSessionSealReason::None || observation.event.has_value()
             : observation.seal_reason != UiDrawSessionSealReason::None)) {
        impl->fail(Failure::InvalidObservation);
        return;
    }
    if (observation.event_ordinal == 1U && !is_valid_first_observation(observation)) {
        impl->fail(Failure::InvalidFirstObservation);
        return;
    }
    if (observation.status == UiDrawSessionObservationStatus::Sealed) {
        impl->feed_seal_reason_ = observation.seal_reason;
        impl->processing_sealed_ = true;
        impl->callback_lease_.reset();
        impl->pending_callback_.reset();
        return;
    }
    if (observation.event.has_value()) {
        if (!event_matches_boundary(*observation.event, observation.boundary)) {
            impl->fail(Failure::EventSequenceMismatch);
            return;
        }
        impl->consume_event(*observation.event, completed);
    }
    if (impl->processing_sealed_) {
        return;
    }
    if (completed) {
        if (impl->summary_seen_ && impl->end_seen_ && !impl->packet_interval_open_ && !impl->pending_callback_.has_value()) {
            impl->completed_observation_ = true;
        } else {
            impl->fail(Failure::CompletionMismatch);
        }
    }
}

// RECOVERY: matches the original except for the entry-null branch weight and one forwarding block after the
// unmatched-termination early return.
void SubmitUiDrawBucketsPacketSessionAdapter::termination_sink(
    void* const context, const SubmitUiDrawBucketsSessionTermination& termination) noexcept {
    using Failure = SubmitUiDrawBucketsPacketSessionFailure;
    using FeedFailure = SubmitUiDrawBucketsFeedFailure;
    using Reason = SubmitUiDrawBucketsFeedTerminationReason;
    if (context == nullptr) {
        return;
    }
    Impl* const impl = static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_.get();
    if (!impl->active_) {
        const unsigned long long expected_invocation_ordinal =
            impl->last_feed_invocation_ordinal_ == std::numeric_limits<std::uint64_t>::max()
                ? 0U
                : impl->last_feed_invocation_ordinal_ + 1U;
        if (termination.observed_events == 0U && expected_invocation_ordinal != 0U &&
            termination.invocation_ordinal == expected_invocation_ordinal &&
            termination.failure == FeedFailure::InternalObserverFailure &&
            (termination.reason == Reason::ObserverSealed || termination.reason == Reason::ExecutionContextRetired ||
             termination.reason == Reason::FeedDestroyed)) {
        } else {
            impl->record_failure(Failure::TerminalMismatch);
            return;
        }
        impl->begin_invocation();
    }
    if (!is_next_feed_termination(impl->last_feed_invocation_ordinal_, termination, impl->last_event_ordinal_)) {
        impl->fail(Failure::TerminalMismatch);
    }
    const std::optional<ui_draw_guest_detail::UiDrawSessionSealReason> seal_reason = impl->feed_seal_reason_;
    if (!termination_failure_matches(seal_reason, termination)) {
        impl->fail(Failure::TerminalMismatch);
    }
    if (termination.reason == Reason::Completed) {
        if (!impl->completed_observation_ || !impl->summary_seen_ || !impl->end_seen_ || impl->packet_interval_open_ ||
            impl->pending_callback_.has_value() || termination.failure != FeedFailure::None) {
            impl->fail(Failure::TerminalMismatch);
        }
    } else if (impl->completed_observation_) {
        impl->fail(Failure::TerminalMismatch);
    }
    impl->last_termination_ = termination;
    impl->last_feed_invocation_ordinal_ = termination.invocation_ordinal > impl->last_feed_invocation_ordinal_
                                             ? termination.invocation_ordinal
                                             : impl->last_feed_invocation_ordinal_;
    impl->callback_lease_.reset();
    impl->pending_callback_.reset();
    const bool completed = termination.reason == Reason::Completed && impl->current_failure_ == Failure::None;
    impl->packet_session_.reset();
    saturating_increment(impl->terminal_session_releases_);
    saturating_increment(impl->terminated_invocations_);
    if (completed) {
        saturating_increment(impl->completed_invocations_);
    } else {
        saturating_increment(impl->abandoned_invocations_);
    }
    impl->reset_invocation_state(false);

}

SubmitUiDrawBucketsPacketSessionAdapter::SubmitUiDrawBucketsPacketSessionAdapter()
    : impl_(std::make_unique<Impl>()) {}

SubmitUiDrawBucketsPacketSessionAdapter::SubmitUiDrawBucketsPacketSessionAdapter(
    UiTextVifTransportClaimRegistry& transport)
    : impl_(std::make_unique<Impl>(transport)) {}

SubmitUiDrawBucketsPacketSessionAdapter::~SubmitUiDrawBucketsPacketSessionAdapter() = default;

SubmitUiDrawBucketsPacketSessionFeedBinding
SubmitUiDrawBucketsPacketSessionAdapter::feed_binding() noexcept {
    return {&observation_sink, this, &termination_sink};
}

SubmitPlainTextLinesTransportClaimBinding
SubmitUiDrawBucketsPacketSessionAdapter::plain_text_transport_claim_binding() noexcept {
    return {this, &plain_transport_try_reserve, &plain_transport_can_publish,
            &plain_transport_publish, &plain_transport_cancel, &plain_transport_report_write};
}

bool SubmitUiDrawBucketsPacketSessionAdapter::plain_transport_try_reserve(
    void* const context, const SubmitPlainTextLinesTransportInvocation& invocation,
    const VifPacketProducerTicketReservationPass& packet,
    SubmitPlainTextLinesTransportClaimToken& token) noexcept {
    if (context == nullptr) {
        token = {};
        return false;
    }
    return static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_->try_reserve_plain_transport(
        invocation, packet, token);
}

bool SubmitUiDrawBucketsPacketSessionAdapter::plain_transport_can_publish(
    void* const context, const SubmitPlainTextLinesTransportClaimToken token) noexcept {
    if (context == nullptr) {
        return false;
    }
    return static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_->can_publish_plain_transport(
        token);
}

bool SubmitUiDrawBucketsPacketSessionAdapter::plain_transport_publish(
    void* const context, const guest::EeMemory& memory,
    const SubmitPlainTextLinesTransportClaimToken token,
    const StreamEnvironmentPacketGuestResult& result) noexcept {
    if (context == nullptr) {
        return false;
    }
    return static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_->publish_plain_transport(
        memory, token, result);
}

void SubmitUiDrawBucketsPacketSessionAdapter::plain_transport_cancel(
    void* const context, const SubmitPlainTextLinesTransportClaimToken token) noexcept {
    if (context == nullptr) {
        return;
    }
    static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_->cancel_plain_transport(token);
}

void SubmitUiDrawBucketsPacketSessionAdapter::plain_transport_report_write(
    void* const context, const guest::GuestAddress begin, const std::size_t byte_count) noexcept {
    if (context == nullptr) {
        return;
    }
    Impl& impl = *static_cast<SubmitUiDrawBucketsPacketSessionAdapter*>(context)->impl_;
    if (byte_count == 0U) {
        return;
    }
    static_cast<void>(impl.transport_->report_write(begin, byte_count));
    saturating_increment(impl.plain_transport_reported_writes_);
}

void SubmitUiDrawBucketsPacketSessionAdapter::expire_unclaimed_plain_text_entry(
    const guest::GuestAddress entry_address) noexcept {
    impl_->expire_unclaimed_plain_text_entry(entry_address);
}

SubmitUiDrawBucketsPacketSessionDiagnostics
SubmitUiDrawBucketsPacketSessionAdapter::diagnostics() const noexcept {
    const Impl* const impl = impl_.get();
    const UiTextVifTransportDiagnostics transport = impl->transport_->diagnostics();
    return {
        .started_invocations = impl->started_invocations_,
        .terminated_invocations = impl->terminated_invocations_,
        .completed_invocations = impl->completed_invocations_,
        .abandoned_invocations = impl->abandoned_invocations_,
        .failed_invocations = impl->failed_invocations_,
        .callback_calls = impl->callback_calls_,
        .text_callback_entries = impl->text_callback_entries_,
        .text_callback_returns = impl->text_callback_returns_,
        .compatibility_callback_entries = impl->compatibility_callback_entries_,
        .payload_profile_available_callbacks = impl->payload_profile_available_callbacks_,
        .payload_profile_unavailable_callbacks = impl->payload_profile_unavailable_callbacks_,
        .plain_transport_reservation_attempts = impl->plain_transport_reservation_attempts_,
        .plain_transport_reservations = impl->plain_transport_reservations_,
        .plain_transport_reservation_rejections = impl->plain_transport_reservation_rejections_,
        .plain_transport_publications = impl->plain_transport_publications_,
        .plain_transport_publication_rejections = impl->plain_transport_publication_rejections_,
        .plain_transport_cancellations = impl->plain_transport_cancellations_,
        .plain_transport_reported_writes = impl->plain_transport_reported_writes_,
        .terminal_session_releases = impl->terminal_session_releases_,
        .active_event_ordinal = impl->active_ ? impl->last_event_ordinal_ : 0U,
        .active_packet_session_epoch =
            impl->active_ && impl->packet_session_.has_value() ? impl->packet_session_->epoch().value : 0U,
        .shared_transport_claims = transport.occupied_claims,
        .shared_transport_published_claims = transport.published_claims,
        .shared_transport_ledger_tickets = transport.ledger_active_tickets,
        .shared_transport_ledger_reservations = transport.ledger_reserved_tickets,
        .first_failure = impl->first_failure_,
        .last_text_callback = impl->last_text_callback_,
        .last_termination = impl->last_termination_,
        .active = impl->active_,
        .packet_interval_open = impl->active_ && impl->packet_interval_open_,
        .callback_pending = impl->active_ && impl->pending_callback_.has_value(),
        .callback_lease_open = impl->active_ && impl->callback_lease_.has_value(),
        .sealed = impl->active_ && impl->processing_sealed_,
    };
}

} // namespace takedown::runtime::slus_21050
