#include "takedown/runtime/slus_21050_femain_renderer_retirement_adapter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

namespace takedown::runtime {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::string_view kDeliveryWitnessDomain =
    "slus-21050-femain-renderer-terminal-primary-delivery-v1";

constexpr guest::GuestAddress kMovieWorkAddress = 0x0051a870U;
constexpr std::size_t kMovieWorkBytes = 56U;
constexpr std::size_t kBufferRangeBytes = 1804288U;
constexpr guest::GuestAddress kHandoffAddress = 0x004e27f8U;
constexpr std::size_t kHandoffBytes = 72U;

[[nodiscard]] bool published_phase(const Slus21050FemainRendererRetirementAdapterPhase phase) noexcept {
    return phase != Slus21050FemainRendererRetirementAdapterPhase::ReceiptPending;
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        hash ^= (value >> (index * 8U)) & 0xFFU;
        hash *= kFnvPrime;
    }
}

void hash_text(std::uint64_t& hash, const std::string_view text) noexcept {
    hash_u64(hash, text.size());
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnvPrime;
    }
}

// Every identity the renderer worker reports must still describe the frame the
// owner published, so the evidence is re-derived from the live participants
// rather than trusted on its own authentication tag.
// RECOVERY: statement-for-statement identical to the original GIMPLE except that
// the original's entry block profile count is 1073741822 and this one is
// 1073741824 (a two-unit profile rounding difference); no source form tried
// changed it.
[[nodiscard]] bool source_evidence_matches(
    const Slus21050FemainRendererRetirementAdapterContext& context,
    const Slus21050FemainFrameGraphicsSourceEvidence& evidence,
    const Slus21050FemainFrameReservation& reservation,
    const std::uint64_t expected_player_identity,
    const std::uint64_t expected_player_session_generation,
    const Slus21050MovieFrameLease& expected_lease) noexcept {
    const auto& token = evidence.token;
    if (context.owner == nullptr || !context.owner->publication_pending() ||
        context.memory == nullptr || !token.published_reservation.has_value()) {
        return false;
    }
    if (evidence.authentication_tag == 0U ||
        evidence.authentication_tag !=
            authenticate_slus_21050_femain_frame_graphics_source_evidence(evidence)) {
        return false;
    }
    if (token.authentication_tag == 0U ||
        token.authentication_tag !=
            authenticate_slus_21050_femain_frame_graphics_source_token(token)) {
        return false;
    }
    if (!(token.source == kSlus21050FemainLiveExpectedSource)) {
        return false;
    }
    if (evidence.source_event_ordinal == 0U || evidence.raw_submission_event_ordinal == 0U ||
        evidence.packet_completion_event_ordinal == 0U ||
        evidence.source_event_ordinal >= evidence.raw_submission_event_ordinal ||
        evidence.raw_submission_event_ordinal >= evidence.packet_completion_event_ordinal) {
        return false;
    }
    if (token.emulation_thread_identity == 0U ||
        token.emulation_thread_identity != context.emulation_thread_identity) {
        return false;
    }
    if (token.owner_identity == 0U || token.owner_identity != context.owner->identity()) {
        return false;
    }
    if (token.player_identity == 0U || token.player_identity != expected_player_identity) {
        return false;
    }
    if (token.player_session_generation == 0U ||
        token.player_session_generation != expected_player_session_generation) {
        return false;
    }
    if (token.media_session_generation != expected_player_session_generation ||
        !(token.frame_lease == expected_lease)) {
        return false;
    }
    if (token.frame_generation == 0U || token.frame_generation != reservation.frame_generation ||
        token.media_ordinal != reservation.frame.media_ordinal ||
        token.source_sequence != reservation.frame.source_sequence ||
        token.source_checksum != reservation.frame.source_checksum ||
        token.ledger_before.ledger_identity != reservation.ledger_identity ||
        token.ledger_before.session_generation != reservation.session_generation) {
        return false;
    }
    if (token.published_reservation.has_value()) {
        const auto slot = static_cast<std::size_t>(reservation.slot);
        if (!(*token.published_reservation == reservation) ||
            token.ledger_before.state != Slus21050FemainFrameLedgerState::Active ||
            slot >= kSlus21050FemainFrameTransactionSlotCount ||
            token.ledger_before.slots[slot] != Slus21050FemainFrameLedgerSlotState::Published) {
            return false;
        }
    }
    const auto plan = context.owner->consumer_plan();
    const auto handoff = context.owner->handoff_token();
    if (!plan.has_value() || !handoff.has_value() || !(plan->layout == token.layout) ||
        plan->frame_generation != token.frame_generation ||
        plan->media_ordinal != token.media_ordinal ||
        plan->source_sequence != token.source_sequence ||
        plan->source_checksum != token.source_checksum ||
        plan->payload_digest != token.payload_digest || !plan->player_lease.has_value() ||
        !(*plan->player_lease == token.frame_lease) ||
        plan->timeline_ordinal != token.timeline_ordinal) {
        return false;
    }
    if (token.source_range.span_bytes == 0U ||
        token.source_range.span_bytes > plan->buffer_bytes ||
        (token.source_range.buffer != plan->first_buffer &&
         token.source_range.buffer != plan->second_buffer)) {
        return false;
    }
    if (!token.published_reservation.has_value()) {
        if (!(plan->work_mutation == token.work_mutation) ||
            !(plan->buffer_mutation == token.buffer_mutation) ||
            !(handoff == token.handoff)) {
            return false;
        }
    }
    if (token.published_reservation.has_value()) {
        auto expected_handoff = *handoff;
        expected_handoff.mutation = token.handoff.has_value()
                                        ? token.handoff->mutation
                                        : guest::EeMainRamMutationToken{};
        if (!token.handoff.has_value() || !(expected_handoff == *token.handoff)) {
            return false;
        }
    }
    if (!context.memory->validate_main_ram_mutation(token.work_mutation, kMovieWorkAddress,
                                                    kMovieWorkBytes) ||
        !context.memory->validate_main_ram_mutation(token.buffer_mutation, plan->first_buffer,
                                                    kBufferRangeBytes) ||
        !token.handoff.has_value() ||
        !context.memory->validate_main_ram_mutation(token.handoff->mutation, kHandoffAddress,
                                                    kHandoffBytes)) {
        return false;
    }
    const auto& marker = token.source_marker;
    return marker.token != 0U && marker.generation != 0U && marker.candidate_source != 0U &&
           marker.request.source != 0U && marker.request.chain_root != 0U;
}

