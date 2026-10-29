#include "takedown/runtime/slus_21050_femain_serialized_token.hpp"

#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
#include <optional>
#include <cstdint>

namespace takedown::runtime {

namespace {

using SerializedFailure = Slus21050FemainSerializedTokenFailure;

// Internal boundary validator: checks the live source identity, the frame and
// ledger snapshot, the shared event evidence, the execution identity, the
// published reservation and the resolver proof, in the retail order.
SerializedFailure validate_source_boundary(const Slus21050FemainSerializedSourceBoundary& source) noexcept {
    const auto& evidence = source.source_evidence;
    const auto& token = evidence.token;
    if (evidence.authentication_tag == 0U && token.authentication_tag == 0U) {
        return SerializedFailure::SourceEvidenceMissing;
    }
    if (evidence.authentication_tag == 0U ||
        evidence.authentication_tag != authenticate_slus_21050_femain_frame_graphics_source_evidence(evidence)) {
        return SerializedFailure::SourceEvidenceAuthenticationMismatch;
    }
    if (token.authentication_tag == 0U ||
        token.authentication_tag != authenticate_slus_21050_femain_frame_graphics_source_token(token)) {
        return SerializedFailure::SourceEvidenceAuthenticationMismatch;
    }
    const auto& identity = token.source;
    if (identity.revision != kSlus21050FemainLiveExpectedSource.revision ||
        identity.function != kSlus21050FemainLiveExpectedSource.function ||
        identity.function_address != 2040840U || identity.function_bytes != 536U ||
        identity.function_fingerprint != 10763330479812237276ULL ||
        identity.executable_fingerprint != 13547543810260351304ULL) {
        return SerializedFailure::SourceIdentityInvalid;
    }
    if (token.emulation_thread_identity == 0U || token.owner_identity == 0U || token.player_identity == 0U ||
        token.player_session_generation == 0U || token.frame_lease.session_generation == 0U ||
        token.frame_lease.lease_generation == 0U || token.frame_generation == 0U ||
        token.media_session_generation == 0U || token.media_clock_generation == 0U ||
        token.ledger_before.ledger_identity == 0U || token.ledger_before.session_generation == 0U ||
        token.payload_digest == 0U || token.source_range.buffer == 0U ||
        token.source_range.span_bytes == 0U || token.source_marker.token == 0U ||
        token.source_marker.generation == 0U || token.source_marker.candidate_source == 0U ||
        token.source_marker.request.source == 0U || token.source_marker.request.chain_root == 0U ||
        token.source_marker.request.source_tag_location == 0U) {
        return SerializedFailure::SourceIdentityInvalid;
    }
    if (static_cast<std::uint8_t>(token.ledger_before.state) != 1U) {
        return SerializedFailure::LedgerSnapshotInvalid;
    }
    if (!token.ledger_before.proof_valid) {
        return SerializedFailure::LedgerSnapshotInvalid;
    }
    if (token.ledger_before.overflowed) {
        return SerializedFailure::LedgerSnapshotInvalid;
    }
    if (token.player_session_generation != token.frame_lease.session_generation ||
        token.player_session_generation != token.media_session_generation ||
        token.player_session_generation != token.ledger_before.session_generation) {
        return SerializedFailure::SourceIdentityInvalid;
    }
    if (source.frame_id == 0U) {
        return SerializedFailure::FrameIdentityInvalid;
    }
    if (evidence.source_event_ordinal == 0U || evidence.raw_submission_event_ordinal == 0U ||
        evidence.packet_completion_event_ordinal == 0U ||
        evidence.source_event_ordinal >= evidence.raw_submission_event_ordinal ||
        evidence.raw_submission_event_ordinal >= evidence.packet_completion_event_ordinal) {
        return SerializedFailure::SourceTransportOrderInvalid;
    }

    const auto& marker_event = evidence.diagnostic_marker_event;
    const auto& raw_event = evidence.diagnostic_raw_submission_event;
    const auto& eop_event = evidence.diagnostic_packet_completion_event;
    const bool any_identity = marker_event.domain_identity != 0U || marker_event.observation_epoch != 0U ||
                              marker_event.ordinal != 0U || raw_event.domain_identity != 0U ||
                              raw_event.observation_epoch != 0U || raw_event.ordinal != 0U ||
                              eop_event.domain_identity != 0U || eop_event.observation_epoch != 0U ||
                              eop_event.ordinal != 0U;
    if (any_identity) {
        if (marker_event.domain_identity == 0U || marker_event.observation_epoch == 0U ||
            marker_event.ordinal == 0U || static_cast<std::uint8_t>(marker_event.kind) > 2U ||
            raw_event.domain_identity == 0U || raw_event.observation_epoch == 0U ||
            raw_event.ordinal == 0U || static_cast<std::uint8_t>(raw_event.kind) > 2U ||
            eop_event.domain_identity == 0U || eop_event.observation_epoch == 0U ||
            eop_event.ordinal == 0U || static_cast<std::uint8_t>(eop_event.kind) > 2U) {
            return SerializedFailure::SourceTransportOrderInvalid;
        }
        if (marker_event.domain_identity != raw_event.domain_identity ||
            marker_event.domain_identity != eop_event.domain_identity) {
            return SerializedFailure::SourceTransportOrderInvalid;
        }
        if (marker_event.observation_epoch != evidence.diagnostic_observation_epoch ||
            marker_event.observation_epoch != raw_event.observation_epoch ||
            marker_event.observation_epoch != eop_event.observation_epoch) {
            return SerializedFailure::SourceTransportOrderInvalid;
        }
        if (static_cast<std::uint8_t>(marker_event.kind) != 0U ||
            static_cast<std::uint8_t>(raw_event.kind) != 1U ||
            static_cast<std::uint8_t>(eop_event.kind) != 2U) {
            return SerializedFailure::SourceTransportOrderInvalid;
        }
        if (marker_event.ordinal >= raw_event.ordinal || raw_event.ordinal >= eop_event.ordinal) {
            return SerializedFailure::SourceTransportOrderInvalid;
        }
    }

    const auto& packet_event = source.source_packet_completion;
    if (packet_event.domain_identity == 0U || packet_event.ordinal == 0U ||
        static_cast<std::uint8_t>(packet_event.kind) != 0U) {
        return SerializedFailure::SourceEventInvalid;
    }
    const auto& execution = source.execution;
    if (execution.emulation_thread_identity == 0U || execution.worker_epoch == 0U || execution.runtime_epoch == 0U) {
        return SerializedFailure::ExecutionIdentityInvalid;
    }
    if (token.emulation_thread_identity != execution.emulation_thread_identity) {
        return SerializedFailure::ExecutionIdentityMismatch;
    }

    if (token.published_reservation.has_value()) {
        const auto& reservation = *token.published_reservation;
        if (reservation.ledger_identity == 0U || reservation.session_generation == 0U ||
            reservation.frame_generation == 0U) {
            return SerializedFailure::PublishedReservationMismatch;
        }
        if (token.ledger_before.ledger_identity != reservation.ledger_identity ||
            token.ledger_before.session_generation != reservation.session_generation ||
            token.frame_generation != reservation.frame_generation ||
            reservation.frame.media_ordinal != token.media_ordinal ||
            reservation.frame.source_sequence != token.source_sequence ||
            reservation.frame.source_checksum != token.source_checksum || reservation.slot > 1U ||
            static_cast<std::uint8_t>(token.ledger_before.slots[reservation.slot]) != 2U) {
            return SerializedFailure::PublishedReservationMismatch;
        }
    }

    if (evidence.resolver_proof.has_value()) {
        const auto& proof = *evidence.resolver_proof;
        if (!validate_slus_21050_femain_live_resolver_source_proof(proof)) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (!token.handoff.has_value()) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        const auto& handoff = *token.handoff;
        if (handoff.buffer_index_before > 1U) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.publication_queue.queue_read != handoff.queue_table[handoff.buffer_index_before]) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.publication_queue.queue_end != handoff.pending_end) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.publication_queue.transfers_active != handoff.busy_before) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.publication_queue.buffer_index != handoff.buffer_index_before) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.publication_queue.queue_base != handoff.buffer_address_base) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.candidate.frame_generation != token.frame_generation) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.candidate.source != token.source_marker.request.source ||
            proof.candidate.source != token.source_marker.candidate_source) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.candidate.chain_root != token.source_marker.request.chain_root ||
            proof.candidate.source_tag_location != token.source_marker.request.source_tag_location ||
            proof.candidate.source_tag_ordinal != token.source_marker.request.source_tag_ordinal ||
            proof.candidate.source_payload_word_ordinal != token.source_marker.request.source_payload_word_ordinal) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (std::memcmp(&token.buffer_mutation, &token.work_mutation, 16U) != 0) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        const auto range_begin = static_cast<std::uint64_t>(token.source_range.buffer);
        const auto range_span = static_cast<std::uint64_t>(token.source_range.span_bytes);
        const auto candidate_source = static_cast<std::uint64_t>(proof.candidate.source);
        if (token.source_range.buffer == 0U || token.source_range.span_bytes == 0U) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (candidate_source < range_begin) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if ((candidate_source & 15U) != 0U) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (proof.candidate.source_qwc == 0U) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        if (range_span > ~range_begin) {
            return SerializedFailure::SourceIdentityInvalid;
        }
        const std::uint64_t source_end =
            candidate_source + static_cast<std::uint64_t>(proof.candidate.source_qwc) * 16U;
        if (source_end > range_begin + range_span) {
            return SerializedFailure::SourceIdentityInvalid;
        }
    }
    return SerializedFailure::None;
}

}  // namespace

