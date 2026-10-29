#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/runtime/slus_21050_stream_environment_guest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

// An opaque game-side occurrence identifier. It correlates a future typed
// producer with its packet pass but is deliberately not a ticket identity:
// one callback can emit several passes, and repeated owners are valid.
struct VifPacketProducerTicketOwner {
    std::uint64_t opaque_value{};

    bool operator==(const VifPacketProducerTicketOwner&) const = default;
};

// Never-reused allocation identity for one producer ticket in one ledger. It
// is separate from the owner because owner values can repeat. A reservation
// allocates IDs before its packets commit, so numeric ID order is not a
// temporal publication order when normal shadow tickets interleave.
struct VifPacketProducerTicketId {
    std::uint64_t value{};

    bool operator==(const VifPacketProducerTicketId&) const = default;
};

// Never-reused identity for one ordered reservation group in one ledger. A
// group binds every pass in one future native callback before its first guest
// packet write. It is deliberately separate from ticket IDs: each pass still
// receives its own ticket identity when the group is admitted.
struct VifPacketProducerTicketReservationId {
    std::uint64_t value{};

    bool operator==(const VifPacketProducerTicketReservationId&) const = default;
};

// Ledger-local token returned by a successful all-or-none reservation.
// `pass_count` distinguishes a malformed group shape, but this is not a
// security capability: callers must retain and use it only with its issuing
// ledger on the serialized emulation thread.
struct VifPacketProducerTicketReservation {
    VifPacketProducerTicketReservationId id;
    std::size_t pass_count{};

    bool operator==(const VifPacketProducerTicketReservation&) const = default;
};

// The complete logical element range remains part of the ticket so a reported
// overlapping later write retires the pass conservatively. `first_element` is
// the one physical quadword eventually resolved to a VIF1/PATH2 marker and
// the only quadword whose bytes this shadow checkpoint authenticates. It is a
// terminal-source anchor, not full-pass content authentication.
struct StreamEnvironmentProducerAnchor {
    VifPacketQwordRange element_output;
    VifPacketQwordRange first_element;
    guest::GsQuadword expected_first_element{};

    bool operator==(const StreamEnvironmentProducerAnchor&) const = default;
};

// Ownerless no-flush storage and source shape for one future producer pass.
// A game-specific planner can retain this compact value without retaining a
// mutable complete stream plan, then bind the real callback owner immediately
// before ledger admission. It is not a capability or proof of plan provenance.
struct VifPacketProducerTicketReservationPass {
    StreamEnvironmentProducerAnchor anchor;
    VifPacketQwordRange packet_output;
    VifPacketQwordRange header_output;

    bool operator==(const VifPacketProducerTicketReservationPass&) const = default;
};

// One future no-flush stream pass. The request captures values, rather than a
// plan reference, so its owner and packet-memory write footprint cannot be
// reconstructed from mutable guest state after a packet commit. `packet_output`
// covers the packet body and `header_output` covers the live header-count
// write. For a fresh header, `header_output` is contained by `packet_output`;
// for a reused header, it is a separate range. Neither form can overlap the
// pass's encoded element output. The intended call path uses the factories with
// an already planner-validated stream plan. Public plan, pass, and request
// types remain composition inputs; the factories and ledger apply local
// conservative shape checks but do not authenticate plan provenance.
struct VifPacketProducerTicketReservationRequest {
    VifPacketProducerTicketOwner owner;
    StreamEnvironmentProducerAnchor anchor;
    VifPacketQwordRange packet_output;
    VifPacketQwordRange header_output;
    StreamEnvironmentPacketCapacityMode capacity_mode{
        StreamEnvironmentPacketCapacityMode::ExistingRoom};
    bool has_flush_plan{};

    bool operator==(const VifPacketProducerTicketReservationRequest&) const = default;
};

struct VifPacketProducerTicket {
    VifPacketProducerTicketId id;
    VifPacketProducerTicketOwner owner;
    StreamEnvironmentProducerAnchor anchor;
    // A normal shadow-only publication has no reservation association. A
    // reservation-backed ticket retains it so later callback teardown can
    // cancel every pass in that callback as one unit.
    std::optional<VifPacketProducerTicketReservationId> reservation_id;
    std::size_t reservation_pass_ordinal{};
    std::size_t reservation_pass_count{};