[[nodiscard]] bool witness_matches(
    const Slus21050FemainRendererRetirementAdapterContext& context,
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept {
    if (witness.frame_id == 0U || witness.emulation_thread_identity == 0U ||
        witness.worker_epoch == 0U || witness.runtime_epoch == 0U ||
        witness.terminal_primary_event_ordinal == 0U || witness.primary_stream_digest == 0U ||
        witness.authentication_tag == 0U ||
        witness.authentication_tag !=
            authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(witness) ||
        witness.emulation_thread_identity != context.emulation_thread_identity ||
        witness.worker_epoch != context.worker_epoch ||
        witness.runtime_epoch != context.runtime_epoch ||
        witness.terminal_primary_event_ordinal <=
            witness.source_evidence.packet_completion_event_ordinal) {
        return false;
    }
    return validate_slus_21050_femain_renderer_terminal_primary_delivery_range(
        witness, witness.source_evidence);
}

} // namespace

std::uint64_t authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_text(hash, kDeliveryWitnessDomain);
    hash_u64(hash, witness.source_evidence.authentication_tag);
    hash_u64(hash, witness.frame_id);
    hash_u64(hash, witness.emulation_thread_identity);
    hash_u64(hash, witness.worker_epoch);
    hash_u64(hash, witness.runtime_epoch);
    hash_u64(hash, witness.terminal_primary_event_ordinal);
    hash_u64(hash, witness.primary_first_ordinal);
    hash_u64(hash, witness.primary_command_count);
    hash_u64(hash, witness.primary_stream_digest);
    return hash;
}

bool validate_slus_21050_femain_renderer_terminal_primary_delivery_range(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness,
    const Slus21050FemainFrameGraphicsSourceEvidence& source) noexcept {
    const auto packet_completion = source.packet_completion_event_ordinal;
    if (packet_completion == 0U || packet_completion == std::numeric_limits<std::uint64_t>::max() ||
        witness.primary_first_ordinal != packet_completion + 1U ||
        witness.primary_command_count == 0U ||
        witness.primary_first_ordinal >
            std::numeric_limits<std::uint64_t>::max() - witness.primary_command_count ||
        witness.terminal_primary_event_ordinal !=
            witness.primary_first_ordinal + witness.primary_command_count ||
        witness.primary_stream_digest == 0U) {
        return false;
    }
    return true;
}