Slus21050FemainSerializedTokenFailure validate_slus_21050_femain_serialized_source_boundary(
    const Slus21050FemainSerializedSourceBoundary& source) noexcept {
    return validate_source_boundary(source);
}

std::string_view to_string(Slus21050FemainSerializedTokenFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainSerializedTokenFailure::None:
        return "none";
    case Slus21050FemainSerializedTokenFailure::Disabled:
        return "disabled";
    case Slus21050FemainSerializedTokenFailure::SourceEvidenceMissing:
        return "source-evidence-missing";
    case Slus21050FemainSerializedTokenFailure::SourceEvidenceAuthenticationMismatch:
        return "source-evidence-authentication-mismatch";
    case Slus21050FemainSerializedTokenFailure::SourceIdentityInvalid:
        return "source-identity-invalid";
    case Slus21050FemainSerializedTokenFailure::SourceTransportOrderInvalid:
        return "source-transport-order-invalid";
    case Slus21050FemainSerializedTokenFailure::LedgerSnapshotInvalid:
        return "ledger-snapshot-invalid";
    case Slus21050FemainSerializedTokenFailure::SourceEventInvalid:
        return "source-event-invalid";
    case Slus21050FemainSerializedTokenFailure::ExecutionIdentityInvalid:
        return "execution-identity-invalid";
    case Slus21050FemainSerializedTokenFailure::ExecutionIdentityMismatch:
        return "execution-identity-mismatch";
    case Slus21050FemainSerializedTokenFailure::TerminalWitnessMissing:
        return "terminal-witness-missing";
    case Slus21050FemainSerializedTokenFailure::TerminalWitnessAuthenticationMismatch:
        return "terminal-witness-authentication-mismatch";
    case Slus21050FemainSerializedTokenFailure::TerminalEventInvalid:
        return "terminal-event-invalid";
    case Slus21050FemainSerializedTokenFailure::EventDomainMismatch:
        return "event-domain-mismatch";
    case Slus21050FemainSerializedTokenFailure::EventOrderInvalid:
        return "event-order-invalid";
    case Slus21050FemainSerializedTokenFailure::FrameIdentityInvalid:
        return "frame-identity-invalid";
    case Slus21050FemainSerializedTokenFailure::PrimaryRangeInvalid:
        return "primary-range-invalid";
    case Slus21050FemainSerializedTokenFailure::PublishedReservationMismatch:
        return "published-reservation-mismatch";
    case Slus21050FemainSerializedTokenFailure::AuthenticationMismatch:
        return "authentication-mismatch";
    case Slus21050FemainSerializedTokenFailure::SourceAlreadyBound:
        return "source-already-bound";
    case Slus21050FemainSerializedTokenFailure::SourceNotBound:
        return "source-not-bound";
    case Slus21050FemainSerializedTokenFailure::Replay:
        return "replay";
    default:
        return "unknown";
    }
}

