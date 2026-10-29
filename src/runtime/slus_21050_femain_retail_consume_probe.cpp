#include "takedown/runtime/slus_21050_femain_retail_consume_probe.hpp"
#include <cstdint>
#include <limits>
#include <string_view>

namespace takedown::runtime {

std::string_view to_string(Slus21050FemainRetailConsumeProbeFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainRetailConsumeProbeFailure::None:
        return "none";
    case Slus21050FemainRetailConsumeProbeFailure::Disabled:
        return "disabled";
    case Slus21050FemainRetailConsumeProbeFailure::BindingInvalid:
        return "binding-invalid";
    case Slus21050FemainRetailConsumeProbeFailure::ObservationIdentityMismatch:
        return "observation-identity-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ObservationOrdinalMismatch:
        return "observation-ordinal-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::EventOrderInvalid:
        return "event-order-invalid";
    case Slus21050FemainRetailConsumeProbeFailure::CallInstructionMismatch:
        return "call-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CallTargetMismatch:
        return "call-target-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CallStateMismatch:
        return "call-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::DelayInstructionMismatch:
        return "delay-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::DelayStateMismatch:
        return "delay-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CalleeInstructionMismatch:
        return "callee-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CalleeArgumentMismatch:
        return "callee-argument-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CalleeStateMismatch:
        return "callee-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadInstructionMismatch:
        return "queue-read-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadAddressMismatch:
        return "queue-read-address-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadDomainMismatch:
        return "queue-read-domain-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadPreimageUnavailable:
        return "queue-read-preimage-unavailable";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadSequenceMismatch:
        return "queue-read-sequence-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::QueueReadCountOverflow:
        return "queue-read-count-overflow";
    case Slus21050FemainRetailConsumeProbeFailure::FinalDrainPrefixMissing:
        return "final-drain-prefix-missing";
    case Slus21050FemainRetailConsumeProbeFailure::FinalDrainInstructionMismatch:
        return "final-drain-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::FinalDrainStateMismatch:
        return "final-drain-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::FinalDrainPathMismatch:
        return "final-drain-path-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::FinalDrainWriteMissing:
        return "final-drain-write-missing";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalLoadInstructionMismatch:
        return "terminal-load-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalBranchInstructionMismatch:
        return "terminal-branch-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalBranchStateMismatch:
        return "terminal-branch-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalBranchPathMismatch:
        return "terminal-branch-path-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalBranchMissing:
        return "terminal-branch-missing";
    case Slus21050FemainRetailConsumeProbeFailure::TerminalDelayInstructionMismatch:
        return "terminal-delay-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ReturnBranchInstructionMismatch:
        return "return-branch-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ReturnBranchPathMismatch:
        return "return-branch-path-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ReturnJrInstructionMismatch:
        return "return-jr-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ReturnJrStateMismatch:
        return "return-jr-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::ReturnDelayInstructionMismatch:
        return "return-delay-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CallerResumeInstructionMismatch:
        return "caller-resume-instruction-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::CallerResumeStateMismatch:
        return "caller-resume-state-mismatch";
    case Slus21050FemainRetailConsumeProbeFailure::InstructionNotRetired:
        return "instruction-not-retired";
    case Slus21050FemainRetailConsumeProbeFailure::IncompleteInvocation:
        return "incomplete-invocation";
    case Slus21050FemainRetailConsumeProbeFailure::ReplayedObservation:
        return "replayed-observation";
    default:
        return "unknown";
    }
}

bool Slus21050FemainRetailConsumeProbe::binding_valid(
    const Slus21050FemainRetailConsumeBinding& binding) const noexcept {
    const auto& owner = binding.owner;
    if (owner.owner_identity == 0U || owner.player_identity == 0U ||
        owner.player_session_generation == 0U || owner.ledger_identity == 0U ||
        owner.ledger_session_generation == 0U || binding.publication_serial == 0U ||
        binding.runtime_epoch == 0U || binding.observation_epoch == 0U ||
        binding.invocation_serial == 0U || binding.guest_execution_context_identity == 0U) {
        return false;
    }
    const auto& reservation = binding.published_reservation;
    if (reservation.ledger_identity == 0U || owner.ledger_identity != reservation.ledger_identity) {
        return false;
    }
    if (reservation.session_generation == 0U || owner.player_session_generation != reservation.session_generation ||
        owner.ledger_session_generation != reservation.session_generation) {
        return false;
    }
    if (reservation.frame_generation == 0U || reservation.slot > 1U) {
        return false;
    }
    if (binding.published_queue_read == 0U || binding.published_queue_end == 0U ||
        binding.published_queue_read >= binding.published_queue_end) {
        return false;
    }
    if (binding.published_buffer_index > 1U || binding.published_queue_base == 0U) {
        return false;
    }
    const std::uint32_t queue_delta = binding.published_queue_end - binding.published_queue_read;
    return queue_delta > 7U && (queue_delta & 7U) == 0U;
}

bool Slus21050FemainRetailConsumeProbe::bind(const Slus21050FemainRetailConsumeBinding& binding) noexcept {
    if (!enabled_) {
        failure_ = Failure::Disabled;
    }
    if (bound_ || phase_ != Phase::Empty) {
        if (failure_ == Failure::None) {
            failure_ = Failure::ReplayedObservation;
            evidence_ = {};
        }
        pending_post_ = PendingPost::None;
        pending_instruction_guest_step_ = 0U;
        pending_untracked_pc_ = 0U;
        pending_untracked_instruction_ = 0U;
        pending_queue_store_write_observed_ = false;
        pending_queue_store_guest_step_ = 0U;
        pending_queue_store_pc_ = 0U;
        ordinary_advance_branch_pc_ = 0U;
        ordinary_advance_delay_pc_ = 0U;
        ordinary_advance_target_pc_ = 0U;
        ordinary_advance_branch_word_ = 0U;
        ordinary_advance_delay_word_ = 0U;
        final_drain_condition_valid_ = false;
        final_drain_condition_value_ = 0U;
        phase_ = Phase::Failed;
        return false;
    }
    if (!binding_valid(binding)) {
        if (failure_ == Failure::None) {
            failure_ = Failure::BindingInvalid;
            evidence_ = {};
        }
        pending_post_ = PendingPost::None;
        pending_instruction_guest_step_ = 0U;
        pending_untracked_pc_ = 0U;
        pending_untracked_instruction_ = 0U;
        pending_queue_store_write_observed_ = false;
        pending_queue_store_guest_step_ = 0U;
        pending_queue_store_pc_ = 0U;
        ordinary_advance_branch_pc_ = 0U;
        ordinary_advance_delay_pc_ = 0U;
        ordinary_advance_target_pc_ = 0U;
        ordinary_advance_branch_word_ = 0U;
        ordinary_advance_delay_word_ = 0U;
        final_drain_condition_valid_ = false;
        final_drain_condition_value_ = 0U;
        phase_ = Phase::Failed;
        return false;
    }
    binding_ = binding;
    evidence_ = {};
    evidence_.binding = binding;
    bound_ = true;
    return true;
}

