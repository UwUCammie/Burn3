#pragma once

#include "takedown/runtime/slus_21050_femain_guest_boundary.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace takedown::runtime {

// This ledger is deliberately passive.  It is not registered with the
// runtime and owns no guest memory; a later FEMain host producer may use it as
// a proof sidecar only after its admission has been reviewed.
inline constexpr std::size_t kSlus21050FemainFrameTransactionCapacity = 64U;
inline constexpr std::size_t kSlus21050FemainFrameTransactionSlotCount = 2U;

enum class Slus21050FemainFrameLedgerStatus : std::uint8_t {
    Ok,
    Inactive,
    AlreadyActive,
    Paused,
    Terminal,
    NoFreeSlot,
    CapacityExhausted,
    Overflowed,
    SessionGenerationExhausted,
    FrameGenerationExhausted,
    StaleReservation,
    AcknowledgementTooEarly,
    DuplicateAcknowledgement,
    OutOfOrder,
    InvalidBoundary,
    NoPublishedFrame,
    NotDrained,
    StaleTransaction,
    TransactionGenerationExhausted,
    EpochNotFull,
    EpochGenerationExhausted,
    MediaOrdinalExhausted,
    LogicalEventGenerationExhausted,
    EmptyDropBatch,
    // An externally supplied session generation is older than the ledger's
    // next-generation high-water mark and must never be reused.
    StaleSessionGeneration,
};

[[nodiscard]] std::string_view to_string(Slus21050FemainFrameLedgerStatus status) noexcept;

enum class Slus21050FemainFrameLedgerState : std::uint8_t {
    Inactive,
    Active,
    Paused,
    EosDraining,
    Overflowed,
};

enum class Slus21050FemainFrameLedgerSlotState : std::uint8_t {
    Free,
    Reserved,
    Published,
    GraphicsComplete,
};

enum class Slus21050FemainFrameLedgerCancelReason : std::uint8_t {
    Abort,
    StartSkip,
};

// Media ordinals are the source timeline identity.  Source sequence and
// checksum let a later bridge attach decoder identity without making the
// ledger depend on the movie player's frame type.
struct Slus21050FemainFrameIdentity {
    std::uint64_t media_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    Slus21050FemainFrameGuestBoundary produced_at{};

    bool operator==(const Slus21050FemainFrameIdentity&) const = default;
};

// A reservation is an issuer-bound value token. It contains all identity used
// to locate a record, so changing or crossing a copied token cannot retarget a
// different ledger or slot. The issuer is deliberately excluded from the
// event digest so equivalent event streams remain reproducible.
struct Slus21050FemainFrameReservation {
    std::uint64_t ledger_identity{};
    std::uint64_t session_generation{};
    std::uint64_t frame_generation{};
    std::uint8_t slot{};
    Slus21050FemainFrameIdentity frame{};

    bool operator==(const Slus21050FemainFrameReservation&) const = default;
};

struct Slus21050FemainFrameGraphicsCompletion {
    Slus21050FemainFrameReservation reservation{};
    Slus21050FemainFrameGuestBoundary completed_at{};
    bool vif1_before_gif{false};
};

struct Slus21050FemainFrameConsumption {
    Slus21050FemainFrameReservation reservation{};
    Slus21050FemainFrameGuestBoundary consumed_at{};
};

struct Slus21050FemainFrameLedgerBeginResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::uint64_t session_generation{};

    [[nodiscard]] bool began() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
};

struct Slus21050FemainFrameLedgerReservationResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    Slus21050FemainFrameReservation reservation{};

    [[nodiscard]] bool reserved() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
};