Slus21050FemainSerializedEventDomain::Slus21050FemainSerializedEventDomain(
    const std::uint64_t domain_identity, const std::uint64_t first_ordinal) noexcept
    : domain_identity_(domain_identity),
      next_ordinal_(first_ordinal),
      last_ordinal_(0U),
      valid_(domain_identity != 0U && first_ordinal != 0U),
      exhausted_(false) {}

std::optional<Slus21050FemainSerializedEvent> Slus21050FemainSerializedEventDomain::issue(
    const Slus21050FemainSerializedEventKind kind) noexcept {
    if (!valid_ || exhausted_) {
        return std::nullopt;
    }
    if (static_cast<std::uint8_t>(kind) > 2U) {
        return std::nullopt;
    }
    const std::uint64_t ordinal = next_ordinal_;
    last_ordinal_ = ordinal;
    if (ordinal == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
    } else {
        next_ordinal_ = ordinal + 1U;
    }
    return Slus21050FemainSerializedEvent{domain_identity_, ordinal, kind};
}

Slus21050FemainSerializedTokenResult Slus21050FemainSerializedTokenBuilder::result() const noexcept {
    return Slus21050FemainSerializedTokenResult{failure_, token_};
}

Slus21050FemainSerializedTokenResult Slus21050FemainSerializedTokenBuilder::fail(
    const Slus21050FemainSerializedTokenFailure failure) noexcept {
    if (failure_ == Slus21050FemainSerializedTokenFailure::None) {
        failure_ = failure;
    }
    source_.reset();
    token_.reset();
    phase_ = Phase::Failed;
    return Slus21050FemainSerializedTokenResult{failure_, token_};
}

