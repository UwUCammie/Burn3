#include "takedown/runtime/slus_21050_femain_frame_closure_diagnostic.hpp"

namespace takedown::runtime {

namespace {

[[nodiscard]] constexpr bool not_reordered(const std::uint64_t before,
                                           const std::uint64_t after) noexcept {
    return after >= before;
}

[[nodiscard]] std::uint32_t return_address_register(const guest::R5900State& state) {
    const auto value = state.read_gpr(31U);
    return value.words[0];
}

} // namespace

std::string_view to_string(const Slus21050FemainFrameClosurePhase phase) noexcept {
    switch (phase) {
    case Slus21050FemainFrameClosurePhase::Disabled:
        return "disabled";
    case Slus21050FemainFrameClosurePhase::Idle:
        return "idle";
    case Slus21050FemainFrameClosurePhase::FirstDelay:
        return "first-delay";
    case Slus21050FemainFrameClosurePhase::FirstCallee:
        return "first-callee";
    case Slus21050FemainFrameClosurePhase::FirstBody:
        return "first-body";
    case Slus21050FemainFrameClosurePhase::FirstReturnDelay:
        return "first-return-delay";
    case Slus21050FemainFrameClosurePhase::FirstResume:
        return "first-resume";
    case Slus21050FemainFrameClosurePhase::SecondDelay:
        return "second-delay";
    case Slus21050FemainFrameClosurePhase::SecondCallee:
        return "second-callee";
    case Slus21050FemainFrameClosurePhase::SecondBody:
        return "second-body";
    case Slus21050FemainFrameClosurePhase::SecondReturnDelay:
        return "second-return-delay";
    case Slus21050FemainFrameClosurePhase::SecondResume:
        return "second-resume";
    case Slus21050FemainFrameClosurePhase::Finished:
        return "finished";
    case Slus21050FemainFrameClosurePhase::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(const Slus21050FemainFrameClosureFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainFrameClosureFailure::None:
        return "none";
    case Slus21050FemainFrameClosureFailure::Disabled:
        return "disabled";
    case Slus21050FemainFrameClosureFailure::InvalidExecutionContext:
        return "invalid-execution-context";
    case Slus21050FemainFrameClosureFailure::ThreadContextChanged:
        return "thread-context-changed";
    case Slus21050FemainFrameClosureFailure::CallbackContextChanged:
        return "callback-context-changed";
    case Slus21050FemainFrameClosureFailure::SnapshotCounterReordered:
        return "snapshot-counter-reordered";
    case Slus21050FemainFrameClosureFailure::PublicationIdentityChanged:
        return "publication-identity-changed";
    case Slus21050FemainFrameClosureFailure::InstructionMismatch:
        return "instruction-mismatch";
    case Slus21050FemainFrameClosureFailure::ReturnAddressMismatch:
        return "return-address-mismatch";
    case Slus21050FemainFrameClosureFailure::UnexpectedBoundary:
        return "unexpected-boundary";
    case Slus21050FemainFrameClosureFailure::StateMismatch:
        return "state-mismatch";
    case Slus21050FemainFrameClosureFailure::Incomplete:
        return "incomplete";
    case Slus21050FemainFrameClosureFailure::Overflow:
        return "overflow";
    case Slus21050FemainFrameClosureFailure::Finished:
        return "finished";
    case Slus21050FemainFrameClosureFailure::InstructionUnavailable:
        return "instruction-unavailable";
    }
    return "unknown";
}

Slus21050FemainFrameClosureDiagnostic::Slus21050FemainFrameClosureDiagnostic(
    const bool enabled) noexcept
    : enabled_(enabled),
      finished_(false),
      phase_(enabled ? Phase::Idle : Phase::Disabled),
      failure_(enabled ? Failure::None : Failure::Disabled) {}

bool Slus21050FemainFrameClosureDiagnostic::valid_context(const Context& context) noexcept {
    if (context.thread_id == 0U) {
        return false;
    }
    return context.callback_active ? context.callback_identity != 0U
                                   : context.callback_identity == 0U;
}

bool Slus21050FemainFrameClosureDiagnostic::counters_monotonic(
    const Snapshot& before, const Snapshot& after) noexcept {
    return not_reordered(before.guest_step, after.guest_step) &&
           not_reordered(before.guest_cycle, after.guest_cycle) &&
           not_reordered(before.guest_vblank, after.guest_vblank) &&
           not_reordered(before.audio_sample, after.audio_sample) &&
           not_reordered(before.timeline_event_count, after.timeline_event_count) &&
           not_reordered(before.raw_event_count, after.raw_event_count) &&
           not_reordered(before.eop_event_count, after.eop_event_count) &&
           not_reordered(before.primary_command_count, after.primary_command_count) &&
           not_reordered(before.worker_dispatch_count, after.worker_dispatch_count);
}

bool Slus21050FemainFrameClosureDiagnostic::publication_consistent(
    const Snapshot& before, const Snapshot& after) noexcept {
    if (before.publication.present != after.publication.present) {
        return false;
    }
    if (!before.publication.present) {
        return true;
    }
    return before.publication == after.publication;
}

const Slus21050FemainFrameClosureSequence*
Slus21050FemainFrameClosureDiagnostic::sequence_for_first_call(
    const guest::GuestAddress pc, std::size_t& index) noexcept {
    for (std::size_t candidate = 0U; candidate < kSequenceCount; ++candidate) {
        if (kSlus21050FemainFrameClosureSequences[candidate].first_call == pc) {
            index = candidate;
            return &kSlus21050FemainFrameClosureSequences[candidate];
        }
    }
    return nullptr;
}

bool Slus21050FemainFrameClosureDiagnostic::observe_snapshot(
    const Snapshot& snapshot) noexcept {
    const auto& context = snapshot.execution;
    if (!valid_context(context)) {
        fail(Failure::InvalidExecutionContext);
        return false;
    }
    if (active_stage_ == ActiveStage::None) {
        context_bound_ = true;
        execution_context_ = context;
    } else if (execution_context_.thread_id != context.thread_id) {
        fail(Failure::ThreadContextChanged);
        return false;
    } else if (context.callback_active != execution_context_.callback_active ||
               context.callback_identity != execution_context_.callback_identity ||
               context.callback_activation_generation !=
                   execution_context_.callback_activation_generation) {
        fail(Failure::CallbackContextChanged);
        return false;
    }
    if (snapshot_bound_) {
        if (!counters_monotonic(previous_snapshot_, snapshot)) {
            fail(Failure::SnapshotCounterReordered);
            return false;
        }
        if (active_stage_ != ActiveStage::None &&
            previous_snapshot_.observation_epoch != snapshot.observation_epoch) {
            fail(Failure::SnapshotCounterReordered);
            return false;
        }
        if (active_stage_ != ActiveStage::None &&
            !publication_consistent(previous_snapshot_, snapshot)) {
            fail(Failure::PublicationIdentityChanged);
            return false;
        }
    }
    previous_snapshot_ = snapshot;
    snapshot_bound_ = true;
    return true;
}

bool Slus21050FemainFrameClosureDiagnostic::require_instruction(
    const Observation& observation, const guest::GuestAddress pc,
    const std::uint32_t word) noexcept {
    if (observation.pc != pc || observation.instruction != word) {
        fail(Failure::InstructionMismatch);
        return false;
    }
    return true;
}

bool Slus21050FemainFrameClosureDiagnostic::require_return_address(
    const Observation& observation, const guest::GuestAddress resume) noexcept {
    if (observation.return_address != resume) {
        fail(Failure::ReturnAddressMismatch);
        return false;
    }
    return true;
}

bool Slus21050FemainFrameClosureDiagnostic::append_candidate(
    const Snapshot& after) noexcept {
    if (candidate_count_ >= kMaximumCandidates || next_candidate_ordinal_ == 0U) {
        fail(Failure::Overflow);
        return false;
    }
    Slus21050FemainFrameClosureCandidate& slot = candidates_[candidate_count_];
    slot = Slus21050FemainFrameClosureCandidate{
        .ordinal = next_candidate_ordinal_,
        .sequence_index = sequence_index_,
        .sequence = kSlus21050FemainFrameClosureSequences[sequence_index_],
        .before = pending_before_,
        .between = pending_between_,
        .after = after,
    };
    ++candidate_count_;
    if (next_candidate_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        next_candidate_ordinal_ = 0U;
    } else {
        ++next_candidate_ordinal_;
    }
    return true;
}

void Slus21050FemainFrameClosureDiagnostic::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    if (failure_ != Failure::Disabled) {
        phase_ = Phase::Failed;
    }
}

bool Slus21050FemainFrameClosureDiagnostic::observe(
    const Observation& observation) noexcept {
    if (!enabled_) {
        failure_ = Failure::Disabled;
        return false;
    }
    if (finished_) {
        fail(Failure::Finished);
        return false;
    }
    if (failure_ != Failure::None) {
        return false;
    }
    if (!observe_snapshot(observation.snapshot)) {
        return false;
    }
    const auto& sequence = kSlus21050FemainFrameClosureSequences[sequence_index_];
    switch (active_stage_) {
    case ActiveStage::None: {
        std::size_t index = 0U;
        const auto* first = sequence_for_first_call(observation.pc, index);
        if (first == nullptr) {
            return true;
        }
        if (observation.instruction != kSlus21050FemainFrameClosureJalWord) {
            fail(Failure::InstructionMismatch);
            return false;
        }
        sequence_index_ = index;
        pending_before_ = observation.snapshot;
        active_stage_ = ActiveStage::FirstDelay;
        phase_ = Phase::FirstDelay;
        return true;
    }
    case ActiveStage::FirstDelay:
        if (!require_instruction(observation, sequence.first_call + 4U,
                                 kSlus21050FemainFrameClosureNopWord)) {
            return false;
        }
        active_stage_ = ActiveStage::FirstCallee;
        phase_ = Phase::FirstCallee;
        return true;
    case ActiveStage::FirstCallee:
        if (!require_instruction(observation, kSlus21050FemainFrameClosureScalarTarget,
                                 kSlus21050FemainFrameClosureCalleeEntryWord)) {
            return false;
        }
        active_stage_ = ActiveStage::FirstBody;
        phase_ = Phase::FirstBody;
        return true;
    case ActiveStage::FirstBody:
        if (observation.pc != kSlus21050FemainFrameClosureCalleeReturnPc) {
            if (is_scalar_target(observation.pc)) {
                fail(Failure::UnexpectedBoundary);
                return false;
            }
            return true;
        }
        if (observation.instruction != kSlus21050FemainFrameClosureCalleeReturnWord) {
            fail(Failure::InstructionMismatch);
            return false;
        }
        if (!require_return_address(observation, sequence.second_call)) {
            return false;
        }
        active_stage_ = ActiveStage::FirstReturnDelay;
        phase_ = Phase::FirstReturnDelay;
        return true;
    case ActiveStage::FirstReturnDelay:
        if (!require_instruction(observation, kSlus21050FemainFrameClosureReturnDelayPc,
                                 kSlus21050FemainFrameClosureReturnDelayWord)) {
            return false;
        }
        if (!require_return_address(observation, sequence.second_call)) {
            return false;
        }
        active_stage_ = ActiveStage::FirstResume;
        phase_ = Phase::FirstResume;
        return true;
    case ActiveStage::FirstResume:
        if (!require_instruction(observation, sequence.second_call,
                                 kSlus21050FemainFrameClosureJalWord)) {
            return false;
        }
        if (!require_return_address(observation, sequence.second_call)) {
            return false;
        }
        pending_between_ = observation.snapshot;
        active_stage_ = ActiveStage::SecondDelay;
        phase_ = Phase::SecondDelay;
        return true;
    case ActiveStage::SecondDelay:
        if (!require_instruction(observation, sequence.second_call + 4U,
                                 kSlus21050FemainFrameClosureNopWord)) {
            return false;
        }
        active_stage_ = ActiveStage::SecondCallee;
        phase_ = Phase::SecondCallee;
        return true;
    case ActiveStage::SecondCallee:
        if (!require_instruction(observation, kSlus21050FemainFrameClosureScalarTarget,
                                 kSlus21050FemainFrameClosureCalleeEntryWord)) {
            return false;
        }
        active_stage_ = ActiveStage::SecondBody;
        phase_ = Phase::SecondBody;
        return true;
    case ActiveStage::SecondBody:
        if (observation.pc != kSlus21050FemainFrameClosureCalleeReturnPc) {
            if (is_scalar_target(observation.pc)) {
                fail(Failure::UnexpectedBoundary);
                return false;
            }
            return true;
        }
        if (observation.instruction != kSlus21050FemainFrameClosureCalleeReturnWord) {
            fail(Failure::InstructionMismatch);
            return false;
        }
        if (!require_return_address(observation, sequence.final_pc)) {
            return false;
        }
        active_stage_ = ActiveStage::SecondReturnDelay;
        phase_ = Phase::SecondReturnDelay;
        return true;
    case ActiveStage::SecondReturnDelay:
        if (!require_instruction(observation, kSlus21050FemainFrameClosureReturnDelayPc,
                                 kSlus21050FemainFrameClosureReturnDelayWord)) {
            return false;
        }
        if (!require_return_address(observation, sequence.final_pc)) {
            return false;
        }
        active_stage_ = ActiveStage::SecondResume;
        phase_ = Phase::SecondResume;
        return true;
    case ActiveStage::SecondResume:
        if (!require_instruction(observation, sequence.final_pc,
                                 sequence.final_instruction_word)) {
            return false;
        }
        if (!require_return_address(observation, sequence.final_pc)) {
            return false;
        }
        if (!append_candidate(observation.snapshot)) {
            return false;
        }
        active_stage_ = ActiveStage::None;
        sequence_index_ = 0U;
        phase_ = Phase::Idle;
        return true;
    }
    fail(Failure::StateMismatch);
    return false;
}

bool Slus21050FemainFrameClosureDiagnostic::observe(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const Snapshot& snapshot) noexcept {
    if (!memory.contains(state.pc(), 4U)) {
        fail(Failure::InstructionUnavailable);
        return false;
    }
    return observe({
        .pc = state.pc(),
        .instruction = memory.read_u32_le(state.pc()),
        .return_address = return_address_register(state),
        .snapshot = snapshot,
    });
}

bool Slus21050FemainFrameClosureDiagnostic::finish() noexcept {
    if (!enabled_) {
        failure_ = Failure::Disabled;
        return false;
    }
    if (finished_) {
        fail(Failure::Finished);
        return false;
    }
    if (failure_ != Failure::None) {
        return false;
    }
    if (active_stage_ != ActiveStage::None) {
        failure_ = Failure::Incomplete;
        phase_ = Phase::Failed;
        return false;
    }
    finished_ = true;
    phase_ = Phase::Finished;
    return true;
}

void Slus21050FemainFrameClosureDiagnostic::reset() noexcept {
    finished_ = false;
    phase_ = enabled_ ? Phase::Idle : Phase::Disabled;
    failure_ = enabled_ ? Failure::None : Failure::Disabled;
    active_stage_ = ActiveStage::None;
    sequence_index_ = 0U;
    next_candidate_ordinal_ = 1U;
    context_bound_ = false;
    execution_context_ = {};
    snapshot_bound_ = false;
    previous_snapshot_ = {};
    pending_before_ = {};
    pending_between_ = {};
    candidates_ = {};
    candidate_count_ = 0U;
}

const Slus21050FemainFrameClosureDiagnostic::Candidate*
Slus21050FemainFrameClosureDiagnostic::latest_candidate() const noexcept {
    return candidate_count_ == 0U ? nullptr : &candidates_[candidate_count_ - 1U];
}

const Slus21050FemainFrameClosureDiagnostic::Snapshot*
Slus21050FemainFrameClosureDiagnostic::before_snapshot() const noexcept {
    const auto* candidate = latest_candidate();
    return candidate == nullptr ? nullptr : &candidate->before;
}

const Slus21050FemainFrameClosureDiagnostic::Snapshot*
Slus21050FemainFrameClosureDiagnostic::between_snapshot() const noexcept {
    const auto* candidate = latest_candidate();
    return candidate == nullptr ? nullptr : &candidate->between;
}

const Slus21050FemainFrameClosureDiagnostic::Snapshot*
Slus21050FemainFrameClosureDiagnostic::after_snapshot() const noexcept {
    const auto* candidate = latest_candidate();
    return candidate == nullptr ? nullptr : &candidate->after;
}

} // namespace takedown::runtime
