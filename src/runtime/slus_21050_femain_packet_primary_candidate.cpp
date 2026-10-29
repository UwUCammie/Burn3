#include "takedown/runtime/slus_21050_femain_packet_primary_candidate.hpp"

#include "takedown/presentation/gs_renderer_worker.hpp"
#include "takedown/runtime/slus_21050_femain_provenance_window.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace takedown::runtime {
namespace {

using Diagnostic = Slus21050FemainTerminalProducerDiagnostic;
using Failure = Slus21050FemainPacketPrimaryCandidateFailure;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kInvalidDigest = std::numeric_limits<std::uint64_t>::max();
constexpr std::string_view kCandidateDomain = "slus-21050-femain-packet-primary-candidate-v1";
constexpr std::string_view kStreamDomain = "slus-21050-femain-packet-primary-stream-v1";

void mix_word(std::uint64_t& hash, const std::uint64_t value) noexcept {
    hash = (hash ^ value) * kFnvPrime;
}

void mix_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::size_t shift = 0U; shift != 64U; shift += 8U) {
        mix_word(hash, (value >> shift) & 255U);
    }
}

void mix_domain(std::uint64_t& hash, const std::string_view domain) noexcept {
    mix_u64(hash, domain.size());
    for (const char byte : domain) {
        mix_word(hash, static_cast<std::uint8_t>(byte));
    }
}

[[nodiscard]] std::uint64_t finish(const std::uint64_t hash) noexcept {
    return hash != 0U ? hash : kInvalidDigest;
}

[[nodiscard]] bool same_packet(const Diagnostic::PacketRecord& record,
                               const Diagnostic::PacketRecord& packet) noexcept {
    return record.ordinal == packet.ordinal &&
           record.observation_epoch == packet.observation_epoch && record.path == packet.path &&
           record.raw_first_event == packet.raw_first_event &&
           record.raw_last_event == packet.raw_last_event &&
           record.eop_event == packet.eop_event &&
           record.source_event_domain_identity == packet.source_event_domain_identity &&
           record.marker_shared_event == packet.marker_shared_event &&
           record.marker_raw_shared_event == packet.marker_raw_shared_event &&
           record.eop_shared_event == packet.eop_shared_event;
}

// The renderer worker queues each decoded primary command under this kind.
[[nodiscard]] std::optional<presentation::GsRendererWorkerCommandKind> worker_kind_for(
    const Diagnostic::PrimaryCommandKind kind) noexcept {
    switch (kind) {
    case Diagnostic::PrimaryCommandKind::GifTagBegin:
        return presentation::GsRendererWorkerCommandKind::GifTagBegin;
    case Diagnostic::PrimaryCommandKind::GsStateReset:
        return presentation::GsRendererWorkerCommandKind::GsStateReset;
    case Diagnostic::PrimaryCommandKind::RegisterWrite:
        return presentation::GsRendererWorkerCommandKind::RegisterWrite;
    case Diagnostic::PrimaryCommandKind::ImageTransfer:
        return presentation::GsRendererWorkerCommandKind::ImageTransfer;
    default:
        return std::nullopt;
    }
}

} // namespace

std::string_view to_string(const Slus21050FemainPacketPrimaryCandidateFailure failure) noexcept {
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::SourcePacketCorrelationRejected:
        return "source-packet-correlation-rejected";
    case Failure::SnapshotLifecycleInvalid:
        return "snapshot-lifecycle-invalid";
    case Failure::ContextMismatch:
        return "context-mismatch";
    case Failure::WorkerEvidenceUnavailable:
        return "worker-evidence-unavailable";
    case Failure::PrimaryRangeInvalid:
        return "primary-range-invalid";
    case Failure::PrimaryRecordMismatch:
        return "primary-record-mismatch";
    case Failure::PrimaryKindInvalid:
        return "primary-kind-invalid";
    case Failure::WorkerRecordMismatch:
        return "worker-record-mismatch";
    case Failure::CommandDigestMissing:
        return "command-digest-missing";
    }
    return "unknown";
}