std::optional<Slus21050FemainSerializedSourceTerminalToken>
Slus21050FemainSerializedTokenBuilder::take_token() noexcept {
    if (phase_ != Phase::Completed || !token_.has_value()) {
        return std::nullopt;
    }
    auto token = token_;
    token_.reset();
    source_.reset();
    phase_ = Phase::Consumed;
    return token;
}

Slus21050FemainSerializedTokenResult Slus21050FemainSerializedTokenBuilder::bind_source(
    const Slus21050FemainSerializedSourceBoundary& source) noexcept {
    if (!enabled_) {
        return Slus21050FemainSerializedTokenResult{Slus21050FemainSerializedTokenFailure::Disabled, std::nullopt};
    }
    if (phase_ != Phase::Empty) {
        const auto failure = phase_ == Phase::Failed ? failure_
                                                     : Slus21050FemainSerializedTokenFailure::SourceAlreadyBound;
        if (failure_ == Slus21050FemainSerializedTokenFailure::None) {
            failure_ = failure;
        }
        source_.reset();
        token_.reset();
        phase_ = Phase::Failed;
        return Slus21050FemainSerializedTokenResult{failure_, token_};
    }

    const auto& evidence = source.source_evidence;
    const auto& token = evidence.token;
    const std::uint64_t evidence_tag = evidence.authentication_tag;
    const std::uint64_t token_tag = token.authentication_tag;
    Slus21050FemainSerializedTokenFailure source_failure = Slus21050FemainSerializedTokenFailure::None;
    if (evidence_tag == 0U) {
        if (token_tag == 0U) {
            source_failure = Slus21050FemainSerializedTokenFailure::SourceEvidenceMissing;
        } else {
            source_failure = Slus21050FemainSerializedTokenFailure::SourceEvidenceAuthenticationMismatch;
        }
    } else if (evidence_tag != authenticate_slus_21050_femain_frame_graphics_source_evidence(evidence)) {
        source_failure = Slus21050FemainSerializedTokenFailure::SourceEvidenceAuthenticationMismatch;
    } else if (token_tag == 0U) {
        source_failure = Slus21050FemainSerializedTokenFailure::SourceEvidenceAuthenticationMismatch;
    } else if (token_tag != authenticate_slus_21050_femain_frame_graphics_source_token(token)) {
        source_failure = Slus21050FemainSerializedTokenFailure::SourceEvidenceAuthenticationMismatch;
    } else {
        source_failure = validate_source_boundary(source);
    }

    if (source_failure != Slus21050FemainSerializedTokenFailure::None) {
        if (failure_ == Slus21050FemainSerializedTokenFailure::None) {
            failure_ = source_failure;
        }
        source_.reset();
        token_.reset();
        phase_ = Phase::Failed;
        return Slus21050FemainSerializedTokenResult{failure_, token_};
    }
    source_ = source;
    failure_ = Slus21050FemainSerializedTokenFailure::None;
    phase_ = Phase::SourceBound;
    return Slus21050FemainSerializedTokenResult{Slus21050FemainSerializedTokenFailure::None, token_};
}

