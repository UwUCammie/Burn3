#include "takedown/runtime/slus_21050_femain_provenance_window.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

namespace takedown::runtime {
namespace {

std::uint64_t allocate_domain_identity() noexcept {
    static std::atomic<std::uint64_t> next{0U};
    return next.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace

std::string_view to_string(const Slus21050FemainProvenanceWindowFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainProvenanceWindowFailure::None:
        return "none";
    case Slus21050FemainProvenanceWindowFailure::Disabled:
        return "disabled";
    case Slus21050FemainProvenanceWindowFailure::AlreadyStarted:
        return "already-started";
    case Slus21050FemainProvenanceWindowFailure::NotStarted:
        return "not-started";
    case Slus21050FemainProvenanceWindowFailure::AlreadyCompleted:
        return "already-completed";
    case Slus21050FemainProvenanceWindowFailure::EmulationThreadChanged:
        return "emulation-thread-changed";
    case Slus21050FemainProvenanceWindowFailure::MeasurementCounterReordered:
        return "measurement-counter-reordered";
    case Slus21050FemainProvenanceWindowFailure::SnapshotLifecycleMismatch:
        return "snapshot-lifecycle-mismatch";
    case Slus21050FemainProvenanceWindowFailure::SourceAlreadyBound:
        return "source-already-bound";
    case Slus21050FemainProvenanceWindowFailure::SourceEvidenceInvalid:
        return "source-evidence-invalid";
    case Slus21050FemainProvenanceWindowFailure::SourceReservationMissing:
        return "source-reservation-missing";
    case Slus21050FemainProvenanceWindowFailure::SourceEventUnavailable:
        return "source-event-unavailable";
    case Slus21050FemainProvenanceWindowFailure::SourceBuilderRejected:
        return "source-builder-rejected";
    case Slus21050FemainProvenanceWindowFailure::SourcePacketCorrelationRejected:
        return "source-packet-correlation-rejected";
    case Slus21050FemainProvenanceWindowFailure::ReportUnavailable:
        return "report-unavailable";
    default:
        return "unknown";
    }
}

Slus21050FemainProvenanceWindow::Slus21050FemainProvenanceWindow(const bool enabled)
    : enabled_(enabled),
      phase_(enabled ? Phase::Ready : Phase::Disabled),
      failure_(Slus21050FemainProvenanceWindowFailure::None),
      token_builder_(enabled) {
    if (enabled) {
        const std::uint64_t previous = allocate_domain_identity();
        const std::uint64_t identity = previous != 0U ? previous : std::numeric_limits<std::uint64_t>::max();
        event_domain_ = std::make_unique<Slus21050FemainSerializedEventDomain>(identity, 1U);
    } else {
        event_domain_.reset();
    }
}

Slus21050FemainProvenanceWindow::~Slus21050FemainProvenanceWindow() = default;

Slus21050FemainProvenanceWindowFailure
Slus21050FemainProvenanceWindow::fail(const Slus21050FemainProvenanceWindowFailure failure) noexcept {
    if (phase_ == Phase::Failed) {
        return failure_;
    }
    failure_ = failure;
    phase_ = Phase::Failed;
    return failure;
}

Slus21050FemainProvenanceWindowFailure Slus21050FemainProvenanceWindow::begin(
    const Slus21050FemainProvenanceBoundary& boundary,
    const std::uint64_t diagnostic_observation_epoch) noexcept {
    if (!enabled_) {
        return Slus21050FemainProvenanceWindowFailure::Disabled;
    }
    if (phase_ == Phase::Failed) {
        return failure_;
    }
    if (phase_ != Phase::Ready) {
        return fail(Slus21050FemainProvenanceWindowFailure::AlreadyStarted);
    }
    if (event_domain_ == nullptr || !event_domain_->valid() || diagnostic_observation_epoch == 0U) {
        return fail(Slus21050FemainProvenanceWindowFailure::ReportUnavailable);
    }

    emulation_thread_ = std::this_thread::get_id();
    diagnostic_observation_epoch_ = diagnostic_observation_epoch;
    start_ = boundary;
    phase_ = Phase::Started;
    return Slus21050FemainProvenanceWindowFailure::None;
}

std::uint64_t Slus21050FemainProvenanceWindow::event_domain_identity() const noexcept {
    return event_domain_ != nullptr ? event_domain_->domain_identity() : 0U;
}

std::uint64_t Slus21050FemainProvenanceWindow::event_domain_last_ordinal() const noexcept {
    return event_domain_ != nullptr ? event_domain_->last_ordinal() : 0U;
}

const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult*
Slus21050FemainProvenanceWindow::seal_result() const noexcept {
    return seal_result_.has_value() ? &*seal_result_ : nullptr;
}

Slus21050FemainSourcePacketCorrelationResult correlate_slus_21050_femain_source_packet(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept {
    using Failure = Slus21050FemainSourcePacketCorrelationFailure;
    using Result = Slus21050FemainSourcePacketCorrelationResult;
    using PacketRecord = Slus21050FemainTerminalProducerDiagnostic::PacketRecord;

    const auto& evidence = source.source_evidence;
    if (validate_slus_21050_femain_serialized_source_boundary(source) != Slus21050FemainSerializedTokenFailure::None ||
        !evidence.token.published_reservation.has_value() || evidence.diagnostic_observation_epoch == 0U) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }
    if (seal.failure != Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure::None ||
        !seal.snapshot.has_value()) {
        return Result{Failure::SnapshotUnavailable, std::nullopt};
    }

    const auto& marker_event = evidence.diagnostic_marker_event;
    const auto& raw_event = evidence.diagnostic_raw_submission_event;
    const auto& eop_event = evidence.diagnostic_packet_completion_event;
    if (!marker_event.valid() || !raw_event.valid() || !eop_event.valid()) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }
    if (marker_event.domain_identity != raw_event.domain_identity ||
        marker_event.domain_identity != eop_event.domain_identity) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }
    const std::uint64_t epoch = evidence.diagnostic_observation_epoch;
    if (epoch != marker_event.observation_epoch || epoch != raw_event.observation_epoch ||
        epoch != eop_event.observation_epoch) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }
    // Event kinds in the shared domain: marker reached = 0, raw submission = 1,
    // packet completion = 2.
    if (static_cast<std::uint8_t>(marker_event.kind) != 0U ||
        static_cast<std::uint8_t>(raw_event.kind) != 1U ||
        static_cast<std::uint8_t>(eop_event.kind) != 2U) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }
    if (marker_event.ordinal >= raw_event.ordinal || raw_event.ordinal >= eop_event.ordinal) {
        return Result{Failure::SourceEvidenceInvalid, std::nullopt};
    }

    const std::uint64_t snapshot_epoch = seal.snapshot->observation_epoch;
    if (snapshot_epoch == 0U || epoch != snapshot_epoch) {
        return Result{Failure::ObservationEpochMismatch, std::nullopt};
    }

    const auto& marker = evidence.token.source_marker;
    std::optional<PacketRecord> match{};
    for (const PacketRecord& packet : seal.snapshot->packets) {
        if (packet.observation_epoch != snapshot_epoch) {
            continue;
        }
        // GIF path 2 carries the shared-domain PATH2 source marker.
        if (static_cast<std::uint8_t>(packet.path) != 1U) {
            continue;
        }
        if (!packet.complete || packet.invalidated || !packet.primary_command_stream_available ||
            !packet.marker_bound || !packet.marker_quadword_match) {
            continue;
        }
        if (packet.ordinal == 0U || packet.raw_first_event == 0U ||
            packet.raw_first_event > packet.raw_last_event || packet.raw_quadwords == 0U) {
            continue;
        }
        if (packet.raw_first_event > packet.marker_raw_event ||
            packet.raw_last_event < packet.marker_raw_event) {
            continue;
        }
        if (packet.eop_event == 0U) {
            continue;
        }
        if (packet.source_event_domain_identity != marker_event.domain_identity ||
            packet.marker_shared_event != marker_event.ordinal ||
            packet.marker_raw_shared_event != raw_event.ordinal ||
            packet.eop_shared_event != eop_event.ordinal) {
            continue;
        }
        if (packet.marker_generation != marker.generation ||
            packet.marker_source != marker.request.source ||
            packet.marker_chain_root != marker.request.chain_root ||
            packet.marker_tag_location != marker.request.source_tag_location ||
            packet.marker_tag_ordinal != marker.request.source_tag_ordinal ||
            packet.marker_payload_word_ordinal != marker.request.source_payload_word_ordinal) {
            continue;
        }
        if (match.has_value()) {
            return Result{Failure::PacketAmbiguous, std::nullopt};
        }
        match = packet;
    }
    if (!match.has_value()) {
        return Result{Failure::PacketNotFound, std::nullopt};
    }
    return Result{Failure::None, match};
}

