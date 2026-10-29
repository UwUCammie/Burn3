#pragma once

#include "takedown/runtime/slus_21050_femain_live_terminal_coordinator.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_invocation_barrier.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// Serialized, value-only consistency result joining one resolver-backed PATH2
// packet to the exact published queue later consumed by the reviewed FEMain
// drain. This does not establish that the packet is the last packet of a movie
// frame: there is no shared terminal event, renderer completion, retirement,
// scanout, or presentation evidence here. The join has no participant pointer
// and cannot call terminal, renderer, owner, ledger, or guest APIs. Its output
// remains candidate input for a later authenticated frame-terminal producer.
class Slus21050FemainPacketDrainJoin final {
  public:
    enum class Phase : std::uint8_t { Disabled, Empty, Joined, Failed };
    enum class Failure : std::uint8_t {
        None,
        Disabled,
        Replay,
        SourceBoundaryInvalid,
        ResolverProofMissing,
        ResolverProofInvalid,
        PublicationInvalid,
        SourcePublicationMismatch,
        BatchUnavailable,
        BatchPacketMismatch,
        BatchRawCountMismatch,
        BatchRawDigestMismatch,
        ConsumeWitnessInvalid,
        ConsumeBindingMismatch,
        ConsumeQueueMismatch,
        ConsumeSequenceMismatch,
        ConsumeTerminalMismatch,
        ConsumeReturnMismatch,
    };

    struct Evidence {
        Slus21050FemainSerializedSourceBoundary source{};
        Slus21050FemainMoviePublicationPacket publication{};
        std::size_t raw_quadword_count{};
        std::uint64_t raw_stream_digest{};
        Slus21050FemainRetailConsumeEvidence consume{};
        Slus21050FemainRetailConsumeObservationIdentity consume_identity{};
        std::uint64_t authentication_tag{};

        bool operator==(const Evidence&) const = default;
    };

    explicit Slus21050FemainPacketDrainJoin(bool enabled = false) noexcept;

    Slus21050FemainPacketDrainJoin(const Slus21050FemainPacketDrainJoin&) = delete;
    Slus21050FemainPacketDrainJoin& operator=(const Slus21050FemainPacketDrainJoin&) = delete;
    Slus21050FemainPacketDrainJoin(Slus21050FemainPacketDrainJoin&&) = delete;
    Slus21050FemainPacketDrainJoin& operator=(Slus21050FemainPacketDrainJoin&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] const std::optional<Evidence>& evidence() const noexcept { return evidence_; }

    // Transfers the genuine retail completion capability retained by a
    // successful join exactly once. Empty and failed joins, and a second take,
    // return no capability. The value-only evidence remains available after
    // the move; taking this witness does not revalidate or authorize it.
    [[nodiscard]] std::optional<Slus21050FemainRetailConsumeCompletionWitness>
    take_consume_witness() noexcept;

    // The live coordinator is queried at the serialized join point so a reset
    // or late callback after an earlier CompleteBatch snapshot fails closed.
    // Passing the completion witness by value forces the caller to move it;
    // the witness move operation invalidates the source object, preventing a
    // second join from reusing the same retail completion capability. The
    // join retains that moved capability only after every validation succeeds;
    // failed joins retain no capability.
    [[nodiscard]] bool join(
        const Slus21050FemainSerializedSourceBoundary& source,
        const Slus21050FemainLiveTerminalCoordinator& coordinator,
        std::uint64_t diagnostic_primary_first_ordinal,
        Slus21050FemainRetailConsumeCompletionWitness consume_witness) noexcept;

  private:
    [[nodiscard]] bool fail(Failure failure) noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    std::optional<Evidence> evidence_{};
    std::optional<Slus21050FemainRetailConsumeCompletionWitness> consume_witness_{};
};

[[nodiscard]] std::string_view
to_string(Slus21050FemainPacketDrainJoin::Phase phase) noexcept;
[[nodiscard]] std::string_view
to_string(Slus21050FemainPacketDrainJoin::Failure failure) noexcept;
[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_packet_drain_evidence(
    const Slus21050FemainPacketDrainJoin::Evidence& evidence) noexcept;

} // namespace takedown::runtime
