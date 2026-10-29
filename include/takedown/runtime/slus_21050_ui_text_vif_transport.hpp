#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_vif_packet_producer_ticket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace takedown::runtime::slus_21050 {

// Stable identity copied from one reviewed dynamic SubmitUiDrawBuckets text
// callback. None of these values identifies a packet ticket: the same callback
// can publish several passes and a retained payload key can recur in a later
// callback occurrence.
struct UiTextVifTransportCallbackOccurrence {
    std::uint64_t packet_session_epoch{};
    std::uint64_t callback_ordinal{};
    std::uint64_t source_ordinal{};
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};

    bool operator==(const UiTextVifTransportCallbackOccurrence&) const = default;
};

// Game-level pass roles mirror the recovered callback order without exposing
// the source-private mapper types through this startup-owned transport API.
enum class UiTextVifTransportPassRole : std::uint8_t {
    PlainOriginal,
    DropShadowOffset,
    DropShadowOriginal,
    OutlineAddOffset,
    OutlineSubtractOffset,
    OutlineAddNegatedXOffset,
    OutlineSubtractNegatedXOffset,
    OutlineOriginal,
};

struct UiTextVifTransportPassOccurrence {
    UiTextVifTransportCallbackOccurrence callback;
    // Vector position in the source line list. Filtered lines can leave gaps.
    std::size_t line_slot{};
    // Position in the exact effect-specific pass sequence for this line.
    std::size_t pass_slot{};
    UiTextVifTransportPassRole role{UiTextVifTransportPassRole::PlainOriginal};

    bool operator==(const UiTextVifTransportPassOccurrence&) const = default;
};

// Bounded copies retained by the registry after checked narrowing of the
// source-private size_t positions.
struct UiTextVifTransportCallbackIdentity {
    std::uint64_t packet_session_epoch{};
    std::uint64_t callback_ordinal{};
    std::uint64_t source_ordinal{};
    UiSubmissionKey key;
    std::uint32_t bucket_index{};
    std::uint32_t node_index{};
    UiTextEffectKind effect{UiTextEffectKind::Plain};

    bool operator==(const UiTextVifTransportCallbackIdentity&) const = default;
};

struct UiTextVifTransportPassIdentity {
    UiTextVifTransportCallbackIdentity callback;
    std::uint32_t line_slot{};
    std::uint32_t pass_slot{};
    UiTextVifTransportPassRole role{UiTextVifTransportPassRole::PlainOriginal};

    bool operator==(const UiTextVifTransportPassIdentity&) const = default;
};

static_assert(std::is_standard_layout_v<UiTextVifTransportCallbackOccurrence>);
static_assert(std::is_standard_layout_v<UiTextVifTransportPassOccurrence>);
static_assert(std::is_standard_layout_v<UiTextVifTransportCallbackIdentity>);
static_assert(std::is_standard_layout_v<UiTextVifTransportPassIdentity>);
static_assert(std::is_trivially_copyable_v<UiTextVifTransportCallbackIdentity>);
static_assert(std::is_trivially_copyable_v<UiTextVifTransportPassIdentity>);

// Never-reused identity for one registry slot lifetime. It exists before the
// producer ledger exposes its preallocated ticket ID and remains stable until
// the claim is released after exact final consumption.
struct UiTextVifTransportClaimId {
    std::uint64_t value{};

    bool operator==(const UiTextVifTransportClaimId&) const = default;
};

// Registry-local handle for every pass in one callback reservation. It is not
// interchangeable with the ledger's reservation token.
struct UiTextVifTransportCallbackReservation {
    std::uint64_t value{};
    std::uint32_t pass_count{};

    bool operator==(const UiTextVifTransportCallbackReservation&) const = default;
};

struct UiTextVifTransportPassClaimRequest {
    UiTextVifTransportPassOccurrence pass;
    VifPacketProducerTicketReservationRequest packet;

    bool operator==(const UiTextVifTransportPassClaimRequest&) const = default;
};