struct Slus21050FemainFrameLedgerSnapshot {
    Slus21050FemainFrameLedgerState state{Slus21050FemainFrameLedgerState::Inactive};
    std::uint64_t session_generation{};
    std::uint64_t next_frame_generation{};
    std::uint64_t next_media_ordinal{};
    std::uint64_t next_publication_ordinal{};
    std::uint64_t next_graphics_ordinal{};
    std::uint64_t next_consumption_ordinal{};
    std::size_t logical_event_count{};
    std::size_t frame_record_count{};
    std::size_t in_flight_count{};
    std::uint64_t published_frames{};
    std::uint64_t graphics_completed_frames{};
    std::uint64_t consumed_frames{};
    std::uint64_t repeated_frames{};
    std::uint64_t dropped_frames{};
    std::uint64_t cancelled_frames{};
    bool overflowed{false};
    bool proof_valid{false};
    std::uint64_t digest{};
    std::array<Slus21050FemainFrameLedgerSlotState, kSlus21050FemainFrameTransactionSlotCount>
        slots{};
    // Epoch metadata is appended to preserve positional aggregate compatibility
    // with the original snapshot layout.
    std::uint64_t total_logical_event_count{};
    std::uint64_t epoch_generation{};
    std::uint64_t epoch_first_media_ordinal{};
    std::uint64_t epoch_start_digest{};
    // The instance identity is deliberately outside the event digest. It
    // binds a read-only proof snapshot to one ledger object so an equivalent
    // stream from another ledger cannot be substituted at admission.
    std::uint64_t ledger_identity{};

    // Public stream equivalence intentionally excludes ledger_identity so
    // deterministic event streams from separate ledgers remain comparable.
    // Live admission binds identity explicitly before using this equivalence.
    bool operator==(const Slus21050FemainFrameLedgerSnapshot& other) const noexcept {
        return state == other.state && session_generation == other.session_generation &&
               next_frame_generation == other.next_frame_generation &&
               next_media_ordinal == other.next_media_ordinal &&
               next_publication_ordinal == other.next_publication_ordinal &&
               next_graphics_ordinal == other.next_graphics_ordinal &&
               next_consumption_ordinal == other.next_consumption_ordinal &&
               logical_event_count == other.logical_event_count &&
               frame_record_count == other.frame_record_count &&
               in_flight_count == other.in_flight_count &&
               published_frames == other.published_frames &&
               graphics_completed_frames == other.graphics_completed_frames &&
               consumed_frames == other.consumed_frames && repeated_frames == other.repeated_frames &&
               dropped_frames == other.dropped_frames && cancelled_frames == other.cancelled_frames &&
               overflowed == other.overflowed && proof_valid == other.proof_valid &&
               digest == other.digest && slots == other.slots &&
               total_logical_event_count == other.total_logical_event_count &&
               epoch_generation == other.epoch_generation &&
               epoch_first_media_ordinal == other.epoch_first_media_ordinal &&
               epoch_start_digest == other.epoch_start_digest;
    }
};

struct Slus21050FemainFrameTransactionEvidence {
    Slus21050FemainFrameIdentity frame{};
    Slus21050FemainFrameGuestBoundary published_at{};
    Slus21050FemainFrameGuestBoundary graphics_completed_at{};
    Slus21050FemainFrameGuestBoundary consumed_at{};
    bool vif1_before_gif{};

    bool operator==(const Slus21050FemainFrameTransactionEvidence&) const = default;
};

// Evidence for the first half of a frame lifecycle. Publication reserves the
// next source frame and publishes it as one atomic ledger mutation. The later
// graphics and consume boundaries are intentionally absent so a caller can
// retain the returned reservation while a renderer finishes its work.
struct Slus21050FemainFramePublicationEvidence {
    Slus21050FemainFrameIdentity frame{};
    Slus21050FemainFrameGuestBoundary published_at{};

    bool operator==(const Slus21050FemainFramePublicationEvidence&) const = default;
};

