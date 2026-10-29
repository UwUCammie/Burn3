#pragma once

#include "takedown/runtime/guest_work_measurement_window.hpp"
#include "takedown/runtime/slus_21050_femain_publication_candidate.hpp"
#include "takedown/runtime/slus_21050_femain_serialized_token.hpp"
#include "takedown/runtime/slus_21050_femain_terminal_producer_diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace takedown::runtime {

// Exact serialized caller labels at one existing guest-work instruction
// boundary. These are correlation values only; they do not claim a live
// FEMain player, owner, ledger, source packet, or terminal producer.
struct Slus21050FemainProvenanceBoundary {
    GuestWorkMeasurementSnapshot measurement{};
    std::uint32_t guest_pc{};
    std::int32_t guest_thread_id{};
    bool guest_callback_active{};
    std::uint64_t guest_callback_identity{};

    bool operator==(const Slus21050FemainProvenanceBoundary&) const = default;
};

enum class Slus21050FemainProvenanceWindowFailure : std::uint8_t {
    None,
    Disabled,
    AlreadyStarted,
    NotStarted,
    AlreadyCompleted,
    EmulationThreadChanged,
    MeasurementCounterReordered,
    SnapshotLifecycleMismatch,
    SourceAlreadyBound,
    SourceEvidenceInvalid,
    SourceReservationMissing,
    SourceEventUnavailable,
    SourceBuilderRejected,
    SourcePacketCorrelationRejected,
    ReportUnavailable,
};

[[nodiscard]] std::string_view to_string(Slus21050FemainProvenanceWindowFailure failure) noexcept;

enum class Slus21050FemainSourcePacketCorrelationFailure : std::uint8_t {
    None,
    SourceEvidenceInvalid,
    SnapshotUnavailable,
    ObservationEpochMismatch,
    PacketNotFound,
    PacketAmbiguous,
};

struct Slus21050FemainSourcePacketCorrelationResult {
    Slus21050FemainSourcePacketCorrelationFailure failure{
        Slus21050FemainSourcePacketCorrelationFailure::SnapshotUnavailable};
    std::optional<Slus21050FemainTerminalProducerDiagnostic::PacketRecord> packet{};

    [[nodiscard]] bool matched() const noexcept {
        return failure == Slus21050FemainSourcePacketCorrelationFailure::None &&
               packet.has_value();
    }
};

// Match one validated published source boundary to exactly one packet in a
// sealed terminal-diagnostic snapshot. The join requires the marker, raw word,
// and EOP identities issued by the live diagnostic tee to match exactly in one
// domain and observation epoch, in addition to marker metadata and packet
// structure. This is value-only candidate evidence: it neither issues an event
// nor creates authority.
[[nodiscard]] Slus21050FemainSourcePacketCorrelationResult
correlate_slus_21050_femain_source_packet(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult& seal) noexcept;

