#pragma once

#include "takedown/runtime/slus_21050_femain_movie_consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::guest {
class EeMemory;
class R5900State;
} // namespace takedown::guest

namespace takedown::runtime {

enum class Slus21050FemainMovieTransactionStatus : std::uint8_t {
    Ready,
    Committed,
    Retained,
    Empty,
    Occupied,
    NoPreparedFrame,
    ConsumerRejected,
    PlayerOwnershipLost,
    RetainFailed,
    CompletionPending,
    AlreadyCommitted,
    LedgerRejected,
    LedgerSessionMismatch,
    DroppedFrameBatchRejected,
    DroppedFrameBoundaryMismatch,
    Cancelled,
    CancellationRejected,
};

[[nodiscard]] std::string_view to_string(Slus21050FemainMovieTransactionStatus status) noexcept;

// Stable identity of the exact consumer plan which crossed the publication
// seam. Mutation tokens are deliberately excluded because the successful
// guest stores retire them; all persistent frame, layout, payload, buffer, and
// player-lease identities remain represented.
struct Slus21050FemainMoviePublicationPlanIdentity {
    std::uint64_t layout_generation{};
    guest::GuestAddress first_buffer{};
    guest::GuestAddress second_buffer{};
    std::size_t buffer_bytes{};
    std::uint64_t frame_generation{};
    std::uint64_t media_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    std::optional<Slus21050MovieFrameLease> player_lease{};
    std::optional<std::uint64_t> timeline_ordinal{};

    bool operator==(const Slus21050FemainMoviePublicationPlanIdentity&) const = default;
};

// Value-only publication evidence. It owns no participant pointer or guest
// span and cannot mutate or retire anything. publication_serial distinguishes
// successive frames published by the same long-lived owner instance.
struct Slus21050FemainMoviePublicationPacket {
    std::uint64_t owner_identity{};
    std::uint64_t publication_serial{};
    std::uint64_t player_identity{};
    std::uint64_t player_session_generation{};
    std::uint64_t ledger_identity{};
    std::uint64_t ledger_session_generation{};
    Slus21050FemainFrameReservation published_reservation{};
    Slus21050FemainMoviePublicationPlanIdentity plan{};
    Slus21050FemainMoviePublicationQueueSnapshot queue{};

    bool operator==(const Slus21050FemainMoviePublicationPacket&) const = default;
};

// Shared value-only shape validation for a packet which has already crossed
// the publication seam. Live authorities and diagnostic correlation joins use
// the same checks so neither can accidentally accept a weaker identity.
[[nodiscard]] bool validate_slus_21050_femain_movie_publication_packet(
    const Slus21050FemainMoviePublicationPacket& packet) noexcept;

struct Slus21050FemainMovieTransactionResult {
    Slus21050FemainMovieTransactionStatus status{Slus21050FemainMovieTransactionStatus::Empty};
    Slus21050FemainMovieConsumerStatus consumer_status{Slus21050FemainMovieConsumerStatus::Ready};
    std::optional<Slus21050FemainFrameLedgerStatus> ledger_status{};
    std::optional<Slus21050MovieDroppedFrameBatchStatus> dropped_frame_batch_status{};
    std::optional<Slus21050FemainMoviePublicationPacket> publication_packet{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainMovieTransactionStatus::Ready;
    }
    [[nodiscard]] bool committed() const noexcept {
        return status == Slus21050FemainMovieTransactionStatus::Committed;
    }
};

struct Slus21050FemainMovieTransactionBoundaries {
    Slus21050FemainFrameGuestBoundary produced_at{};
    Slus21050FemainFrameGuestBoundary published_at{};
    Slus21050FemainFrameGuestBoundary graphics_completed_at{};
    Slus21050FemainFrameGuestBoundary consumed_at{};
    bool vif1_before_gif{};

    bool operator==(const Slus21050FemainMovieTransactionBoundaries&) const = default;
};

// Boundaries for the first half of a split owner transaction. Publication
// intentionally carries no graphics or consumption evidence: the owner keeps
// the player receipt and exact ledger reservation until the renderer reports
// the later retirement boundary.
struct Slus21050FemainMoviePublicationBoundaries {
    Slus21050FemainFrameGuestBoundary produced_at{};
    Slus21050FemainFrameGuestBoundary published_at{};