std::uint64_t authenticate_slus_21050_femain_packet_primary_candidate(
    const Slus21050FemainPacketPrimaryCandidate& candidate) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    mix_domain(hash, kCandidateDomain);
    mix_u64(hash, candidate.source.source_evidence.authentication_tag);
    mix_u64(hash, authenticate_slus_21050_femain_frame_graphics_source_evidence(
                             candidate.source.source_evidence));
    mix_u64(hash, candidate.source.source_packet_completion.domain_identity);
    mix_u64(hash, candidate.source.source_packet_completion.ordinal);
    mix_word(hash, static_cast<std::uint64_t>(candidate.source.source_packet_completion.kind));
    mix_u64(hash, candidate.source.execution.emulation_thread_identity);
    mix_u64(hash, candidate.source.execution.worker_epoch);
    mix_u64(hash, candidate.source.execution.runtime_epoch);
    mix_u64(hash, candidate.source.frame_id);
    mix_u64(hash, candidate.packet.ordinal);
    mix_u64(hash, candidate.packet.observation_epoch);
    mix_word(hash, static_cast<std::uint64_t>(candidate.packet.path));
    for (const unsigned int word : candidate.packet.first_quadword) {
        mix_u64(hash, word);
    }
    mix_u64(hash, candidate.packet.raw_first_event);
    mix_u64(hash, candidate.packet.raw_last_event);
    mix_u64(hash, candidate.packet.raw_quadwords);
    mix_u64(hash, candidate.packet.eop_event);
    mix_u64(hash, candidate.packet.gif_tag_low);
    mix_u64(hash, candidate.packet.gif_tag_high);
    mix_u64(hash, candidate.packet.loop_count);
    mix_u64(hash, candidate.packet.register_count);
    mix_u64(hash, candidate.packet.primitive);
    mix_u64(hash, candidate.packet.format);
    mix_word(hash, candidate.packet.eop_in_tag);
    mix_word(hash, candidate.packet.pre_in_tag);
    mix_word(hash, candidate.packet.complete);
    mix_word(hash, candidate.packet.primary_command_stream_available);
    mix_word(hash, candidate.packet.invalidated);
    mix_word(hash, candidate.packet.marker_bound);
    mix_word(hash, candidate.packet.marker_quadword_match);
    mix_u64(hash, candidate.packet.marker_raw_event);
    mix_u64(hash, candidate.packet.source_event_domain_identity);
    mix_u64(hash, candidate.packet.marker_shared_event);
    mix_u64(hash, candidate.packet.marker_raw_shared_event);
    mix_u64(hash, candidate.packet.eop_shared_event);
    mix_u64(hash, candidate.packet.marker_generation);
    mix_u64(hash, candidate.packet.marker_source);
    mix_u64(hash, candidate.packet.marker_chain_root);
    mix_u64(hash, candidate.packet.marker_tag_location);
    mix_u64(hash, candidate.packet.marker_tag_ordinal);
    mix_u64(hash, candidate.packet.marker_payload_word_ordinal);
    mix_u64(hash, candidate.packet.expected_payload_quadwords);
    mix_u64(hash, candidate.packet.primary_first_ordinal);
    mix_u64(hash, candidate.packet.primary_last_ordinal);
    mix_u64(hash, candidate.packet.primary_command_count);
    mix_u64(hash, candidate.packet.worker_first_serial);
    mix_u64(hash, candidate.packet.worker_last_serial);
    mix_u64(hash, candidate.packet.worker_dispatch_count);
    mix_word(hash, candidate.packet.worker_serial_range_contiguous);
    mix_u64(hash, candidate.primary_first_ordinal);
    mix_u64(hash, candidate.primary_command_count);
    mix_u64(hash, candidate.worker_first_serial);
    mix_u64(hash, candidate.worker_last_serial);
    mix_u64(hash, candidate.primary_stream_digest);
    return finish(hash);
}

