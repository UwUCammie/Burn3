#include "takedown/runtime/slus_21050_femain_packet_drain_join.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= kFnvPrime;
    }
}

void hash_bool(std::uint64_t& hash, const bool value) noexcept {
    hash_u64(hash, value ? 1U : 0U);
}

void hash_boundary(std::uint64_t& hash, const Slus21050FemainFrameGuestBoundary& boundary) noexcept {
    hash_u64(hash, boundary.guest_vblank);
    hash_u64(hash, boundary.audio_sample);
}

void hash_reservation(std::uint64_t& hash, const Slus21050FemainFrameReservation& reservation) noexcept {
    hash_u64(hash, reservation.ledger_identity);
    hash_u64(hash, reservation.session_generation);
    hash_u64(hash, reservation.frame_generation);
    hash_u64(hash, reservation.slot);
    hash_u64(hash, reservation.frame.media_ordinal);
    hash_u64(hash, reservation.frame.source_sequence);
    hash_u64(hash, reservation.frame.source_checksum);
    hash_boundary(hash, reservation.frame.produced_at);
}

void hash_call(std::uint64_t& hash, const Slus21050FemainRetailConsumeCallContext& call) noexcept {
    hash_u64(hash, call.guest_step);
    hash_boundary(hash, call.boundary);
    hash_u64(hash, call.queue_read);
    hash_u64(hash, call.queue_end);
    hash_u64(hash, call.transfers_active);
    hash_u64(hash, call.a0);
    hash_u64(hash, call.t9);
}

void hash_packet(std::uint64_t& hash, const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    hash_u64(hash, packet.owner_identity);
    hash_u64(hash, packet.publication_serial);
    hash_u64(hash, packet.player_identity);
    hash_u64(hash, packet.player_session_generation);
    hash_u64(hash, packet.ledger_identity);
    hash_u64(hash, packet.ledger_session_generation);
    hash_reservation(hash, packet.published_reservation);
    const auto& plan = packet.plan;
    hash_u64(hash, plan.layout_generation);
    hash_u64(hash, plan.first_buffer);
    hash_u64(hash, plan.second_buffer);
    hash_u64(hash, plan.buffer_bytes);
    hash_u64(hash, plan.frame_generation);
    hash_u64(hash, plan.media_ordinal);
    hash_u64(hash, plan.source_sequence);
    hash_u64(hash, plan.source_checksum);
    hash_u64(hash, plan.payload_digest);
    hash_bool(hash, plan.player_lease.has_value());
    if (plan.player_lease.has_value()) {
        hash_u64(hash, plan.player_lease->session_generation);
        hash_u64(hash, plan.player_lease->lease_generation);
    }
    hash_bool(hash, plan.timeline_ordinal.has_value());
    if (plan.timeline_ordinal.has_value()) {
        hash_u64(hash, *plan.timeline_ordinal);
    }
    hash_u64(hash, packet.queue.queue_read);
    hash_u64(hash, packet.queue.queue_end);
    hash_u64(hash, packet.queue.transfers_active);
    hash_u64(hash, packet.queue.buffer_index);
    hash_u64(hash, packet.queue.queue_base);
}

void hash_binding(std::uint64_t& hash, const Slus21050FemainRetailConsumeBinding& binding) noexcept {
    hash_u64(hash, binding.owner.owner_identity);
    hash_u64(hash, binding.owner.player_identity);
    hash_u64(hash, binding.owner.player_session_generation);
    hash_u64(hash, binding.owner.ledger_identity);
    hash_u64(hash, binding.owner.ledger_session_generation);
    hash_reservation(hash, binding.published_reservation);
    hash_u64(hash, binding.publication_serial);
    hash_u64(hash, binding.runtime_epoch);
    hash_u64(hash, binding.observation_epoch);
    hash_u64(hash, binding.invocation_serial);
    hash_u64(hash, binding.guest_execution_context_identity);
    hash_u64(hash, binding.published_queue_read);
    hash_u64(hash, binding.published_queue_end);
    hash_u64(hash, binding.published_buffer_index);
    hash_u64(hash, binding.published_queue_base);
}

