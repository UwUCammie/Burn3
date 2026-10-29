#include "takedown/runtime/slus_21050_femain_publication_candidate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace takedown::runtime {
namespace {

// Runtime-visible copy of the domain separator (not a compile-time constant).
const char* kCandidateDomain = "slus-21050-femain-publication-candidate-v1";

using ScalarWriteRecord = Slus21050FemainTerminalProducerDiagnostic::ScalarWriteRecord;
using ScalarField = Slus21050FemainTerminalProducerDiagnostic::ScalarField;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Expected shape of one retained scalar write, recovered from the retail
// derivation's constant table (queue-end, queue-read, buffer-index stores).
struct ExpectedWrite {
    ScalarField field{ScalarField::Unknown};
    guest::GuestAddress pc{};
    guest::GuestAddress offset{};
    std::uint8_t width{};
};

constexpr std::array<ExpectedWrite, 3> kExpectedWrites{{
    ExpectedWrite{ScalarField::QueueEnd, 0x1f254cU, 0x4e283cU, 4U},
    ExpectedWrite{ScalarField::QueueRead, 0x1f2554U, 0x4e2838U, 4U},
    ExpectedWrite{ScalarField::BufferIndex, 0x1f2564U, 0x4e2804U, 1U},
}};

[[nodiscard]] std::uint64_t mix_word(const std::uint64_t hash, const std::uint64_t value) noexcept {
    return (hash ^ value) * kFnvPrime;
}

[[nodiscard]] std::uint64_t mix_bytes(std::uint64_t hash, const std::uint64_t value) noexcept {
    #pragma GCC unroll 0
    for (std::size_t shift = 0U; shift != 64U; shift += 8U) {
        hash = mix_word(hash, (value >> shift) & 255U);
    }
    return hash;
}

[[nodiscard]] bool write_shape_matches(const ScalarWriteRecord& write, const ExpectedWrite& expected) noexcept {
    return write.field == expected.field &&
           write.event.pc == expected.pc &&
           write.event.original_address == expected.offset &&
           write.event.offset == expected.offset &&
           write.event.width == expected.width &&
           write.event.previous_payload_available &&
           write.event.domain == guest::EeMemoryWriteDomain::MainRam;
}

} // namespace

std::string_view to_string(const Slus21050FemainPublicationCandidateFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainPublicationCandidateFailure::None:
        return "none";
    case Slus21050FemainPublicationCandidateFailure::SourceCandidateUnavailable:
        return "source-candidate-unavailable";
    case Slus21050FemainPublicationCandidateFailure::PacketPrimaryCandidateRejected:
        return "packet-primary-candidate-rejected";
    case Slus21050FemainPublicationCandidateFailure::SnapshotUnavailable:
        return "snapshot-unavailable";
    case Slus21050FemainPublicationCandidateFailure::PublicationWriteMissing:
        return "publication-write-missing";
    case Slus21050FemainPublicationCandidateFailure::PublicationWriteAmbiguous:
        return "publication-write-ambiguous";
    case Slus21050FemainPublicationCandidateFailure::PublicationWriteInvalid:
        return "publication-write-invalid";
    case Slus21050FemainPublicationCandidateFailure::CounterMismatch:
        return "counter-mismatch";
    case Slus21050FemainPublicationCandidateFailure::InterveningGraphicsEvent:
        return "intervening-graphics-event";
    default:
        return "unknown";
    }
}

std::uint64_t authenticate_slus_21050_femain_publication_candidate(
    const Slus21050FemainPublicationCandidate& candidate) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash = mix_bytes(hash, 42U);
    const char* const domain = kCandidateDomain;
    #pragma GCC unroll 0
    for (const char* byte = domain; byte != domain + 42; ++byte) {
        hash = mix_word(hash, static_cast<std::uint8_t>(*byte));
    }
    hash = mix_bytes(hash, candidate.packet_primary.authentication_tag);
    hash = mix_bytes(hash, authenticate_slus_21050_femain_packet_primary_candidate(candidate.packet_primary));
    hash = mix_bytes(hash, candidate.packet_primary.packet.raw_first_timeline_event);
    hash = mix_bytes(hash, candidate.packet_primary.packet.raw_last_timeline_event);
    hash = mix_bytes(hash, candidate.packet_primary.packet.eop_timeline_event);

    for (const ScalarWriteRecord& write : candidate.publication_writes) {
        hash = mix_bytes(hash, write.observation_epoch);
        hash = mix_bytes(hash, write.timeline_event);
        hash = mix_bytes(hash, write.raw_events_observed);
        hash = mix_bytes(hash, write.eop_events_observed);
        hash = mix_bytes(hash, write.primary_events_observed);
        hash = mix_word(hash, static_cast<std::uint64_t>(write.field));
        hash = mix_bytes(hash, write.event.guest_step);
        hash = mix_bytes(hash, write.event.original_address);
        hash = mix_bytes(hash, write.event.pc);
        hash = mix_bytes(hash, write.event.offset);
        hash = mix_bytes(hash, write.event.changed_byte_mask);
        const std::uint64_t width = write.event.width;
        hash = mix_word(hash, width);
        hash = mix_word(hash, static_cast<std::uint64_t>(write.event.domain));
        hash = mix_word(hash, write.event.previous_payload_available ? 1U : 0U);
        for (std::uint64_t index = 0U; index < std::min<std::uint64_t>(width, 16U); ++index) {
            hash = mix_word(hash, write.event.payload[index]);
            hash = mix_word(hash, write.event.previous_payload[index]);
        }
    }
    return hash;
}