// Evidence for the second half of a frame lifecycle. The reservation carries
// the ledger, session, ledger frame-generation, slot, and source identity that
// publication issued; both acknowledgements must prove that exact value. A
// future owner adapter may additionally bind this reservation to its separate
// movie-player lease.
struct Slus21050FemainFrameRetirementEvidence {
    Slus21050FemainFrameReservation reservation{};
    Slus21050FemainFrameGuestBoundary graphics_completed_at{};
    Slus21050FemainFrameGuestBoundary consumed_at{};
    bool vif1_before_gif{false};

    bool operator==(const Slus21050FemainFrameRetirementEvidence&) const = default;
};

class Slus21050FemainFrameTransactionLedger;
struct Slus21050FemainFrameTransactionLedgerTestAccess;

// Opaque, instance-bound proof that one complete frame lifecycle was valid
// against an exact ledger snapshot. Copies are allowed for fixed-capacity
// queues, but a successful commit or any other ledger mutation makes every
// copy stale.
class Slus21050FemainFrameTransactionTicket {
  public:
    constexpr Slus21050FemainFrameTransactionTicket() noexcept = default;
    [[nodiscard]] const Slus21050FemainFrameReservation& reservation() const noexcept {
        return reservation_;
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    Slus21050FemainFrameTransactionEvidence evidence_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameTransactionPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameTransactionTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameTransactionCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    Slus21050FemainFrameReservation reservation{};

    [[nodiscard]] bool committed() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
};

// Opaque, instance-bound proof that reserve->publish was valid against one
// exact ledger snapshot. This capability is for an ordinary single-frame
// publication; drop-batch publication has a distinct ticket type below so a
// caller cannot pass the wrong capability to either commit operation.
class Slus21050FemainFramePublicationTicket {
  public:
    constexpr Slus21050FemainFramePublicationTicket() noexcept = default;

    [[nodiscard]] const Slus21050FemainFrameReservation& reservation() const noexcept {
        return reservation_;
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    Slus21050FemainFramePublicationEvidence evidence_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFramePublicationPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFramePublicationTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFramePublicationCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    Slus21050FemainFrameReservation reservation{};

    [[nodiscard]] bool published() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
    [[nodiscard]] bool committed() const noexcept { return published(); }
};

// Opaque, instance-bound proof for a drop prefix followed by reserve->publish.
// Its fixed identity copy is part of the capability and keeps timed catch-up
// from splitting the drop, reservation, and publication across mutations.
class Slus21050FemainFrameDropBatchPublicationTicket {
  public:
    constexpr Slus21050FemainFrameDropBatchPublicationTicket() noexcept = default;

    [[nodiscard]] const Slus21050FemainFrameReservation& reservation() const noexcept {
        return reservation_;
    }
    [[nodiscard]] std::span<const Slus21050FemainFrameIdentity> dropped_frames() const noexcept {
        return {dropped_frames_.data(), dropped_frame_count_};
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    std::array<Slus21050FemainFrameIdentity, kSlus21050FemainFrameTransactionCapacity - 1U>
        dropped_frames_{};
    std::size_t dropped_frame_count_{};
    Slus21050FemainFramePublicationEvidence evidence_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameDropBatchPublicationPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameDropBatchPublicationTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameDropBatchPublicationCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    Slus21050FemainFrameReservation reservation{};

    [[nodiscard]] bool published() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
    [[nodiscard]] bool committed() const noexcept { return published(); }
};

// Opaque, instance-bound proof that graphics completion and consume are valid
// for one already-published reservation. A successful commit advances both
// stages together, so a copy cannot be replayed after either stage or any
// intervening ledger mutation.
class Slus21050FemainFrameRetirementTicket {
  public:
    constexpr Slus21050FemainFrameRetirementTicket() noexcept = default;