void hash_queue_write(std::uint64_t& hash,
                      const Slus21050FemainRetailConsumeQueueReadWrite& write) noexcept {
    hash_u64(hash, write.guest_step);
    hash_u64(hash, write.pc);
    hash_u64(hash, write.previous_value);
    hash_u64(hash, write.value);
}

void hash_consume(std::uint64_t& hash, const Slus21050FemainRetailConsumeEvidence& consume) noexcept {
    hash_binding(hash, consume.binding);
    hash_call(hash, consume.call);
    hash_call(hash, consume.callee);
    hash_u64(hash, consume.queue_read_aggregate.count);
    hash_queue_write(hash, consume.queue_read_aggregate.first);
    hash_queue_write(hash, consume.queue_read_aggregate.last);
    hash_u64(hash, consume.queue_read_aggregate.rolling_digest);
    hash_u64(hash, consume.terminal.guest_step);
    hash_boundary(hash, consume.terminal.boundary);
    hash_u64(hash, consume.terminal.queue_read);
    hash_u64(hash, consume.terminal.queue_end);
    hash_u64(hash, consume.terminal.transfers_active);
    hash_u64(hash, consume.terminal.post_pc);
    hash_u64(hash, consume.terminal.post_next_pc);
    hash_bool(hash, consume.terminal.branch_taken);
    hash_u64(hash, consume.return_path.guest_step);
    hash_u64(hash, consume.return_path.return_address);
    hash_u64(hash, consume.return_path.post_pc);
    hash_u64(hash, consume.return_path.post_next_pc);
    hash_u64(hash, consume.return_path.caller_resume_guest_step);
    hash_bool(hash, consume.successful_return);
}

bool publication_shape_valid(const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    const auto& reservation = packet.published_reservation;
    const auto& plan = packet.plan;
    const auto& queue = packet.queue;
    return packet.owner_identity != 0U && packet.publication_serial != 0U &&
           packet.player_identity != 0U && packet.player_session_generation != 0U &&
           packet.ledger_identity != 0U && packet.ledger_session_generation != 0U &&
           packet.ledger_identity == reservation.ledger_identity &&
           packet.ledger_session_generation == reservation.session_generation &&
           reservation.frame_generation != 0U &&
           reservation.slot < kSlus21050FemainFrameTransactionSlotCount &&
           reservation.frame.source_checksum != 0U && plan.layout_generation != 0U &&
           plan.first_buffer != 0U && plan.second_buffer != 0U && plan.buffer_bytes != 0U &&
           reservation.frame_generation == plan.frame_generation &&
           plan.media_ordinal == reservation.frame.media_ordinal &&
           plan.source_sequence == reservation.frame.source_sequence &&
           reservation.frame.source_checksum == plan.source_checksum && plan.payload_digest != 0U &&
           plan.player_lease.has_value() && plan.player_lease->session_generation != 0U &&
           plan.player_lease->lease_generation != 0U && queue.queue_read != 0U &&
           queue.queue_end != 0U && queue.queue_read < queue.queue_end && queue.queue_base != 0U &&
           queue.buffer_index <= 1U;
}

bool source_matches_publication(const Slus21050FemainSerializedSourceBoundary& source,
                                const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    const auto& evidence = source.source_evidence;
    const auto& token = evidence.token;
    if (!evidence.resolver_proof.has_value()) {
        return false;
    }
    const auto& proof = *evidence.resolver_proof;
    const auto& candidate = proof.candidate;
    if (!validate_slus_21050_femain_live_resolver_source_proof(proof)) {
        return false;
    }
    return token.published_reservation.has_value() &&
           *token.published_reservation == packet.published_reservation &&
           token.owner_identity == packet.owner_identity &&
           token.player_identity == packet.player_identity &&
           token.player_session_generation == packet.player_session_generation &&
           token.frame_generation == packet.plan.frame_generation &&
           token.media_ordinal == packet.plan.media_ordinal &&
           token.source_sequence == packet.plan.source_sequence &&
           token.source_checksum == packet.plan.source_checksum &&
           token.payload_digest == packet.plan.payload_digest &&
           token.layout.generation == packet.plan.layout_generation &&
           packet.plan.player_lease.has_value() && token.frame_lease == *packet.plan.player_lease &&
           token.timeline_ordinal == packet.plan.timeline_ordinal &&
           token.ledger_before.ledger_identity == packet.ledger_identity &&
           token.ledger_before.session_generation == packet.ledger_session_generation &&
           token.ledger_before.proof_valid && !token.ledger_before.overflowed &&
           packet.published_reservation.slot < token.ledger_before.slots.size() &&
           token.ledger_before.slots[packet.published_reservation.slot] ==
               Slus21050FemainFrameLedgerSlotState::Published &&
           candidate.publication_serial == packet.publication_serial &&
           packet.player_session_generation ==
               candidate.player_session_generation &&
           packet.plan.frame_generation == candidate.frame_generation &&
           candidate.source == token.source_marker.request.source &&
           candidate.expected_quadword ==
               token.source_marker.request.expected_quadword &&
           proof.publication_queue == packet.queue &&
           proof.matched_source_qwc ==
               candidate.source_qwc &&
           proof.matched_source_qwc != 0U &&
           proof.matched_span_digest != 0U;
}