Slus21050FemainPublicationCandidateResult derive_slus_21050_femain_publication_candidate(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept {
    if (seal.failure != Slus21050FemainTerminalProducerDiagnostic::SnapshotSealFailure::None || !seal.snapshot.has_value()) {
        return {Slus21050FemainPublicationCandidateFailure::SnapshotUnavailable, {}};
    }
    const auto packet_primary = derive_slus_21050_femain_packet_primary_candidate(source, seal);
    if (packet_primary.failure != Slus21050FemainPacketPrimaryCandidateFailure::None || !packet_primary.candidate.has_value()) {
        return {Slus21050FemainPublicationCandidateFailure::PacketPrimaryCandidateRejected, {}};
    }
    const auto& snapshot = *seal.snapshot;
    const auto& packet = packet_primary.candidate->packet;
    const std::uint64_t eop_timeline = packet.eop_timeline_event;
    if (eop_timeline == 0U) {
        return {Slus21050FemainPublicationCandidateFailure::PublicationWriteInvalid, {}};
    }

    std::optional<std::array<ScalarWriteRecord, 3>> matched{};
    bool invalid_shape_seen = false;
    bool counter_mismatch_seen = false;
    bool intervening_graphics_seen = false;
    const std::size_t write_count = snapshot.scalar_writes.size();
    for (std::size_t index = 0U; index + 3U <= write_count; ++index) {
        std::array<ScalarWriteRecord, 3> writes{};
        bool shape_matches = true;
        for (std::size_t offset = 0U; offset != 3U; ++offset) {
            writes[offset] = snapshot.scalar_writes[index + offset];
            shape_matches = shape_matches && write_shape_matches(writes[offset], kExpectedWrites[offset]);
        }
        if (!shape_matches) {
            continue;
        }

        const std::uint64_t epoch = writes[0].observation_epoch;
        const std::uint64_t first_timeline = writes[0].timeline_event;
        bool ordered = epoch != 0U && epoch == packet.observation_epoch &&
                       writes[1].observation_epoch == epoch && writes[2].observation_epoch == epoch &&
                       eop_timeline < first_timeline && first_timeline != std::numeric_limits<std::uint64_t>::max() &&
                       writes[1].timeline_event == first_timeline + 1U &&
                       writes[1].timeline_event != std::numeric_limits<std::uint64_t>::max() &&
                       writes[2].timeline_event == writes[1].timeline_event + 1U;
        if (!ordered) {
            invalid_shape_seen = true;
            continue;
        }

        bool counters_match = true;
        for (const ScalarWriteRecord& write : writes) {
            counters_match = counters_match && write.raw_events_observed == packet.raw_last_event &&
                             write.eop_events_observed == packet.eop_event &&
                             write.primary_events_observed == packet.primary_last_ordinal;
        }
        if (!counters_match) {
            counter_mismatch_seen = true;
            continue;
        }

        bool intervening = false;
        for (const auto& pr : snapshot.packets) {
            if ((first_timeline > pr.raw_first_timeline_event && eop_timeline < pr.raw_last_timeline_event) ||
                (eop_timeline < pr.eop_timeline_event && first_timeline > pr.eop_timeline_event)) {
                intervening = true;
            }
        }
        for (const auto& command : snapshot.primary_commands) {
            if (eop_timeline < command.timeline_event && first_timeline > command.timeline_event) {
                intervening = true;
            }
        }
        if (intervening) {
            intervening_graphics_seen = true;
            continue;
        }
        if (matched.has_value()) {
            return {Slus21050FemainPublicationCandidateFailure::PublicationWriteAmbiguous, {}};
        }
        matched = writes;
    }

    if (!matched.has_value()) {
        if (intervening_graphics_seen) {
            return {Slus21050FemainPublicationCandidateFailure::InterveningGraphicsEvent, {}};
        }
        if (counter_mismatch_seen) {
            return {Slus21050FemainPublicationCandidateFailure::CounterMismatch, {}};
        }
        if (invalid_shape_seen) {
            return {Slus21050FemainPublicationCandidateFailure::PublicationWriteInvalid, {}};
        }
        return {Slus21050FemainPublicationCandidateFailure::PublicationWriteMissing, {}};
    }

    Slus21050FemainPublicationCandidate candidate{};
    candidate.packet_primary = *packet_primary.candidate;
    candidate.publication_writes = *matched;
    candidate.authentication_tag = authenticate_slus_21050_femain_publication_candidate(candidate);
    return {Slus21050FemainPublicationCandidateFailure::None, candidate};
}

} // namespace takedown::runtime
