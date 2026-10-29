#pragma once

#include "takedown/runtime/slus_21050_femain_live_terminal_coordinator.hpp"
#include "takedown/runtime/slus_21050_femain_publication_candidate.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// Serialized-caller-only consistency classifier for one authenticated FEMain
// publication packet. The coordinator owns the complete callback batch; this
// object consumes its closed value view together with the already-derived
// immutable publication/queue/provenance candidate. The candidate proves that
// the exact source packet was followed by the reviewed retail publication
// stores. Those stores are not proof that the packet is the last packet of a
// frame, so this class deliberately emits no renderer or terminal witness.
class Slus21050FemainLivePublicationBatchClassifier final {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Empty,
        Classified,
        Failed,
    };

    enum class Failure : std::uint8_t {
        None,
        Disabled,
        Replay,
        SourceBoundaryInvalid,
        ExecutionIdentityInvalid,
        ExecutionIdentityMismatch,
        FrameIdentityInvalid,
        FrameIdentityMismatch,
        PublicationCandidateAuthenticationMismatch,
        PublicationCandidatePrimaryAuthenticationMismatch,
        PublicationCandidateSourceMismatch,
        PublicationEvidenceInvalid,
        CompleteBatchUnavailable,
        BatchPacketMismatch,
        BatchRawCountMismatch,
        BatchCountMismatch,
        BatchDigestMismatch,
        Capacity,
    };

    // This is a consistency result, not a terminal-frame claim. The primary
    // ordinal is retained only in the diagnostic's own immutable domain and
    // must never be passed as a renderer witness range.
    struct Classification {
        Slus21050FemainSerializedSourceBoundary source{};
        Slus21050FemainPublicationCandidate publication{};
        std::size_t raw_quadword_count{};
        std::size_t primary_command_count{};
        std::uint64_t diagnostic_primary_first_ordinal{};
        std::uint64_t diagnostic_primary_stream_digest{};

        bool operator==(const Classification&) const = default;
    };

    explicit Slus21050FemainLivePublicationBatchClassifier(bool enabled = false) noexcept;
    ~Slus21050FemainLivePublicationBatchClassifier() = default;

    Slus21050FemainLivePublicationBatchClassifier(
        const Slus21050FemainLivePublicationBatchClassifier&) = delete;
    Slus21050FemainLivePublicationBatchClassifier&
    operator=(const Slus21050FemainLivePublicationBatchClassifier&) = delete;
    Slus21050FemainLivePublicationBatchClassifier(Slus21050FemainLivePublicationBatchClassifier&&) =
        delete;
    Slus21050FemainLivePublicationBatchClassifier&
    operator=(Slus21050FemainLivePublicationBatchClassifier&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] std::optional<Slus21050FemainSerializedTokenFailure>
    source_boundary_failure() const noexcept {
        return source_boundary_failure_;
    }
    [[nodiscard]] const std::optional<Classification>& classification() const noexcept {
        return classification_;
    }

    // Classify one exact source-to-publication batch. The source boundary and
    // execution/frame identities must agree; the publication candidate must
    // retain valid queue-end -> queue-read -> buffer-index provenance; and the
    // batch must be the coordinator-owned, closed packet whose first raw word,
    // raw count, command count, and canonical stream digest all match that
    // candidate. This method has no participant side effects and establishes
    // no terminal, renderer, or consume authority.
    [[nodiscard]] bool
    classify(const Slus21050FemainSerializedSourceBoundary& source,
             const Slus21050FemainSerializedExecutionIdentity& execution, std::uint64_t frame_id,
             const Slus21050FemainPublicationCandidate& publication,
             const Slus21050FemainLiveTerminalCoordinator::CompleteBatch& batch) noexcept;

  private:
    [[nodiscard]] bool fail(Failure failure) noexcept;
    [[nodiscard]] bool
    valid_publication_candidate(const Slus21050FemainSerializedSourceBoundary& source,
                                const Slus21050FemainPublicationCandidate& publication) noexcept;
    [[nodiscard]] bool
    valid_packet_provenance(const Slus21050FemainSerializedSourceBoundary& source,
                            const Slus21050FemainPublicationCandidate& publication) const noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    std::optional<Slus21050FemainSerializedTokenFailure> source_boundary_failure_{};
    std::optional<Classification> classification_{};
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainLivePublicationBatchClassifier::Phase phase) noexcept;
[[nodiscard]] std::string_view
to_string(Slus21050FemainLivePublicationBatchClassifier::Failure failure) noexcept;

} // namespace takedown::runtime