std::string_view to_string(const Slus21050FemainRendererRetirementAdapterStatus status) noexcept {
    switch (status) {
    case Slus21050FemainRendererRetirementAdapterStatus::Ok:
        return "ok";
    case Slus21050FemainRendererRetirementAdapterStatus::InvalidState:
        return "invalid-state";
    case Slus21050FemainRendererRetirementAdapterStatus::InvalidBinding:
        return "invalid-binding";
    case Slus21050FemainRendererRetirementAdapterStatus::ContextMismatch:
        return "context-mismatch";
    case Slus21050FemainRendererRetirementAdapterStatus::ThreadMismatch:
        return "thread-mismatch";
    case Slus21050FemainRendererRetirementAdapterStatus::Pending:
        return "pending";
    case Slus21050FemainRendererRetirementAdapterStatus::WorkerFailure:
        return "worker-failure";
    case Slus21050FemainRendererRetirementAdapterStatus::RetirementRejected:
        return "retirement-rejected";
    }
    return "unknown";
}

std::uintptr_t Slus21050FemainRendererRetirementAdapter::object_identity(
    const void* const object) noexcept {
    return reinterpret_cast<std::uintptr_t>(object);
}

Slus21050FemainRendererRetirementAdapter::Result
Slus21050FemainRendererRetirementAdapter::make_result(const Status status) const noexcept {
    Result result{.status = status};
    if (completion_.has_value()) {
        result.completion = completion_;
    }
    if (retirement_.has_value()) {
        result.retirement = retirement_;
    }
    return result;
}

Slus21050FemainRendererRetirementAdapter::Result
Slus21050FemainRendererRetirementAdapter::fail(const Status status) noexcept {
    phase_ = Phase::Failed;
    return make_result(status);
}

bool Slus21050FemainRendererRetirementAdapter::thread_matches() const noexcept {
    return owning_thread_ != std::thread::id{} && owning_thread_ == std::this_thread::get_id();
}

bool Slus21050FemainRendererRetirementAdapter::context_identities_match(
    const Context& context) const noexcept {
    if (context.owner == nullptr || context.ledger == nullptr || context.memory == nullptr ||
        context.worker == nullptr) {
        return false;
    }
    return object_identity(context.owner) == owner_identity_ &&
           object_identity(context.ledger) == ledger_identity_address_ &&
           object_identity(context.memory) == memory_identity_ &&
           object_identity(context.worker) == worker_identity_ &&
           context.emulation_thread_identity != 0U &&
           context.emulation_thread_identity == emulation_thread_identity_ &&
           context.worker_epoch != 0U && context.worker_epoch == worker_epoch_ &&
           context.runtime_epoch != 0U && context.runtime_epoch == runtime_epoch_;
}