// Passive, one-shot A-to-C provenance coordinator. It owns the event domain
// and token builder required by a live source/terminal transaction. Its only
// source candidate method consumes already-completed live probe evidence
// carrying an exact Published ledger reservation; it cannot observe callbacks
// or mint source, terminal, renderer, or consume authority by itself.
class Slus21050FemainProvenanceWindow final {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Ready,
        Started,
        Completed,
        Failed,
    };

    explicit Slus21050FemainProvenanceWindow(bool enabled = false);
    ~Slus21050FemainProvenanceWindow();

    Slus21050FemainProvenanceWindow(const Slus21050FemainProvenanceWindow&) = delete;
    Slus21050FemainProvenanceWindow& operator=(const Slus21050FemainProvenanceWindow&) = delete;
    Slus21050FemainProvenanceWindow(Slus21050FemainProvenanceWindow&&) = delete;
    Slus21050FemainProvenanceWindow& operator=(Slus21050FemainProvenanceWindow&&) = delete;

    [[nodiscard]] Slus21050FemainProvenanceWindowFailure
    begin(const Slus21050FemainProvenanceBoundary& boundary,
          std::uint64_t diagnostic_observation_epoch) noexcept;
    [[nodiscard]] Slus21050FemainProvenanceWindowFailure
    complete(const Slus21050FemainProvenanceBoundary& boundary,
             Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult seal_result) noexcept;

    // Bind one already-completed live PATH2 probe as a non-authoritative
    // candidate in this window's shared event domain. The probe revalidates
    // its live participants during the one-shot handoff, and its evidence must
    // carry an exact published owner reservation. complete() later requires
    // the sealed terminal snapshot to contain the exact shared-identity packet
    // and retains that packet before the window can complete.
    [[nodiscard]] Slus21050FemainProvenanceWindowFailure bind_published_source_candidate(
        Slus21050FemainFrameGraphicsSourceProbe& probe,
        const Slus21050FemainSerializedExecutionIdentity& execution,
        std::uint64_t frame_id,
        std::uint64_t diagnostic_observation_epoch) noexcept;

    // Append the immutable coordinator result to the existing terminal
    // diagnostic artifact after that diagnostic has written its own report.
    [[nodiscard]] bool append_report(const std::filesystem::path& path);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Slus21050FemainProvenanceWindowFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] std::uint64_t event_domain_identity() const noexcept;
    [[nodiscard]] std::uint64_t event_domain_last_ordinal() const noexcept;
    [[nodiscard]] bool terminal_authority_available() const noexcept { return false; }
    [[nodiscard]] bool source_authority_available() const noexcept { return false; }
    [[nodiscard]] bool source_candidate_available() const noexcept {
        return failure_ == Slus21050FemainProvenanceWindowFailure::None &&
               (phase_ == Phase::Started || phase_ == Phase::Completed) &&
               source_boundary_.has_value();
    }
    [[nodiscard]] bool source_packet_correlated() const noexcept {
        return failure_ == Slus21050FemainProvenanceWindowFailure::None &&
               phase_ == Phase::Completed && source_packet_.has_value();
    }
    [[nodiscard]] bool token_emitted() const noexcept { return false; }
    [[nodiscard]] const std::optional<Slus21050FemainProvenanceBoundary>& start() const noexcept {
        return start_;
    }
    [[nodiscard]] const std::optional<Slus21050FemainProvenanceBoundary>& end() const noexcept {
        return end_;
    }
    [[nodiscard]] const Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult*
    seal_result() const noexcept;
    [[nodiscard]] const Slus21050FemainSerializedSourceBoundary*
    source_boundary() const noexcept {
        return source_candidate_available() ? &*source_boundary_ : nullptr;
    }
    [[nodiscard]] const Slus21050FemainTerminalProducerDiagnostic::PacketRecord*
    source_packet() const noexcept {
        return source_packet_correlated() ? &*source_packet_ : nullptr;
    }
    [[nodiscard]] Slus21050FemainPublicationCandidateFailure
    publication_candidate_failure() const noexcept {
        return publication_candidate_failure_;
    }
    [[nodiscard]] const Slus21050FemainPublicationCandidate*
    publication_candidate() const noexcept {
        return phase_ == Phase::Completed && publication_candidate_.has_value()
                   ? &*publication_candidate_
                   : nullptr;
    }

  private:
    [[nodiscard]] Slus21050FemainProvenanceWindowFailure
    fail(Slus21050FemainProvenanceWindowFailure failure) noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Slus21050FemainProvenanceWindowFailure failure_{Slus21050FemainProvenanceWindowFailure::None};
    std::unique_ptr<Slus21050FemainSerializedEventDomain> event_domain_{};
    Slus21050FemainSerializedTokenBuilder token_builder_{};
    std::thread::id emulation_thread_{};
    std::uint64_t diagnostic_observation_epoch_{};
    std::optional<Slus21050FemainProvenanceBoundary> start_{};
    std::optional<Slus21050FemainProvenanceBoundary> end_{};
    std::optional<Slus21050FemainSerializedSourceBoundary> source_boundary_{};
    std::optional<Slus21050FemainTerminalProducerDiagnostic::PacketRecord> source_packet_{};
    Slus21050FemainPublicationCandidateFailure publication_candidate_failure_{
        Slus21050FemainPublicationCandidateFailure::SourceCandidateUnavailable};
    std::optional<Slus21050FemainPublicationCandidate> publication_candidate_{};
    std::uint64_t source_observation_epoch_{};
    std::optional<Slus21050FemainTerminalProducerDiagnosticSnapshotSealResult> seal_result_{};
    bool report_appended_{};
};

} // namespace takedown::runtime
