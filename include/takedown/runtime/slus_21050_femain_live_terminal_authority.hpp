#pragma once

#include "takedown/runtime/slus_21050_femain_live_transaction.hpp"
#include "takedown/runtime/slus_21050_femain_serialized_token.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// This is the lifecycle owner for a future real FEMain producer -> terminal
// delivery hook. It is deliberately disabled by default and has no observer
// registration or renderer/ledger pointer. The producer must forward the
// already-authenticated source evidence and terminal witness from the owning
// emulation thread; this class only binds those values to one exact published
// packet and owns the shared serialized event sequence. One authority instance
// belongs to one live transaction owner for its lifetime; create a fresh
// authority with a fresh transaction rather than moving it between owners.
class Slus21050FemainLiveTerminalAuthority final
    : public Slus21050FemainLiveTransactionAuthority {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Idle,
        PublicationBound,
        SourceBound,
        PrimaryOpen,
        Complete,
        Consumed,
        Cancelled,
        Failed,
    };

    enum class Failure : std::uint8_t {
        None,
        Disabled,
        EventDomainUnavailable,
        PublicationPacketInvalid,
        PublicationAlreadyBound,
        ExecutionIdentityInvalid,
        FrameIdentityInvalid,
        SourceNotExpected,
        SourceEvidenceInvalid,
        SourcePacketMismatch,
        SourceAlreadyBound,
        PrimaryNotReady,
        PrimaryAlreadySealed,
        TerminalNotReady,
        TerminalWitnessInvalid,
        SerializedTokenRejected,
        PacketMismatch,
        CancellationRejected,
        ResetRejected,
        Replay,
    };

    explicit Slus21050FemainLiveTerminalAuthority(bool enabled = false) noexcept;
    ~Slus21050FemainLiveTerminalAuthority() override = default;

    Slus21050FemainLiveTerminalAuthority(
        const Slus21050FemainLiveTerminalAuthority&) = delete;
    Slus21050FemainLiveTerminalAuthority& operator=(
        const Slus21050FemainLiveTerminalAuthority&) = delete;

    [[nodiscard]] bool ready_for_publication() const noexcept override;
    [[nodiscard]] bool authorize_publication(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const guest::EeMemory& memory) const noexcept override;

    // Called immediately after LiveTransaction::step() has returned Published.
    // The owner and ledger are revalidated against the complete packet before
    // it is copied; no value-only packet can bind an unrelated live frame.
    // The execution identity and renderer frame ID belong to the serialized
    // producer and must be nonzero for the lifecycle to proceed.
    [[nodiscard]] bool bind_publication(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMoviePublicationPacket& packet,
        const Slus21050FemainSerializedExecutionIdentity& execution,
        std::uint64_t frame_id) noexcept;

    // The real producer calls this once when the exact post-publication PATH2
    // source packet completes. It issues the shared source event itself; the
    // caller never supplies an ordinal or a value-only terminal token.
    [[nodiscard]] bool on_source_packet_completed(
        const Slus21050FemainFrameGraphicsSourceEvidence& evidence) noexcept;

    // The real serialized primary producer calls this once per frame-specific
    // primary delivery. The first call opens the primary range and each later
    // call extends it by one internally issued event.
    [[nodiscard]] bool on_primary_command_delivery() noexcept;

    // The real terminal producer supplies an authenticated witness for the
    // complete frame-specific primary stream. This method issues the terminal
    // event, validates the contiguous shared range, and retains a one-shot
    // token for the renderer lifecycle. It never manufactures a witness.
    [[nodiscard]] bool on_terminal_primary_delivery(
        const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept;

    // The token can be consumed exactly once. A missing or repeated token is
    // a replay failure and leaves the authority fenced.
    [[nodiscard]] std::optional<Slus21050FemainSerializedSourceTerminalToken>
    take_serialized_token() noexcept;

    // Cancellation is valid only for the opaque witness returned by
    // LiveTransaction after exact owner/ledger cancellation. This method only
    // invalidates the serialized candidate; it cannot cancel live state.
    [[nodiscard]] bool cancel(
        const Slus21050FemainLiveCancellationWitness& witness) noexcept;

    // A cancelled lifecycle can be reused only after the cancellation has
    // succeeded. Event ordinals are never reset or reused.
    [[nodiscard]] bool reset_after_cancel() noexcept;

    // A consumed token can be reused only after the opaque witness returned by
    // LiveTransaction proves exact renderer/ledger retirement for its packet.
    [[nodiscard]] bool reset_after_retirement(
        const Slus21050FemainLiveRetirementWitness& witness) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] std::uint64_t event_domain_identity() const noexcept {
        return event_domain_.domain_identity();
    }
    [[nodiscard]] std::uint64_t last_event_ordinal() const noexcept {
        return event_domain_.last_ordinal();
    }
    [[nodiscard]] std::uint64_t primary_event_count() const noexcept {
        return primary_event_count_;
    }
    [[nodiscard]] std::optional<Slus21050FemainMoviePublicationPacket>
    publication_packet() const noexcept {
        return publication_packet_;
    }
    [[nodiscard]] std::optional<Slus21050FemainSerializedSourceBoundary>
    source_boundary() const noexcept {
        return source_boundary_;
    }
    [[nodiscard]] std::optional<Slus21050FemainSerializedTerminalBoundary>
    terminal_boundary() const noexcept {
        return terminal_boundary_;
    }

  private:
    [[nodiscard]] bool fail(Failure failure) noexcept;
    [[nodiscard]] bool reject_publication(Failure failure) noexcept;
    void clear_for_next_publication() noexcept;
    [[nodiscard]] bool valid_publication_packet(
        const Slus21050FemainMoviePublicationPacket& packet) const noexcept;
    [[nodiscard]] bool live_publication_matches(
        const Slus21050FemainMovieTransactionOwner& owner,
        const Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMoviePublicationPacket& packet) const noexcept;
    [[nodiscard]] bool source_matches_publication(
        const Slus21050FemainFrameGraphicsSourceEvidence& evidence) const noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    Slus21050FemainSerializedEventDomain event_domain_;
    std::optional<Slus21050FemainMoviePublicationPacket> publication_packet_{};
    Slus21050FemainSerializedExecutionIdentity execution_{};
    std::uint64_t frame_id_{};
    std::optional<Slus21050FemainSerializedTokenBuilder> builder_{};
    std::optional<Slus21050FemainSerializedSourceBoundary> source_boundary_{};
    std::optional<Slus21050FemainSerializedTerminalBoundary> terminal_boundary_{};
    std::uint64_t primary_event_count_{};
    // One authority belongs to one live transaction owner. Publication serials
    // are monotonic for that owner's lifetime, and the high-water mark survives
    // cancel/retire resets so an old packet cannot bind a later lifecycle.
    std::uint64_t last_owner_identity_{};
    std::uint64_t last_publication_serial_{};
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainLiveTerminalAuthority::Phase phase) noexcept;
[[nodiscard]] std::string_view to_string(
    Slus21050FemainLiveTerminalAuthority::Failure failure) noexcept;

} // namespace takedown::runtime
