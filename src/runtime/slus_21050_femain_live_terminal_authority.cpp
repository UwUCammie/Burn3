#include "takedown/runtime/slus_21050_femain_live_terminal_authority.hpp"
#include "takedown/runtime/slus_21050_femain_live_admission.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"
#include "takedown/runtime/slus_21050_femain_graphics_source_probe.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace takedown::runtime {
std::string_view to_string(const Slus21050FemainLiveTerminalAuthority::Phase phase) noexcept {
    switch (phase) {
        case Slus21050FemainLiveTerminalAuthority::Phase::Disabled:
            return "disabled";
        case Slus21050FemainLiveTerminalAuthority::Phase::Idle:
            return "idle";
        case Slus21050FemainLiveTerminalAuthority::Phase::PublicationBound:
            return "publication-bound";
        case Slus21050FemainLiveTerminalAuthority::Phase::SourceBound:
            return "source-bound";
        case Slus21050FemainLiveTerminalAuthority::Phase::PrimaryOpen:
            return "primary-open";
        case Slus21050FemainLiveTerminalAuthority::Phase::Complete:
            return "complete";
        case Slus21050FemainLiveTerminalAuthority::Phase::Consumed:
            return "consumed";
        case Slus21050FemainLiveTerminalAuthority::Phase::Cancelled:
            return "cancelled";
        case Slus21050FemainLiveTerminalAuthority::Phase::Failed:
            return "failed";
        default:
            return "unknown";
    }
}

std::string_view to_string(const Slus21050FemainLiveTerminalAuthority::Failure failure) noexcept {
    switch (failure) {
        case Slus21050FemainLiveTerminalAuthority::Failure::None:
            return "none";
        case Slus21050FemainLiveTerminalAuthority::Failure::Disabled:
            return "disabled";
        case Slus21050FemainLiveTerminalAuthority::Failure::EventDomainUnavailable:
            return "event-domain-unavailable";
        case Slus21050FemainLiveTerminalAuthority::Failure::PublicationPacketInvalid:
            return "publication-packet-invalid";
        case Slus21050FemainLiveTerminalAuthority::Failure::PublicationAlreadyBound:
            return "publication-already-bound";
        case Slus21050FemainLiveTerminalAuthority::Failure::ExecutionIdentityInvalid:
            return "execution-identity-invalid";
        case Slus21050FemainLiveTerminalAuthority::Failure::FrameIdentityInvalid:
            return "frame-identity-invalid";
        case Slus21050FemainLiveTerminalAuthority::Failure::SourceNotExpected:
            return "source-not-expected";
        case Slus21050FemainLiveTerminalAuthority::Failure::SourceEvidenceInvalid:
            return "source-evidence-invalid";
        case Slus21050FemainLiveTerminalAuthority::Failure::SourcePacketMismatch:
            return "source-packet-mismatch";
        case Slus21050FemainLiveTerminalAuthority::Failure::SourceAlreadyBound:
            return "source-already-bound";
        case Slus21050FemainLiveTerminalAuthority::Failure::PrimaryNotReady:
            return "primary-not-ready";
        case Slus21050FemainLiveTerminalAuthority::Failure::PrimaryAlreadySealed:
            return "primary-already-sealed";
        case Slus21050FemainLiveTerminalAuthority::Failure::TerminalNotReady:
            return "terminal-not-ready";
        case Slus21050FemainLiveTerminalAuthority::Failure::TerminalWitnessInvalid:
            return "terminal-witness-invalid";
        case Slus21050FemainLiveTerminalAuthority::Failure::SerializedTokenRejected:
            return "serialized-token-rejected";
        case Slus21050FemainLiveTerminalAuthority::Failure::PacketMismatch:
            return "packet-mismatch";
        case Slus21050FemainLiveTerminalAuthority::Failure::CancellationRejected:
            return "cancellation-rejected";
        case Slus21050FemainLiveTerminalAuthority::Failure::ResetRejected:
            return "reset-rejected";
        case Slus21050FemainLiveTerminalAuthority::Failure::Replay:
            return "replay";
        default:
            return "unknown";
    }
}

namespace {

// Process-wide event-domain identities. Zero means exhausted or invalid.
std::uint64_t allocate_event_domain_identity() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    auto identity = next.load(std::memory_order_relaxed);
    while (identity != 0U && identity != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(identity, identity + 1U, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return identity;
        }
    }
    return 0U;
}

} // namespace