void Slus21050FemainRetailConsumeProbe::reset() noexcept {
    failure_ = Failure::None;
    phase_ = enabled_ ? Phase::Empty : Phase::Disabled;
    bound_ = false;
    last_guest_step_valid_ = false;
    last_guest_step_ = 0U;
    last_observation_ordinal_valid_ = false;
    last_observation_ordinal_ = 0U;
    pending_post_ = PendingPost::None;
    pending_instruction_guest_step_ = 0U;
    pending_untracked_pc_ = 0U;
    pending_untracked_instruction_ = 0U;
    pending_queue_store_write_observed_ = false;
    pending_queue_store_guest_step_ = 0U;
    pending_queue_store_pc_ = 0U;
    final_drain_iteration_ = false;
    final_drain_condition_valid_ = false;
    final_drain_condition_value_ = 0U;
    ordinary_advance_branch_pc_ = 0U;
    ordinary_advance_delay_pc_ = 0U;
    ordinary_advance_target_pc_ = 0U;
    ordinary_advance_branch_word_ = 0U;
    ordinary_advance_delay_word_ = 0U;
    binding_ = {};
    evidence_ = {};
}

bool Slus21050FemainRetailConsumeProbe::identity_matches(
    const std::uint64_t runtime_epoch, const std::uint64_t observation_epoch,
    const std::uint64_t invocation_serial,
    const std::uint64_t guest_execution_context_identity) const noexcept {
    return bound_ && binding_.runtime_epoch == runtime_epoch &&
           binding_.observation_epoch == observation_epoch &&
           binding_.invocation_serial == invocation_serial &&
           binding_.guest_execution_context_identity == guest_execution_context_identity;
}

void Slus21050FemainRetailConsumeProbe::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    evidence_ = {};
    pending_post_ = PendingPost::None;
    pending_instruction_guest_step_ = 0U;
    pending_untracked_pc_ = 0U;
    pending_untracked_instruction_ = 0U;
    pending_queue_store_write_observed_ = false;
    pending_queue_store_guest_step_ = 0U;
    pending_queue_store_pc_ = 0U;
    ordinary_advance_branch_pc_ = 0U;
    ordinary_advance_delay_pc_ = 0U;
    ordinary_advance_target_pc_ = 0U;
    ordinary_advance_branch_word_ = 0U;
    ordinary_advance_delay_word_ = 0U;
    final_drain_condition_valid_ = false;
    final_drain_condition_value_ = 0U;
    phase_ = Phase::Failed;
}

bool Slus21050FemainRetailConsumeProbe::event_ordinal_matches(const std::uint64_t ordinal) noexcept {
    if (ordinal == 0U) {
        fail(Failure::ObservationOrdinalMismatch);
        return false;
    }
    if (!last_observation_ordinal_valid_) {
        last_observation_ordinal_ = ordinal;
        last_observation_ordinal_valid_ = true;
        return true;
    }
    if (last_observation_ordinal_ == std::numeric_limits<std::uint64_t>::max() ||
        ordinal != last_observation_ordinal_ + 1U) {
        fail(Failure::ObservationOrdinalMismatch);
        last_observation_ordinal_ = ordinal;
        return false;
    }
    last_observation_ordinal_ = ordinal;
    return true;
}

bool Slus21050FemainRetailConsumeProbe::step_matches(
    const std::uint64_t guest_step, const bool allow_same_guest_step) noexcept {
    if (last_guest_step_valid_) {
        if (last_guest_step_ > guest_step ||
            (!allow_same_guest_step && last_guest_step_ == guest_step)) {
            fail(Failure::EventOrderInvalid);
            last_guest_step_ = guest_step;
            last_guest_step_valid_ = true;
            return false;
        }
    }
    return true;
}

bool Slus21050FemainRetailConsumeProbe::observation_matches(
    const std::uint64_t guest_step, const std::uint64_t runtime_epoch,
    const std::uint64_t observation_epoch, const std::uint64_t invocation_serial,
    const std::uint64_t observation_ordinal,
    const std::uint64_t guest_execution_context_identity,
    const bool allow_same_guest_step) noexcept {
    if (!identity_matches(runtime_epoch, observation_epoch, invocation_serial,
                          guest_execution_context_identity)) {
        fail(Failure::ObservationIdentityMismatch);
        return false;
    }
    if (!event_ordinal_matches(observation_ordinal)) {
        return false;
    }
    if (last_guest_step_valid_) {
        if (guest_step < last_guest_step_ ||
            (!allow_same_guest_step && guest_step == last_guest_step_)) {
            fail(Failure::EventOrderInvalid);
            return false;
        }
    }
    last_guest_step_ = guest_step;
    last_guest_step_valid_ = true;
    return true;
}

bool Slus21050FemainRetailConsumeProbe::queue_count_reaches_end() const noexcept {
    const auto& aggregate = evidence_.queue_read_aggregate;
    if (aggregate.count == 0U) {
        return false;
    }
    const std::uint32_t queue_end = binding_.published_queue_end;
    const std::uint32_t queue_read = binding_.published_queue_read;
    const std::uint32_t queue_delta = queue_end - queue_read;
    if (aggregate.first.previous_value != queue_read || aggregate.first.value != queue_read + 8U) {
        return false;
    }
    if (aggregate.last.previous_value != queue_end - 8U || aggregate.last.value != queue_end) {
        return false;
    }
    return aggregate.count == static_cast<std::uint64_t>(queue_delta >> 3U);
}

void Slus21050FemainRetailConsumeProbe::close() noexcept {
    if (!enabled_ || phase_ == Phase::Disabled || phase_ == Phase::Complete || phase_ == Phase::Failed) {
        return;
    }
    if (failure_ == Failure::None) {
        failure_ = Failure::IncompleteInvocation;
        evidence_ = {};
    }
    pending_post_ = PendingPost::None;
    pending_instruction_guest_step_ = 0U;
    pending_untracked_pc_ = 0U;
    pending_untracked_instruction_ = 0U;
    pending_queue_store_write_observed_ = false;
    pending_queue_store_guest_step_ = 0U;
    pending_queue_store_pc_ = 0U;
    ordinary_advance_branch_pc_ = 0U;
    ordinary_advance_delay_pc_ = 0U;
    ordinary_advance_target_pc_ = 0U;
    ordinary_advance_branch_word_ = 0U;
    ordinary_advance_delay_word_ = 0U;
    final_drain_condition_valid_ = false;
    final_drain_condition_value_ = 0U;
    phase_ = Phase::Failed;
}

Slus21050FemainRetailConsumeProbeResult Slus21050FemainRetailConsumeProbe::result() const noexcept {
    Slus21050FemainRetailConsumeProbeResult out{};
    out.failure = failure_;
    if (phase_ == Phase::Complete && failure_ == Failure::None &&
        pending_post_ == PendingPost::None && evidence_.successful_return) {
        out.evidence = evidence_;
    }
    return out;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == kSlus21050FemainRetailConsumeDelayPc &&
        observation.instruction == kSlus21050FemainRetailConsumeDelayWord &&
        observation.executing_delay_slot && observation.next_pc == 2037624U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::CallerDelay;
        return;
    }
    if (failure_ == Failure::None) {
        failure_ = Failure::DelayInstructionMismatch;
    }
}