Slus21050FemainProvenanceWindowFailure
Slus21050FemainProvenanceWindow::bind_published_source_candidate(
    Slus21050FemainFrameGraphicsSourceProbe& probe,
    const Slus21050FemainSerializedExecutionIdentity& execution,
    const std::uint64_t frame_id,
    const std::uint64_t diagnostic_observation_epoch) noexcept {
    using Failure = Slus21050FemainProvenanceWindowFailure;

    if (!enabled_) {
        return Failure::Disabled;
    }
    if (phase_ == Phase::Failed) {
        return failure_;
    }
    if (phase_ == Phase::Completed) {
        return Failure::AlreadyCompleted;
    }
    if (phase_ != Phase::Started || !start_.has_value()) {
        return fail(Failure::NotStarted);
    }
    if (emulation_thread_ != std::this_thread::get_id()) {
        if (phase_ == Phase::Failed) {
            return failure_;
        }
        return fail(Failure::EmulationThreadChanged);
    }

    // Shared binding: one source per window, bound only in the started
    // window's event domain at the window's diagnostic epoch.
    if (source_boundary_.has_value() ||
        token_builder_.phase() != Slus21050FemainSerializedTokenBuilder::Phase::Empty ||
        event_domain_ == nullptr || event_domain_->last_ordinal() != 0U) {
        return fail(Failure::SourceAlreadyBound);
    }
    if (diagnostic_observation_epoch == 0U || diagnostic_observation_epoch != diagnostic_observation_epoch_) {
        return fail(Failure::SourceEvidenceInvalid);
    }
    if (frame_id == 0U || execution.emulation_thread_identity == 0U ||
        execution.worker_epoch == 0U || execution.runtime_epoch == 0U) {
        return fail(Failure::SourceEvidenceInvalid);
    }
    if (probe.diagnostic_observation_epoch() == 0U ||
        probe.diagnostic_observation_epoch() != diagnostic_observation_epoch_) {
        return fail(Failure::SourceEvidenceInvalid);
    }
    auto evidence = probe.take_evidence();
    if (!evidence.has_value()) {
        return fail(Failure::SourceEvidenceInvalid);
    }
    if (evidence->diagnostic_observation_epoch == 0U ||
        evidence->diagnostic_observation_epoch != diagnostic_observation_epoch_) {
        return fail(Failure::SourceEvidenceInvalid);
    }
    if (!evidence->token.published_reservation.has_value()) {
        return fail(Failure::SourceReservationMissing);
    }

    Slus21050FemainSerializedSourceBoundary candidate{};
    candidate.source_packet_completion.ordinal = 1U;
    candidate.source_evidence = *evidence;
    candidate.source_packet_completion.domain_identity = event_domain_->domain_identity();
    candidate.execution = execution;
    candidate.frame_id = frame_id;
    if (validate_slus_21050_femain_serialized_source_boundary(candidate) !=
        Slus21050FemainSerializedTokenFailure::None) {
        return fail(Failure::SourceEvidenceInvalid);
    }

    const auto event = event_domain_->issue(static_cast<Slus21050FemainSerializedEventKind>(0U));
    if (!event.has_value()) {
        return fail(Failure::SourceEventUnavailable);
    }
    if (event->domain_identity != candidate.source_packet_completion.domain_identity ||
        event->ordinal != candidate.source_packet_completion.ordinal ||
        event->kind != candidate.source_packet_completion.kind) {
        return fail(Failure::SourceEventUnavailable);
    }

    const auto bound = token_builder_.bind_source(candidate);
    if (bound.failure != Slus21050FemainSerializedTokenFailure::None || bound.token.has_value()) {
        return fail(Failure::SourceBuilderRejected);
    }
    source_boundary_ = candidate;
    source_observation_epoch_ = diagnostic_observation_epoch_;
    return Failure::None;
}