Slus21050FemainPacketPrimaryCandidateResult derive_slus_21050_femain_packet_primary_candidate(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept {
    const auto correlation = correlate_slus_21050_femain_source_packet(source, seal);
    if (!correlation.matched() || !seal.snapshot.has_value()) {
        return {.failure = Failure::SourcePacketCorrelationRejected};
    }
    const auto& snapshot = *seal.snapshot;
    const auto& packet = *correlation.packet;
    const auto& token = source.source_evidence.token;

    if (!snapshot.context_bound || !snapshot.guest_work_scope_requested ||
        !snapshot.guest_work_scope_started || !snapshot.guest_work_scope_completed ||
        !snapshot.collection_closed || snapshot.dropped_records != 0U ||
        !snapshot.invalidations.empty()) {
        return {.failure = Failure::SnapshotLifecycleInvalid};
    }

    const auto& context = snapshot.context;
    if (context.runtime_epoch == 0U || context.worker_epoch == 0U || context.player_session == 0U ||
        context.owner_generation == 0U || context.frame_id == 0U || context.ledger_identity == 0U ||
        context.ledger_session == 0U ||
        context.runtime_epoch != source.execution.runtime_epoch ||
        context.worker_epoch != source.execution.worker_epoch ||
        context.player_session != token.player_session_generation ||
        context.owner_generation != token.frame_generation || context.frame_id != source.frame_id ||
        !token.published_reservation.has_value() ||
        context.owner_generation != token.published_reservation->frame_generation ||
        context.ledger_identity != token.published_reservation->ledger_identity ||
        context.ledger_session != token.published_reservation->session_generation) {
        return {.failure = Failure::ContextMismatch};
    }

    if (!snapshot.worker_dispatch_expected || !snapshot.worker_mapping_valid ||
        !snapshot.guest_work_scope_worker_fence_completed ||
        snapshot.guest_work_scope_worker_fence_serial == 0U) {
        return {.failure = Failure::WorkerEvidenceUnavailable};
    }

    if (packet.primary_command_count == 0U || packet.primary_first_ordinal == 0U || packet.primary_first_ordinal > packet.primary_last_ordinal ||
        packet.primary_last_ordinal == std::numeric_limits<std::uint64_t>::max() ||
        packet.primary_command_count != packet.primary_last_ordinal - packet.primary_first_ordinal + 1U ||
        packet.primary_command_count != packet.worker_dispatch_count || packet.worker_first_serial == 0U ||
        packet.worker_first_serial > packet.worker_last_serial ||
        packet.worker_last_serial == std::numeric_limits<std::uint64_t>::max() ||
        packet.worker_dispatch_count != packet.worker_last_serial - packet.worker_first_serial + 1U ||
        !packet.worker_serial_range_contiguous ||
        snapshot.guest_work_scope_worker_fence_serial <= packet.worker_last_serial) {
        return {.failure = Failure::PrimaryRangeInvalid};
    }

    // The correlated packet must appear exactly once in the sealed snapshot.
    std::size_t packet_index = std::numeric_limits<std::size_t>::max();
    for (std::size_t index = 0U; index < snapshot.packets.size(); ++index) {
        if (!same_packet(snapshot.packets[index], packet)) {
            continue;
        }
        if (packet_index != std::numeric_limits<std::size_t>::max()) {
            return {.failure = Failure::SourcePacketCorrelationRejected};
        }
        packet_index = index;
    }
    if (packet_index == std::numeric_limits<std::size_t>::max()) {
        return {.failure = Failure::SourcePacketCorrelationRejected};
    }

    std::uint64_t previous_primary_ordinal = 0U;
    for (const auto& command : snapshot.primary_commands) {
        if (command.observation_epoch != packet.observation_epoch || command.ordinal == 0U ||
            command.ordinal <= previous_primary_ordinal) {
            return {.failure = Failure::PrimaryRecordMismatch};
        }
        previous_primary_ordinal = command.ordinal;
    }
    std::uint64_t previous_worker_serial = 0U;
    for (const auto& dispatch : snapshot.worker_dispatches) {
        if (dispatch.observation_epoch != packet.observation_epoch || dispatch.serial == 0U ||
            dispatch.serial <= previous_worker_serial) {
            return {.failure = Failure::WorkerRecordMismatch};
        }
        previous_worker_serial = dispatch.serial;
    }

    std::uint64_t stream_digest = kFnvOffsetBasis;
    mix_domain(stream_digest, kStreamDomain);
    mix_u64(stream_digest, packet.primary_command_count);
    std::uint64_t expected_primary = packet.primary_first_ordinal;
    std::uint64_t expected_worker = packet.worker_first_serial;
    std::uint64_t matched_primary = 0U;
    std::size_t worker_cursor = 0U;
    for (const auto& command : snapshot.primary_commands) {
        if (command.packet_record != packet_index) {
            continue;
        }
        if (command.observation_epoch != packet.observation_epoch ||
            command.ordinal != expected_primary || command.path != packet.path ||
            !command.worker_dispatched || command.worker_serial != expected_worker) {
            return {.failure = Failure::PrimaryRecordMismatch};
        }
        if (command.command_digest == 0U) {
            return {.failure = Failure::CommandDigestMissing};
        }
        if (command.kind == Diagnostic::PrimaryCommandKind::GsStateReset) {
            return {.failure = Failure::PrimaryKindInvalid};
        }
        const auto expected_worker_kind = worker_kind_for(command.kind);
        if (!expected_worker_kind.has_value()) {
            return {.failure = Failure::PrimaryKindInvalid};
        }
        while (worker_cursor < snapshot.worker_dispatches.size() &&
               command.worker_serial > snapshot.worker_dispatches[worker_cursor].serial) {
            ++worker_cursor;
        }
        if (worker_cursor == snapshot.worker_dispatches.size() ||
            snapshot.worker_dispatches[worker_cursor].serial != command.worker_serial ||
            snapshot.worker_dispatches[worker_cursor].kind != *expected_worker_kind) {
            return {.failure = Failure::WorkerRecordMismatch};
        }
        ++worker_cursor;
        mix_u64(stream_digest, command.ordinal);
        mix_word(stream_digest, static_cast<std::uint64_t>(command.kind));
        mix_word(stream_digest, static_cast<std::uint64_t>(command.path));
        mix_u64(stream_digest, command.guest_sequence);
        mix_u64(stream_digest, command.command_digest);
        ++matched_primary;
        ++expected_primary;
        ++expected_worker;
    }
    if (matched_primary != packet.primary_command_count || expected_primary - 1U != packet.primary_last_ordinal ||
        expected_worker - 1U != packet.worker_last_serial) {
        return {.failure = Failure::PrimaryRecordMismatch};
    }

    Slus21050FemainPacketPrimaryCandidate candidate{
        .source = source,
        .packet = *correlation.packet,
        .primary_first_ordinal = packet.primary_first_ordinal,
        .primary_command_count = packet.primary_command_count,
        .worker_first_serial = packet.worker_first_serial,
        .worker_last_serial = packet.worker_last_serial,
        .primary_stream_digest = finish(stream_digest),
    };
    candidate.authentication_tag = authenticate_slus_21050_femain_packet_primary_candidate(candidate);
    return {.failure = Failure::None, .candidate = std::move(candidate)};
}

} // namespace takedown::runtime