namespace {

// Runtime-visible domain separator (read through a non-constant pointer).
const char* kSerializedTokenDomain = "slus-21050-femain-serialized-source-terminal-token-v1";

constexpr std::uint64_t kSerializedTokenBasis = 1469598103934665603ULL;
constexpr std::uint64_t kSerializedTokenPrime = 1099511628211ULL;

// Folds an integer into the FNV-style hash, least-significant byte first.
std::uint64_t fold_bytes(std::uint64_t hash, std::uint64_t value) noexcept {
    for (unsigned index = 0U; index != 8U; ++index) {
        hash = (hash ^ ((value >> (index * 8U)) & 255U)) * kSerializedTokenPrime;
    }
    return hash;
}

// Folds one whole value (kind, slot or flag) into the hash in a single step.
std::uint64_t fold_value(std::uint64_t hash, std::uint64_t value) noexcept {
    return (hash ^ value) * kSerializedTokenPrime;
}

}  // namespace

std::uint64_t authenticate_slus_21050_femain_serialized_source_terminal_token(
    const Slus21050FemainSerializedSourceTerminalToken& token) noexcept {
    std::uint64_t hash = kSerializedTokenBasis;
    hash = fold_bytes(hash, 53U);
    const char* const domain = kSerializedTokenDomain;
#pragma GCC unroll 0
    for (const char* byte = domain; byte != domain + 53; ++byte) {
        hash = fold_value(hash, static_cast<std::uint8_t>(*byte));
    }
    hash = fold_bytes(hash, token.source.source_evidence.authentication_tag);
    hash = fold_bytes(hash, token.source.source_packet_completion.domain_identity);
    hash = fold_bytes(hash, token.source.source_packet_completion.ordinal);
    hash = fold_value(hash, static_cast<std::uint8_t>(token.source.source_packet_completion.kind));
    hash = fold_bytes(hash, token.source.execution.emulation_thread_identity);
    hash = fold_bytes(hash, token.source.execution.worker_epoch);
    hash = fold_bytes(hash, token.source.execution.runtime_epoch);
    hash = fold_bytes(hash, token.source.frame_id);
    hash = fold_bytes(hash, token.terminal.terminal_witness.authentication_tag);
    hash = fold_bytes(hash, token.terminal.primary_first_event.domain_identity);
    hash = fold_bytes(hash, token.terminal.primary_first_event.ordinal);
    hash = fold_value(hash, static_cast<std::uint8_t>(token.terminal.primary_first_event.kind));
    hash = fold_bytes(hash, token.terminal.primary_event_count);
    hash = fold_bytes(hash, token.terminal.terminal_primary.domain_identity);
    hash = fold_bytes(hash, token.terminal.terminal_primary.ordinal);
    hash = fold_value(hash, static_cast<std::uint8_t>(token.terminal.terminal_primary.kind));
    const bool reserved = token.published_reservation.has_value();
    hash = fold_value(hash, reserved ? 1U : 0U);
    if (reserved) {
        const auto& reservation = *token.published_reservation;
        hash = fold_bytes(hash, reservation.ledger_identity);
        hash = fold_bytes(hash, reservation.session_generation);
        hash = fold_bytes(hash, reservation.frame_generation);
        hash = fold_value(hash, reservation.slot);
        hash = fold_bytes(hash, reservation.frame.media_ordinal);
        hash = fold_bytes(hash, reservation.frame.source_sequence);
        hash = fold_bytes(hash, reservation.frame.source_checksum);
        hash = fold_bytes(hash, reservation.frame.produced_at.guest_vblank);
        hash = fold_bytes(hash, reservation.frame.produced_at.audio_sample);
    }
    return hash;
}

