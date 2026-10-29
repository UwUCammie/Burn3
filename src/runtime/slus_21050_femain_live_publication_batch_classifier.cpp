#include "takedown/runtime/slus_21050_femain_live_publication_batch_classifier.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using ScalarWriteRecord = Slus21050FemainTerminalProducerDiagnostic::ScalarWriteRecord;
using ScalarField = Slus21050FemainTerminalProducerDiagnostic::ScalarField;

// Expected shape of one retained publication store (queue-end, queue-read,
// buffer-index), matching the retail derivation's constant table.
struct ExpectedPublicationWrite {
    ScalarField field{ScalarField::Unknown};
    guest::GuestAddress pc{};
    guest::GuestAddress offset{};
    std::uint8_t width{};
};

constexpr std::array<ExpectedPublicationWrite, 3U> kExpectedPublicationWrites{{
    ExpectedPublicationWrite{ScalarField::QueueEnd, 0x1f254cU, 0x4e283cU, 4U},
    ExpectedPublicationWrite{ScalarField::QueueRead, 0x1f2554U, 0x4e2838U, 4U},
    ExpectedPublicationWrite{ScalarField::BufferIndex, 0x1f2564U, 0x4e2804U, 1U},
}};

[[nodiscard]] bool write_shape_matches(const ScalarWriteRecord& write,
                                       const ExpectedPublicationWrite& expected) noexcept {
    return write.field == expected.field && write.event.pc == expected.pc &&
           write.event.original_address == expected.offset &&
           write.event.offset == expected.offset && write.event.width == expected.width &&
           write.event.previous_payload_available &&
           write.event.domain == guest::EeMemoryWriteDomain::MainRam;
}

// The three shared diagnostic events must agree on domain and epoch and be
// strictly ordered MarkerReached < RawSubmission < PacketCompletion.
[[nodiscard]] bool
source_events_consistent(const Slus21050FemainSerializedSourceBoundary& source) noexcept {
    const auto& evidence = source.source_evidence;
    const auto& marker_event = evidence.diagnostic_marker_event;
    const auto& raw_event = evidence.diagnostic_raw_submission_event;
    const auto& eop_event = evidence.diagnostic_packet_completion_event;
    return marker_event.valid() && raw_event.valid() && eop_event.valid() &&
           marker_event.domain_identity == raw_event.domain_identity &&
           marker_event.domain_identity == eop_event.domain_identity &&
           evidence.diagnostic_observation_epoch == marker_event.observation_epoch &&
           marker_event.observation_epoch == raw_event.observation_epoch &&
           marker_event.observation_epoch == eop_event.observation_epoch &&
           marker_event.kind == Slus21050FemainSourceDiagnosticEventKind::MarkerReached &&
           raw_event.kind == Slus21050FemainSourceDiagnosticEventKind::RawSubmission &&
           eop_event.kind == Slus21050FemainSourceDiagnosticEventKind::PacketCompletion &&
           marker_event.ordinal < raw_event.ordinal && raw_event.ordinal < eop_event.ordinal;
}

[[nodiscard]] bool
execution_identity_present(const Slus21050FemainSerializedExecutionIdentity& execution) noexcept {
    return execution.emulation_thread_identity != 0U && execution.worker_epoch != 0U &&
           execution.runtime_epoch != 0U;
}

} // namespace