    bool operator==(const Slus21050FemainMoviePublicationBoundaries&) const = default;
};

// Boundaries for the second half of a split owner transaction. Graphics
// completion and consumption are admitted and committed as one ledger
// operation so neither acknowledgement can be replayed independently.
struct Slus21050FemainMovieRetirementBoundaries {
    Slus21050FemainFrameGuestBoundary graphics_completed_at{};
    Slus21050FemainFrameGuestBoundary consumed_at{};
    bool vif1_before_gif{};

    bool operator==(const Slus21050FemainMovieRetirementBoundaries&) const = default;
};

// Passive capacity-one owner for one movie player. The owner must be destroyed
// before its bound player and the exact ledger object used by a split
// publication must outlive the owner through explicit retirement. It checks out
// exactly one immutable prepared frame, retains it on every pre-publication
// failure, and completes the player receipt only after the CPU-aware guest
// transaction succeeds. No runtime registers this class yet; a live caller
// must invoke commit/publication/retirement while guest execution is
// exclusively stopped on the owning thread.
class Slus21050FemainMovieTransactionOwner {
  public:
    explicit Slus21050FemainMovieTransactionOwner(Slus21050MoviePlayer& player) noexcept;
    ~Slus21050FemainMovieTransactionOwner();
    Slus21050FemainMovieTransactionOwner(const Slus21050FemainMovieTransactionOwner&) = delete;
    Slus21050FemainMovieTransactionOwner&
    operator=(const Slus21050FemainMovieTransactionOwner&) = delete;
    Slus21050FemainMovieTransactionOwner(Slus21050FemainMovieTransactionOwner&&) = delete;
    Slus21050FemainMovieTransactionOwner&
    operator=(Slus21050FemainMovieTransactionOwner&&) = delete;

    [[nodiscard]] Slus21050FemainMovieTransactionResult
    admit(const guest::EeMemory& memory) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    admit_with_dropped_frame_batch(const guest::EeMemory& memory,
                                   const Slus21050MovieDroppedFrameBatch& dropped_frames) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult commit(guest::EeMemory& memory,
                                                               guest::R5900State& cpu) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    commit_with_ledger(guest::EeMemory& memory, guest::R5900State& cpu,
                       Slus21050FemainFrameTransactionLedger& ledger,
                       const Slus21050FemainMovieTransactionBoundaries& boundaries) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult commit_drop_batch_with_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMovieTransactionBoundaries& current_frame_boundaries) noexcept;