    [[nodiscard]] const Slus21050FemainFrameReservation& reservation() const noexcept {
        return reservation_;
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    Slus21050FemainFrameRetirementEvidence evidence_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameRetirementPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameRetirementTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameRetirementCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    Slus21050FemainFrameReservation reservation{};

    [[nodiscard]] bool retired() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
    [[nodiscard]] bool committed() const noexcept { return retired(); }
};

// Opaque proof that an exact, non-empty ordered drop prefix was valid against
// one ledger snapshot. This transaction is used to fill the remainder of a
// proof epoch before the player authorizes the immediately due frame.
class Slus21050FemainFrameDropPrefixTransactionTicket {
  public:
    constexpr Slus21050FemainFrameDropPrefixTransactionTicket() noexcept = default;

    [[nodiscard]] std::span<const Slus21050FemainFrameIdentity> dropped_frames() const noexcept {
        return {dropped_frames_.data(), dropped_frame_count_};
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    std::array<Slus21050FemainFrameIdentity, kSlus21050FemainFrameTransactionCapacity>
        dropped_frames_{};
    std::size_t dropped_frame_count_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameDropPrefixTransactionPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameDropPrefixTransactionTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameDropPrefixTransactionCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};

    [[nodiscard]] bool committed() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok;
    }
};

// Opaque proof that an exact ordered drop prefix and the immediately following
// complete frame lifecycle were valid against one ledger snapshot. The ticket
// owns its bounded drop identities, so caller storage may change after
// preflight without changing the admitted transaction.
class Slus21050FemainFrameDropBatchTransactionTicket {
  public:
    constexpr Slus21050FemainFrameDropBatchTransactionTicket() noexcept = default;

    [[nodiscard]] std::span<const Slus21050FemainFrameIdentity> dropped_frames() const noexcept {
        return {dropped_frames_.data(), dropped_frame_count_};
    }
    [[nodiscard]] const Slus21050FemainFrameReservation& reservation() const noexcept {
        return reservation_;
    }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    std::array<Slus21050FemainFrameIdentity, kSlus21050FemainFrameTransactionCapacity - 1U>
        dropped_frames_{};
    std::size_t dropped_frame_count_{};
    Slus21050FemainFrameTransactionEvidence evidence_{};
    Slus21050FemainFrameReservation reservation_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameDropBatchTransactionPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameDropBatchTransactionTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameLedgerEpochSeal {
    std::uint64_t session_generation{};
    std::uint64_t epoch_generation{};
    std::size_t logical_event_count{};
    std::uint64_t total_logical_event_count{};
    std::uint64_t first_media_ordinal{};
    std::uint64_t next_media_ordinal{};
    std::uint64_t next_publication_ordinal{};
    std::uint64_t next_graphics_ordinal{};
    std::uint64_t next_consumption_ordinal{};
    std::uint64_t next_frame_generation{};
    std::size_t next_slot{};
    std::uint64_t start_digest{};
    std::uint64_t terminal_digest{};

    bool operator==(const Slus21050FemainFrameLedgerEpochSeal&) const = default;
};

class Slus21050FemainFrameLedgerEpochRolloverTicket {
  public:
    constexpr Slus21050FemainFrameLedgerEpochRolloverTicket() noexcept = default;
    [[nodiscard]] const Slus21050FemainFrameLedgerEpochSeal& seal() const noexcept { return seal_; }

  private:
    friend class Slus21050FemainFrameTransactionLedger;
    const Slus21050FemainFrameTransactionLedger* owner_{};
    Slus21050FemainFrameLedgerSnapshot before_{};
    Slus21050FemainFrameLedgerEpochSeal seal_{};
    std::uint64_t ledger_identity_{};
    std::uint64_t revision_{};
};

struct Slus21050FemainFrameLedgerEpochRolloverPreflightResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameLedgerEpochRolloverTicket> ticket{};

    [[nodiscard]] bool admitted() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && ticket.has_value();
    }
};

struct Slus21050FemainFrameLedgerEpochRolloverCommitResult {
    Slus21050FemainFrameLedgerStatus status{Slus21050FemainFrameLedgerStatus::Inactive};
    std::optional<Slus21050FemainFrameLedgerEpochSeal> sealed_epoch{};