std::string_view
to_string(const Slus21050FemainLivePublicationBatchClassifier::Phase phase) noexcept {
    using Phase = Slus21050FemainLivePublicationBatchClassifier::Phase;
    switch (phase) {
    case Phase::Disabled:
        return "disabled";
    case Phase::Empty:
        return "empty";
    case Phase::Classified:
        return "classified";
    case Phase::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view
to_string(const Slus21050FemainLivePublicationBatchClassifier::Failure failure) noexcept {
    using Failure = Slus21050FemainLivePublicationBatchClassifier::Failure;
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::Disabled:
        return "disabled";
    case Failure::Replay:
        return "replay";
    case Failure::SourceBoundaryInvalid:
        return "source-boundary-invalid";
    case Failure::ExecutionIdentityInvalid:
        return "execution-identity-invalid";
    case Failure::ExecutionIdentityMismatch:
        return "execution-identity-mismatch";
    case Failure::FrameIdentityInvalid:
        return "frame-identity-invalid";
    case Failure::FrameIdentityMismatch:
        return "frame-identity-mismatch";
    case Failure::PublicationCandidateAuthenticationMismatch:
        return "publication-candidate-authentication-mismatch";
    case Failure::PublicationCandidatePrimaryAuthenticationMismatch:
        return "publication-candidate-primary-authentication-mismatch";
    case Failure::PublicationCandidateSourceMismatch:
        return "publication-candidate-source-mismatch";
    case Failure::PublicationEvidenceInvalid:
        return "publication-evidence-invalid";
    case Failure::CompleteBatchUnavailable:
        return "complete-batch-unavailable";
    case Failure::BatchPacketMismatch:
        return "batch-packet-mismatch";
    case Failure::BatchRawCountMismatch:
        return "batch-raw-count-mismatch";
    case Failure::BatchCountMismatch:
        return "batch-count-mismatch";
    case Failure::BatchDigestMismatch:
        return "batch-digest-mismatch";
    case Failure::Capacity:
        return "capacity";
    }
    return "unknown";
}

Slus21050FemainLivePublicationBatchClassifier::Slus21050FemainLivePublicationBatchClassifier(
    const bool enabled) noexcept
    : enabled_(enabled), phase_(enabled ? Phase::Empty : Phase::Disabled) {}

bool Slus21050FemainLivePublicationBatchClassifier::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    phase_ = enabled_ ? Phase::Failed : Phase::Disabled;
    return false;
}

bool Slus21050FemainLivePublicationBatchClassifier::valid_packet_provenance(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainPublicationCandidate& publication) const noexcept {
    if (!source_events_consistent(source)) {
        return false;
    }
    const auto& evidence = source.source_evidence;
    const auto& marker_event = evidence.diagnostic_marker_event;
    const auto& raw_event = evidence.diagnostic_raw_submission_event;
    const auto& eop_event = evidence.diagnostic_packet_completion_event;
    const auto& marker = evidence.token.source_marker;
    const auto& packet = publication.packet_primary.packet;
    if (!evidence.token.published_reservation.has_value() ||
        packet.observation_epoch != evidence.diagnostic_observation_epoch ||
        packet.path != guest::GifPath::Path2 || !packet.complete || packet.invalidated ||
        !packet.primary_command_stream_available || !packet.marker_bound ||
        !packet.marker_quadword_match || packet.ordinal == 0U || packet.raw_first_event == 0U ||
        packet.raw_first_event > packet.raw_last_event || packet.raw_quadwords == 0U ||
        packet.raw_first_event > packet.marker_raw_event ||
        packet.raw_last_event < packet.marker_raw_event || packet.eop_event == 0U ||
        packet.raw_first_timeline_event == 0U ||
        packet.raw_first_timeline_event > packet.raw_last_timeline_event ||
        packet.raw_last_timeline_event > packet.eop_timeline_event ||
        packet.source_event_domain_identity != marker_event.domain_identity ||
        packet.marker_shared_event != marker_event.ordinal ||
        packet.marker_raw_shared_event != raw_event.ordinal ||
        packet.eop_shared_event != eop_event.ordinal ||
        packet.marker_generation != marker.generation ||
        packet.marker_source != marker.request.source ||
        packet.marker_chain_root != marker.request.chain_root ||
        packet.marker_tag_location != marker.request.source_tag_location ||
        packet.marker_tag_ordinal != marker.request.source_tag_ordinal ||
        packet.marker_payload_word_ordinal != marker.request.source_payload_word_ordinal ||
        packet.first_quadword != marker.request.expected_quadword ||
        packet.primary_command_count == 0U || packet.primary_first_ordinal == 0U ||
        packet.primary_first_ordinal > packet.primary_last_ordinal ||
        packet.primary_last_ordinal == std::numeric_limits<std::uint64_t>::max() ||
        packet.primary_command_count !=
            packet.primary_last_ordinal - packet.primary_first_ordinal + 1U ||
        packet.primary_command_count != packet.worker_dispatch_count ||
        packet.worker_first_serial == 0U ||
        packet.worker_first_serial > packet.worker_last_serial ||
        packet.worker_last_serial == std::numeric_limits<std::uint64_t>::max() ||
        packet.worker_dispatch_count !=
            packet.worker_last_serial - packet.worker_first_serial + 1U ||
        !packet.worker_serial_range_contiguous) {
        return false;
    }
    if (packet.primary_first_ordinal != publication.packet_primary.primary_first_ordinal ||
        packet.primary_command_count != publication.packet_primary.primary_command_count ||
        packet.worker_first_serial != publication.packet_primary.worker_first_serial ||
        packet.worker_last_serial != publication.packet_primary.worker_last_serial ||
        publication.packet_primary.primary_stream_digest == 0U) {
        return false;
    }
    for (std::size_t index = 0U; index < publication.publication_writes.size(); ++index) {
        const auto& write = publication.publication_writes[index];
        if (!write_shape_matches(write, kExpectedPublicationWrites[index]) ||
            write.observation_epoch != packet.observation_epoch || write.timeline_event == 0U ||
            packet.eop_timeline_event >= write.timeline_event ||
            write.raw_events_observed != packet.raw_last_event ||
            write.eop_events_observed != packet.eop_event ||
            write.primary_events_observed != packet.primary_last_ordinal) {
            return false;
        }
        if (index != 0U &&
            write.timeline_event != publication.publication_writes[index - 1U].timeline_event + 1U) {
            return false;
        }
    }
    return publication.publication_writes.front().timeline_event !=
           std::numeric_limits<std::uint64_t>::max();
}