namespace {

using SerializedTokenFailure = Slus21050FemainSerializedTokenFailure;

bool serialized_candidates_match(const Slus21050FemainLiveSourceCandidate& witness,
                                 const Slus21050FemainLiveSourceCandidate& source) noexcept {
    if (witness.source != source.source || witness.chain_root != source.chain_root ||
        witness.source_tag_location != source.source_tag_location ||
        witness.source_tag_ordinal != source.source_tag_ordinal ||
        witness.terminal_tag_location != source.terminal_tag_location ||
        witness.terminal_tag_ordinal != source.terminal_tag_ordinal) {
        return false;
    }
    if (std::memcmp(&witness.source_tag_words, &source.source_tag_words, 16U) != 0) {
        return false;
    }
    if (std::memcmp(&witness.terminal_tag_words, &source.terminal_tag_words, 16U) != 0) {
        return false;
    }
    if (witness.source_payload_word_ordinal != source.source_payload_word_ordinal ||
        witness.source_qwc != source.source_qwc) {
        return false;
    }
    if (std::memcmp(&witness.expected_quadword, &source.expected_quadword, 16U) != 0) {
        return false;
    }
    if (witness.publication_serial != source.publication_serial ||
        witness.player_session_generation != source.player_session_generation) {
        return false;
    }
    if (witness.frame_generation != source.frame_generation) {
        return false;
    }
    return witness.authentication_tag == source.authentication_tag;
}

bool serialized_queues_match(const Slus21050FemainMoviePublicationQueueSnapshot& witness,
                             const Slus21050FemainMoviePublicationQueueSnapshot& source) noexcept {
    if (witness.queue_read != source.queue_read || witness.queue_end != source.queue_end ||
        witness.transfers_active != source.transfers_active) {
        return false;
    }
    if (witness.buffer_index != source.buffer_index) {
        return false;
    }
    return witness.queue_base == source.queue_base;
}

// Witness versus source resolver proofs when both are present: candidate,
// publication queue, then matched span (quadword count and digest).
bool serialized_resolver_matches(const Slus21050FemainLiveResolverSourceProof& witness,
                                 const Slus21050FemainLiveResolverSourceProof& source) noexcept {
    if (!serialized_candidates_match(witness.candidate, source.candidate)) {
        return false;
    }
    if (!serialized_queues_match(witness.publication_queue, source.publication_queue)) {
        return false;
    }
    if (witness.matched_source_qwc != source.matched_source_qwc) {
        return false;
    }
    return witness.matched_span_digest == source.matched_span_digest;
}

// Diagnostic identity comparison between the terminal witness evidence and the
// source evidence. Every identity must match field by field; the marker, raw and
// packet-completion identities must each match in ordinal and kind.
bool serialized_diagnostics_match(const Slus21050FemainFrameGraphicsSourceEvidence& witness,
                                  const Slus21050FemainFrameGraphicsSourceEvidence& source) noexcept {
    if (witness.diagnostic_observation_epoch != source.diagnostic_observation_epoch ||
        witness.source_event_ordinal != source.source_event_ordinal ||
        witness.raw_submission_event_ordinal != source.raw_submission_event_ordinal ||
        witness.packet_completion_event_ordinal != source.packet_completion_event_ordinal) {
        return false;
    }
    const auto& wm = witness.diagnostic_marker_event;
    const auto& sm = source.diagnostic_marker_event;
    if (wm.domain_identity != sm.domain_identity || wm.observation_epoch != sm.observation_epoch) {
        return false;
    }
    if (wm.ordinal != sm.ordinal || wm.kind != sm.kind) {
        return false;
    }
    const auto& wr = witness.diagnostic_raw_submission_event;
    const auto& sr = source.diagnostic_raw_submission_event;
    if (wr.domain_identity != sr.domain_identity || wr.observation_epoch != sr.observation_epoch) {
        return false;
    }
    if (wr.ordinal != sr.ordinal || wr.kind != sr.kind) {
        return false;
    }
    const auto& we = witness.diagnostic_packet_completion_event;
    const auto& se = source.diagnostic_packet_completion_event;
    if (we.domain_identity != se.domain_identity || we.observation_epoch != se.observation_epoch) {
        return false;
    }
    if (we.ordinal != se.ordinal || we.kind != se.kind) {
        return false;
    }
    return witness.authentication_tag == source.authentication_tag;
}

}  // namespace