    bool operator==(const VifPacketProducerTicket&) const = default;
};

enum class VifPacketProducerTicketPublishStatus : std::uint8_t {
    Published,
    InvalidOwner,
    InvalidAnchor,
    CommitResultMismatch,
    SourceUnavailable,
    SourceContentsMismatch,
    CapacityExhausted,
    GenerationExhausted,
};

struct VifPacketProducerTicketPublishResult {
    VifPacketProducerTicketPublishStatus status{
        VifPacketProducerTicketPublishStatus::InvalidAnchor};
    VifPacketProducerTicketId id{};

    [[nodiscard]] bool published() const noexcept {
        return status == VifPacketProducerTicketPublishStatus::Published;
    }
};

enum class VifPacketProducerTicketReservationStatus : std::uint8_t {
    Reserved,
    EmptyRequest,
    InvalidOwner,
    InvalidAnchor,
    InvalidPacketOutput,
    InvalidHeaderOutput,
    PacketDoesNotContainElementOutput,
    EmbeddedFlush,
    OverlappingElementOutput,
    OverlappingPacketOutput,
    OverlappingPacketHeaderOutput,
    OverlappingOutstandingReservationWrite,
    CapacityExhausted,
    TicketGenerationExhausted,
    ReservationGenerationExhausted,
};

// Failed admissions never reserve a prefix. `available_count` is sampled
// before admission and is diagnostic only; a future live callback must still
// keep ledger access serialized with its packet writes.
struct VifPacketProducerTicketReservationResult {
    VifPacketProducerTicketReservationStatus status{
        VifPacketProducerTicketReservationStatus::EmptyRequest};
    VifPacketProducerTicketReservation reservation{};
    std::size_t requested_count{};
    std::size_t available_count{};

    [[nodiscard]] bool reserved() const noexcept {
        return status == VifPacketProducerTicketReservationStatus::Reserved;
    }
};

enum class VifPacketProducerTicketReservationPreflightStatus : std::uint8_t {
    Ready,
    StaleReservation,
    PassOutOfOrder,
    PassAlreadyPublished,
};

// This preflight has no guest-memory or ledger side effects. It exists so a
// future callback can reject an ordering mistake before it writes its next
// packet; `publish_reserved_after_stream_commit` treats the same error as a
// post-commit failure and clears the sidecar ledger fail-closed.
struct VifPacketProducerTicketReservationPreflightResult {
    VifPacketProducerTicketReservationPreflightStatus status{
        VifPacketProducerTicketReservationPreflightStatus::StaleReservation};

    [[nodiscard]] bool ready() const noexcept {
        return status == VifPacketProducerTicketReservationPreflightStatus::Ready;
    }
};

enum class VifPacketProducerTicketReservedPublishStatus : std::uint8_t {
    Published,
    StaleReservation,
    PassOutOfOrder,
    PassAlreadyPublished,
    CommitResultMismatch,
    SourceUnavailable,
    SourceContentsMismatch,
    ReservationInvalidated,
};

// A nonzero ID on a failed result identifies the preallocated pass whose
// post-commit authentication failed. Such failures abort the entire group,
// because the guest packet write has already happened. `ledger_cleared` means
// the committed packet-storage footprint itself was not trustworthy, so every
// sidecar ticket and reservation was retired fail-closed.
struct VifPacketProducerTicketReservedPublishResult {
    VifPacketProducerTicketReservedPublishStatus status{
        VifPacketProducerTicketReservedPublishStatus::StaleReservation};
    VifPacketProducerTicketId id{};
    bool reservation_aborted{};
    bool ledger_cleared{};

    [[nodiscard]] bool published() const noexcept {
        return status == VifPacketProducerTicketReservedPublishStatus::Published;
    }
};

struct VifPacketProducerTicketReservationCancelResult {
    bool cancelled{};
    std::size_t retired_tickets{};
    std::size_t released_reservations{};
};

enum class VifPacketProducerTicketRelocationStatus : std::uint8_t {
    Applied,
    InvalidRange,
};