    [[nodiscard]] bool committed() const noexcept {
        return status == Slus21050FemainFrameLedgerStatus::Ok && sealed_epoch.has_value();
    }
};

// Fixed-capacity, serialized-thread ledger for one FEMain producer/consumer
// session.  Capacity counts admitted media events (reserve, drop, or repeat),
// not the acknowledgements needed to drain an admitted reservation.  Every
// rejected operation is side-effect free except the explicit capacity overflow
// transition: overflow marks the session unprovable and blocks further
// production until the caller aborts and drains it.
class Slus21050FemainFrameTransactionLedger {
  public:
    using Status = Slus21050FemainFrameLedgerStatus;
    using State = Slus21050FemainFrameLedgerState;
    using SlotState = Slus21050FemainFrameLedgerSlotState;

    Slus21050FemainFrameTransactionLedger() noexcept;

    Slus21050FemainFrameLedgerBeginResult begin_session() noexcept;

    // Starts a session whose generation is owned by an external movie
    // producer. The ordinary overload remains the deterministic standalone
    // sequence used by ledger-only callers; this overload lets a live owner
    // bind the ledger to the exact player session before reserving a frame.
    // External generations must be at or above the next-generation
    // high-water mark. Larger values advance that mark, while lower or
    // previously used values return StaleSessionGeneration without mutation.
    Slus21050FemainFrameLedgerBeginResult begin_session(
        std::uint64_t session_generation) noexcept;

    [[nodiscard]] Status pause() noexcept;
    [[nodiscard]] Status resume() noexcept;
    [[nodiscard]] Status request_eos() noexcept;
    [[nodiscard]] Status finish_eos() noexcept;
    [[nodiscard]] Status cancel_and_drain(Slus21050FemainFrameLedgerCancelReason reason) noexcept;
    // Cancel one exact outstanding reservation and drain the session. The
    // reservation is checked before any mutation so a caller cannot use this
    // lifecycle escape to cancel a different ledger/session. The session's
    // proof is deliberately invalidated and no graphics or consume event is
    // synthesized.
    [[nodiscard]] Status cancel_and_drain(
        const Slus21050FemainFrameReservation& reservation,
        Slus21050FemainFrameLedgerCancelReason reason) noexcept;

    // The ordinal must equal the next source ordinal.  A successful
    // reservation advances the source cursor before any guest write occurs.
    // Only one reservation may be outstanding because the current producer
    // writes one frame to both physical guest buffers as one transaction.
    [[nodiscard]] Slus21050FemainFrameLedgerReservationResult
    reserve(const Slus21050FemainFrameIdentity& frame) noexcept;

    // Drop and repeat are logical media events only.  Neither allocates a
    // slot or publication generation; both are included in the bounded digest.
    [[nodiscard]] Status record_drop(const Slus21050FemainFrameIdentity& frame) noexcept;
    [[nodiscard]] Status record_repeat(const Slus21050FemainFrameGuestBoundary& boundary) noexcept;

    [[nodiscard]] Status publish(const Slus21050FemainFrameReservation& reservation,
                                 const Slus21050FemainFrameGuestBoundary& published_at) noexcept;

    [[nodiscard]] Status
    complete_graphics(const Slus21050FemainFrameGraphicsCompletion& completion) noexcept;

    [[nodiscard]] Status consume(const Slus21050FemainFrameConsumption& consumption) noexcept;

    // Side-effect-free admission and all-or-nothing application of one
    // reserve->publish->graphics-complete->consume lifecycle. The ticket is
    // bound to this ledger and its complete preflight snapshot.
    [[nodiscard]] Slus21050FemainFrameTransactionPreflightResult preflight_frame_transaction(
        const Slus21050FemainFrameTransactionEvidence& evidence) const noexcept;
    [[nodiscard]] Slus21050FemainFrameTransactionCommitResult
    commit_frame_transaction(const Slus21050FemainFrameTransactionTicket& ticket) noexcept;