Slus21050FemainLiveTerminalAuthority::Slus21050FemainLiveTerminalAuthority(const bool enabled) noexcept
    : enabled_(enabled),
      phase_(enabled ? Phase::Idle : Phase::Disabled),
      failure_(Failure::None),
      event_domain_(allocate_event_domain_identity(), 1U) {
    if (!event_domain_.valid()) {
        phase_ = Phase::Failed;
        failure_ = Failure::EventDomainUnavailable;
    }
}

bool Slus21050FemainLiveTerminalAuthority::ready_for_publication() const noexcept {
    return enabled_ && event_domain_.valid() && phase_ == Phase::Idle;
}

bool Slus21050FemainLiveTerminalAuthority::authorize_publication(
    const Slus21050FemainMovieTransactionOwner& owner,
    const Slus21050FemainFrameTransactionLedger& ledger,
    const guest::EeMemory& /*memory*/) const noexcept {
    if (enabled_ && event_domain_.valid() && phase_ == Phase::Idle) {
        if (owner.occupied() && !owner.publication_pending()) {
            const auto snapshot = ledger.snapshot();
            if (snapshot.state == Slus21050FemainFrameLedgerState::Active &&
                snapshot.session_generation != 0U && snapshot.proof_valid) {
                return !snapshot.overflowed;
            }
        }
    }
    return false;
}

bool Slus21050FemainLiveTerminalAuthority::fail(const Failure failure) noexcept {
    if (!enabled_ && failure == Failure::Disabled) {
        failure_ = Failure::Disabled;
        phase_ = Phase::Disabled;
        return false;
    }
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    phase_ = Phase::Failed;
    return false;
}

bool Slus21050FemainLiveTerminalAuthority::reject_publication(const Failure failure) noexcept {
    failure_ = failure;
    return false;
}

bool Slus21050FemainLiveTerminalAuthority::valid_publication_packet(
    const Slus21050FemainMoviePublicationPacket& packet) const noexcept {
    return validate_slus_21050_femain_movie_publication_packet(packet);
}

bool Slus21050FemainLiveTerminalAuthority::bind_publication(
    const Slus21050FemainMovieTransactionOwner& owner,
    const Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationPacket& packet,
    const Slus21050FemainSerializedExecutionIdentity& execution,
    const std::uint64_t frame_id) noexcept {
    if (!enabled_ || phase_ == Phase::Disabled) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::Idle) {
        const Failure reason = phase_ == Phase::Failed ? failure_ : Failure::PublicationAlreadyBound;
        return fail(reason);
    }
    if (!event_domain_.valid()) {
        return fail(Failure::EventDomainUnavailable);
    }
    if (!valid_publication_packet(packet)) {
        return reject_publication(Failure::PublicationPacketInvalid);
    }
    if (!live_publication_matches(owner, ledger, packet)) {
        return reject_publication(Failure::SourcePacketMismatch);
    }
    if (last_owner_identity_ != 0U && last_owner_identity_ != packet.owner_identity) {
        return reject_publication(Failure::Replay);
    }
    if (last_owner_identity_ == packet.owner_identity &&
        packet.publication_serial <= last_publication_serial_) {
        return reject_publication(Failure::Replay);
    }
    if (execution.emulation_thread_identity == 0U || execution.worker_epoch == 0U ||
        execution.runtime_epoch == 0U) {
        return reject_publication(Failure::ExecutionIdentityInvalid);
    }
    if (frame_id == 0U) {
        return reject_publication(Failure::FrameIdentityInvalid);
    }

    publication_packet_ = packet;
    last_owner_identity_ = packet.owner_identity;
    last_publication_serial_ = packet.publication_serial;
    execution_ = execution;
    frame_id_ = frame_id;
    builder_.emplace(true);
    source_boundary_.reset();
    terminal_boundary_.reset();
    primary_event_count_ = 0U;
    failure_ = Failure::None;
    phase_ = Phase::PublicationBound;
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::live_publication_matches(
    const Slus21050FemainMovieTransactionOwner& owner,
    const Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationPacket& packet) const noexcept {
    if (!owner.publication_packet_matches(packet)) {
        return false;
    }
    if (!owner.published_ledger_matches(ledger)) {
        return false;
    }
    const auto snapshot = ledger.snapshot();
    const unsigned long long slot = packet.published_reservation.slot;
    if (snapshot.state != Slus21050FemainFrameLedgerState::Active) {
        return false;
    }
    if (!snapshot.proof_valid || snapshot.overflowed) {
        return false;
    }
    if (snapshot.ledger_identity != packet.ledger_identity) {
        return false;
    }
    if (snapshot.session_generation != packet.ledger_session_generation) {
        return false;
    }
    if (slot > 1U) {
        return false;
    }
    return snapshot.slots[slot] == Slus21050FemainFrameLedgerSlotState::Published;
}