// Retire counts make a shadow run diagnosable without involving a renderer or
// the terminal VIF1/PATH2 marker registry.
struct VifPacketProducerTicketRelocationResult {
    VifPacketProducerTicketRelocationStatus status{
        VifPacketProducerTicketRelocationStatus::InvalidRange};
    std::size_t relocated{};
    std::size_t retired_destination_overwrite{};
    std::size_t retired_destination_contents_mismatch{};
    std::size_t retired_partial_source{};
    std::size_t retired_incomplete_reservation{};
    std::size_t aborted_reservations{};
    std::size_t released_reservations{};
};

enum class VifPacketProducerTicketInvalidationStatus : std::uint8_t {
    Applied,
    InvalidRange,
};

struct VifPacketProducerTicketInvalidationResult {
    VifPacketProducerTicketInvalidationStatus status{
        VifPacketProducerTicketInvalidationStatus::InvalidRange};
    std::size_t retired{};
    std::size_t aborted_reservations{};
    std::size_t released_reservations{};
};

enum class VifPacketProducerTicketTakeStatus : std::uint8_t {
    Taken,
    InvalidId,
    TicketUnavailable,
    ReservationIncomplete,
};

// Transfers one completed ticket out of the ledger without cancelling its
// callback siblings. A reservation-backed ticket is transferable only after
// every pass in its reservation has published; before that point the complete
// group is still the unit of failure and cancellation. The returned ticket is
// an owned POD snapshot suitable for a later terminal transport registry.
struct VifPacketProducerTicketTakeResult {
    VifPacketProducerTicketTakeStatus status{
        VifPacketProducerTicketTakeStatus::TicketUnavailable};
    std::optional<VifPacketProducerTicket> ticket;

    [[nodiscard]] bool taken() const noexcept {
        return status == VifPacketProducerTicketTakeStatus::Taken && ticket.has_value();
    }
};

// Derives the one safe logical ticket anchor from an already-validated stream
// plan. Fresh and reused headers naturally produce different addresses because
// both plans expose `element_output`, rather than making header identity stand
// in for a pass.
[[nodiscard]] std::optional<StreamEnvironmentProducerAnchor>
make_stream_environment_producer_anchor(const StreamEnvironmentPacketGuestPlan& plan) noexcept;
[[nodiscard]] std::optional<StreamEnvironmentProducerAnchor>
make_stream_environment_producer_anchor(
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept;

// Captures the compact ownerless shape of one no-flush stream pass. The
// intended caller supplies a planner-validated plan. Flush-shaped plans return
// no pass because their relocation and overwrite accounting needs a separate
// proven commit composition.
[[nodiscard]] std::optional<VifPacketProducerTicketReservationPass>
make_stream_environment_packet_ticket_reservation_pass(
    const StreamEnvironmentPacketGuestPlan& plan) noexcept;
[[nodiscard]] std::optional<VifPacketProducerTicketReservationPass>
make_stream_environment_packet_ticket_reservation_pass(
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept;

// Binds a real caller-supplied owner to a retained no-flush pass shape. The
// binding applies local range checks but does not reserve ledger capacity or
// establish plan provenance.
[[nodiscard]] std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    VifPacketProducerTicketOwner owner,
    const VifPacketProducerTicketReservationPass& pass) noexcept;

// Convenience overloads that capture and bind one no-flush pass directly from
// a stream plan.
[[nodiscard]] std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    VifPacketProducerTicketOwner owner, const StreamEnvironmentPacketGuestPlan& plan) noexcept;
[[nodiscard]] std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    VifPacketProducerTicketOwner owner,
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept;

// A fixed-capacity, shadow-only sidecar for typed packet producers. It owns no
// guest memory and never starts DMA, arms a source marker, or submits a native
// draw. A caller must explicitly report every controlled commit, copy, and
// overwrite; unreported writers make a ticket unsuitable for live activation.
class VifPacketProducerTicketLedger {
  public:
    static constexpr std::size_t kCapacity = 64U;

    VifPacketProducerTicketLedger() = default;
    VifPacketProducerTicketLedger(const VifPacketProducerTicketLedger&) = delete;
    VifPacketProducerTicketLedger& operator=(const VifPacketProducerTicketLedger&) = delete;
    VifPacketProducerTicketLedger(VifPacketProducerTicketLedger&&) = delete;
    VifPacketProducerTicketLedger& operator=(VifPacketProducerTicketLedger&&) = delete;

