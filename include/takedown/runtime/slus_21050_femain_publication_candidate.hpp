#pragma once

#include "takedown/runtime/slus_21050_femain_packet_primary_candidate.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

enum class Slus21050FemainPublicationCandidateFailure : std::uint8_t {
    None,
    SourceCandidateUnavailable,
    PacketPrimaryCandidateRejected,
    SnapshotUnavailable,
    PublicationWriteMissing,
    PublicationWriteAmbiguous,
    PublicationWriteInvalid,
    CounterMismatch,
    InterveningGraphicsEvent,
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainPublicationCandidateFailure failure) noexcept;

// Value-only evidence that the exact authenticated source packet and its
// complete local-worker primary stream were followed by the retail FEMain
// queue-end -> queue-read -> buffer-index publication stores without an
// intervening graphics callback. This remains a candidate: the scalar stores
// do not by themselves prove that this packet is the last packet of a frame.
struct Slus21050FemainPublicationCandidate {
    Slus21050FemainPacketPrimaryCandidate packet_primary{};
    std::array<Slus21050FemainTerminalProducerDiagnostic::ScalarWriteRecord, 3U>
        publication_writes{};
    std::uint64_t authentication_tag{};
};

struct Slus21050FemainPublicationCandidateResult {
    Slus21050FemainPublicationCandidateFailure failure{
        Slus21050FemainPublicationCandidateFailure::PacketPrimaryCandidateRejected};
    std::optional<Slus21050FemainPublicationCandidate> candidate{};

    [[nodiscard]] bool derived() const noexcept {
        return failure == Slus21050FemainPublicationCandidateFailure::None && candidate.has_value();
    }
};

[[nodiscard]] Slus21050FemainPublicationCandidateResult
derive_slus_21050_femain_publication_candidate(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_publication_candidate(
    const Slus21050FemainPublicationCandidate& candidate) noexcept;

} // namespace takedown::runtime