    // Split lifecycle admission. Publication atomically reserves and publishes
    // one frame, then leaves it Published until a later retirement ticket
    // proves graphics completion and consume. Publication admission reserves
    // mutation-revision headroom for the entire four-stage lifecycle so it can
    // never strand a Published slot at revision exhaustion. Lifecycle controls
    // retain the exact remaining acknowledgement headroom for an outstanding
    // Reserved, Published, or GraphicsComplete record; explicit cancellation
    // remains available when a caller must abandon that record.
    [[nodiscard]] Slus21050FemainFramePublicationPreflightResult
    preflight_frame_publication(
        const Slus21050FemainFramePublicationEvidence& evidence) const noexcept;
    [[nodiscard]] Slus21050FemainFramePublicationCommitResult
    commit_frame_publication(const Slus21050FemainFramePublicationTicket& ticket) noexcept;

    // Split drop-batch publication. The drop prefix, reserve, and publish are
    // one atomic ledger mutation; the returned reservation is retired through
    // the same graphics+consume ticket API. The full drops+four-stage revision
    // budget is checked before admission.
    [[nodiscard]] Slus21050FemainFrameDropBatchPublicationPreflightResult
    preflight_drop_batch_frame_publication(
        std::span<const Slus21050FemainFrameIdentity> dropped_frames,
        const Slus21050FemainFramePublicationEvidence& evidence) const noexcept;
    [[nodiscard]] Slus21050FemainFrameDropBatchPublicationCommitResult
    commit_drop_batch_frame_publication(
        const Slus21050FemainFrameDropBatchPublicationTicket& ticket) noexcept;

    // Split retirement admission. This operation only advances the already
    // published record: complete_graphics and consume are simulated together
    // and committed together, with no opportunity for a partial acknowledgement.
    [[nodiscard]] Slus21050FemainFrameRetirementPreflightResult
    preflight_frame_retirement(
        const Slus21050FemainFrameRetirementEvidence& evidence) const noexcept;
    [[nodiscard]] Slus21050FemainFrameRetirementCommitResult
    commit_frame_retirement(const Slus21050FemainFrameRetirementTicket& ticket) noexcept;

    // Side-effect-free admission and all-or-nothing application of an exact
    // ordered drop prefix followed by one complete current-frame lifecycle.
    // At most 63 drops are accepted so the current frame remains the final
    // event in the fixed 64-event proof window. An oversized span reports
    // CapacityExhausted before ledger lifecycle validation.
    [[nodiscard]] Slus21050FemainFrameDropBatchTransactionPreflightResult
    preflight_drop_batch_frame_transaction(
        std::span<const Slus21050FemainFrameIdentity> dropped_frames,
        const Slus21050FemainFrameTransactionEvidence& evidence) const noexcept;
    [[nodiscard]] Slus21050FemainFrameTransactionCommitResult commit_drop_batch_frame_transaction(
        const Slus21050FemainFrameDropBatchTransactionTicket& ticket) noexcept;

    // Side-effect-free admission and all-or-nothing application of an exact
    // non-empty ordered drop prefix. It may fill all remaining epoch capacity,
    // allowing the caller to acknowledge the same player prefix and roll the
    // now-full drained epoch before authorizing the due frame.
    [[nodiscard]] Slus21050FemainFrameDropPrefixTransactionPreflightResult
    preflight_drop_prefix_transaction(
        std::span<const Slus21050FemainFrameIdentity> dropped_frames) const noexcept;
    [[nodiscard]] Slus21050FemainFrameDropPrefixTransactionCommitResult
    commit_drop_prefix_transaction(
        const Slus21050FemainFrameDropPrefixTransactionTicket& ticket) noexcept;