enum class UiTextVifTransportClaimState : std::uint8_t {
    Reserved,
    Published,
    MarkerArmed,
};

// Fixed-size diagnostic snapshot. Published tickets reflect their current
// relocated anchor.
struct UiTextVifTransportClaim {
    UiTextVifTransportClaimId id;
    UiTextVifTransportCallbackReservation reservation;
    UiTextVifTransportPassIdentity pass;
    VifPacketProducerTicketOwner owner;
    UiTextVifTransportClaimState state{UiTextVifTransportClaimState::Reserved};
    std::optional<VifPacketProducerTicket> ticket;
    std::optional<guest::Vif1DmacPreSnapshotResolvedMarker> marker;

    bool operator==(const UiTextVifTransportClaim& other) const noexcept {
        if (id != other.id || reservation != other.reservation || pass != other.pass ||
            owner != other.owner || state != other.state || ticket != other.ticket ||
            marker.has_value() != other.marker.has_value()) {
            return false;
        }
        if (!marker.has_value()) {
            return true;
        }
        const auto& first = *marker;
        const auto& second = *other.marker;
        return first.token == second.token && first.generation == second.generation &&
               first.candidate_source == second.candidate_source &&
               first.request.source == second.request.source &&
               first.request.chain_root == second.request.chain_root &&
               first.request.source_tag_location == second.request.source_tag_location &&
               first.request.source_tag_ordinal == second.request.source_tag_ordinal &&
               first.request.source_payload_word_ordinal ==
                   second.request.source_payload_word_ordinal &&
               first.request.expected_quadword == second.request.expected_quadword;
    }
};

enum class UiTextVifTransportReserveStatus : std::uint8_t {
    Reserved,
    EmptyRequest,
    OutputTooSmall,
    InvalidCallbackIdentity,
    CallbackIdentityMismatch,
    CallbackAlreadyReserved,
    InvalidPassOrder,
    PositionOverflow,
    CapacityExhausted,
    ClaimGenerationExhausted,
    ReservationGenerationExhausted,
    LedgerRejected,
};

struct UiTextVifTransportReserveResult {
    UiTextVifTransportReserveStatus status{UiTextVifTransportReserveStatus::EmptyRequest};
    UiTextVifTransportCallbackReservation reservation;
    std::size_t requested_count{};
    std::size_t available_count{};
    VifPacketProducerTicketReservationStatus ledger_status{
        VifPacketProducerTicketReservationStatus::EmptyRequest};

    [[nodiscard]] bool reserved() const noexcept {
        return status == UiTextVifTransportReserveStatus::Reserved;
    }
};

enum class UiTextVifTransportPreflightStatus : std::uint8_t {
    Ready,
    ClaimUnavailable,
    ClaimNotReserved,
    LedgerRejected,
};

struct UiTextVifTransportPreflightResult {
    UiTextVifTransportPreflightStatus status{
        UiTextVifTransportPreflightStatus::ClaimUnavailable};
    VifPacketProducerTicketReservationPreflightStatus ledger_status{
        VifPacketProducerTicketReservationPreflightStatus::StaleReservation};

    [[nodiscard]] bool ready() const noexcept {
        return status == UiTextVifTransportPreflightStatus::Ready;
    }
};

enum class UiTextVifTransportPublishStatus : std::uint8_t {
    Published,
    ClaimUnavailable,
    ClaimNotReserved,
    LedgerRejected,
    LedgerInvariantViolation,
};

struct UiTextVifTransportPublishResult {
    UiTextVifTransportPublishStatus status{
        UiTextVifTransportPublishStatus::ClaimUnavailable};
    UiTextVifTransportClaimId claim_id;
    VifPacketProducerTicketId ticket_id;
    VifPacketProducerTicketReservedPublishStatus ledger_status{
        VifPacketProducerTicketReservedPublishStatus::StaleReservation};
    std::size_t retired_claims{};

    [[nodiscard]] bool published() const noexcept {
        return status == UiTextVifTransportPublishStatus::Published;
    }
};