bool Slus21050FemainLivePublicationBatchClassifier::valid_publication_candidate(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainPublicationCandidate& publication) noexcept {
    if (publication.authentication_tag == 0U ||
        publication.authentication_tag !=
            authenticate_slus_21050_femain_publication_candidate(publication)) {
        return fail(Failure::PublicationCandidateAuthenticationMismatch);
    }
    if (publication.packet_primary.authentication_tag == 0U ||
        publication.packet_primary.authentication_tag !=
            authenticate_slus_21050_femain_packet_primary_candidate(publication.packet_primary)) {
        return fail(Failure::PublicationCandidatePrimaryAuthenticationMismatch);
    }
    if (publication.packet_primary.source != source) {
        return fail(Failure::PublicationCandidateSourceMismatch);
    }
    return valid_packet_provenance(source, publication) ||
           fail(Failure::PublicationEvidenceInvalid);
}

bool Slus21050FemainLivePublicationBatchClassifier::classify(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainSerializedExecutionIdentity& execution, const std::uint64_t frame_id,
    const Slus21050FemainPublicationCandidate& publication,
    const Slus21050FemainLiveTerminalCoordinator::CompleteBatch& batch) noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::Empty) {
        return fail(phase_ == Phase::Failed ? failure_ : Failure::Replay);
    }
    source_boundary_failure_ = validate_slus_21050_femain_serialized_source_boundary(source);
    if (*source_boundary_failure_ != Slus21050FemainSerializedTokenFailure::None) {
        return fail(Failure::SourceBoundaryInvalid);
    }
    source_boundary_failure_.reset();
    if (!execution_identity_present(execution)) {
        return fail(Failure::ExecutionIdentityInvalid);
    }
    if (source.execution != execution) {
        return fail(Failure::ExecutionIdentityMismatch);
    }
    if (frame_id == 0U) {
        return fail(Failure::FrameIdentityInvalid);
    }
    if (frame_id != source.frame_id) {
        return fail(Failure::FrameIdentityMismatch);
    }
    if (!valid_publication_candidate(source, publication)) {
        return false;
    }
    if (!batch.complete()) {
        return fail(Failure::CompleteBatchUnavailable);
    }
    const auto first_raw = batch.first_raw_quadword();
    const auto& packet = publication.packet_primary.packet;
    if (batch.packet_path() != packet.path || !first_raw.has_value() ||
        first_raw->path != packet.path || first_raw->quadword != packet.first_quadword) {
        return fail(Failure::BatchPacketMismatch);
    }
    if (batch.raw_quadword_count() != packet.raw_quadwords) {
        return fail(Failure::BatchRawCountMismatch);
    }
    const std::size_t command_count = batch.commands().size();
    if (command_count != packet.primary_command_count) {
        return fail(Failure::BatchCountMismatch);
    }
    if (command_count > Slus21050FemainLiveTerminalCoordinator::kMaximumCommands) {
        return fail(Failure::Capacity);
    }
    if (batch.primary_stream_digest() != publication.packet_primary.primary_stream_digest) {
        return fail(Failure::BatchDigestMismatch);
    }
    classification_ = Classification{
        .source = source,
        .publication = publication,
        .raw_quadword_count = batch.raw_quadword_count(),
        .primary_command_count = command_count,
        .diagnostic_primary_first_ordinal = batch.primary_first_ordinal(),
        .diagnostic_primary_stream_digest = batch.primary_stream_digest(),
    };
    phase_ = Phase::Classified;
    return true;
}

} // namespace takedown::runtime