std::optional<Slus21050FemainSerializedSourceTerminalToken>
Slus21050FemainLiveTerminalAuthority::take_serialized_token() noexcept {
    if (phase_ == Phase::Complete && builder_) {
        auto token = builder_->take_token();
        if (token) {
            phase_ = Phase::Consumed;
            return token;
        }
        if (failure_ == Failure::None) {
            failure_ = Failure::Replay;
        }
        phase_ = Phase::Failed;
        return {};
    }
    const Failure reason = phase_ == Phase::Failed ? failure_ : Failure::Replay;
    static_cast<void>(fail(reason));
    return {};
}

void Slus21050FemainLiveTerminalAuthority::clear_for_next_publication() noexcept {
    publication_packet_.reset();
    execution_ = {};
    frame_id_ = 0U;
    builder_.reset();
    source_boundary_.reset();
    terminal_boundary_.reset();
    primary_event_count_ = 0U;
    failure_ = Failure::None;
    phase_ = enabled_ ? Phase::Idle : Phase::Disabled;
}

bool Slus21050FemainLiveTerminalAuthority::reset_after_cancel() noexcept {
    if (phase_ != Phase::Cancelled) {
        if (failure_ == Failure::None) {
            failure_ = Failure::ResetRejected;
        }
        phase_ = Phase::Failed;
        return false;
    }
    clear_for_next_publication();
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::on_source_packet_completed(
    const Slus21050FemainFrameGraphicsSourceEvidence& evidence) noexcept {
    if (phase_ != Phase::PublicationBound || !builder_) {
        const Failure reason = phase_ == Phase::Failed ? failure_ : Failure::SourceAlreadyBound;
        return fail(reason);
    }

    Slus21050FemainSerializedSourceBoundary probe{};
    probe.source_evidence = evidence;
    probe.source_packet_completion.domain_identity = event_domain_.domain_identity();
    probe.source_packet_completion.ordinal = 1U;
    probe.source_packet_completion.kind = Slus21050FemainSerializedEventKind::SourcePacketCompletion;
    probe.execution = execution_;
    probe.frame_id = frame_id_;
    if (validate_slus_21050_femain_serialized_source_boundary(probe) !=
        Slus21050FemainSerializedTokenFailure::None) {
        return fail(Failure::SourceEvidenceInvalid);
    }

    if (!publication_packet_ || !source_matches_publication(evidence)) {
        return fail(Failure::SourcePacketMismatch);
    }
    const auto source_event = event_domain_.issue(Slus21050FemainSerializedEventKind::SourcePacketCompletion);
    if (!source_event) {
        return fail(Failure::EventDomainUnavailable);
    }

    Slus21050FemainSerializedSourceBoundary source{};
    source.source_evidence = evidence;
    source.source_packet_completion = *source_event;
    source.execution = execution_;
    source.frame_id = frame_id_;
    const auto bound = builder_->bind_source(source);
    if (!bound.complete() &&
        builder_->phase() != Slus21050FemainSerializedTokenBuilder::Phase::SourceBound) {
        return fail(Failure::SerializedTokenRejected);
    }
    source_boundary_ = source;
    phase_ = Phase::SourceBound;
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::on_primary_command_delivery() noexcept {
    if (phase_ != Phase::SourceBound && phase_ != Phase::PrimaryOpen) {
        const Failure reason = phase_ == Phase::Failed ? failure_ : Failure::PrimaryNotReady;
        return fail(reason);
    }
    if (primary_event_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(Failure::PrimaryAlreadySealed);
    }
    const auto event = event_domain_.issue(Slus21050FemainSerializedEventKind::PrimaryCommandDelivery);
    if (!event) {
        return fail(Failure::EventDomainUnavailable);
    }
    primary_event_count_ = primary_event_count_ + 1U;
    phase_ = Phase::PrimaryOpen;
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::on_terminal_primary_delivery(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept {
    if (phase_ != Phase::PrimaryOpen || !source_boundary_ || !builder_ ||
        primary_event_count_ == 0U) {
        const Failure reason = phase_ == Phase::Failed ? failure_ : Failure::TerminalNotReady;
        return fail(reason);
    }

    const auto& source = source_boundary_->source_evidence;
    if (!(witness.source_evidence == source)) {
        return fail(Failure::TerminalWitnessInvalid);
    }
    if (witness.frame_id != frame_id_ ||
        witness.emulation_thread_identity != execution_.emulation_thread_identity ||
        witness.worker_epoch != execution_.worker_epoch ||
        witness.runtime_epoch != execution_.runtime_epoch) {
        return fail(Failure::TerminalWitnessInvalid);
    }
    if (witness.primary_stream_digest == 0U || witness.authentication_tag == 0U) {
        return fail(Failure::TerminalWitnessInvalid);
    }
    if (witness.authentication_tag !=
        authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(witness)) {
        return fail(Failure::TerminalWitnessInvalid);
    }
    if (!validate_slus_21050_femain_renderer_terminal_primary_delivery_range(witness, source)) {
        return fail(Failure::TerminalWitnessInvalid);
    }
    if (witness.primary_command_count != primary_event_count_) {
        return fail(Failure::TerminalWitnessInvalid);
    }

    const std::uint64_t first_ordinal = event_domain_.last_ordinal() - primary_event_count_ + 1U;
    if (first_ordinal == 0U) {
        return fail(Failure::EventDomainUnavailable);
    }
    const auto terminal_event = event_domain_.issue(Slus21050FemainSerializedEventKind::TerminalPrimaryDelivery);
    if (!terminal_event) {
        return fail(Failure::EventDomainUnavailable);
    }

    Slus21050FemainSerializedTerminalBoundary terminal{};
    terminal.terminal_witness = witness;
    terminal.primary_first_event.domain_identity = event_domain_.domain_identity();
    terminal.primary_first_event.ordinal = first_ordinal;
    terminal.primary_first_event.kind = Slus21050FemainSerializedEventKind::PrimaryCommandDelivery;
    terminal.primary_event_count = primary_event_count_;
    terminal.terminal_primary = *terminal_event;
    const auto finalized = builder_->finalize_terminal(terminal);
    if (finalized.failure != Slus21050FemainSerializedTokenFailure::None || !finalized.token) {
        return fail(Failure::SerializedTokenRejected);
    }

    terminal_boundary_ = terminal;
    phase_ = Phase::Complete;
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::cancel(
    const Slus21050FemainLiveCancellationWitness& witness) noexcept {
    if (!publication_packet_ || phase_ == Phase::Idle || phase_ == Phase::Disabled ||
        phase_ == Phase::Cancelled) {
        return fail(Failure::CancellationRejected);
    }
    if (!witness.matches(*publication_packet_)) {
        return fail(Failure::PacketMismatch);
    }
    builder_.reset();
    source_boundary_.reset();
    terminal_boundary_.reset();
    primary_event_count_ = 0U;
    phase_ = Phase::Cancelled;
    failure_ = Failure::None;
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::reset_after_retirement(
    const Slus21050FemainLiveRetirementWitness& witness) noexcept {
    if (phase_ != Phase::Consumed) {
        if (failure_ == Failure::None) {
            failure_ = Failure::ResetRejected;
        }
        phase_ = Phase::Failed;
        return false;
    }
    if (!publication_packet_ || !witness.matches(*publication_packet_)) {
        return fail(Failure::PacketMismatch);
    }
    clear_for_next_publication();
    return true;
}

bool Slus21050FemainLiveTerminalAuthority::source_matches_publication(
    const Slus21050FemainFrameGraphicsSourceEvidence& evidence) const noexcept {
    if (!publication_packet_) {
        return false;
    }
    const auto& packet = *publication_packet_;
    const auto& token = evidence.token;
    if (!(token.source == kSlus21050FemainLiveExpectedSource)) {
        return false;
    }
    if (!token.published_reservation) {
        return false;
    }
    const auto& reservation = *token.published_reservation;
    const auto& packet_reservation = packet.published_reservation;
    if (reservation.ledger_identity != packet_reservation.ledger_identity ||
        reservation.session_generation != packet_reservation.session_generation ||
        reservation.frame_generation != packet_reservation.frame_generation ||
        reservation.slot != packet_reservation.slot ||
        reservation.frame.media_ordinal != packet_reservation.frame.media_ordinal ||
        reservation.frame.source_sequence != packet_reservation.frame.source_sequence ||
        reservation.frame.source_checksum != packet_reservation.frame.source_checksum ||
        reservation.frame.produced_at.guest_vblank != packet_reservation.frame.produced_at.guest_vblank ||
        reservation.frame.produced_at.audio_sample != packet_reservation.frame.produced_at.audio_sample) {
        return false;
    }
    if (token.emulation_thread_identity != execution_.emulation_thread_identity ||
        token.owner_identity != packet.owner_identity ||
        token.player_identity != packet.player_identity ||
        token.player_session_generation != packet.player_session_generation ||
        packet.player_session_generation != token.media_session_generation) {
        return false;
    }
    if (!packet.plan.player_lease || token.frame_lease != *packet.plan.player_lease) {
        return false;
    }
    if (token.frame_generation != packet.plan.frame_generation ||
        token.media_ordinal != packet.plan.media_ordinal ||
        token.source_sequence != packet.plan.source_sequence ||
        token.source_checksum != packet.plan.source_checksum ||
        token.payload_digest != packet.plan.payload_digest ||
        token.layout.generation != packet.plan.layout_generation) {
        return false;
    }
    const auto& ledger_before = token.ledger_before;
    if (ledger_before.ledger_identity != packet.ledger_identity ||
        ledger_before.session_generation != packet.ledger_session_generation ||
        ledger_before.state != Slus21050FemainFrameLedgerState::Active ||
        !ledger_before.proof_valid || ledger_before.overflowed) {
        return false;
    }
    if (ledger_before.slots[packet.published_reservation.slot] !=
        Slus21050FemainFrameLedgerSlotState::Published) {
        return false;
    }

    if (!token.handoff) {
        return false;
    }
    const auto& handoff = *token.handoff;
    if (handoff.buffer_index_before > 1U) {
        return false;
    }
    const auto& queue = packet.queue;
    if (handoff.frame_generation != packet.plan.frame_generation ||
        handoff.layout_generation != packet.plan.layout_generation ||
        handoff.queue_table[handoff.buffer_index_before] != queue.queue_read ||
        handoff.pending_end != queue.queue_end ||
        handoff.busy_before != queue.transfers_active ||
        static_cast<std::uint8_t>(handoff.buffer_index_before == 0U) != queue.buffer_index ||
        handoff.buffer_address_base != queue.queue_base) {
        return false;
    }
    if (handoff.player_lease != packet.plan.player_lease) {
        return false;
    }
    if (token.timeline_ordinal != packet.plan.timeline_ordinal) {
        return false;
    }
    if (handoff.source_sequence != packet.plan.source_sequence ||
        handoff.source_checksum != packet.plan.source_checksum ||
        handoff.payload_digest != packet.plan.payload_digest) {
        return false;
    }

    if (!evidence.resolver_proof) {
        // Legacy diagnostic evidence is covered by the existing transport proof.
        return true;
    }
    const auto& proof = *evidence.resolver_proof;
    if (!validate_slus_21050_femain_live_resolver_source_proof(proof)) {
        return false;
    }
    if (!(proof.publication_queue == queue)) {
        return false;
    }
    const auto& candidate = proof.candidate;
    const auto& marker = token.source_marker;
    if (candidate.publication_serial != packet.publication_serial ||
        candidate.player_session_generation != packet.player_session_generation ||
        candidate.frame_generation != packet.plan.frame_generation) {
        return false;
    }
    if (candidate.source != marker.request.source || candidate.source != marker.candidate_source ||
        candidate.chain_root != marker.request.chain_root ||
        candidate.source_tag_location != marker.request.source_tag_location ||
        candidate.source_tag_ordinal != marker.request.source_tag_ordinal ||
        candidate.source_payload_word_ordinal != marker.request.source_payload_word_ordinal ||
        !(candidate.expected_quadword == marker.request.expected_quadword)) {
        return false;
    }

    const auto& range = token.source_range;
    if (range.buffer == 0U || range.span_bytes == 0U) {
        return false;
    }
    if (candidate.source < range.buffer || (candidate.source & 15U) != 0U ||
        candidate.source_qwc == 0U) {
        return false;
    }
    const std::uint64_t range_begin = range.buffer;
    if (range.span_bytes > ~range_begin) {
        return false;
    }
    const std::uint64_t source_end =
        static_cast<std::uint64_t>(candidate.source) + static_cast<std::uint64_t>(candidate.source_qwc) * 16U;
    return source_end <= range.span_bytes + range_begin;
}

} // namespace takedown::runtime