bool Slus21050FemainRendererRetirementAdapter::owner_and_ledger_match(
    const Context& context, const bool published, const bool validate_source_evidence) const noexcept {
    if (context.owner == nullptr || context.ledger == nullptr) {
        return false;
    }
    const auto current_owner_session = context.owner->player_session_generation();
    const auto current_lease = context.owner->lease();
    const auto snapshot = context.ledger->snapshot();
    const bool player_ok = context.owner->player_identity_matches() &&
                           context.owner->player_identity() != 0U &&
                           context.owner->player_identity() == owner_player_identity_;
    const bool session_ok = current_owner_session.has_value() &&
                            *current_owner_session == owner_player_session_generation_;
    const bool lease_ok =
        current_lease.has_value() && *current_lease == owner_frame_lease_;
    const bool ledger_ok = snapshot.ledger_identity != 0U && snapshot.ledger_identity == ledger_identity_;
    const bool ledger_session_ok = snapshot.session_generation != 0U &&
                                   snapshot.session_generation == ledger_session_generation_;
    if (!published) {
        return context_identities_match(context) && player_ok && session_ok && lease_ok &&
               context.owner->guest_commit_complete() && context.owner->occupied() &&
               !context.owner->publication_pending() && ledger_ok && ledger_session_ok &&
               reservation_.ledger_identity != 0U &&
               reservation_.ledger_identity == ledger_identity_ &&
               reservation_.session_generation != 0U &&
               reservation_.session_generation == ledger_session_generation_;
    }
    if (!(player_ok && session_ok && lease_ok && ledger_ok && ledger_session_ok)) {
        return false;
    }
    if (validate_source_evidence &&
        (!source_evidence_.has_value() ||
         !source_evidence_matches(context, *source_evidence_, reservation_,
                                  owner_player_identity_, owner_player_session_generation_,
                                  owner_frame_lease_))) {
        return false;
    }
    const auto current_reservation = context.owner->published_reservation();
    const auto current_ledger_identity = context.owner->published_ledger_identity();
    const auto current_ledger_session = context.owner->published_ledger_session_generation();
    const auto slot = static_cast<std::size_t>(reservation_.slot);
    if (!context.owner->publication_pending() || !current_reservation.has_value() ||
        !current_ledger_identity.has_value() || !current_ledger_session.has_value() ||
        !(*current_reservation == reservation_) ||
        *current_ledger_identity != ledger_identity_ ||
        *current_ledger_session != ledger_session_generation_ ||
        slot >= kSlus21050FemainFrameTransactionSlotCount) {
        return false;
    }
    return snapshot.slots[slot] == Slus21050FemainFrameLedgerSlotState::Published;
}

Slus21050FemainRendererRetirementAdapter::Result Slus21050FemainRendererRetirementAdapter::bind(
    const Context& context, const Witness& witness) noexcept {
    if (phase_ != Phase::Empty) {
        return make_result(Status::InvalidState);
    }
    if (context.owner == nullptr || context.ledger == nullptr || context.memory == nullptr ||
        context.worker == nullptr || context.emulation_thread_identity == 0U ||
        context.worker_epoch == 0U || context.runtime_epoch == 0U ||
        !witness_matches(context, witness)) {
        return make_result(Status::InvalidBinding);
    }
    const auto reservation = context.owner->published_reservation();
    const auto owner_ledger_identity = context.owner->published_ledger_identity();
    const auto owner_ledger_session = context.owner->published_ledger_session_generation();
    const auto owner_session = context.owner->player_session_generation();
    const auto owner_lease = context.owner->lease();
    const auto snapshot = context.ledger->snapshot();
    const auto slot = reservation.has_value() ? static_cast<std::size_t>(reservation->slot)
                                              : kSlus21050FemainFrameTransactionSlotCount;
    if (!context.owner->player_identity_matches() || !context.owner->publication_pending() ||
        !reservation.has_value() || !owner_ledger_identity.has_value() ||
        !owner_ledger_session.has_value() || !owner_session.has_value() ||
        !owner_lease.has_value() || context.owner->player_identity() == 0U ||
        owner_lease->session_generation == 0U || owner_lease->lease_generation == 0U ||
        reservation->ledger_identity == 0U || reservation->session_generation == 0U ||
        reservation->frame_generation == 0U ||
        *owner_ledger_identity != snapshot.ledger_identity ||
        *owner_ledger_session != snapshot.session_generation ||
        reservation->ledger_identity != *owner_ledger_identity ||
        reservation->session_generation != *owner_ledger_session ||
        reservation->session_generation != *owner_session ||
        owner_lease->session_generation != reservation->session_generation ||
        slot >= kSlus21050FemainFrameTransactionSlotCount ||
        snapshot.slots[slot] != Slus21050FemainFrameLedgerSlotState::Published) {
        return make_result(Status::InvalidBinding);
    }
    if (!source_evidence_matches(context, witness.source_evidence, *reservation,
                                 context.owner->player_identity(), *owner_session,
                                 *owner_lease)) {
        return make_result(Status::InvalidBinding);
    }
    owner_identity_ = object_identity(context.owner);
    ledger_identity_address_ = object_identity(context.ledger);
    memory_identity_ = object_identity(context.memory);
    worker_identity_ = object_identity(context.worker);
    emulation_thread_identity_ = context.emulation_thread_identity;
    owner_player_identity_ = context.owner->player_identity();
    owner_player_session_generation_ = *owner_session;
    owner_frame_lease_ = *owner_lease;
    reservation_ = *reservation;
    ledger_identity_ = snapshot.ledger_identity;
    ledger_session_generation_ = snapshot.session_generation;
    worker_epoch_ = context.worker_epoch;
    runtime_epoch_ = context.runtime_epoch;
    frame_id_ = witness.frame_id;
    terminal_primary_event_ordinal_ = witness.terminal_primary_event_ordinal;
    primary_stream_digest_ = witness.primary_stream_digest;
    source_evidence_ = witness.source_evidence;
    owning_thread_ = std::this_thread::get_id();
    phase_ = Phase::Bound;
    return make_result(Status::Ok);
}