    // Split publication for an ordinary prepared frame. The typed ledger
    // publication ticket is committed at the consumer's final pre-store seam;
    // the player receipt remains outstanding until retire_with_ledger().
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    publish_with_ledger(guest::EeMemory& memory, guest::R5900State& cpu,
                        Slus21050FemainFrameTransactionLedger& ledger,
                        const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept;

    // Split publication for an exact authorized player drop batch followed by
    // its current frame. The batch remains live in the player until retirement
    // succeeds, so no drop is acknowledged during publication.
    [[nodiscard]] Slus21050FemainMovieTransactionResult publish_drop_batch_with_ledger(
        guest::EeMemory& memory, guest::R5900State& cpu,
        Slus21050FemainFrameTransactionLedger& ledger,
        const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept;

    // Retires the exact reservation produced by one of the split publication
    // calls. The ledger and player checks are read-only and happen before
    // retirement preflight/commit. A receipt failure leaves the owner in the
    // receipt-only GuestCommitted phase, where retries do not touch the ledger
    // or guest memory again.
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    retire_with_ledger(Slus21050FemainFrameTransactionLedger& ledger,
                       const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept;

    // Abandons a GuestPublished reservation after the bound player has already
    // advanced its session through close/reopen. The exact ledger reservation
    // is cancelled and the proof session is drained; graphics and consume are
    // never fabricated. Calling this while the player session still owns the
    // checked-out receipt is rejected because this owner/ledger pair has no
    // safe way to discard that receipt.
    [[nodiscard]] Slus21050FemainMovieTransactionResult cancel_published_with_ledger(
        Slus21050FemainFrameTransactionLedger& ledger,
        Slus21050FemainFrameLedgerCancelReason reason) noexcept;

    [[nodiscard]] Slus21050FemainMovieTransactionResult retain() noexcept;

    [[nodiscard]] bool occupied() const noexcept;
    [[nodiscard]] bool guest_commit_complete() const noexcept;
    [[nodiscard]] std::optional<Slus21050MovieFrameLease> lease() const noexcept;
    // Read-only diagnostic views for a future live admission seam. These
    // copies expose the exact checked-out identity without exposing mutable
    // owner state or enabling a guest-memory commit.
    [[nodiscard]] std::uint64_t identity() const noexcept { return owner_identity_; }
    [[nodiscard]] std::uint64_t player_identity() const noexcept;
    [[nodiscard]] bool player_identity_matches() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> player_session_generation() const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainMovieConsumerPlan> consumer_plan() const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainHandoffToken> handoff_token() const noexcept;
    // True only while guest publication succeeded and graphics/consumption
    // retirement is still outstanding. GuestCommitted is receipt-only.
    [[nodiscard]] bool publication_pending() const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainFrameReservation>
    published_reservation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> published_ledger_identity() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> published_ledger_session_generation() const noexcept;
    [[nodiscard]] bool
    published_ledger_matches(const Slus21050FemainFrameTransactionLedger& ledger) const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainMoviePublicationPacket>
    publication_packet() const noexcept;
    [[nodiscard]] bool publication_packet_matches(
        const Slus21050FemainMoviePublicationPacket& packet) const noexcept;

  private:
    enum class State : std::uint8_t { Empty, Prepared, GuestPublished, GuestCommitted };

    void clear() noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    admit_impl(const guest::EeMemory& memory,
               const Slus21050MovieDroppedFrameBatch* dropped_frames) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    retain_after_rejection(Slus21050FemainMovieConsumerStatus status) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    retain_after_ledger_rejection(Slus21050FemainFrameLedgerStatus status) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    retain_after_batch_rejection(Slus21050MovieDroppedFrameBatchStatus status) noexcept;
    [[nodiscard]] bool
    ledger_session_matches(const Slus21050FemainFrameTransactionLedger& ledger) const noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    publish_impl(guest::EeMemory& memory, guest::R5900State& cpu,
                 Slus21050FemainFrameTransactionLedger& ledger,
                 const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    publish_drop_batch_impl(guest::EeMemory& memory, guest::R5900State& cpu,
                            Slus21050FemainFrameTransactionLedger& ledger,
                            const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult
    retire_impl(Slus21050FemainFrameTransactionLedger& ledger,
                const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult publication_pending_result() const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainMoviePublicationPacket>
    make_publication_packet_candidate(
        const Slus21050FemainFrameReservation& reservation,
        const Slus21050FemainFrameLedgerSnapshot& ledger_snapshot) const noexcept;
    void commit_publication_packet(
        const Slus21050FemainMoviePublicationPacket& packet) noexcept;
    void clear_published_ledger_binding() noexcept;
    [[nodiscard]] Slus21050FemainMovieTransactionResult complete_receipt() noexcept;

    Slus21050MoviePlayer& player_;
    const std::uint64_t owner_identity_{};
    const std::uint64_t bound_player_identity_{};
    Slus21050FemainMovieDoubleBufferConsumer consumer_{};
    State state_{State::Empty};
    std::optional<std::uint64_t> player_session_{};
    std::optional<Slus21050MoviePreparedFrame> frame_{};
    std::optional<Slus21050FemainMovieConsumerPlan> plan_{};
    std::optional<Slus21050FemainHandoffToken> handoff_{};
    std::optional<Slus21050MovieDroppedFrameBatch> dropped_frames_{};
    std::optional<Slus21050FemainFrameLedgerStatus> committed_ledger_status_{};
    std::optional<Slus21050FemainFrameReservation> published_reservation_{};
    const Slus21050FemainFrameTransactionLedger* published_ledger_{};
    std::optional<std::uint64_t> published_ledger_identity_{};
    std::optional<std::uint64_t> published_ledger_session_{};
    std::optional<Slus21050FemainMoviePublicationPacket> publication_packet_{};
    std::uint64_t next_publication_serial_{1U};
};

} // namespace takedown::runtime