bool consume_queue_sequence_valid(const Slus21050FemainRetailConsumeEvidence& evidence,
                                  const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    const auto read = packet.queue.queue_read;
    const auto end = packet.queue.queue_end;
    if (read >= end || ((end - read) & 7U) != 0U) {
        return false;
    }
    const unsigned long long expected_count = (end - read) >> 3U;
    const auto& aggregate = evidence.queue_read_aggregate;
    return aggregate.count == expected_count && aggregate.first.previous_value == read &&
           aggregate.first.value == read + 8U && aggregate.last.previous_value == end - 8U &&
           aggregate.last.value == end && aggregate.rolling_digest != 0U &&
           evidence.call.guest_step <= evidence.callee.guest_step &&
           evidence.callee.guest_step <= aggregate.first.guest_step &&
           aggregate.first.guest_step <= aggregate.last.guest_step &&
           aggregate.last.guest_step <= evidence.terminal.guest_step &&
           evidence.terminal.guest_step <= evidence.return_path.guest_step &&
           evidence.return_path.guest_step <= evidence.return_path.caller_resume_guest_step;
}

bool binding_matches_publication(const Slus21050FemainRetailConsumeBinding& binding,
                                 const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    return binding.owner.owner_identity == packet.owner_identity &&
           binding.owner.player_identity == packet.player_identity &&
           binding.owner.player_session_generation == packet.player_session_generation &&
           binding.owner.ledger_identity == packet.ledger_identity &&
           binding.owner.ledger_session_generation == packet.ledger_session_generation &&
           binding.published_reservation == packet.published_reservation &&
           binding.publication_serial == packet.publication_serial && binding.runtime_epoch != 0U &&
           binding.observation_epoch != 0U && binding.invocation_serial != 0U &&
           binding.guest_execution_context_identity != 0U &&
           binding.published_queue_read == packet.queue.queue_read &&
           binding.published_queue_end == packet.queue.queue_end &&
           binding.published_buffer_index == packet.queue.buffer_index &&
           binding.published_queue_base == packet.queue.queue_base;
}

} // namespace

std::string_view to_string(const Slus21050FemainPacketDrainJoin::Phase phase) noexcept {
    switch (phase) {
        case Slus21050FemainPacketDrainJoin::Phase::Disabled:
            return "disabled";
        case Slus21050FemainPacketDrainJoin::Phase::Empty:
            return "empty";
        case Slus21050FemainPacketDrainJoin::Phase::Joined:
            return "joined";
        case Slus21050FemainPacketDrainJoin::Phase::Failed:
            return "failed";
        default:
            return "unknown";
    }
}