    // Seals a completely full and drained 64-event proof epoch, then clears
    // only its bounded records. Stream/session identity, global media cursors,
    // counters, slot rotation, timing boundaries, and digest continuity remain.
    [[nodiscard]] Slus21050FemainFrameLedgerEpochRolloverPreflightResult
    preflight_epoch_rollover() const noexcept;
    [[nodiscard]] Status validate_epoch_rollover(
        const Slus21050FemainFrameLedgerEpochRolloverTicket& ticket) const noexcept;
    [[nodiscard]] Slus21050FemainFrameLedgerEpochRolloverCommitResult
    commit_epoch_rollover(const Slus21050FemainFrameLedgerEpochRolloverTicket& ticket) noexcept;

    [[nodiscard]] Slus21050FemainFrameLedgerSnapshot snapshot() const noexcept;
    [[nodiscard]] bool has_outstanding_frames() const noexcept;
    [[nodiscard]] bool drained() const noexcept;

  private:
    friend struct Slus21050FemainFrameTransactionLedgerTestAccess;

    // Internal transaction simulation needs value copies, but callers must
    // never duplicate or restore an instance identity and mutation revision.
    Slus21050FemainFrameTransactionLedger(const Slus21050FemainFrameTransactionLedger&) = default;
    Slus21050FemainFrameTransactionLedger&
    operator=(const Slus21050FemainFrameTransactionLedger&) = default;
    Slus21050FemainFrameTransactionLedger(Slus21050FemainFrameTransactionLedger&&) noexcept =
        default;
    Slus21050FemainFrameTransactionLedger&
    operator=(Slus21050FemainFrameTransactionLedger&&) noexcept = default;

    enum class RecordState : std::uint8_t {
        Reserved,
        Published,
        GraphicsComplete,
        Consumed,
        Cancelled,
    };

    struct FrameRecord {
        Slus21050FemainFrameReservation reservation{};
        RecordState state{RecordState::Reserved};
        Slus21050FemainFrameGuestBoundary published_at{};
        Slus21050FemainFrameGuestBoundary completed_at{};
        Slus21050FemainFrameGuestBoundary consumed_at{};
        bool published_boundary_valid{false};
        bool completed_boundary_valid{false};
        bool consumed_boundary_valid{false};
    };

    [[nodiscard]] static bool outstanding_record(RecordState state) noexcept;
    [[nodiscard]] std::size_t remaining_acknowledgement_mutations() const noexcept;
    [[nodiscard]] Status check_lifecycle_control_headroom() const noexcept;

    static constexpr std::uint64_t kFirstGeneration = 1U;
    static constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    [[nodiscard]] FrameRecord*
    find_record(const Slus21050FemainFrameReservation& reservation) noexcept;
    [[nodiscard]] const FrameRecord*
    find_record(const Slus21050FemainFrameReservation& reservation) const noexcept;
    [[nodiscard]] std::size_t find_free_slot() const noexcept;
    [[nodiscard]] std::size_t in_flight_count() const noexcept;
    [[nodiscard]] Status terminal_status() const noexcept;
    [[nodiscard]] Status check_event_capacity() const noexcept;
    [[nodiscard]] Status overflow() noexcept;
    [[nodiscard]] Status require_active() const noexcept;
    [[nodiscard]] Status require_source_ordinal(std::uint64_t media_ordinal) const noexcept;
    [[nodiscard]] bool
    boundary_not_before(const Slus21050FemainFrameGuestBoundary& first,
                        const Slus21050FemainFrameGuestBoundary& second) const noexcept;
    [[nodiscard]] Status
    apply_frame_transaction(const Slus21050FemainFrameTransactionEvidence& evidence,
                            Slus21050FemainFrameReservation& reservation) noexcept;
    [[nodiscard]] Status
    apply_frame_publication(const Slus21050FemainFramePublicationEvidence& evidence,
                            Slus21050FemainFrameReservation& reservation) noexcept;
    [[nodiscard]] Status
    apply_frame_retirement(const Slus21050FemainFrameRetirementEvidence& evidence) noexcept;
    [[nodiscard]] Status apply_drop_batch_frame_publication(
        std::span<const Slus21050FemainFrameIdentity> dropped_frames,
        const Slus21050FemainFramePublicationEvidence& evidence,
        Slus21050FemainFrameReservation& reservation) noexcept;
    [[nodiscard]] Status
    apply_drop_batch_frame_transaction(std::span<const Slus21050FemainFrameIdentity> dropped_frames,
                                       const Slus21050FemainFrameTransactionEvidence& evidence,
                                       Slus21050FemainFrameReservation& reservation) noexcept;
    [[nodiscard]] Status
    apply_drop_prefix_transaction(
        std::span<const Slus21050FemainFrameIdentity> dropped_frames) noexcept;

