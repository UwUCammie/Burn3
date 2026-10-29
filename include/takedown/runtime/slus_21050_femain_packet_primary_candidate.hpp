#pragma once

#include "takedown/runtime/slus_21050_femain_serialized_token.hpp"
#include "takedown/runtime/slus_21050_femain_terminal_producer_diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

enum class Slus21050FemainPacketPrimaryCandidateFailure : std::uint8_t {
    None,
    SourcePacketCorrelationRejected,
    SnapshotLifecycleInvalid,
    ContextMismatch,
    WorkerEvidenceUnavailable,
    PrimaryRangeInvalid,
    PrimaryRecordMismatch,
    PrimaryKindInvalid,
    WorkerRecordMismatch,
    CommandDigestMissing,
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainPacketPrimaryCandidateFailure failure) noexcept;

// Owned, value-only evidence that every decoded primary command belonging to
// the exactly correlated source packet reached the local renderer worker FIFO.
// This authenticates packet content and delivery shape only. It does not
// classify the packet as the last packet of a movie frame and cannot serve as
// terminal, renderer-completion, consume, scanout, or presentation authority.
struct Slus21050FemainPacketPrimaryCandidate {
    Slus21050FemainSerializedSourceBoundary source{};
    Slus21050FemainTerminalProducerDiagnostic::PacketRecord packet{};
    std::uint64_t primary_first_ordinal{};
    std::uint64_t primary_command_count{};
    std::uint64_t worker_first_serial{};
    std::uint64_t worker_last_serial{};
    std::uint64_t primary_stream_digest{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainPacketPrimaryCandidate&) const = default;
};

struct Slus21050FemainPacketPrimaryCandidateResult {
    Slus21050FemainPacketPrimaryCandidateFailure failure{
        Slus21050FemainPacketPrimaryCandidateFailure::SourcePacketCorrelationRejected};
    std::optional<Slus21050FemainPacketPrimaryCandidate> candidate{};

    [[nodiscard]] bool derived() const noexcept {
        return failure == Slus21050FemainPacketPrimaryCandidateFailure::None &&
               candidate.has_value();
    }
};

[[nodiscard]] Slus21050FemainPacketPrimaryCandidateResult
derive_slus_21050_femain_packet_primary_candidate(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_packet_primary_candidate(
    const Slus21050FemainPacketPrimaryCandidate& candidate) noexcept;

} // namespace takedown::runtime