    // Atomically reserves every pass in a future ordered callback. Requests
    // bind the owner, anchor, and exact no-flush packet-memory outputs before
    // guest mutation. A reservation also preallocates the eventual ticket IDs,
    // so a normal post-commit shadow publish cannot consume an admitted
    // group's identities or capacity.
    [[nodiscard]] VifPacketProducerTicketReservationResult try_reserve_stream_passes(
        std::span<const VifPacketProducerTicketReservationRequest> requests) noexcept;

    // Checks whether `pass_ordinal` is the next claim without declaring that a
    // guest packet has been written. A real callback must use this before its
    // individual commit if it wants an ordering failure to remain retryable.
    [[nodiscard]] VifPacketProducerTicketReservationPreflightResult
    can_publish_reserved_pass(const VifPacketProducerTicketReservation& reservation,
                              std::size_t pass_ordinal) const noexcept;

    // Completes exactly the next pass in a reservation immediately after its
    // individual packet commit. It reads no plan and accepts no caller-owned
    // owner/anchor, so completion can only publish the values bound at
    // reservation time. It first reports the proven no-flush packet and header
    // writes, retiring conflicting tickets or groups while exempting this
    // group. Every post-commit failure aborts every reserved and already-
    // published pass in the group when that group is still present.
    [[nodiscard]] VifPacketProducerTicketReservedPublishResult
    publish_reserved_after_stream_commit(const guest::EeMemory& memory,
                                         const VifPacketProducerTicketReservation& reservation,
                                         std::size_t pass_ordinal,
                                         const StreamEnvironmentPacketGuestResult& result) noexcept;

    // Cancels every pending or published pass associated with a reservation.
    // This can repair a callback only before it has armed a downstream marker
    // or native draw; after that, a future live bridge must couple this with
    // downstream cancellation or consumption.
    [[nodiscard]] VifPacketProducerTicketReservationCancelResult
    cancel_reservation(const VifPacketProducerTicketReservation& reservation) noexcept;

    [[nodiscard]] VifPacketProducerTicketPublishResult try_publish_after_stream_commit(
        const guest::EeMemory& memory, VifPacketProducerTicketOwner owner,
        const StreamEnvironmentProducerAnchor& anchor,
        const StreamEnvironmentPacketGuestResult& result) noexcept;

    // Applies an already-completed old-range to new-range copy. A ticket moves
    // only if its full element range is contained by the copied source; a
    // partial move is retired rather than guessed. Existing destination tickets
    // retire before source tickets move into that range.
    [[nodiscard]] VifPacketProducerTicketRelocationResult relocate_after_copy(
        const guest::EeMemory& memory, VifPacketQwordRange source,
        VifPacketQwordRange destination) noexcept;
    [[nodiscard]] VifPacketProducerTicketRelocationResult relocate_after_ring_flush(
        const guest::EeMemory& memory, const VifPacketRingFlushResult& flush) noexcept;

    // Any *reported* byte overlap retires a ticket even when a later write
    // happens to reproduce the old values. The address range is a nonwrapping
    // logical guest range; circular scratchpad copies are intentionally
    // outside this first controlled staging seam.
    [[nodiscard]] VifPacketProducerTicketInvalidationResult invalidate_written_range(
        guest::GuestAddress begin, std::size_t byte_count) noexcept;

    [[nodiscard]] bool cancel(VifPacketProducerTicketId id) noexcept;
    // Unlike cancel(), this removes exactly one completed ticket and never
    // aborts a sibling pass. It is the handoff from packet-memory tracking to
    // a terminal source-marker owner after the source snapshot is bound.
    [[nodiscard]] VifPacketProducerTicketTakeResult
    take_completed(VifPacketProducerTicketId id) noexcept;
    // A future live owner must call this at the EE/reset/session boundary;
    // this shadow-only ledger has no implicit memory-lifetime hook. IDs are
    // intentionally not recycled across clear().
    void clear() noexcept;
    [[nodiscard]] std::optional<VifPacketProducerTicket>
    find(VifPacketProducerTicketId id) const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t reserved_count() const noexcept { return reserved_count_; }
    [[nodiscard]] std::size_t occupied_count() const noexcept {
        return active_count_ + reserved_count_;
    }
    [[nodiscard]] std::size_t available_count() const noexcept {
        return tickets_.size() - occupied_count();
    }