Slus21050FemainProvenanceWindowFailure Slus21050FemainProvenanceWindow::complete(
    const Slus21050FemainProvenanceBoundary& boundary,
    Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult seal_result) noexcept {
    using Failure = Slus21050FemainProvenanceWindowFailure;

    if (!enabled_) {
        return Failure::Disabled;
    }
    if (phase_ == Phase::Failed) {
        return failure_;
    }
    if (phase_ == Phase::Completed) {
        return Failure::AlreadyCompleted;
    }
    if (phase_ != Phase::Started || !start_.has_value()) {
        return fail(Failure::NotStarted);
    }
    if (emulation_thread_ != std::this_thread::get_id()) {
        if (phase_ == Phase::Failed) {
            return failure_;
        }
        return fail(Failure::EmulationThreadChanged);
    }

    // Guest work measurements are monotonic from start to end.
    const auto& start = start_->measurement;
    const auto& end = boundary.measurement;
    if (end.committed_steps < start.committed_steps || end.ee_cycles < start.ee_cycles ||
        end.vblank_starts < start.vblank_starts || end.audio_frames < start.audio_frames ||
        end.monotonic_nanoseconds < start.monotonic_nanoseconds) {
        return fail(Failure::MeasurementCounterReordered);
    }

    if (seal_result.failure == Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure::None
            ? !seal_result.snapshot.has_value()
            : seal_result.snapshot.has_value()) {
        return fail(Failure::SnapshotLifecycleMismatch);
    }
    if (seal_result.snapshot.has_value()) {
        const auto& snapshot = *seal_result.snapshot;
        if (snapshot.observation_epoch != diagnostic_observation_epoch_ ||
            !snapshot.guest_work_scope_requested || !snapshot.guest_work_scope_started ||
            !snapshot.guest_work_scope_completed || !snapshot.collection_closed) {
            return fail(Failure::SnapshotLifecycleMismatch);
        }
    }

    if (source_boundary_.has_value()) {
        const auto correlation = correlate_slus_21050_femain_source_packet(*source_boundary_, seal_result);
        if (correlation.failure != Slus21050FemainSourcePacketCorrelationFailure::None ||
            !correlation.packet.has_value()) {
            return fail(Failure::SourcePacketCorrelationRejected);
        }
        source_packet_ = correlation.packet;

        const auto publication = derive_slus_21050_femain_publication_candidate(*source_boundary_, seal_result);
        publication_candidate_failure_ = publication.failure;
        if (publication.candidate.has_value()) {
            publication_candidate_ = publication.candidate;
        }
    }

    end_ = boundary;
    seal_result_ = std::move(seal_result);
    phase_ = Phase::Completed;
    failure_ = Failure::None;
    return Failure::None;
}