enum class UiTextVifTransportCancelStatus : std::uint8_t {
    Cancelled,
    ReservationUnavailable,
    MarkerOwnsClaim,
    LedgerInvariantViolation,
};

struct UiTextVifTransportCancelResult {
    UiTextVifTransportCancelStatus status{
        UiTextVifTransportCancelStatus::ReservationUnavailable};
    VifPacketProducerTicketReservationCancelResult ledger;
    std::size_t retired_claims{};

    [[nodiscard]] bool cancelled() const noexcept {
        return status == UiTextVifTransportCancelStatus::Cancelled;
    }
};

enum class UiTextVifTransportReleaseStatus : std::uint8_t {
    Released,
    ClaimUnavailable,
    ClaimNotPublished,
    MarkerOwnsClaim,
    LedgerRejected,
    LedgerInvariantViolation,
};

// Terminal-facing value produced only after the private ledger transfers the
// exact completed ticket and the registry retires its claim slot. Unlike a
// live claim snapshot, success always carries a ticket.
struct UiTextVifTransportReleasedClaim {
    UiTextVifTransportClaimId id;
    UiTextVifTransportCallbackReservation reservation;
    UiTextVifTransportPassIdentity pass;
    VifPacketProducerTicket ticket;

    bool operator==(const UiTextVifTransportReleasedClaim&) const = default;
};

struct UiTextVifTransportReleaseResult {
    UiTextVifTransportReleaseStatus status{
        UiTextVifTransportReleaseStatus::ClaimUnavailable};
    UiTextVifTransportClaimId claim_id;
    VifPacketProducerTicketTakeStatus ledger_status{
        VifPacketProducerTicketTakeStatus::TicketUnavailable};
    std::optional<UiTextVifTransportReleasedClaim> released_claim;

    [[nodiscard]] bool released() const noexcept {
        return status == UiTextVifTransportReleaseStatus::Released &&
               released_claim.has_value();
    }
};

struct UiTextVifTransportRelocationReport {
    VifPacketProducerTicketRelocationResult ledger;
    std::size_t retired_claims{};
};

struct UiTextVifTransportWriteReport {
    VifPacketProducerTicketInvalidationResult ledger;
    std::size_t retired_claims{};
};

struct UiTextVifTransportDiagnostics {
    std::size_t occupied_claims{};
    std::size_t reserved_claims{};
    std::size_t published_claims{};
    std::size_t marker_armed_claims{};
    std::size_t ledger_active_tickets{};
    std::size_t ledger_reserved_tickets{};
    std::uint64_t admitted_callbacks{};
    std::uint64_t admitted_claims{};
    std::uint64_t published_tickets{};
    std::uint64_t relocated_tickets{};
    std::uint64_t reconciled_retired_claims{};
    std::uint64_t fail_closed_retire_calls{};
    std::uint64_t fail_closed_retired_claims{};
    std::uint64_t cancelled_callbacks{};
    std::uint64_t released_claims{};
    std::uint64_t pre_snapshot_candidate_callbacks{};
    std::uint64_t pre_snapshot_candidates_offered{};
    std::uint64_t pre_snapshot_resolved_callbacks{};
    std::uint64_t marker_armed_claims_total{};
    std::uint64_t incomplete_marker_groups{};
    std::uint64_t marker_reached_claims{};
    std::uint64_t marker_cancelled_claims{};
    std::uint64_t terminal_provenance_failures{};
    std::uint64_t attachment_resets{};
    std::uint64_t attachment_retired_claims{};
    std::uint64_t invariant_failures{};
};