std::string_view to_string(const Slus21050FemainPacketDrainJoin::Failure failure) noexcept {
    switch (failure) {
        case Slus21050FemainPacketDrainJoin::Failure::None:
            return "none";
        case Slus21050FemainPacketDrainJoin::Failure::Disabled:
            return "disabled";
        case Slus21050FemainPacketDrainJoin::Failure::Replay:
            return "replay";
        case Slus21050FemainPacketDrainJoin::Failure::SourceBoundaryInvalid:
            return "source-boundary-invalid";
        case Slus21050FemainPacketDrainJoin::Failure::ResolverProofMissing:
            return "resolver-proof-missing";
        case Slus21050FemainPacketDrainJoin::Failure::ResolverProofInvalid:
            return "resolver-proof-invalid";
        case Slus21050FemainPacketDrainJoin::Failure::PublicationInvalid:
            return "publication-invalid";
        case Slus21050FemainPacketDrainJoin::Failure::SourcePublicationMismatch:
            return "source-publication-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::BatchUnavailable:
            return "batch-unavailable";
        case Slus21050FemainPacketDrainJoin::Failure::BatchPacketMismatch:
            return "batch-packet-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::BatchRawCountMismatch:
            return "batch-raw-count-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::BatchRawDigestMismatch:
            return "batch-raw-digest-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeWitnessInvalid:
            return "consume-witness-invalid";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeBindingMismatch:
            return "consume-binding-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeQueueMismatch:
            return "consume-queue-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeSequenceMismatch:
            return "consume-sequence-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeTerminalMismatch:
            return "consume-terminal-mismatch";
        case Slus21050FemainPacketDrainJoin::Failure::ConsumeReturnMismatch:
            return "consume-return-mismatch";
        default:
            return "unknown";
    }
}

Slus21050FemainPacketDrainJoin::Slus21050FemainPacketDrainJoin(const bool enabled) noexcept
    : enabled_(enabled), phase_(enabled ? Phase::Empty : Phase::Disabled) {}

bool Slus21050FemainPacketDrainJoin::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    phase_ = enabled_ ? Phase::Failed : Phase::Disabled;
    evidence_.reset();
    consume_witness_.reset();
    return false;
}

std::optional<Slus21050FemainRetailConsumeCompletionWitness>
Slus21050FemainPacketDrainJoin::take_consume_witness() noexcept {
    if (!consume_witness_.has_value()) {
        return std::nullopt;
    }
    std::optional<Slus21050FemainRetailConsumeCompletionWitness> result(std::move(consume_witness_));
    consume_witness_.reset();
    return result;
}

std::uint64_t authenticate_slus_21050_femain_packet_drain_evidence(
    const Slus21050FemainPacketDrainJoin::Evidence& evidence) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_u64(hash, authenticate_slus_21050_femain_frame_graphics_source_evidence(
                       evidence.source.source_evidence));
    hash_u64(hash, evidence.source.source_evidence.authentication_tag);
    hash_u64(hash, evidence.source.source_packet_completion.domain_identity);
    hash_u64(hash, evidence.source.source_packet_completion.ordinal);
    hash_u64(hash, static_cast<std::uint64_t>(evidence.source.source_packet_completion.kind));
    hash_u64(hash, evidence.source.execution.emulation_thread_identity);
    hash_u64(hash, evidence.source.execution.worker_epoch);
    hash_u64(hash, evidence.source.execution.runtime_epoch);
    hash_u64(hash, evidence.source.frame_id);
    hash_packet(hash, evidence.publication);
    hash_u64(hash, evidence.raw_quadword_count);
    hash_u64(hash, evidence.raw_stream_digest);
    hash_consume(hash, evidence.consume);
    hash_u64(hash, evidence.consume_identity.runtime_epoch);
    hash_u64(hash, evidence.consume_identity.observation_epoch);
    hash_u64(hash, evidence.consume_identity.invocation_serial);
    hash_u64(hash, evidence.consume_identity.guest_execution_context_identity);
    return hash;
}