void Slus21050FemainRetailConsumeProbe::observe_pre_ordinary_advance_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == ordinary_advance_delay_pc_ &&
        observation.instruction == ordinary_advance_delay_word_ &&
        observation.executing_delay_slot && observation.next_pc == ordinary_advance_target_pc_) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::OrdinaryAdvanceDelay;
        return;
    }
    fail(Failure::QueueReadInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_read_add(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == kSlus21050FemainRetailFinalDrainReadAddPc &&
        observation.instruction == kSlus21050FemainRetailFinalDrainReadAddWord &&
        !observation.executing_delay_slot && observation.next_pc == kSlus21050FemainRetailFinalDrainReadStorePc) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::FinalReadAdd;
        return;
    }
    fail(Failure::FinalDrainInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_active_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == kSlus21050FemainRetailFinalDrainActiveDelayPc &&
        observation.instruction == kSlus21050FemainRetailFinalDrainActiveDelayWord &&
        observation.executing_delay_slot && observation.next_pc == kSlus21050FemainRetailTerminalLoadPc) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::FinalActiveDelay;
        phase_ = Phase::FinalDrainActiveDelaySeen;
        return;
    }
    fail(Failure::FinalDrainPathMismatch);
}


void Slus21050FemainRetailConsumeProbe::observe_pre_terminal_taken(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038924U && observation.instruction == 604176383U &&
        observation.executing_delay_slot && observation.next_pc == 2038944U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::TerminalDelay;
        return;
    }
    fail(Failure::TerminalDelayInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_terminal_nonterminal_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038924U && observation.instruction == 604176383U &&
        observation.executing_delay_slot && observation.next_pc == 2038928U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::FinalDrainNonterminalDelay;
        return;
    }
    fail(Failure::TerminalDelayInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_drain_condition_load(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038928U && observation.instruction == 2474811743U &&
        !observation.executing_delay_slot && observation.next_pc == 2038932U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::FinalDrainConditionLoad;
        return;
    }
    fail(Failure::FinalDrainPathMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_branch_likely_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038936U && observation.instruction == 2359492608U &&
        observation.executing_delay_slot && observation.next_pc == 2037912U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::BranchLikelyDelay;
        return;
    }
    fail(Failure::FinalDrainPathMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_return_branch(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038944U && observation.instruction == 320995332U &&
        !observation.executing_delay_slot && observation.t9 == 4294967295U &&
        observation.v0 == 4294967295U && observation.ra == 2041348U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::ReturnBranch;
        return;
    }
    fail(Failure::ReturnBranchInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_return_branch_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2038948U && observation.instruction == 2409889792U &&
        observation.executing_delay_slot && observation.next_pc == 2038964U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::ReturnBranchDelay;
        return;
    }
    fail(Failure::ReturnBranchPathMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_return_delay(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2039012U && observation.instruction == 666698016U &&
        observation.executing_delay_slot && observation.next_pc == 2041348U) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::ReturnDelay;
        phase_ = Phase::ReturnDelaySeen;
        return;
    }
    fail(Failure::ReturnDelayInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_caller_resumed(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc == 2041348U && observation.instruction == 1107296312U &&
        !observation.executing_delay_slot) {
        evidence_.return_path.caller_resume_guest_step = observation.guest_step;
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::CallerResume;
        phase_ = Phase::CallerResumed;
        return;
    }
    fail(Failure::CallerResumeInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_caller(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.instruction != 201835998U || observation.executing_delay_slot ||
        observation.next_pc != 2041344U) {
        fail(Failure::CallInstructionMismatch);
        return;
    }
    if (!observation.queue_state_valid || observation.queue_read != binding_.published_queue_read ||
        observation.queue_end != binding_.published_queue_end || !observation.boundary_valid) {
        fail(Failure::CallStateMismatch);
        return;
    }
    Slus21050FemainRetailConsumeCallContext context{};
    context.guest_step = observation.guest_step;
    context.boundary = observation.boundary;
    context.queue_read = observation.queue_read;
    context.queue_end = observation.queue_end;
    context.transfers_active = observation.transfers_active;
    context.a0 = observation.a0;
    context.t9 = observation.t9;
    evidence_.call = context;
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::Caller;
    phase_ = Phase::CallerSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_callee(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2037624U || observation.instruction != 666762976U ||
        observation.executing_delay_slot) {
        fail(Failure::CalleeInstructionMismatch);
        return;
    }
    if (observation.a0 != 4294967295U) {
        fail(Failure::CalleeArgumentMismatch);
        return;
    }
    if (!observation.queue_state_valid || observation.queue_read != binding_.published_queue_read ||
        observation.queue_end != binding_.published_queue_end || !observation.boundary_valid) {
        fail(Failure::CalleeStateMismatch);
        return;
    }
    Slus21050FemainRetailConsumeCallContext context{};
    context.guest_step = observation.guest_step;
    context.boundary = observation.boundary;
    context.transfers_active = observation.transfers_active;
    context.t9 = observation.t9;
    evidence_.callee = context;
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::Callee;
    phase_ = Phase::CalleeSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_ordinary_advance_branch(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    struct Shape {
        guest::GuestAddress delay_pc;
        std::uint32_t branch_word;
        std::uint32_t delay_word;
        guest::GuestAddress target_pc;
    };
    Shape shape{};
    bool known = true;
    switch (observation.pc) {
    case 2037960U:
        shape = {2037964U, 268435690U, 608305160U, 2038900U};
        break;
    case 2037984U:
        shape = {2037988U, 268435684U, 608305160U, 2038900U};
        break;
    case 2038032U:
        shape = {2038036U, 268435672U, 608305160U, 2038900U};
        break;
    case 2038372U:
        shape = {2038376U, 268435586U, 2407702984U, 2038896U};
        break;
    case 2038704U:
        shape = {2038708U, 268435503U, 2407702984U, 2038896U};
        break;
    case 2038712U:
        shape = {2038716U, 268435502U, 2359427076U, 2038900U};
        break;
    case 2038820U:
        shape = {2038824U, 268435474U, 2407702984U, 2038896U};
        break;
    default:
        known = false;
        break;
    }
    if (known && observation.instruction == shape.branch_word && !observation.executing_delay_slot &&
        observation.next_pc == shape.delay_pc) {
        ordinary_advance_branch_pc_ = observation.pc;
        ordinary_advance_delay_pc_ = shape.delay_pc;
        ordinary_advance_target_pc_ = shape.target_pc;
        ordinary_advance_branch_word_ = shape.branch_word;
        ordinary_advance_delay_word_ = shape.delay_word;
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::OrdinaryAdvanceBranch;
        phase_ = Phase::OrdinaryAdvanceBranchSeen;
        return;
    }
    fail(Failure::QueueReadInstructionMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_ordinary_advance_add(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    const std::uint32_t expected_previous =
        evidence_.queue_read_aggregate.count == 0U ? binding_.published_queue_read
                                                   : evidence_.queue_read_aggregate.last.value;
    if (observation.pc == 2038896U && observation.instruction == 608305160U &&
        !observation.executing_delay_slot && observation.next_pc == 2038900U &&
        observation.queue_state_valid && observation.queue_read == expected_previous &&
        observation.queue_end == binding_.published_queue_end && observation.v0 == expected_previous) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::OrdinaryAdvanceAdd;
        return;
    }
    fail(Failure::QueueReadSequenceMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_queue_read_store(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    const bool at_callee = observation.pc == 2038360U && phase_ == Phase::CalleeActive;
    const bool at_next = observation.pc == 2038900U && phase_ == Phase::OrdinaryAdvanceStoreReady;
    if (!at_callee && !at_next) {
        fail(Failure::QueueReadInstructionMismatch);
        return;
    }
    const std::uint32_t expected_next_pc = at_callee ? 2038364U : 2038904U;
    if (observation.instruction != 2944573896U || observation.executing_delay_slot ||
        observation.next_pc != expected_next_pc) {
        fail(Failure::QueueReadInstructionMismatch);
        return;
    }
    if (!observation.queue_state_valid || observation.queue_end != binding_.published_queue_end) {
        fail(Failure::QueueReadAddressMismatch);
        return;
    }
    const std::uint32_t queue_end = binding_.published_queue_end;
    const std::uint64_t expected_count =
        static_cast<std::uint64_t>((queue_end - binding_.published_queue_read) >> 3U);
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? binding_.published_queue_read : aggregate.last.value;
    if (aggregate.count >= expected_count || observation.queue_read != expected_previous ||
        queue_end <= observation.queue_read || observation.v0 != expected_previous + 8U) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    pending_queue_store_guest_step_ = observation.guest_step;
    pending_queue_store_pc_ = observation.pc;
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::QueueReadStore;
    phase_ = Phase::QueueReadStoreSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_read_load(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038892U || observation.instruction != 2407702984U ||
        observation.executing_delay_slot || observation.next_pc != 2038896U ||
        !observation.queue_state_valid || observation.queue_end != binding_.published_queue_end) {
        fail(Failure::FinalDrainInstructionMismatch);
        return;
    }
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? binding_.published_queue_read : aggregate.last.value;
    if (observation.queue_read != expected_previous) {
        fail(Failure::FinalDrainStateMismatch);
        return;
    }
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::FinalReadLoad;
    phase_ = Phase::FinalDrainReadLoadSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_drain(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    switch (observation.pc) {
    case 2037624U:
    case 2041340U:
    case 2041344U:
        fail(Failure::ReplayedObservation);
        return;
    case 2037960U:
    case 2037984U:
    case 2038032U:
    case 2038372U:
    case 2038704U:
    case 2038712U:
    case 2038820U:
        observe_pre_ordinary_advance_branch(observation);
        return;
    case 2038360U:
        observe_pre_queue_read_store(observation);
        return;
    case 2038900U:
        fail(Failure::FinalDrainPrefixMissing);
        return;
    case 2038892U: {
        const std::uint32_t queue_end = binding_.published_queue_end;
        const std::uint32_t queue_read_start = binding_.published_queue_read;
        const std::uint64_t expected_count =
            static_cast<std::uint64_t>((queue_end - queue_read_start) >> 3U);
        const auto& aggregate = evidence_.queue_read_aggregate;
        const std::uint32_t expected_previous =
            aggregate.count == 0U ? queue_read_start : aggregate.last.value;
        if (!observation.queue_state_valid || observation.queue_end != queue_end ||
            observation.queue_read != expected_previous || aggregate.count >= expected_count) {
            fail(Failure::QueueReadSequenceMismatch);
            return;
        }
        bool last_iteration = false;
        if (observation.queue_read == queue_end - 8U && aggregate.count + 1U == expected_count) {
            last_iteration = true;
        }
        const bool more = observation.queue_read < queue_end - 8U && aggregate.count + 1U < expected_count;
        if (!more && !last_iteration) {
            fail(Failure::QueueReadSequenceMismatch);
            return;
        }
        final_drain_iteration_ = last_iteration;
        observe_pre_final_read_load(observation);
        return;
    }
    case 2038896U:
    case 2038904U:
    case 2038908U:
    case 2038912U:
    case 2038916U:
    case 2038920U:
    case 2038924U:
    case 2038928U:
    case 2038932U:
    case 2038936U:
    case 2038944U:
    case 2039008U:
        fail(Failure::FinalDrainPrefixMissing);
        return;
    default:
        pending_untracked_pc_ = observation.pc;
        pending_untracked_instruction_ = observation.instruction;
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::Untracked;
        return;
    }
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_read_store(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038900U || observation.instruction != 2944573896U ||
        observation.executing_delay_slot || observation.next_pc != 2038904U) {
        fail(Failure::FinalDrainInstructionMismatch);
        return;
    }
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? binding_.published_queue_read : aggregate.last.value;
    if (observation.v0 != expected_previous + 8U) {
        fail(Failure::FinalDrainStateMismatch);
        return;
    }
    pending_queue_store_guest_step_ = observation.guest_step;
    pending_queue_store_pc_ = 2038900U;
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::FinalReadStore;
    phase_ = Phase::FinalDrainReadStoreSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_active_load(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t queue_end = binding_.published_queue_end;
    const std::uint32_t queue_read_start = binding_.published_queue_read;
    const std::uint32_t expected_previous = aggregate.count == 0U ? queue_read_start : aggregate.last.value;
    const bool qualified = aggregate.count != 0U && observation.queue_state_valid &&
                           observation.queue_end == queue_end && observation.queue_read == expected_previous;
    const std::uint64_t expected_count = static_cast<std::uint64_t>((queue_end - queue_read_start) >> 3U);
    const bool sequence_ok = final_drain_iteration_
                                 ? (aggregate.count == expected_count && queue_end == observation.queue_read)
                                 : (aggregate.count < expected_count && queue_end > observation.queue_read);
    if (!qualified || !sequence_ok) {
        fail(Failure::FinalDrainWriteMissing);
        return;
    }
    if (observation.pc != 2038904U || observation.instruction != 2474811840U ||
        observation.executing_delay_slot || observation.next_pc != 2038908U) {
        fail(Failure::FinalDrainWriteMissing);
        return;
    }
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::FinalActiveLoad;
    phase_ = Phase::FinalDrainActiveLoadSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_final_active_branch(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038908U || observation.instruction != 339738631U ||
        observation.executing_delay_slot || observation.next_pc != 2038912U) {
        fail(Failure::FinalDrainInstructionMismatch);
        return;
    }
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? binding_.published_queue_read : aggregate.last.value;
    const bool queue_matches = aggregate.count != 0U && observation.queue_state_valid &&
                               observation.queue_read == expected_previous &&
                               observation.queue_end == binding_.published_queue_end;
    if (observation.transfers_active == 0U && observation.v0 == 0U && queue_matches) {
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::FinalActiveBranch;
        phase_ = Phase::FinalDrainActiveBranchSeen;
        return;
    }
    fail(Failure::FinalDrainStateMismatch);
}

void Slus21050FemainRetailConsumeProbe::observe_pre_terminal_load(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038916U || observation.instruction != 2407899592U ||
        observation.executing_delay_slot || observation.next_pc != 2038920U) {
        fail(Failure::TerminalLoadInstructionMismatch);
        return;
    }
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t expected =
        final_drain_iteration_ ? binding_.published_queue_end : aggregate.last.value;
    if (!observation.queue_state_valid || observation.transfers_active != 0U ||
        observation.queue_read != expected || observation.queue_end != binding_.published_queue_end) {
        fail(Failure::TerminalBranchStateMismatch);
        return;
    }
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::TerminalLoad;
    phase_ = Phase::TerminalLoadSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_terminal_branch(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038920U || observation.instruction != 279052293U ||
        observation.executing_delay_slot || observation.next_pc != 2038924U) {
        fail(Failure::TerminalBranchInstructionMismatch);
        return;
    }
    const std::uint32_t queue_end = binding_.published_queue_end;
    const bool both_at_end = observation.a1 == queue_end && observation.v0 == queue_end;
    const bool before_end = !final_drain_iteration_ && observation.a1 < queue_end && queue_end == observation.v0;
    if (!observation.queue_state_valid || observation.transfers_active != 0U ||
        observation.queue_read != observation.a1 || observation.queue_end != observation.v0) {
        fail(Failure::TerminalBranchStateMismatch);
        return;
    }
    if (final_drain_iteration_ ? !both_at_end : !before_end) {
        fail(Failure::TerminalBranchStateMismatch);
        return;
    }
    if (final_drain_iteration_) {
        Slus21050FemainRetailConsumeTerminalBranchPath path{};
        path.guest_step = observation.guest_step;
        path.boundary = observation.boundary;
        path.queue_read = observation.a1;
        path.queue_end = observation.v0;
        path.transfers_active = 0U;
        path.post_pc = 0U;
        path.post_next_pc = 0U;
        path.branch_taken = false;
        evidence_.terminal = path;
    }
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::TerminalBranch;
    phase_ = Phase::TerminalBranchSeen;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_branch_likely(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2038932U || observation.instruction != 1346436864U ||
        observation.executing_delay_slot || observation.next_pc != 2038936U ||
        !final_drain_condition_valid_ || observation.v0 != final_drain_condition_value_ ||
        observation.v0 != 0U) {
        fail(Failure::FinalDrainPathMismatch);
        return;
    }
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::BranchLikely;
}

void Slus21050FemainRetailConsumeProbe::observe_pre_return_jr(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    if (observation.pc != 2039008U || observation.instruction != 65011720U ||
        observation.executing_delay_slot) {
        fail(Failure::ReturnJrInstructionMismatch);
        return;
    }
    if (observation.ra != 2041348U) {
        fail(Failure::ReturnJrStateMismatch);
        return;
    }
    evidence_.return_path.guest_step = observation.guest_step;
    evidence_.return_path.return_address = 2041348U;
    pending_instruction_guest_step_ = observation.guest_step;
    pending_post_ = PendingPost::ReturnJr;
    phase_ = Phase::ReturnJrSeen;
}

// Pre-instruction dispatcher: entry gates, unbound check, pending check,
// identity/ordinal/step gates, then the phase switch. Each phase expects a
// fixed pc; the matching handler validates it and records the pending kind.
void Slus21050FemainRetailConsumeProbe::observe_pre_instruction(
    const Slus21050FemainRetailConsumePreInstruction& observation) noexcept {
    const std::uint32_t pc = observation.pc;
    const bool pc_in_pre_set = (observation.pc == 2037624U || observation.pc == 2037960U || observation.pc == 2037964U || observation.pc == 2037984U || observation.pc == 2037988U || observation.pc == 2038032U || observation.pc == 2038036U || observation.pc == 2038372U || observation.pc == 2038376U || observation.pc == 2038704U || observation.pc == 2038708U || observation.pc == 2038712U || observation.pc == 2038716U || observation.pc == 2038820U || observation.pc == 2038824U || observation.pc == 2038892U || observation.pc == 2038896U || observation.pc == 2038900U || observation.pc == 2038904U || observation.pc == 2038908U || observation.pc == 2038912U || observation.pc == 2038916U || observation.pc == 2038920U || observation.pc == 2038924U || observation.pc == 2038928U || observation.pc == 2038932U || observation.pc == 2038936U || observation.pc == 2038944U || observation.pc == 2038948U || observation.pc == 2039008U || observation.pc == 2041340U || observation.pc == 2041344U || observation.pc == 2039012U || observation.pc == 2041348U);
    if (!enabled_ || failure_ != Failure::None || phase_ == Phase::Complete || phase_ == Phase::Failed) {
        return;
    }
    if (!bound_) {
        if (pc_in_pre_set) {
            failure_ = Failure::BindingInvalid;
        }
        return;
    }
    if (pending_post_ != PendingPost::None) {
        failure_ = Failure::EventOrderInvalid;
        return;
    }
    if (!identity_matches(observation.runtime_epoch, observation.observation_epoch,
                          observation.invocation_serial, observation.guest_execution_context_identity)) {
        if (failure_ == Failure::None) {
            failure_ = Failure::ObservationIdentityMismatch;
        }
        return;
    }
    if (!event_ordinal_matches(observation.observation_ordinal)) {
        return;
    }
    if (last_guest_step_valid_ &&
        (observation.guest_step < last_guest_step_ || observation.guest_step == last_guest_step_)) {
        fail(Failure::EventOrderInvalid);
        return;
    }
    last_guest_step_ = observation.guest_step;
    last_guest_step_valid_ = true;

    switch (phase_) {
    case Phase::Empty:
        if (observation.pc == 2041340U) {
            observe_pre_caller(observation);
        }
        return;
    case Phase::CallerSeen:
        fail(Failure::EventOrderInvalid);
        return;
    case Phase::DelaySeen:
        if (observation.pc == 2041344U && observation.instruction == 604307455U &&
            observation.executing_delay_slot && observation.next_pc == 2037624U) {
            pending_instruction_guest_step_ = observation.guest_step;
            pending_post_ = PendingPost::CallerDelay;
            return;
        }
        fail(Failure::DelayInstructionMismatch);
        return;
    case Phase::CalleeSeen:
        observe_pre_callee(observation);
        return;
    case Phase::CalleeActive:
        observe_pre_drain(observation);
        return;
    case Phase::QueueReadStoreSeen:
        fail(Failure::EventOrderInvalid);
        return;
    case Phase::QueueReadStoreWriteAwaited:
        fail(Failure::QueueReadSequenceMismatch);
        return;
    case Phase::FinalDrainReadLoadSeen:
    case Phase::FinalDrainReadAddSeen:
        if (observation.pc == 2038896U && observation.instruction == 608305160U &&
            !observation.executing_delay_slot && observation.next_pc == 2038900U) {
            pending_instruction_guest_step_ = observation.guest_step;
            pending_post_ = PendingPost::FinalReadAdd;
            return;
        }
        fail(Failure::FinalDrainInstructionMismatch);
        return;
    case Phase::FinalDrainReadStoreSeen:
        observe_pre_final_read_store(observation);
        return;
    case Phase::FinalDrainStoreWriteAwaited:
        fail(Failure::FinalDrainWriteMissing);
        return;
    case Phase::FinalDrainActiveLoadSeen:
        observe_pre_final_active_load(observation);
        return;
    case Phase::FinalDrainActiveBranchSeen:
        observe_pre_final_active_branch(observation);
        return;
    case Phase::FinalDrainActiveDelaySeen:
        observe_pre_final_active_delay(observation);
        return;
    case Phase::FinalDrainReady:
        observe_pre_terminal_load(observation);
        return;
    case Phase::TerminalLoadSeen:
        observe_pre_terminal_branch(observation);
        return;
    case Phase::TerminalBranchSeen:
        fail(Failure::EventOrderInvalid);
        return;
    case Phase::TerminalTaken:
        observe_pre_terminal_taken(observation);
        return;
    case Phase::FinalDrainNonterminalDelaySeen:
        observe_pre_terminal_nonterminal_delay(observation);
        return;
    case Phase::FinalDrainConditionLoadSeen:
        observe_pre_final_drain_condition_load(observation);
        return;
    case Phase::FinalDrainBranchLikelySeen:
        observe_pre_branch_likely(observation);
        return;
    case Phase::FinalDrainBranchLikelyTaken:
        observe_pre_branch_likely_delay(observation);
        return;
    case Phase::OrdinaryAdvanceBranchSeen:
    case Phase::OrdinaryAdvanceDelaySeen:
        observe_pre_ordinary_advance_delay(observation);
        return;
    case Phase::OrdinaryAdvanceAddReady:
        observe_pre_ordinary_advance_add(observation);
        return;
    case Phase::OrdinaryAdvanceStoreReady:
        observe_pre_queue_read_store(observation);
        return;
    case Phase::ReturnBranchSeen:
        observe_pre_return_branch(observation);
        return;
    case Phase::ReturnBranchDelaySeen:
        observe_pre_return_branch_delay(observation);
        return;
    case Phase::CalleeEpilogue:
        if (observation.pc == 2039008U) {
            observe_pre_return_jr(observation);
            return;
        }
        if (pc_in_pre_set || observation.pc == 2038360U || observation.pc == 2038900U) {
            fail(Failure::ReplayedObservation);
            return;
        }
        pending_untracked_pc_ = observation.pc;
        pending_untracked_instruction_ = observation.instruction;
        pending_instruction_guest_step_ = observation.guest_step;
        pending_post_ = PendingPost::Untracked;
        return;
    case Phase::ReturnJrSeen:
        observe_pre_return_delay(observation);
        return;
    case Phase::ReturnDelaySeen:
        observe_pre_caller_resumed(observation);
        return;
    case Phase::CallerResumed:
        fail(Failure::EventOrderInvalid);
        return;
    default:
        return;
    }
}

// Retail queue-read scalar-write observer. Non-store observations only run the
// identity, ordinal and step gates and the pending-kind ordering check. Store
// observations (pc 2038360 / 2038900, or the queue-read address) are validated
// against the pending store and the published queue window, then folded into
// the queue-read aggregate.
void Slus21050FemainRetailConsumeProbe::observe_scalar_write(
    const Slus21050FemainRetailConsumeScalarWrite& observation) noexcept {
    if (!enabled_ || failure_ != Failure::None || phase_ == Phase::Failed || !bound_) {
        return;
    }
    constexpr std::uint32_t kQueueReadAddress = 5122104U;
    const std::uint32_t pc = observation.pc;
    const bool store_pc = pc == 2038360U || pc == 2038900U;
    if (!store_pc && observation.original_address != kQueueReadAddress &&
        observation.canonical_address != kQueueReadAddress) {
        if (!identity_matches(observation.runtime_epoch, observation.observation_epoch,
                              observation.invocation_serial,
                              observation.guest_execution_context_identity)) {
            fail(Failure::ObservationIdentityMismatch);
            return;
        }
        if (!event_ordinal_matches(observation.observation_ordinal)) {
            return;
        }
        const bool pending_matches =
            pending_post_ != PendingPost::None && observation.guest_step == pending_instruction_guest_step_;
        if (last_guest_step_valid_) {
            if (observation.guest_step < last_guest_step_ ||
                (!pending_matches && observation.guest_step == last_guest_step_)) {
                fail(Failure::EventOrderInvalid);
                return;
            }
        }
        last_guest_step_ = observation.guest_step;
        last_guest_step_valid_ = true;
        if (pending_post_ != PendingPost::None) {
            if (pending_matches) {
                return;
            }
            fail(Failure::EventOrderInvalid);
        }
        return;
    }

    if (phase_ == Phase::Complete) {
        if (observation_matches(observation.guest_step, observation.runtime_epoch,
                                observation.observation_epoch, observation.invocation_serial,
                                observation.observation_ordinal,
                                observation.guest_execution_context_identity, false)) {
            fail(Failure::ReplayedObservation);
        }
        return;
    }

    const std::uint32_t phase_value = static_cast<std::uint32_t>(phase_);
    const bool store_phase = phase_value == 6U || phase_value == 7U || phase_value == 10U || phase_value == 11U;
    const bool phase_6_7 = phase_value == 6U || phase_value == 7U;
    const bool phase_10_11 = phase_value == 10U || phase_value == 11U;

    if (!identity_matches(observation.runtime_epoch, observation.observation_epoch,
                          observation.invocation_serial, observation.guest_execution_context_identity)) {
        fail(Failure::ObservationIdentityMismatch);
        return;
    }
    if (!event_ordinal_matches(observation.observation_ordinal)) {
        return;
    }
    if (last_guest_step_valid_ &&
        (observation.guest_step < last_guest_step_ ||
         (!store_phase && observation.guest_step == last_guest_step_))) {
        fail(Failure::EventOrderInvalid);
        return;
    }
    last_guest_step_ = observation.guest_step;
    last_guest_step_valid_ = true;

    if (!store_phase) {
        fail(Failure::ReplayedObservation);
        return;
    }

    const std::uint32_t expected_pc = pending_queue_store_pc_;
    const std::uint64_t expected_pending = pending_queue_store_guest_step_;
    const bool pc_expected = (phase_6_7 && (expected_pc == 2038360U || expected_pc == 2038900U)) ||
                             (expected_pc == 2038900U && phase_10_11);
    if (!pc_expected) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    if (pc != expected_pc || observation.guest_step != expected_pending) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    if (pending_queue_store_write_observed_) {
        fail(Failure::ReplayedObservation);
        return;
    }
    if (observation.domain != guest::EeMemoryWriteDomain::MainRam) {
        fail(Failure::QueueReadDomainMismatch);
        return;
    }
    if (observation.original_address != kQueueReadAddress || observation.canonical_address != kQueueReadAddress) {
        fail(Failure::QueueReadAddressMismatch);
        return;
    }
    if (observation.width != 4U) {
        fail(Failure::QueueReadAddressMismatch);
        return;
    }
    if (!store_pc) {
        fail(Failure::QueueReadInstructionMismatch);
        return;
    }
    if (!observation.previous_value_available) {
        fail(Failure::QueueReadPreimageUnavailable);
        return;
    }

    const std::uint32_t queue_end = binding_.published_queue_end;
    const std::uint32_t queue_read_start = binding_.published_queue_read;
    const std::uint64_t expected_count = static_cast<std::uint64_t>((queue_end - queue_read_start) >> 3U);
    auto& aggregate = evidence_.queue_read_aggregate;
    if (aggregate.count == std::numeric_limits<std::uint64_t>::max()) {
        fail(Failure::QueueReadCountOverflow);
        return;
    }
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? queue_read_start : aggregate.last.value;
    if (observation.previous_value != expected_previous) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    if (observation.value != observation.previous_value + 8U) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    if (aggregate.count >= expected_count) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }
    if (queue_end < observation.value) {
        fail(Failure::QueueReadSequenceMismatch);
        return;
    }

    const Slus21050FemainRetailConsumeQueueReadWrite write{
        observation.guest_step, pc, observation.previous_value, observation.value};
    if (aggregate.count == 0U) {
        aggregate.first = write;
        aggregate.rolling_digest = 1469598103934665603ULL;
    }
    auto fold = [&aggregate](std::uint64_t value, unsigned bytes) {
        for (unsigned byte = 0U; byte != bytes; ++byte) {
            aggregate.rolling_digest = (aggregate.rolling_digest ^ ((value >> (byte * 8U)) & 255U)) * 1099511628211ULL;
        }
    };
    fold(write.guest_step, 8U);
    fold(write.pc, 4U);
    fold(write.previous_value, 4U);
    fold(write.value, 4U);
    aggregate.last = write;
    aggregate.count = aggregate.count + 1U;
    pending_queue_store_write_observed_ = true;
    if (pending_post_ == PendingPost::None) {
        phase_ = phase_6_7 ? Phase::CalleeActive : Phase::FinalDrainActiveLoadSeen;
    }
}

void Slus21050FemainRetailConsumeProbe::observe_post_instruction(
    const Slus21050FemainRetailConsumePostInstruction& observation) noexcept {
    if (!enabled_ || failure_ != Failure::None || phase_ == Phase::Failed || !bound_) {
        return;
    }
    const std::uint32_t executed_pc = observation.executed_pc;
    const bool post_known_pc_expr = (executed_pc == 2037624U || executed_pc == 2037960U || executed_pc == 2037964U || executed_pc == 2037984U || executed_pc == 2037988U || executed_pc == 2038032U || executed_pc == 2038036U || executed_pc == 2038372U || executed_pc == 2038376U || executed_pc == 2038704U || executed_pc == 2038708U || executed_pc == 2038712U || executed_pc == 2038716U || executed_pc == 2038820U || executed_pc == 2038824U || executed_pc == 2038892U || executed_pc == 2038896U || executed_pc == 2038900U || executed_pc == 2038904U || executed_pc == 2038908U || executed_pc == 2038912U || executed_pc == 2038916U || executed_pc == 2038920U || executed_pc == 2038924U || executed_pc == 2038928U || executed_pc == 2038932U || executed_pc == 2038936U || executed_pc == 2038944U || executed_pc == 2038948U || executed_pc == 2039008U || executed_pc == 2041340U || executed_pc == 2041344U || executed_pc == 2039012U || executed_pc == 2041348U || executed_pc == 2038360U);
    const bool post_complete = phase_ == Phase::Complete;
    if (post_complete) {
        if (!observation_matches(observation.guest_step, observation.runtime_epoch,
                                 observation.observation_epoch, observation.invocation_serial,
                                 observation.observation_ordinal,
                                 observation.guest_execution_context_identity, false)) {
            return;
        }
        if (post_known_pc_expr) {
            fail(Failure::ReplayedObservation);
        }
        return;
    }

    if (!identity_matches(observation.runtime_epoch, observation.observation_epoch,
                          observation.invocation_serial, observation.guest_execution_context_identity)) {
        fail(Failure::ObservationIdentityMismatch);
        return;
    }
    if (!event_ordinal_matches(observation.observation_ordinal)) {
        return;
    }
    const bool pending_present = pending_post_ != PendingPost::None;
    if (last_guest_step_valid_) {
        if (observation.guest_step < last_guest_step_ ||
            (!pending_present && observation.guest_step == last_guest_step_)) {
            fail(Failure::EventOrderInvalid);
            return;
        }
    }
    last_guest_step_ = observation.guest_step;
    last_guest_step_valid_ = true;

    if (observation.retirement_status != guest::EeStepStatus::Executed) {
        fail(Failure::InstructionNotRetired);
        return;
    }

    if (!pending_present) {
        if (post_known_pc_expr) {
            fail(Failure::ReplayedObservation);
        }
        return;
    }
    if (observation.guest_step != pending_instruction_guest_step_) {
        fail(Failure::EventOrderInvalid);
        return;
    }

    // Success transitions clear the pending instruction before moving phase.
    auto clear_pending = [this]() {
        pending_post_ = PendingPost::None;
        pending_instruction_guest_step_ = 0U;
        pending_untracked_pc_ = 0U;
        pending_untracked_instruction_ = 0U;
    };
    const auto& aggregate = evidence_.queue_read_aggregate;
    const std::uint32_t queue_read_start = binding_.published_queue_read;
    const std::uint32_t queue_end = binding_.published_queue_end;
    const std::uint32_t expected_previous =
        aggregate.count == 0U ? queue_read_start : aggregate.last.value;
    const bool write_observed = pending_queue_store_write_observed_;
    const bool delay_after = observation.executing_delay_slot_after;
    const std::uint32_t pc_after = observation.pc_after;
    const std::uint32_t next_pc_after = observation.next_pc_after;

    switch (pending_post_) {
    case PendingPost::Untracked:
        if (executed_pc != pending_untracked_pc_ || observation.instruction != pending_untracked_instruction_) {
            fail(Failure::EventOrderInvalid);
        }
        return;
    case PendingPost::Caller:
        if (executed_pc != 2041340U || observation.instruction != 201835998U || pc_after != 2041344U ||
            next_pc_after != 2037624U || !delay_after) {
            fail(Failure::CallTargetMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::DelaySeen;
        return;
    case PendingPost::CallerDelay:
        if (executed_pc != 2041344U || observation.instruction != 604307455U || pc_after != 2037624U ||
            next_pc_after != 2037628U || delay_after || observation.a0_after != 4294967295U) {
            fail(Failure::DelayStateMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::CalleeSeen;
        return;
    case PendingPost::Callee:
        if (executed_pc != 2037624U || observation.instruction != 666762976U || pc_after != 2037628U ||
            next_pc_after != 2037632U || delay_after || observation.a0_after != 4294967295U) {
            fail(Failure::CalleeStateMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::CalleeActive;
        return;
    case PendingPost::OrdinaryAdvanceBranch:
        if (executed_pc != ordinary_advance_branch_pc_ || observation.instruction != ordinary_advance_branch_word_ ||
            pc_after != ordinary_advance_delay_pc_ || next_pc_after != ordinary_advance_target_pc_ || !delay_after) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::OrdinaryAdvanceDelaySeen;
        return;
    case PendingPost::OrdinaryAdvanceDelay:
        if (executed_pc != ordinary_advance_delay_pc_ || observation.instruction != ordinary_advance_delay_word_ ||
            pc_after != ordinary_advance_target_pc_ || next_pc_after != ordinary_advance_target_pc_ + 4U ||
            delay_after) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = ordinary_advance_target_pc_ == 2038896U ? Phase::OrdinaryAdvanceAddReady
                                                          : Phase::OrdinaryAdvanceStoreReady;
        return;
    case PendingPost::OrdinaryAdvanceAdd:
        if (executed_pc != 2038896U || observation.instruction != 608305160U || pc_after != 2038900U ||
            next_pc_after != 2038904U || delay_after) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        if (observation.v0_after != expected_previous + 8U) {
            fail(Failure::QueueReadSequenceMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::OrdinaryAdvanceStoreReady;
        return;
    case PendingPost::QueueReadStore: {
        if (observation.instruction != 2944573896U || delay_after) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        if (pending_queue_store_pc_ != 2038360U && pending_queue_store_pc_ != 2038900U) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        if (pending_queue_store_pc_ != executed_pc) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        const std::uint32_t expected_pc_after = pending_queue_store_pc_ == 2038360U ? 2038364U : 2038904U;
        const std::uint32_t expected_next_pc_after = pending_queue_store_pc_ == 2038360U ? 2038368U : 2038908U;
        if (pc_after != expected_pc_after || next_pc_after != expected_next_pc_after) {
            fail(Failure::QueueReadInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = write_observed ? Phase::CalleeActive : Phase::QueueReadStoreWriteAwaited;
        return;
    }
    case PendingPost::FinalReadLoad:
        if (executed_pc != 2038892U || observation.instruction != 2407702984U || pc_after != 2038896U ||
            next_pc_after != 2038900U || delay_after) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        if (observation.v0_after != expected_previous) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainReadAddSeen;
        return;
    case PendingPost::FinalReadAdd:
        if (executed_pc != 2038896U || observation.instruction != 608305160U || pc_after != 2038900U ||
            next_pc_after != 2038904U || delay_after) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        if (observation.v0_after != expected_previous + 8U) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainReadStoreSeen;
        return;
    case PendingPost::FinalReadStore:
        if (executed_pc != 2038900U || observation.instruction != 2944573896U || pc_after != 2038904U ||
            next_pc_after != 2038908U || delay_after) {
            fail(Failure::FinalDrainInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = write_observed ? Phase::FinalDrainActiveLoadSeen : Phase::FinalDrainStoreWriteAwaited;
        return;
    case PendingPost::FinalActiveLoad:
        if (executed_pc != 2038904U || observation.instruction != 2474811840U || pc_after != 2038908U ||
            next_pc_after != 2038912U || delay_after || observation.v0_after != 0U) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainActiveBranchSeen;
        return;
    case PendingPost::FinalActiveBranch:
        if (executed_pc != 2038908U || observation.instruction != 339738631U || pc_after != 2038912U ||
            !delay_after || next_pc_after != 2038916U) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainActiveDelaySeen;
        return;
    case PendingPost::FinalActiveDelay:
        if (executed_pc != 2038912U || observation.instruction != 2407702988U || delay_after) {
            fail(Failure::FinalDrainStateMismatch);
            return;
        }
        if (pc_after != 2038916U || next_pc_after != 2038920U || observation.v0_after != queue_end) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainReady;
        return;
    case PendingPost::TerminalLoad: {
        const std::uint32_t expected_queue_read = final_drain_iteration_ ? queue_end : aggregate.last.value;
        if (executed_pc != 2038916U || observation.instruction != 2407899592U || pc_after != 2038920U ||
            next_pc_after != 2038924U || delay_after) {
            fail(Failure::TerminalLoadInstructionMismatch);
            return;
        }
        if (observation.a1_after != expected_queue_read || observation.v0_after != queue_end) {
            fail(Failure::TerminalLoadInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::TerminalLoadSeen;
        return;
    }
    case PendingPost::TerminalBranch: {
        if (executed_pc != 2038920U || observation.instruction != 279052293U || pc_after != 2038924U ||
            !delay_after) {
            fail(Failure::TerminalBranchPathMismatch);
            return;
        }
        if (next_pc_after != 2038944U && next_pc_after != 2038928U) {
            fail(Failure::TerminalBranchPathMismatch);
            return;
        }
        const bool taken = next_pc_after == 2038944U;
        if (final_drain_iteration_ != taken) {
            fail(Failure::TerminalBranchPathMismatch);
            return;
        }
        if (taken) {
            const auto& path = evidence_.terminal;
            if (!queue_count_reaches_end() || path.queue_read != queue_end || path.queue_end != queue_end ||
                path.transfers_active != 0U || !observation.boundary_valid) {
                fail(Failure::TerminalBranchStateMismatch);
                return;
            }
            evidence_.terminal.post_pc = 2038924U;
            evidence_.terminal.post_next_pc = 2038944U;
            evidence_.terminal.branch_taken = true;
            evidence_.terminal.boundary = observation.boundary;
            clear_pending();
            phase_ = Phase::TerminalTaken;
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainNonterminalDelaySeen;
        return;
    }
    case PendingPost::TerminalDelay:
        if (executed_pc != 2038924U || observation.instruction != 604176383U || pc_after != 2038944U ||
            next_pc_after != 2038948U || delay_after) {
            fail(Failure::TerminalDelayInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::ReturnBranchSeen;
        return;
    case PendingPost::FinalDrainNonterminalDelay:
        if (executed_pc != 2038924U || observation.instruction != 604176383U || pc_after != 2038928U ||
            next_pc_after != 2038932U || delay_after) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainConditionLoadSeen;
        return;
    case PendingPost::FinalDrainConditionLoad:
        if (executed_pc != 2038928U || observation.instruction != 2474811743U || pc_after != 2038932U ||
            next_pc_after != 2038936U || delay_after || observation.v0_after != 0U) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainBranchLikelySeen;
        return;
    case PendingPost::BranchLikely:
        if (executed_pc != 2038932U || observation.instruction != 1346436864U || pc_after != 2038936U ||
            next_pc_after != 2037912U || !delay_after) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::FinalDrainBranchLikelyTaken;
        return;
    case PendingPost::BranchLikelyDelay:
        if (executed_pc != 2038936U || observation.instruction != 2359492608U || pc_after != 2037912U ||
            next_pc_after != 2037916U || delay_after) {
            fail(Failure::FinalDrainPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::CalleeActive;
        return;
    case PendingPost::ReturnBranch:
        if (executed_pc != 2038944U || observation.instruction != 320995332U || pc_after != 2038948U ||
            next_pc_after != 2038964U || !delay_after) {
            fail(Failure::ReturnBranchPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::ReturnBranchDelaySeen;
        return;
    case PendingPost::ReturnBranchDelay:
        if (executed_pc != 2038948U || observation.instruction != 2409889792U || pc_after != 2038964U ||
            next_pc_after != 2038968U || delay_after) {
            fail(Failure::ReturnBranchPathMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::CalleeEpilogue;
        return;
    case PendingPost::ReturnJr:
        if (executed_pc != 2039008U || observation.instruction != 65011720U || pc_after != 2039012U ||
            next_pc_after != 2041348U || !delay_after || observation.ra_after != 2041348U) {
            fail(Failure::ReturnJrStateMismatch);
            return;
        }
        evidence_.return_path.post_pc = 2039012U;
        evidence_.return_path.post_next_pc = 2041348U;
        clear_pending();
        phase_ = Phase::ReturnJrSeen;
        return;
    case PendingPost::ReturnDelay:
        if (executed_pc != 2039012U || observation.instruction != 666698016U || pc_after != 2041348U ||
            next_pc_after != 2041352U || delay_after) {
            fail(Failure::ReturnDelayInstructionMismatch);
            return;
        }
        clear_pending();
        phase_ = Phase::ReturnDelaySeen;
        return;
    case PendingPost::CallerResume:
        if (executed_pc != 2041348U || observation.instruction != 1107296312U || pc_after != 2041352U ||
            next_pc_after != 2041356U || delay_after) {
            fail(Failure::CallerResumeStateMismatch);
            return;
        }
        evidence_.successful_return = true;
        clear_pending();
        phase_ = Phase::Complete;
        return;
    default:
        return;
    }
}

}  // namespace takedown::runtime