    void reset_session_records() noexcept;
    void clear_epoch_records() noexcept;
    [[nodiscard]] Slus21050FemainFrameLedgerEpochSeal current_epoch_seal() const noexcept;
    void hash_u8(std::uint8_t value) noexcept;
    void hash_u32(std::uint32_t value) noexcept;
    void hash_u64(std::uint64_t value) noexcept;
    void hash_boundary(const Slus21050FemainFrameGuestBoundary& boundary) noexcept;
    void hash_identity(const Slus21050FemainFrameIdentity& identity) noexcept;
    void hash_reservation(const Slus21050FemainFrameReservation& reservation) noexcept;
    void record_transaction_mutation() noexcept;

    State state_{State::Inactive};
    std::uint64_t session_generation_{0U};
    std::uint64_t next_session_generation_{kFirstGeneration};
    std::uint64_t next_frame_generation_{kFirstGeneration};
    std::uint64_t next_media_ordinal_{0U};
    std::uint64_t next_publication_ordinal_{0U};
    std::uint64_t next_graphics_ordinal_{0U};
    std::uint64_t next_consumption_ordinal_{0U};
    std::size_t logical_event_count_{0U};
    std::uint64_t total_logical_event_count_{0U};
    std::uint64_t epoch_generation_{0U};
    std::uint64_t epoch_first_media_ordinal_{0U};
    std::uint64_t epoch_start_digest_{0U};
    std::size_t frame_record_count_{0U};
    std::uint64_t published_frames_{0U};
    std::uint64_t graphics_completed_frames_{0U};
    std::uint64_t consumed_frames_{0U};
    std::uint64_t repeated_frames_{0U};
    std::uint64_t dropped_frames_{0U};
    std::uint64_t cancelled_frames_{0U};
    std::uint64_t digest_{0U};
    bool overflowed_{false};
    bool proof_valid_{false};
    bool has_published_frame_{false};
    Slus21050FemainFrameIdentity last_published_frame_{};
    bool has_consumed_frame_{false};
    Slus21050FemainFrameIdentity last_consumed_frame_{};
    Slus21050FemainFrameGuestBoundary last_consumed_frame_boundary_{};
    bool has_source_boundary_{false};
    bool has_presentation_boundary_{false};
    bool has_graphics_boundary_{false};
    bool has_consumption_boundary_{false};
    Slus21050FemainFrameGuestBoundary last_source_boundary_{};
    Slus21050FemainFrameGuestBoundary last_presentation_boundary_{};
    Slus21050FemainFrameGuestBoundary last_graphics_boundary_{};
    Slus21050FemainFrameGuestBoundary last_consumption_boundary_{};
    std::size_t next_slot_{0U};
    std::array<FrameRecord, kSlus21050FemainFrameTransactionCapacity> records_{};
    std::array<std::int16_t, kSlus21050FemainFrameTransactionSlotCount> slot_records_{{-1, -1}};
    std::uint64_t ledger_identity_{};
    std::uint64_t transaction_revision_{1U};
};

} // namespace takedown::runtime