// RECOVERY: behaviour is exact; only the block order of the shared "false" exit of the
// inlined source/publication match differs from the original GIMPLE.
bool Slus21050FemainPacketDrainJoin::join(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainLiveTerminalCoordinator& coordinator,
    const std::uint64_t diagnostic_primary_first_ordinal,
    Slus21050FemainRetailConsumeCompletionWitness consume_witness) noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::Empty) {
        return fail(phase_ == Phase::Joined ? Failure::Replay : failure_);
    }
    if (validate_slus_21050_femain_serialized_source_boundary(source) !=
        Slus21050FemainSerializedTokenFailure::None) {
        return fail(Failure::SourceBoundaryInvalid);
    }
    if (!source.source_evidence.resolver_proof.has_value()) {
        return fail(Failure::ResolverProofMissing);
    }
    const auto& proof = *source.source_evidence.resolver_proof;
    if (!validate_slus_21050_femain_live_resolver_source_proof(proof)) {
        return fail(Failure::ResolverProofInvalid);
    }
    if (!consume_witness.valid()) {
        return fail(Failure::ConsumeWitnessInvalid);
    }
    const auto& packet = consume_witness.publication_packet();
    if (!publication_shape_valid(packet)) {
        return fail(Failure::PublicationInvalid);
    }
    if (!source_matches_publication(source, packet)) {
        return fail(Failure::SourcePublicationMismatch);
    }
    const auto batch = coordinator.complete_batch(diagnostic_primary_first_ordinal);
    if (!batch.has_value()) {
        return fail(Failure::BatchUnavailable);
    }
    const auto first_raw = batch->first_raw_quadword();
    if (batch->packet_path() != guest::GifPath::Path2 || !first_raw.has_value() ||
        first_raw->path != guest::GifPath::Path2 ||
        !(first_raw->quadword == source.source_evidence.token.source_marker.request.expected_quadword)) {
        return fail(Failure::BatchPacketMismatch);
    }
    if (batch->raw_quadword_count() != proof.matched_source_qwc) {
        return fail(Failure::BatchRawCountMismatch);
    }
    if (batch->raw_stream_digest() == 0U || batch->raw_stream_digest() != proof.matched_span_digest) {
        return fail(Failure::BatchRawDigestMismatch);
    }
    const auto& consume = consume_witness.evidence();
    const auto& identity = consume_witness.identity();
    if (!identity.valid() || identity.runtime_epoch != consume.binding.runtime_epoch ||
        identity.observation_epoch != consume.binding.observation_epoch ||
        identity.invocation_serial != consume.binding.invocation_serial ||
        identity.guest_execution_context_identity !=
            consume.binding.guest_execution_context_identity) {
        return fail(Failure::ConsumeBindingMismatch);
    }
    if (!binding_matches_publication(consume.binding, packet) ||
        !(consume.binding == consume_witness.binding())) {
        return fail(Failure::ConsumeBindingMismatch);
    }
    if (consume.call.queue_read != packet.queue.queue_read ||
        consume.call.queue_end != packet.queue.queue_end ||
        consume.call.transfers_active != packet.queue.transfers_active ||
        consume.callee.queue_read != packet.queue.queue_read ||
        consume.callee.queue_end != packet.queue.queue_end ||
        consume.callee.transfers_active != packet.queue.transfers_active) {
        return fail(Failure::ConsumeQueueMismatch);
    }
    if (!consume_queue_sequence_valid(consume, packet)) {
        return fail(Failure::ConsumeSequenceMismatch);
    }
    if (!consume.terminal.branch_taken || consume.terminal.queue_read != packet.queue.queue_end ||
        consume.terminal.queue_end != packet.queue.queue_end ||
        consume.terminal.transfers_active != 0U ||
        consume.terminal.post_pc != kSlus21050FemainRetailTerminalBranchDelayPc ||
        consume.terminal.post_next_pc != kSlus21050FemainRetailTerminalTargetPc) {
        return fail(Failure::ConsumeTerminalMismatch);
    }
    if (!consume.successful_return ||
        consume.return_path.return_address != kSlus21050FemainRetailCallerResumePc ||
        consume.return_path.post_pc != kSlus21050FemainRetailReturnDelayPc ||
        consume.return_path.post_next_pc != kSlus21050FemainRetailCallerResumePc ||
        consume.return_path.caller_resume_guest_step == 0U) {
        return fail(Failure::ConsumeReturnMismatch);
    }
    Evidence joined{.source = source,
                    .publication = packet,
                    .raw_quadword_count = batch->raw_quadword_count(),
                    .raw_stream_digest = batch->raw_stream_digest(),
                    .consume = consume_witness.evidence(),
                    .consume_identity = identity};
    joined.authentication_tag = authenticate_slus_21050_femain_packet_drain_evidence(joined);
    evidence_ = joined;
    consume_witness_.emplace(std::move(consume_witness));
    phase_ = Phase::Joined;
    return true;
}

} // namespace takedown::runtime