Slus21050FemainSerializedTokenResult make_slus_21050_femain_serialized_source_terminal_token(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainSerializedTerminalBoundary& terminal) noexcept {
    const auto source_failure = validate_source_boundary(source);
    if (source_failure != SerializedTokenFailure::None) {
        return Slus21050FemainSerializedTokenResult{source_failure, std::nullopt};
    }
    using Failure = SerializedTokenFailure;
    const auto& witness = terminal.terminal_witness;
    const auto& witness_evidence = witness.source_evidence;
    const auto& source_evidence = source.source_evidence;

    // Terminal witness must be bound to the same source token, with matching
    // resolver proofs and matching diagnostic identities.
    bool witness_ok = false;
    if (witness_evidence.token == source_evidence.token) {
        const bool witness_proof = witness_evidence.resolver_proof.has_value();
        const bool source_proof = source_evidence.resolver_proof.has_value();
        if (witness_proof == source_proof) {
            bool resolver_ok = true;
            if (witness_proof) {
                resolver_ok = serialized_resolver_matches(*witness_evidence.resolver_proof,
                                                          *source_evidence.resolver_proof);
            }
            if (resolver_ok) {
                witness_ok = serialized_diagnostics_match(witness_evidence, source_evidence);
            }
        }
    }
    if (!witness_ok) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalWitnessMissing, std::nullopt};
    }

    const auto& first = terminal.primary_first_event;
    const auto& last = terminal.terminal_primary;
    const auto count = terminal.primary_event_count;
    if (witness.authentication_tag == 0U) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalWitnessAuthenticationMismatch, std::nullopt};
    }
    if (witness.authentication_tag != authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(witness)) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalWitnessAuthenticationMismatch, std::nullopt};
    }
    if (last.domain_identity == 0U) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalEventInvalid, std::nullopt};
    }
    if (last.ordinal == 0U) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalEventInvalid, std::nullopt};
    }
    if (static_cast<std::uint8_t>(last.kind) != 2U) {
        return Slus21050FemainSerializedTokenResult{Failure::TerminalEventInvalid, std::nullopt};
    }
    if (last.domain_identity != source.source_packet_completion.domain_identity ||
        source.source_packet_completion.domain_identity != first.domain_identity) {
        return Slus21050FemainSerializedTokenResult{Failure::EventDomainMismatch, std::nullopt};
    }
    if (last.ordinal <= source.source_packet_completion.ordinal) {
        return Slus21050FemainSerializedTokenResult{Failure::EventOrderInvalid, std::nullopt};
    }
    if (!validate_slus_21050_femain_renderer_terminal_primary_delivery_range(witness, source_evidence)) {
        return Slus21050FemainSerializedTokenResult{Failure::PrimaryRangeInvalid, std::nullopt};
    }
    if (witness.frame_id == 0U || witness.emulation_thread_identity == 0U || witness.worker_epoch == 0U ||
        witness.runtime_epoch == 0U || witness.primary_stream_digest == 0U) {
        return Slus21050FemainSerializedTokenResult{Failure::FrameIdentityInvalid, std::nullopt};
    }
    if (witness.frame_id != source.frame_id) {
        return Slus21050FemainSerializedTokenResult{Failure::FrameIdentityInvalid, std::nullopt};
    }
    if (witness.emulation_thread_identity != source.execution.emulation_thread_identity ||
        witness.worker_epoch != source.execution.worker_epoch ||
        witness.runtime_epoch != source.execution.runtime_epoch) {
        return Slus21050FemainSerializedTokenResult{Failure::ExecutionIdentityMismatch, std::nullopt};
    }
    if (first.domain_identity == 0U || first.ordinal == 0U || static_cast<std::uint8_t>(first.kind) != 1U) {
        return Slus21050FemainSerializedTokenResult{Failure::PrimaryRangeInvalid, std::nullopt};
    }
    if (count == 0U) {
        return Slus21050FemainSerializedTokenResult{Failure::PrimaryRangeInvalid, std::nullopt};
    }
    if (first.ordinal > static_cast<std::uint64_t>(-count)) {
        return Slus21050FemainSerializedTokenResult{Failure::PrimaryRangeInvalid, std::nullopt};
    }
    const std::uint64_t source_ordinal = source.source_packet_completion.ordinal;
    if (source_ordinal == std::numeric_limits<std::uint64_t>::max()) {
        return Slus21050FemainSerializedTokenResult{Failure::EventOrderInvalid, std::nullopt};
    }
    if (first.ordinal != source_ordinal + 1U) {
        return Slus21050FemainSerializedTokenResult{Failure::EventOrderInvalid, std::nullopt};
    }
    const std::uint64_t last_primary = first.ordinal + (count - 1U);
    if (last_primary == std::numeric_limits<std::uint64_t>::max()) {
        return Slus21050FemainSerializedTokenResult{Failure::EventOrderInvalid, std::nullopt};
    }
    if (last.ordinal != last_primary + 1U) {
        return Slus21050FemainSerializedTokenResult{Failure::EventOrderInvalid, std::nullopt};
    }

    Slus21050FemainSerializedSourceTerminalToken token{};
    token.source = source;
    token.terminal = terminal;
    token.published_reservation = source.source_evidence.token.published_reservation;
    token.authentication_tag = authenticate_slus_21050_femain_serialized_source_terminal_token(token);
    return Slus21050FemainSerializedTokenResult{Failure::None, token};
}