// One serialized, startup-owned bridge from game-specific text-pass identity
// to mutable packet storage and, later, terminal VIF1/PATH2 markers. All hot
// operations use caller-owned spans and fixed arrays. The class allocates no
// memory, owns no locks, performs no guest work, and exposes no mutable ledger.
class UiTextVifTransportClaimRegistry final
    : public guest::Vif1Path2SourceMarkerObserver {
  public:
    static constexpr std::size_t kCapacity = VifPacketProducerTicketLedger::kCapacity;

    UiTextVifTransportClaimRegistry() = default;
    ~UiTextVifTransportClaimRegistry() override;
    UiTextVifTransportClaimRegistry(const UiTextVifTransportClaimRegistry&) = delete;
    UiTextVifTransportClaimRegistry& operator=(const UiTextVifTransportClaimRegistry&) = delete;
    UiTextVifTransportClaimRegistry(UiTextVifTransportClaimRegistry&&) = delete;
    UiTextVifTransportClaimRegistry& operator=(UiTextVifTransportClaimRegistry&&) = delete;

    // The registry is a scoped observer. Rebinding closes every prior claim
    // while preserving never-reused registry identities. Detach uses an
    // expected-observer comparison so cleanup cannot displace a later owner.
    // An attached EeMemory must outlive this registry unless detach() is
    // called first.
    void attach(guest::EeMemory& memory) noexcept;
    void detach() noexcept;

    // Reserves registry capacity and the complete ledger group before any guest
    // packet write. `claim_ids` is untouched on failure and receives request-
    // order identities on success.
    [[nodiscard]] UiTextVifTransportReserveResult try_reserve(
        std::span<const UiTextVifTransportPassClaimRequest> requests,
        std::span<UiTextVifTransportClaimId> claim_ids) noexcept;

    // ClaimId selects the bound reservation ordinal; callers cannot supply an
    // unrelated pass ordinal or owner during publication.
    [[nodiscard]] UiTextVifTransportPreflightResult
    can_publish(UiTextVifTransportClaimId id) const noexcept;
    [[nodiscard]] UiTextVifTransportPublishResult publish_reserved_after_stream_commit(
        const guest::EeMemory& memory, UiTextVifTransportClaimId id,
        const StreamEnvironmentPacketGuestResult& result) noexcept;

    // Cancels one complete callback group while it is still ledger-owned.
    [[nodiscard]] UiTextVifTransportCancelResult
    cancel_callback(UiTextVifTransportCallbackReservation reservation) noexcept;

    // Constant-time gate for packet writers that cannot report an exact
    // mutation. Reserved and Published claims remain ledger-owned; a
    // MarkerArmed claim has already transferred into an immutable timed-DMA
    // snapshot and therefore does not keep this gate active.
    [[nodiscard]] bool has_unarmed_claims() const noexcept;

    // Fail-closed escape hatch for an uninstrumented writer. Clears every
    // ledger-owned Reserved or Published claim and any provisional candidate
    // offer while preserving MarkerArmed slots and never-reused identities.
    [[nodiscard]] std::size_t retire_unarmed_claims() noexcept;

    // Reports already-completed controlled mutations.
    [[nodiscard]] UiTextVifTransportRelocationReport report_copy(
        const guest::EeMemory& memory, VifPacketQwordRange source,
        VifPacketQwordRange destination) noexcept;
    [[nodiscard]] UiTextVifTransportRelocationReport report_ring_flush(
        const guest::EeMemory& memory, const VifPacketRingFlushResult& flush) noexcept;
    [[nodiscard]] UiTextVifTransportWriteReport
    report_write(guest::GuestAddress begin, std::size_t byte_count) noexcept;

    // Releases exactly one fully published claim before a marker owns it. The
    // ledger transfer and slot retirement are atomic on the serialized EE
    // thread; the returned fixed-size snapshot retains the exact final ticket
    // for diagnostics.
    [[nodiscard]] UiTextVifTransportReleaseResult
    release_completed_claim(UiTextVifTransportClaimId id) noexcept;

    // Offers complete, fully published callback groups to the timed VIF1
    // source walk. A group transfers out of the mutable packet ledger only
    // after every pass resolves into the same immutable snapshot batch.
    [[nodiscard]] bool
    has_vif1_dmac_pre_snapshot_candidates() const noexcept override;
    [[nodiscard]] std::size_t on_vif1_dmac_pre_snapshot_candidates(
        const guest::Vif1DmacPreSnapshotObservation& observation,
        std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept override;
    void on_vif1_dmac_pre_snapshot_markers_resolved(
        std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
        std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept override;
    void on_vif1_path2_source_marker_memory_cleared() noexcept override;
    void on_vif1_path2_source_marker_reached(
        const guest::Vif1Path2SourceMarkerReached& marker) noexcept override;
    void on_vif1_path2_source_marker_cancelled(
        const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept override;

    [[nodiscard]] std::optional<UiTextVifTransportClaim>
    find(UiTextVifTransportClaimId id) const noexcept;
    [[nodiscard]] UiTextVifTransportDiagnostics diagnostics() const noexcept;

  private:
    struct Slot {
        UiTextVifTransportClaimId id;
        UiTextVifTransportCallbackReservation reservation;
        VifPacketProducerTicketReservation ledger_reservation;
        std::uint32_t pass_ordinal{};
        UiTextVifTransportPassIdentity pass;
        VifPacketProducerTicketReservationRequest packet;
        UiTextVifTransportClaimState state{UiTextVifTransportClaimState::Reserved};
        VifPacketProducerTicketId ticket_id;
        VifPacketProducerTicket held_ticket;
        guest::Vif1DmacPreSnapshotResolvedMarker marker;
    };

    struct OfferedCandidate {
        UiTextVifTransportClaimId id;
        UiTextVifTransportCallbackReservation reservation;
        std::uint32_t pass_ordinal{};
        VifPacketProducerTicket ticket;
    };

    [[nodiscard]] Slot* find_slot(UiTextVifTransportClaimId id) noexcept;
    [[nodiscard]] const Slot* find_slot(UiTextVifTransportClaimId id) const noexcept;
    [[nodiscard]] std::size_t reconcile_with_ledger() noexcept;
    void reset_attachment_claims() noexcept;
    void retire_slot(std::optional<Slot>& slot) noexcept;

    guest::EeMemory* attached_memory_{};
    VifPacketProducerTicketLedger ledger_;
    std::array<std::optional<Slot>, kCapacity> claims_{};
    std::array<OfferedCandidate, kCapacity> offered_candidates_{};
    std::size_t offered_candidate_count_{};
    guest::Vif1DmacPreSnapshotObservation offered_observation_{};
    std::size_t occupied_claims_{};
    std::uint64_t next_claim_id_{1U};
    std::uint64_t next_reservation_id_{1U};
    std::uint64_t admitted_callbacks_{};
    std::uint64_t admitted_claims_{};
    std::uint64_t published_tickets_{};
    std::uint64_t relocated_tickets_{};
    std::uint64_t reconciled_retired_claims_{};
    std::uint64_t fail_closed_retire_calls_{};
    std::uint64_t fail_closed_retired_claims_{};
    std::uint64_t cancelled_callbacks_{};
    std::uint64_t released_claims_{};
    std::uint64_t pre_snapshot_candidate_callbacks_{};
    std::uint64_t pre_snapshot_candidates_offered_{};
    std::uint64_t pre_snapshot_resolved_callbacks_{};
    std::uint64_t marker_armed_claims_total_{};
    std::uint64_t incomplete_marker_groups_{};
    std::uint64_t marker_reached_claims_{};
    std::uint64_t marker_cancelled_claims_{};
    std::uint64_t terminal_provenance_failures_{};
    std::uint64_t attachment_resets_{};
    std::uint64_t attachment_retired_claims_{};
    std::uint64_t invariant_failures_{};
};

static_assert(UiTextVifTransportClaimRegistry::kCapacity == 64U);
static_assert(std::is_standard_layout_v<UiTextVifTransportClaimId>);
static_assert(std::is_standard_layout_v<UiTextVifTransportCallbackReservation>);
static_assert(std::is_standard_layout_v<UiTextVifTransportPassClaimRequest>);
static_assert(std::is_standard_layout_v<UiTextVifTransportClaim>);
static_assert(std::is_standard_layout_v<UiTextVifTransportReleasedClaim>);

} // namespace takedown::runtime::slus_21050