  private:
    struct ReservedPass {
        VifPacketProducerTicketId ticket_id;
        VifPacketProducerTicketReservationId reservation_id;
        std::size_t pass_ordinal{};
        std::size_t pass_count{};
        VifPacketProducerTicketOwner owner;
        StreamEnvironmentProducerAnchor anchor;
        VifPacketQwordRange packet_output;
        VifPacketQwordRange header_output;
    };

    struct ReservationGroup {
        VifPacketProducerTicketReservationId id;
        std::size_t pass_count{};
        std::size_t next_pass_ordinal{};
    };

    // A claimed pass keeps its packet-storage footprint while its callback
    // still has pending passes. The public ticket intentionally remains an
    // element-source anchor; this private record prevents a later write from
    // invalidating the callback's unclaimed projection through a header or
    // packet-prefix overlap that does not touch the claimed first element.
    struct ClaimedReservationPass {
        VifPacketProducerTicketReservationId reservation_id;
        std::size_t pass_count{};
        VifPacketQwordRange packet_output;
        VifPacketQwordRange header_output;
    };

    struct ReservationAbortCounts {
        bool cancelled{};
        std::size_t retired_tickets{};
        std::size_t released_reservations{};
    };

    [[nodiscard]] ReservationGroup* find_reservation_group(
        const VifPacketProducerTicketReservation& reservation) noexcept;
    [[nodiscard]] ReservationAbortCounts abort_reservation_group(
        const VifPacketProducerTicketReservation& reservation) noexcept;
    void clear_claimed_reservation_passes(
        const VifPacketProducerTicketReservation& reservation) noexcept;
    [[nodiscard]] VifPacketProducerTicketInvalidationResult
    invalidate_written_range_impl(guest::GuestAddress begin, std::size_t byte_count,
                                  const VifPacketProducerTicketReservation*
                                      reservation_exemption) noexcept;
    [[nodiscard]] VifPacketProducerTicketInvalidationResult
    invalidate_stream_packet_memory_writes_impl(
        VifPacketQwordRange packet_output, VifPacketQwordRange header_output,
        const VifPacketProducerTicketReservation* reservation_exemption) noexcept;

    std::array<std::optional<VifPacketProducerTicket>, kCapacity> tickets_{};
    std::array<std::optional<ReservedPass>, kCapacity> reserved_passes_{};
    std::array<std::optional<ClaimedReservationPass>, kCapacity> claimed_reservation_passes_{};
    std::array<std::optional<ReservationGroup>, kCapacity> reservation_groups_{};
    std::size_t active_count_{};
    std::size_t reserved_count_{};
    std::uint64_t next_id_{1U};
    std::uint64_t next_reservation_id_{1U};
};

// Controlled composition for one already-supported StreamEnvironmentPacket
// commit. It is opt-in: existing generic stream commits retain their exact
// behavior, and this wrapper only maintains the shadow ledger after a
// successful guest commit. Admission is intentionally post-commit: a rejected
// shadow ticket never rolls back valid guest work. A future live path must
// reserve its ledger slot before mutating guest memory.
struct StreamEnvironmentPacketProducerTicketCommitResult {
    StreamEnvironmentPacketGuestResult stream;
    VifPacketProducerTicketPublishResult ticket;
};

[[nodiscard]] StreamEnvironmentPacketProducerTicketCommitResult
commit_stream_environment_packet_with_producer_ticket(
    guest::EeMemory& memory, const StreamEnvironmentPacketGuestPlan& plan,
    VifPacketProducerTicketLedger& ledger, VifPacketProducerTicketOwner owner);

// Controlled composition for the verified direct-main-RAM to scratchpad copy.
// It preserves copy semantics and applies the ledger move only after the
// destination bytes exist. General packet writers remain opt-in until their
// overwrite reporting is equally verified.
[[nodiscard]] VifPacketProducerTicketRelocationResult
copy_vif_packet_quadwords_with_producer_tickets(
    guest::EeMemory& memory, VifPacketQwordRange destination,
    VifPacketQwordRange source, VifPacketProducerTicketLedger& ledger);

} // namespace takedown::runtime::slus_21050