Slus21050FemainSerializedTokenResult Slus21050FemainSerializedTokenBuilder::finalize_terminal(
    const Slus21050FemainSerializedTerminalBoundary& terminal) noexcept {
    using Failure = SerializedTokenFailure;
    if (!enabled_) {
        return Slus21050FemainSerializedTokenResult{Failure::Disabled, std::nullopt};
    }
    if (phase_ == Phase::Empty) {
        if (failure_ == Failure::None) {
            failure_ = Failure::SourceNotBound;
        }
        source_.reset();
        token_.reset();
        phase_ = Phase::Failed;
        return Slus21050FemainSerializedTokenResult{failure_, token_};
    }
    if (phase_ == Phase::SourceBound && source_.has_value()) {
        const auto& source = *source_;
        Slus21050FemainSerializedTokenResult validation{};
        const auto source_failure = validate_source_boundary(source);
        if (source_failure != Failure::None) {
            validation = Slus21050FemainSerializedTokenResult{source_failure, std::nullopt};
        } else {
            validation = make_slus_21050_femain_serialized_source_terminal_token(source, terminal);
        }
        if (validation.failure == Failure::None && validation.token.has_value()) {
            token_ = validation.token;
            phase_ = Phase::Completed;
            return Slus21050FemainSerializedTokenResult{failure_, token_};
        }
        if (failure_ == Failure::None) {
            failure_ = validation.failure;
        }
        source_.reset();
        token_.reset();
        phase_ = Phase::Failed;
        return Slus21050FemainSerializedTokenResult{failure_, token_};
    }
    const Failure replay_or_kept = phase_ == Phase::Failed ? failure_ : Failure::Replay;
    if (failure_ == Failure::None) {
        failure_ = replay_or_kept;
    }
    source_.reset();
    token_.reset();
    phase_ = Phase::Failed;
    return Slus21050FemainSerializedTokenResult{failure_, token_};
}

}  // namespace takedown::runtime