Slus21050FemainRendererRetirementAdapter::Result
Slus21050FemainRendererRetirementAdapter::submit(const Context& context) noexcept {
    if (phase_ == Phase::Empty) {
        return make_result(Status::InvalidState);
    }
    if (!thread_matches()) {
        return fail(Status::ThreadMismatch);
    }
    if (phase_ != Phase::Bound) {
        return make_result(Status::InvalidState);
    }
    if (!context_identities_match(context) || !owner_and_ledger_match(context, true, true)) {
        return fail(Status::ContextMismatch);
    }
    try {
        completion_future_ = context.worker->request_frame_completion(frame_id_);
    } catch (...) {
        return fail(Status::WorkerFailure);
    }
    phase_ = Phase::Submitted;
    return make_result(Status::Pending);
}

Slus21050FemainRendererRetirementAdapter::Result Slus21050FemainRendererRetirementAdapter::retire(
    const Context& context, const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept {
    if (phase_ == Phase::Empty || phase_ == Phase::Retired || phase_ == Phase::Failed) {
        return make_result(Status::InvalidState);
    }
    if (!thread_matches()) {
        return fail(Status::ThreadMismatch);
    }
    if (!context_identities_match(context)) {
        return fail(Status::ContextMismatch);
    }
    if (phase_ == Phase::Bound) {
        return make_result(Status::InvalidState);
    }
    if (phase_ == Phase::Submitted) {
        if (!owner_and_ledger_match(context, true, false)) {
            return fail(Status::ContextMismatch);
        }
        return make_result(Status::Pending);
    }
    const bool published = published_phase(phase_);
    if (!owner_and_ledger_match(context, published, false)) {
        return fail(Status::ContextMismatch);
    }
    const auto retirement = context.owner->retire_with_ledger(*context.ledger, boundaries);
    retirement_ = retirement;
    if (retirement.status == Slus21050FemainMovieTransactionStatus::Committed) {
        phase_ = Phase::Retired;
        return make_result(Status::Ok);
    }
    if (retirement.status == Slus21050FemainMovieTransactionStatus::CompletionPending) {
        phase_ = Phase::ReceiptPending;
        return make_result(Status::Pending);
    }
    if (published) {
        phase_ = Phase::CompletionReady;
        return make_result(Status::RetirementRejected);
    }
    return fail(Status::RetirementRejected);
}

Slus21050FemainRendererRetirementAdapter::Result
Slus21050FemainRendererRetirementAdapter::poll(const Context& context) noexcept {
    if (phase_ == Phase::Empty) {
        return make_result(Status::InvalidState);
    }
    if (!thread_matches()) {
        return fail(Status::ThreadMismatch);
    }
    if (phase_ != Phase::Submitted) {
        return make_result(Status::InvalidState);
    }
    if (!context_identities_match(context) || !owner_and_ledger_match(context, true, false)) {
        return fail(Status::ContextMismatch);
    }
    if (!completion_future_.valid()) {
        return fail(Status::WorkerFailure);
    }
    try {
        const auto wait = completion_future_.wait_for(std::chrono::milliseconds(0));
        if (wait == std::future_status::timeout) {
            return make_result(Status::Pending);
        }
        if (wait != std::future_status::ready) {
            return fail(Status::WorkerFailure);
        }
        const auto completion = completion_future_.get();
        if (completion.frame_id != frame_id_ || completion.frame_id == 0U ||
            completion.completed_serial == 0U) {
            return fail(Status::WorkerFailure);
        }
        completion_ = completion;
        phase_ = Phase::CompletionReady;
        return make_result(Status::Ok);
    } catch (...) {
        return fail(Status::WorkerFailure);
    }
}

} // namespace takedown::runtime
