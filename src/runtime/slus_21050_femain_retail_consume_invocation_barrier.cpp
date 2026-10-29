#include "takedown/runtime/slus_21050_femain_retail_consume_invocation_barrier.hpp"

#include "takedown/runtime/slus_21050_femain_live_transaction.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace takedown::runtime {

namespace {

// Runtime epochs are never reused within a process; zero means exhausted.
[[nodiscard]] std::uint64_t allocate_runtime_epoch() noexcept {
    static std::atomic<std::uint64_t> next_epoch{1U};
    auto candidate = next_epoch.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next_epoch.compare_exchange_weak(candidate, candidate + 1U,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0U;
}

} // namespace

Slus21050FemainRetailConsumeCompletionWitness::Slus21050FemainRetailConsumeCompletionWitness(
    const Slus21050FemainMoviePublicationPacket& publication_packet,
    const Slus21050FemainRetailConsumeEvidence& evidence,
    const Slus21050FemainRetailConsumeObservationIdentity& identity) noexcept
    : publication_packet_(publication_packet), evidence_(evidence), identity_(identity),
      valid_(true) {}

Slus21050FemainRetailConsumeCompletionWitness::Slus21050FemainRetailConsumeCompletionWitness(
    Slus21050FemainRetailConsumeCompletionWitness&& other) noexcept
    : publication_packet_(other.publication_packet_), evidence_(other.evidence_),
      identity_(other.identity_), valid_(other.valid_) {
    other.publication_packet_ = {};
    other.evidence_ = {};
    other.identity_ = {};
    other.valid_ = false;
}

Slus21050FemainRetailConsumeCompletionWitness&
Slus21050FemainRetailConsumeCompletionWitness::operator=(
    Slus21050FemainRetailConsumeCompletionWitness&& other) noexcept {
    if (this != &other) {
        publication_packet_ = other.publication_packet_;
        evidence_ = other.evidence_;
        identity_ = other.identity_;
        valid_ = other.valid_;
        other.publication_packet_ = {};
        other.evidence_ = {};
        other.identity_ = {};
        other.valid_ = false;
    }
    return *this;
}

std::string_view to_string(const Slus21050FemainRetailConsumeInvocationBarrierPhase phase) noexcept {
    switch (phase) {
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Disabled:
        return "disabled";
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Idle:
        return "idle";
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Armed:
        return "armed";
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Active:
        return "active";
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Complete:
        return "complete";
    case Slus21050FemainRetailConsumeInvocationBarrierPhase::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(
    const Slus21050FemainRetailConsumeInvocationBarrierFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::None:
        return "none";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::Disabled:
        return "disabled";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::RuntimeIdentityExhausted:
        return "runtime-identity-exhausted";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::InvocationSerialExhausted:
        return "invocation-serial-exhausted";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::InvalidArmIdentity:
        return "invalid-arm-identity";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::InvalidExecutionContext:
        return "invalid-execution-context";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::ReplayedArm:
        return "replayed-arm";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::AdapterNotBound:
        return "adapter-not-bound";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::ExecutionContextMismatch:
        return "execution-context-mismatch";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::CallerInstructionMismatch:
        return "caller-instruction-mismatch";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::InstructionNotRetired:
        return "instruction-not-retired";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::AdapterFailed:
        return "adapter-failed";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::CompletionBoundaryMismatch:
        return "completion-boundary-mismatch";
    case Slus21050FemainRetailConsumeInvocationBarrierFailure::IncompleteInvocation:
        return "incomplete-invocation";
    }
    return "unknown";
}

Slus21050FemainRetailConsumeInvocationBarrier::Slus21050FemainRetailConsumeInvocationBarrier(
    const bool enabled) noexcept
    : enabled_(enabled), phase_(enabled ? Phase::Idle : Phase::Disabled),
      failure_(enabled ? Failure::None : Failure::Disabled),
      runtime_epoch_(enabled ? allocate_runtime_epoch() : 0U) {
    if (enabled_ && runtime_epoch_ == 0U) {
        fail(Failure::RuntimeIdentityExhausted);
    }
}

Slus21050FemainRetailConsumeInvocationBarrier::Slus21050FemainRetailConsumeInvocationBarrier(
    Slus21050FemainRetailConsumeInvocationBarrier&& other) noexcept
    : enabled_(other.enabled_), phase_(other.phase_), failure_(other.failure_),
      runtime_epoch_(other.runtime_epoch_), observation_epoch_(other.observation_epoch_),
      next_invocation_serial_(other.next_invocation_serial_),
      publication_packet_(other.publication_packet_),
      binding_confirmed_(other.binding_confirmed_),
      confirmed_adapter_(other.confirmed_adapter_),
      completion_witness_taken_(other.completion_witness_taken_), identity_(other.identity_),
      execution_context_(other.execution_context_) {
    other.enabled_ = false;
    other.phase_ = Phase::Disabled;
    other.failure_ = Failure::Disabled;
    other.runtime_epoch_ = 0U;
    other.observation_epoch_ = 0U;
    other.next_invocation_serial_ = 0U;
    other.publication_packet_ = {};
    other.binding_confirmed_ = false;
    other.confirmed_adapter_ = nullptr;
    other.completion_witness_taken_ = false;
    other.identity_ = {};
    other.execution_context_ = {};
}

Slus21050FemainRetailConsumeInvocationBarrier&
Slus21050FemainRetailConsumeInvocationBarrier::operator=(
    Slus21050FemainRetailConsumeInvocationBarrier&& other) noexcept {
    if (this != &other) {
        enabled_ = other.enabled_;
        phase_ = other.phase_;
        failure_ = other.failure_;
        runtime_epoch_ = other.runtime_epoch_;
        observation_epoch_ = other.observation_epoch_;
        next_invocation_serial_ = other.next_invocation_serial_;
        publication_packet_ = other.publication_packet_;
        binding_confirmed_ = other.binding_confirmed_;
        confirmed_adapter_ = other.confirmed_adapter_;
        completion_witness_taken_ = other.completion_witness_taken_;
        identity_ = other.identity_;
        execution_context_ = other.execution_context_;
        other.enabled_ = false;
        other.phase_ = Phase::Disabled;
        other.failure_ = Failure::Disabled;
        other.runtime_epoch_ = 0U;
        other.observation_epoch_ = 0U;
        other.next_invocation_serial_ = 0U;
        other.publication_packet_ = {};
        other.binding_confirmed_ = false;
        other.confirmed_adapter_ = nullptr;
        other.completion_witness_taken_ = false;
        other.identity_ = {};
        other.execution_context_ = {};
    }
    return *this;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::valid_context(
    const ExecutionContext& context) noexcept {
    return context.thread_id > 0 && !context.callback_active && context.callback_identity == 0U;
}

std::uint64_t Slus21050FemainRetailConsumeInvocationBarrier::context_identity(
    const ExecutionContext& context) noexcept {
    if (!valid_context(context)) {
        return 0U;
    }
    return static_cast<std::uint64_t>(context.thread_id);
}

void Slus21050FemainRetailConsumeInvocationBarrier::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None || failure_ == Failure::Disabled) {
        failure_ = failure;
    }
    phase_ = Phase::Failed;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::arm(
    const Slus21050FemainMoviePublicationPacket& packet,
    const std::uint64_t observation_epoch) noexcept {
    if (!enabled_) {
        failure_ = Failure::Disabled;
        return false;
    }
    if (phase_ != Phase::Idle) {
        fail(Failure::ReplayedArm);
        return false;
    }
    if (runtime_epoch_ == 0U) {
        fail(Failure::RuntimeIdentityExhausted);
        return false;
    }
    if (packet.owner_identity == 0U || packet.publication_serial == 0U ||
        packet.player_identity == 0U || packet.player_session_generation == 0U ||
        packet.ledger_identity == 0U || packet.ledger_session_generation == 0U ||
        packet.published_reservation.ledger_identity != packet.ledger_identity ||
        packet.published_reservation.session_generation != packet.ledger_session_generation ||
        packet.published_reservation.frame_generation == 0U || observation_epoch == 0U) {
        fail(Failure::InvalidArmIdentity);
        return false;
    }
    if (next_invocation_serial_ == 0U ||
        next_invocation_serial_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(Failure::InvocationSerialExhausted);
        return false;
    }
    publication_packet_ = packet;
    observation_epoch_ = observation_epoch;
    binding_confirmed_ = false;
    confirmed_adapter_ = nullptr;
    completion_witness_taken_ = false;
    identity_ = {};
    execution_context_ = {};
    phase_ = Phase::Armed;
    failure_ = Failure::None;
    return true;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::observe_dispatch(
    const guest::GuestAddress pc, const std::uint32_t instruction,
    const ExecutionContext& context, const Attachment attachment) noexcept {
    if (phase_ != Phase::Armed && phase_ != Phase::Active) {
        return phase_ == Phase::Idle || phase_ == Phase::Complete;
    }
    if (attachment == Attachment::Failed) {
        fail(Failure::AdapterFailed);
        return false;
    }
    if (phase_ == Phase::Armed && pc != kSlus21050FemainRetailConsumeCallerPc) {
        return true;
    }
    if (!valid_context(context)) {
        fail(Failure::InvalidExecutionContext);
        return false;
    }
    if (phase_ == Phase::Active && !(context == execution_context_)) {
        fail(Failure::ExecutionContextMismatch);
        return false;
    }
    if (phase_ == Phase::Armed) {
        if (instruction != kSlus21050FemainRetailConsumeCallerWord) {
            fail(Failure::CallerInstructionMismatch);
            return false;
        }
        execution_context_ = context;
        identity_ = Identity{runtime_epoch_, observation_epoch_, next_invocation_serial_++,
                             context_identity(context)};
        phase_ = Phase::Active;
        return true;
    }
    if (attachment != Attachment::Bound) {
        fail(Failure::AdapterNotBound);
        return false;
    }
    return true;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::confirm_binding(
    const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept {
    if (phase_ != Phase::Active) {
        return false;
    }
    if (adapter.attachment() != Attachment::Bound || !identity_.valid() ||
        !(adapter.observation_identity() == identity_) ||
        adapter.bound_publication_serial() != publication_packet_.publication_serial) {
        fail(adapter.attachment() == Attachment::Failed ? Failure::AdapterFailed
                                                        : Failure::AdapterNotBound);
        return false;
    }
    binding_confirmed_ = true;
    confirmed_adapter_ = &adapter;
    return true;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::observe_scalar_step(
    const guest::EeStepResult& result,
    const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept {
    if (phase_ != Phase::Active) {
        return phase_ == Phase::Complete && confirmed_adapter_ == &adapter;
    }
    if (!binding_confirmed_) {
        fail(Failure::AdapterNotBound);
        return false;
    }
    if (confirmed_adapter_ != &adapter) {
        fail(Failure::AdapterNotBound);
        return false;
    }
    if (result.status != guest::EeStepStatus::Executed) {
        fail(Failure::InstructionNotRetired);
        return false;
    }
    if (!(adapter.observation_identity() == identity_)) {
        fail(Failure::AdapterNotBound);
        return false;
    }
    if (adapter.attachment() == Attachment::Failed) {
        fail(Failure::AdapterFailed);
        return false;
    }
    if (adapter.attachment() == Attachment::Complete) {
        if (!adapter.probe().result().complete()) {
            fail(Failure::AdapterFailed);
            return false;
        }
        if (result.instruction.pc != kSlus21050FemainRetailCallerResumePc ||
            result.instruction.word != kSlus21050FemainRetailCallerResumeWord) {
            fail(Failure::CompletionBoundaryMismatch);
            return false;
        }
        phase_ = Phase::Complete;
        return true;
    }
    if (adapter.attachment() != Attachment::Bound) {
        fail(Failure::AdapterNotBound);
        return false;
    }
    return true;
}

// RECOVERY: GCC lays out the nullopt return for an incomplete probe result one block
// earlier than the original (statement placement only, no behavioural difference).
std::optional<Slus21050FemainRetailConsumeCompletionWitness>
Slus21050FemainRetailConsumeInvocationBarrier::take_completion_witness(
    const Slus21050FemainRetailConsumeObservationAdapter& adapter) noexcept {
    if (phase_ != Phase::Complete || !binding_confirmed_ || completion_witness_taken_ ||
        confirmed_adapter_ != &adapter || !identity_.valid() ||
        adapter.attachment() != Attachment::Complete ||
        !(adapter.observation_identity() == identity_) ||
        adapter.bound_publication_serial() != publication_packet_.publication_serial) {
        return std::nullopt;
    }
    const auto result = adapter.probe().result();
    if (!result.complete()) {
        return std::nullopt;
    }
    const auto& evidence = *result.evidence;
    const auto& binding = evidence.binding;
    const auto& queue = publication_packet_.queue;
    const bool binding_matches =
        binding.owner.owner_identity == publication_packet_.owner_identity &&
        binding.owner.player_identity == publication_packet_.player_identity &&
        binding.owner.player_session_generation == publication_packet_.player_session_generation &&
        binding.owner.ledger_identity == publication_packet_.ledger_identity &&
        binding.owner.ledger_session_generation == publication_packet_.ledger_session_generation &&
        binding.published_reservation == publication_packet_.published_reservation &&
        binding.publication_serial == publication_packet_.publication_serial &&
        binding.runtime_epoch == identity_.runtime_epoch &&
        binding.observation_epoch == identity_.observation_epoch &&
        binding.invocation_serial == identity_.invocation_serial &&
        binding.guest_execution_context_identity == identity_.guest_execution_context_identity &&
        binding.published_queue_read == queue.queue_read &&
        binding.published_queue_end == queue.queue_end &&
        binding.published_buffer_index == queue.buffer_index &&
        binding.published_queue_base == queue.queue_base;
    if (!binding_matches) {
        return std::nullopt;
    }
    Slus21050FemainRetailConsumeCompletionWitness witness(publication_packet_, evidence,
                                                          identity_);
    completion_witness_taken_ = true;
    return witness;
}

void Slus21050FemainRetailConsumeInvocationBarrier::reject(const Failure failure) noexcept {
    if (phase_ == Phase::Idle || phase_ == Phase::Armed || phase_ == Phase::Active) {
        fail(failure != Failure::None ? failure : Failure::AdapterFailed);
    }
}

bool Slus21050FemainRetailConsumeInvocationBarrier::reset() noexcept {
    if (!enabled_) {
        return false;
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Complete) {
        return false;
    }
    observation_epoch_ = 0U;
    publication_packet_ = {};
    binding_confirmed_ = false;
    confirmed_adapter_ = nullptr;
    completion_witness_taken_ = false;
    identity_ = {};
    execution_context_ = {};
    failure_ = Failure::None;
    phase_ = Phase::Idle;
    return true;
}

bool Slus21050FemainRetailConsumeInvocationBarrier::release_cancelled_publication(
    const Slus21050FemainLiveCancellationWitness& witness) noexcept {
    if (!enabled_ || (phase_ != Phase::Armed && phase_ != Phase::Active &&
                      phase_ != Phase::Failed)) {
        return false;
    }
    if (!witness.matches(publication_packet_)) {
        return false;
    }
    observation_epoch_ = 0U;
    publication_packet_ = {};
    binding_confirmed_ = false;
    confirmed_adapter_ = nullptr;
    completion_witness_taken_ = false;
    identity_ = {};
    execution_context_ = {};
    failure_ = Failure::None;
    phase_ = Phase::Idle;
    return true;
}

} // namespace takedown::runtime