bool Slus21050FemainProvenanceWindow::append_report(const std::filesystem::path& path) {
    if (phase_ != Phase::Completed || !start_.has_value() || !end_.has_value() || !seal_result_.has_value() ||
        event_domain_ == nullptr) {
        return false;
    }
    if (report_appended_ || emulation_thread_ != std::this_thread::get_id()) {
        return false;
    }
    report_appended_ = true;

    std::ofstream output;
    output.open(path.c_str(), std::ios::out | std::ios::app);
    if (!output.is_open()) {
        output.setstate(std::ios::failbit);
    }
    if ((output.rdstate() & (std::ios::badbit | std::ios::failbit)) != 0) {
        output.close();
        return false;
    }

    const auto phase_name = [](const Phase phase) -> std::string_view {
        switch (phase) {
        case Phase::Disabled:
            return "disabled";
        case Phase::Ready:
            return "ready";
        case Phase::Started:
            return "started";
        case Phase::Completed:
            return "completed";
        case Phase::Failed:
            return "failed";
        default:
            return "unknown";
        }
    };
    using Builder = Slus21050FemainSerializedTokenBuilder::Phase;
    const auto builder_phase_name = [](const Builder phase) -> std::string_view {
        switch (phase) {
        case Builder::Disabled:
            return "disabled";
        case Builder::Empty:
            return "empty";
        case Builder::SourceBound:
            return "source-bound";
        case Builder::Completed:
            return "completed";
        case Builder::Consumed:
            return "consumed";
        case Builder::Failed:
            return "failed";
        default:
            return "unknown";
        }
    };

    output << "provenance.schema=1\n";
    output << "provenance.phase=" << phase_name(phase_) << "\n";
    output << "provenance.failure=" << to_string(failure_) << "\n";
    output << "provenance.terminal_authority=none\n";
    output << "provenance.source_authority=none\n";
    output << "provenance.serialized_token_emitted=0\n";
    output << "provenance.publication_candidate="
           << (publication_candidate_.has_value() ? "available" : "none") << "\n";
    output << "provenance.publication_candidate_failure=" << to_string(publication_candidate_failure_) << "\n";
    output << "provenance.serialized_builder_phase=" << builder_phase_name(token_builder_.phase()) << "\n";
    output << "provenance.event_domain_identity=" << event_domain_->domain_identity() << "\n";
    output << "provenance.event_domain_last_ordinal=" << event_domain_->last_ordinal() << "\n";
    output << "provenance.emulation_thread_token=0x" << std::hex
           << std::hash<std::thread::id>{}(emulation_thread_) << std::dec << "\n";
    output << "provenance.runtime_epoch=unavailable\n";
    output << "provenance.worker_epoch=unavailable\n";
    output << "provenance.diagnostic_observation_epoch=" << diagnostic_observation_epoch_ << "\n";

    const auto& seal = *seal_result_;
    const bool sealed = seal.failure == Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure::None &&
                        seal.snapshot.has_value();
    output << "provenance.snapshot_seal=" << (sealed ? "sealed" : "rejected") << "\n";
    output << "provenance.snapshot_seal_failure=" << to_string(seal.failure) << "\n";

    output << "provenance.start.committed_steps=" << start_->measurement.committed_steps << "\n";
    output << "provenance.start.ee_cycles=" << start_->measurement.ee_cycles << "\n";
    output << "provenance.start.vblank_starts=" << start_->measurement.vblank_starts << "\n";
    output << "provenance.start.audio_frames=" << start_->measurement.audio_frames << "\n";
    output << "provenance.start.audio_fnv1a64=" << start_->measurement.audio_fnv1a64 << "\n";
    output << "provenance.start.monotonic_nanoseconds=" << start_->measurement.monotonic_nanoseconds << "\n";
    output << "provenance.start.guest_pc=0x" << std::hex << start_->guest_pc << std::dec << "\n";
    output << "provenance.start.guest_thread_id=" << start_->guest_thread_id << "\n";
    output << "provenance.start.guest_callback_active=" << static_cast<int>(start_->guest_callback_active) << "\n";
    output << "provenance.start.guest_callback_identity=" << start_->guest_callback_identity << "\n";
    output << "provenance.end.committed_steps=" << end_->measurement.committed_steps << "\n";
    output << "provenance.end.ee_cycles=" << end_->measurement.ee_cycles << "\n";
    output << "provenance.end.vblank_starts=" << end_->measurement.vblank_starts << "\n";
    output << "provenance.end.audio_frames=" << end_->measurement.audio_frames << "\n";
    output << "provenance.end.audio_fnv1a64=" << end_->measurement.audio_fnv1a64 << "\n";
    output << "provenance.end.monotonic_nanoseconds=" << end_->measurement.monotonic_nanoseconds << "\n";
    output << "provenance.end.guest_pc=0x" << std::hex << end_->guest_pc << std::dec << "\n";
    output << "provenance.end.guest_thread_id=" << end_->guest_thread_id << "\n";
    output << "provenance.end.guest_callback_active=" << static_cast<int>(end_->guest_callback_active) << "\n";
    output << "provenance.end.guest_callback_identity=" << end_->guest_callback_identity << "\n";

    if (source_boundary_.has_value()) {
        const auto& source = *source_boundary_;
        const auto& evidence = source.source_evidence;
        const auto& reservation = *evidence.token.published_reservation;
        output << "provenance.source.status=published-path2-eop-candidate\n";
        output << "provenance.source.observation_epoch=" << source_observation_epoch_ << "\n";
        output << "provenance.source.frame_id=" << source.frame_id << "\n";
        output << "provenance.source.shared_event_ordinal=" << source.source_packet_completion.ordinal << "\n";
        output << "provenance.source.marker_event_ordinal=" << evidence.source_event_ordinal << "\n";
        output << "provenance.source.raw_event_ordinal=" << evidence.raw_submission_event_ordinal << "\n";
        output << "provenance.source.eop_event_ordinal=" << evidence.packet_completion_event_ordinal << "\n";
        output << "provenance.source.ledger_identity=" << reservation.ledger_identity << "\n";
        output << "provenance.source.ledger_session=" << reservation.session_generation << "\n";
        output << "provenance.source.frame_generation=" << reservation.frame_generation << "\n";
        output << "provenance.source.slot=" << static_cast<unsigned int>(reservation.slot) << "\n";
        output << "provenance.source.media_ordinal=" << reservation.frame.media_ordinal << "\n";
        output << "provenance.source.execution_thread=" << source.execution.emulation_thread_identity << "\n";
        output << "provenance.source.worker_epoch=" << source.execution.worker_epoch << "\n";
        output << "provenance.source.runtime_epoch=" << source.execution.runtime_epoch << "\n";
    }

    if (seal.snapshot.has_value()) {
        const auto& snapshot = *seal.snapshot;
        output << "provenance.snapshot.context_bound=" << static_cast<int>(snapshot.context_bound) << "\n";
        output << "provenance.snapshot.context.runtime_epoch=" << snapshot.context.runtime_epoch << "\n";
        output << "provenance.snapshot.context.worker_epoch=" << snapshot.context.worker_epoch << "\n";
        output << "provenance.snapshot.worker_dispatch_expected="
               << static_cast<int>(snapshot.worker_dispatch_expected) << "\n";
        output << "provenance.snapshot.worker_fence_completed="
               << static_cast<int>(snapshot.guest_work_scope_worker_fence_completed) << "\n";
        output << "provenance.snapshot.worker_fence_serial=" << snapshot.guest_work_scope_worker_fence_serial << "\n";
    }

    const bool ok = (output.rdstate() & (std::ios::badbit | std::ios::failbit)) == 0;
    output.close();
    return ok;
}
} // namespace takedown::runtime
