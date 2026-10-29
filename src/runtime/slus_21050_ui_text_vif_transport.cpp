#include "takedown/runtime/slus_21050_ui_text_vif_transport.hpp"

#include <algorithm>
#include <limits>

namespace takedown::runtime::slus_21050 {
namespace {

void saturating_increment(unsigned long long& value) noexcept {
    if (value != std::numeric_limits<unsigned long long>::max()) {
        ++value;
    }
}

void saturating_add(std::uint64_t& value, std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
        value = std::numeric_limits<std::uint64_t>::max();
        return;
    }
    value += amount;
}

[[nodiscard]] bool ticket_matches_reservation(const VifPacketProducerTicket& ticket,
                                              const VifPacketProducerTicketOwner& owner,
                                              const VifPacketProducerTicketReservation& reservation,
                                              const std::size_t pass_ordinal) noexcept {
    return ticket.owner == owner && ticket.reservation_id == reservation.id &&
           ticket.reservation_pass_ordinal == pass_ordinal &&
           ticket.reservation_pass_count == reservation.pass_count;
}

[[nodiscard]] bool has_callback_identity(const UiTextVifTransportCallbackOccurrence& callback) noexcept {
    return callback.packet_session_epoch != 0U && callback.callback_ordinal != 0U &&
           callback.source_ordinal != 0U && callback.key.value != 0U;
}

[[nodiscard]] UiTextVifTransportCallbackIdentity
make_callback_identity(const UiTextVifTransportCallbackOccurrence& callback) noexcept {
    return {callback.packet_session_epoch,
            callback.callback_ordinal,
            callback.source_ordinal,
            callback.key,
            static_cast<std::uint32_t>(callback.bucket_index),
            static_cast<std::uint32_t>(callback.node_index),
            callback.effect};
}

[[nodiscard]] UiTextVifTransportPassIdentity
make_pass_identity(const UiTextVifTransportPassOccurrence& pass) noexcept {
    return {make_callback_identity(pass.callback), static_cast<std::uint32_t>(pass.line_slot),
            static_cast<std::uint32_t>(pass.pass_slot), pass.role};
}

[[nodiscard]] bool same_callback_slot(const UiTextVifTransportCallbackIdentity& lhs,
                                      const UiTextVifTransportCallbackIdentity& rhs) noexcept {
    return lhs.packet_session_epoch == rhs.packet_session_epoch &&
           lhs.callback_ordinal == rhs.callback_ordinal;
}

[[nodiscard]] bool claim_ids_available(const std::uint64_t next_id, const std::size_t count) noexcept {
    if (next_id == 0U) {
        return false;
    }
    const std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max() - next_id + 1U;
    return count <= remaining;
}

[[nodiscard]] bool reached_matches_marker(
    const guest::Vif1Path2SourceMarkerReached& reached,
    const guest::Vif1DmacPreSnapshotResolvedMarker& armed) noexcept {
    return reached.generation == armed.generation && reached.source == armed.request.source &&
           reached.chain_root == armed.request.chain_root &&
           reached.source_tag_location == armed.request.source_tag_location &&
           reached.source_tag_ordinal == armed.request.source_tag_ordinal &&
           reached.source_payload_word_ordinal == armed.request.source_payload_word_ordinal &&
           reached.quadword == armed.request.expected_quadword;
}

[[nodiscard]] bool cancelled_matches_marker(
    const guest::Vif1Path2SourceMarkerCancelled& cancelled,
    const guest::Vif1DmacPreSnapshotResolvedMarker& armed) noexcept {
    return cancelled.source == armed.request.source &&
           cancelled.chain_root == armed.request.chain_root &&
           cancelled.source_tag_location == armed.request.source_tag_location &&
           cancelled.source_tag_ordinal == armed.request.source_tag_ordinal &&
           cancelled.source_payload_word_ordinal == armed.request.source_payload_word_ordinal;
}

[[nodiscard]] std::size_t expected_pass_count(const UiTextEffectKind effect) noexcept {
    switch (effect) {
        case UiTextEffectKind::Plain:
            return 1U;
        case UiTextEffectKind::DropShadow:
            return 2U;
        case UiTextEffectKind::Outline:
            return 5U;
    }
    return 0U;
}

[[nodiscard]] bool positions_fit(const UiTextVifTransportPassOccurrence& pass) noexcept {
    constexpr std::size_t kMax = std::numeric_limits<std::uint32_t>::max();
    return pass.callback.bucket_index <= kMax && pass.callback.node_index <= kMax &&
           pass.line_slot <= kMax && pass.pass_slot <= kMax;
}

template <typename Pass>
[[nodiscard]] bool role_matches(const Pass& pass) noexcept {
    switch (pass.callback.effect) {
        case UiTextEffectKind::Plain:
            return pass.pass_slot == 0U && pass.role == UiTextVifTransportPassRole::PlainOriginal;
        case UiTextEffectKind::DropShadow:
            return (pass.pass_slot == 0U &&
                    pass.role == UiTextVifTransportPassRole::DropShadowOffset) ||
                   (pass.pass_slot == 1U &&
                    pass.role == UiTextVifTransportPassRole::DropShadowOriginal);
        case UiTextEffectKind::Outline:
            switch (pass.pass_slot) {
                case 0U:
                    return pass.role == UiTextVifTransportPassRole::OutlineAddOffset;
                case 1U:
                    return pass.role == UiTextVifTransportPassRole::OutlineSubtractOffset;
                case 2U:
                    return pass.role == UiTextVifTransportPassRole::OutlineAddNegatedXOffset;
                case 3U:
                    return pass.role == UiTextVifTransportPassRole::OutlineSubtractNegatedXOffset;
                case 4U:
                    return pass.role == UiTextVifTransportPassRole::OutlineOriginal;
                default:
                    return false;
            }
    }
    return false;
}

[[nodiscard]] bool valid_pass_order(
    const std::span<const UiTextVifTransportPassClaimRequest> requests) noexcept {
    if (requests.empty()) {
        return false;
    }
    if (!has_callback_identity(requests.front().pass.callback)) {
        return false;
    }
    const UiTextVifTransportCallbackOccurrence callback = requests.front().pass.callback;
    const std::size_t expected = expected_pass_count(callback.effect);
    if (expected == 0U || requests.front().pass.pass_slot != 0U) {
        return false;
    }
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        const auto& pass = requests[index].pass;
        if (pass.callback != callback || !positions_fit(pass) || !role_matches(pass)) {
            return false;
        }
        if (index == 0U) {
            continue;
        }
        const auto& previous = requests[index - 1U].pass;
        if (pass.line_slot == previous.line_slot) {
            if (pass.pass_slot != previous.pass_slot + 1U) {
                return false;
            }
            continue;
        }
        if (pass.line_slot <= previous.line_slot || previous.pass_slot + 1U != expected ||
            pass.pass_slot != 0U) {
            return false;
        }
    }
    return requests.back().pass.pass_slot + 1U == expected;
}

} // namespace

UiTextVifTransportClaimRegistry::~UiTextVifTransportClaimRegistry() {
    detach();
}

void UiTextVifTransportClaimRegistry::detach() noexcept {
    if (attached_memory_ != nullptr) {
        (void)attached_memory_->detach_vif1_path2_source_marker_observer(this);
        attached_memory_ = nullptr;
    }
    reset_attachment_claims();
}

void UiTextVifTransportClaimRegistry::attach(guest::EeMemory& memory) noexcept {
    if (attached_memory_ != nullptr) {
        (void)attached_memory_->detach_vif1_path2_source_marker_observer(this);
    }
    reset_attachment_claims();
    attached_memory_ = &memory;
    memory.set_vif1_path2_source_marker_observer(this);
}

UiTextVifTransportClaimRegistry::Slot*
UiTextVifTransportClaimRegistry::find_slot(const UiTextVifTransportClaimId id) noexcept {
    if (id.value == 0U) {
        return nullptr;
    }
    for (auto& claim : claims_) {
        if (claim.has_value() && claim->id == id) {
            return &*claim;
        }
    }
    return nullptr;
}

const UiTextVifTransportClaimRegistry::Slot*
UiTextVifTransportClaimRegistry::find_slot(const UiTextVifTransportClaimId id) const noexcept {
    if (id.value == 0U) {
        return nullptr;
    }
    for (const auto& claim : claims_) {
        if (claim.has_value() && claim->id == id) {
            return &*claim;
        }
    }
    return nullptr;
}

void UiTextVifTransportClaimRegistry::retire_slot(std::optional<Slot>& slot) noexcept {
    if (!slot.has_value()) {
        return;
    }
    slot.reset();
    --occupied_claims_;
}

void UiTextVifTransportClaimRegistry::reset_attachment_claims() noexcept {
    const std::size_t retired = occupied_claims_;
    const bool had_state = retired != 0U || ledger_.occupied_count() != 0U ||
                           offered_candidate_count_ != 0U;
    ledger_.clear();
    for (auto& claim : claims_) {
        claim.reset();
    }
    occupied_claims_ = 0U;
    offered_candidate_count_ = 0U;
    offered_observation_ = {};
    if (had_state) {
        saturating_increment(attachment_resets_);
        saturating_add(attachment_retired_claims_, retired);
    }
}

std::size_t UiTextVifTransportClaimRegistry::reconcile_with_ledger() noexcept {
    std::size_t retired = 0U;
    for (auto& claim : claims_) {
        if (!claim.has_value()) {
            continue;
        }
        if (claim->state == UiTextVifTransportClaimState::MarkerArmed) {
            continue;
        }
        bool present = false;
        if (claim->state == UiTextVifTransportClaimState::Published) {
            present = ledger_.find(claim->ticket_id).has_value();
        } else {
            present = ledger_.can_publish_reserved_pass(claim->ledger_reservation,
                                                        claim->pass_ordinal)
                          .status != VifPacketProducerTicketReservationPreflightStatus::StaleReservation;
        }
        if (present) {
            continue;
        }
        retire_slot(claim);
        ++retired;
    }
    saturating_add(reconciled_retired_claims_, retired);
    return retired;
}

bool UiTextVifTransportClaimRegistry::has_unarmed_claims() const noexcept {
    return ledger_.occupied_count() != 0U;
}

std::size_t UiTextVifTransportClaimRegistry::retire_unarmed_claims() noexcept {
    saturating_increment(fail_closed_retire_calls_);
    ledger_.clear();
    for (auto& candidate : offered_candidates_) {
        candidate = {};
    }
    offered_candidate_count_ = 0U;
    offered_observation_ = {};
    const std::size_t retired = reconcile_with_ledger();
    saturating_add(fail_closed_retired_claims_, retired);
    return retired;
}

UiTextVifTransportRelocationReport UiTextVifTransportClaimRegistry::report_copy(
    const guest::EeMemory& memory, const VifPacketQwordRange source,
    const VifPacketQwordRange destination) noexcept {
    const auto result = ledger_.relocate_after_copy(memory, source, destination);
    saturating_add(relocated_tickets_, result.relocated);
    const std::size_t retired = reconcile_with_ledger();
    return {result, retired};
}

UiTextVifTransportRelocationReport UiTextVifTransportClaimRegistry::report_ring_flush(
    const guest::EeMemory& memory, const VifPacketRingFlushResult& flush) noexcept {
    const auto result = ledger_.relocate_after_ring_flush(memory, flush);
    saturating_add(relocated_tickets_, result.relocated);
    const std::size_t retired = reconcile_with_ledger();
    return {result, retired};
}

UiTextVifTransportWriteReport
UiTextVifTransportClaimRegistry::report_write(const guest::GuestAddress begin,
                                              const std::size_t byte_count) noexcept {
    const auto result = ledger_.invalidate_written_range(begin, byte_count);
    const std::size_t retired = reconcile_with_ledger();
    return {result, retired};
}

bool UiTextVifTransportClaimRegistry::has_vif1_dmac_pre_snapshot_candidates() const noexcept {
    return ledger_.active_count() != 0U;
}

void UiTextVifTransportClaimRegistry::on_vif1_path2_source_marker_memory_cleared() noexcept {
    reset_attachment_claims();
}

UiTextVifTransportPreflightResult
UiTextVifTransportClaimRegistry::can_publish(const UiTextVifTransportClaimId id) const noexcept {
    const Slot* slot = find_slot(id);
    if (slot == nullptr) {
        return {UiTextVifTransportPreflightStatus::ClaimUnavailable,
                VifPacketProducerTicketReservationPreflightStatus::StaleReservation};
    }
    if (slot->state != UiTextVifTransportClaimState::Reserved) {
        return {UiTextVifTransportPreflightStatus::ClaimNotReserved,
                VifPacketProducerTicketReservationPreflightStatus::PassAlreadyPublished};
    }
    const auto ledger = ledger_.can_publish_reserved_pass(slot->ledger_reservation,
                                                          slot->pass_ordinal);
    return {ledger.ready() ? UiTextVifTransportPreflightStatus::Ready
                           : UiTextVifTransportPreflightStatus::LedgerRejected,
            ledger.status};
}

UiTextVifTransportPublishResult
UiTextVifTransportClaimRegistry::publish_reserved_after_stream_commit(
    const guest::EeMemory& memory, const UiTextVifTransportClaimId id,
    const StreamEnvironmentPacketGuestResult& result) noexcept {
    Slot* slot = find_slot(id);
    if (slot == nullptr) {
        return {UiTextVifTransportPublishStatus::ClaimUnavailable, id, {},
                VifPacketProducerTicketReservedPublishStatus::StaleReservation, 0U};
    }
    if (slot->state != UiTextVifTransportClaimState::Reserved) {
        return {UiTextVifTransportPublishStatus::ClaimNotReserved, id, slot->ticket_id,
                VifPacketProducerTicketReservedPublishStatus::PassAlreadyPublished, 0U};
    }
    const auto ledger_result = ledger_.publish_reserved_after_stream_commit(
        memory, slot->ledger_reservation, slot->pass_ordinal, result);
    if (!ledger_result.published()) {
        const std::size_t retired = reconcile_with_ledger();
        return {UiTextVifTransportPublishStatus::LedgerRejected, id, ledger_result.id,
                ledger_result.status, retired};
    }
    const auto ticket = ledger_.find(ledger_result.id);
    if (!ticket.has_value() || ticket->id != ledger_result.id ||
        !ticket_matches_reservation(*ticket, slot->packet.owner, slot->ledger_reservation,
                                    slot->pass_ordinal) ||
        ticket->anchor != slot->packet.anchor) {
        ledger_.clear();
        const std::size_t retired = reconcile_with_ledger();
        saturating_increment(invariant_failures_);
        return {UiTextVifTransportPublishStatus::LedgerInvariantViolation, id, ledger_result.id,
                ledger_result.status, retired};
    }
    slot->state = UiTextVifTransportClaimState::Published;
    slot->ticket_id = ledger_result.id;
    saturating_increment(published_tickets_);
    const std::size_t retired = reconcile_with_ledger();
    return {UiTextVifTransportPublishStatus::Published, id, ledger_result.id,
            ledger_result.status, retired};
}

UiTextVifTransportCancelResult UiTextVifTransportClaimRegistry::cancel_callback(
    const UiTextVifTransportCallbackReservation reservation) noexcept {
    Slot* selected = nullptr;
    bool marker_owns_claim = false;
    for (auto& claim : claims_) {
        if (!claim.has_value() || claim->reservation != reservation) {
            continue;
        }
        if (selected == nullptr) {
            selected = &*claim;
        }
        marker_owns_claim =
            marker_owns_claim || claim->state == UiTextVifTransportClaimState::MarkerArmed;
    }
    if (selected == nullptr) {
        return {UiTextVifTransportCancelStatus::ReservationUnavailable, {}, 0U};
    }
    if (marker_owns_claim) {
        return {UiTextVifTransportCancelStatus::MarkerOwnsClaim, {}, 0U};
    }
    const auto ledger_result = ledger_.cancel_reservation(selected->ledger_reservation);
    if (!ledger_result.cancelled) {
        saturating_increment(invariant_failures_);
        const std::size_t retired = reconcile_with_ledger();
        return {UiTextVifTransportCancelStatus::LedgerInvariantViolation, ledger_result, retired};
    }
    std::size_t retired = 0U;
    for (auto& claim : claims_) {
        if (claim.has_value() && claim->reservation == reservation) {
            retire_slot(claim);
            ++retired;
        }
    }
    saturating_increment(cancelled_callbacks_);
    return {UiTextVifTransportCancelStatus::Cancelled, ledger_result, retired};
}

UiTextVifTransportReleaseResult
UiTextVifTransportClaimRegistry::release_completed_claim(const UiTextVifTransportClaimId id) noexcept {
    Slot* slot = find_slot(id);
    if (slot == nullptr) {
        return {.status = UiTextVifTransportReleaseStatus::ClaimUnavailable,
                .claim_id = id,
                .ledger_status = VifPacketProducerTicketTakeStatus::TicketUnavailable};
    }
    if (slot->state != UiTextVifTransportClaimState::Published) {
        if (slot->state == UiTextVifTransportClaimState::MarkerArmed) {
            return {.status = UiTextVifTransportReleaseStatus::MarkerOwnsClaim,
                    .claim_id = id,
                    .ledger_status = VifPacketProducerTicketTakeStatus::TicketUnavailable};
        }
        return {.status = UiTextVifTransportReleaseStatus::ClaimNotPublished,
                .claim_id = id,
                .ledger_status = VifPacketProducerTicketTakeStatus::TicketUnavailable};
    }
    const auto current = ledger_.find(slot->ticket_id);
    const auto taken = ledger_.take_completed(slot->ticket_id);
    if (!taken.taken()) {
        if (taken.status == VifPacketProducerTicketTakeStatus::InvalidId ||
            taken.status == VifPacketProducerTicketTakeStatus::TicketUnavailable) {
            (void)reconcile_with_ledger();
            saturating_increment(invariant_failures_);
            return {.status = UiTextVifTransportReleaseStatus::LedgerInvariantViolation,
                    .claim_id = id,
                    .ledger_status = taken.status};
        }
        return {.status = UiTextVifTransportReleaseStatus::LedgerRejected,
                .claim_id = id,
                .ledger_status = taken.status};
    }
    const VifPacketProducerTicket& ticket = *taken.ticket;
    if (!current.has_value() || *current != ticket || ticket.id != slot->ticket_id ||
        !ticket_matches_reservation(ticket, slot->packet.owner, slot->ledger_reservation,
                                    slot->pass_ordinal)) {
        ledger_.clear();
        (void)reconcile_with_ledger();
        saturating_increment(invariant_failures_);
        return {.status = UiTextVifTransportReleaseStatus::LedgerInvariantViolation,
                .claim_id = id,
                .ledger_status = taken.status};
    }
    UiTextVifTransportReleasedClaim released{slot->id, slot->reservation, slot->pass,
                                                   ticket};
    for (auto& claim : claims_) {
        if (claim.has_value() && claim->id == id) {
            retire_slot(claim);
            break;
        }
    }
    saturating_increment(released_claims_);
    return {.status = UiTextVifTransportReleaseStatus::Released,
            .claim_id = id,
            .ledger_status = taken.status,
            .released_claim = released};
}

std::optional<UiTextVifTransportClaim>
UiTextVifTransportClaimRegistry::find(const UiTextVifTransportClaimId id) const noexcept {
    const Slot* slot = find_slot(id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    std::optional<VifPacketProducerTicket> ticket;
    std::optional<guest::Vif1DmacPreSnapshotResolvedMarker> marker;
    if (slot->state == UiTextVifTransportClaimState::Published) {
        ticket = ledger_.find(slot->ticket_id);
    } else if (slot->state == UiTextVifTransportClaimState::MarkerArmed) {
        ticket = slot->held_ticket;
        marker = slot->marker;
    }
    return UiTextVifTransportClaim{slot->id,         slot->reservation, slot->pass,
                                   slot->packet.owner, slot->state,     ticket,
                                   marker};
}

UiTextVifTransportReserveResult UiTextVifTransportClaimRegistry::try_reserve(
    const std::span<const UiTextVifTransportPassClaimRequest> requests,
    const std::span<UiTextVifTransportClaimId> claim_ids) noexcept {
    UiTextVifTransportReserveResult result{.requested_count = requests.size(),
                                           .available_count = kCapacity - occupied_claims_};
    if (requests.empty()) {
        return result;
    }
    if (claim_ids.size() < requests.size()) {
        result.status = UiTextVifTransportReserveStatus::OutputTooSmall;
        return result;
    }
    if (!has_callback_identity(requests.front().pass.callback)) {
        result.status = UiTextVifTransportReserveStatus::InvalidCallbackIdentity;
        return result;
    }
    for (const auto& request : requests) {
        if (request.pass.callback != requests.front().pass.callback) {
            result.status = UiTextVifTransportReserveStatus::CallbackIdentityMismatch;
            return result;
        }
    }
    for (const auto& request : requests) {
        if (!positions_fit(request.pass)) {
            result.status = UiTextVifTransportReserveStatus::PositionOverflow;
            return result;
        }
    }
    const UiTextVifTransportCallbackIdentity callback_identity =
        make_callback_identity(requests.front().pass.callback);
    for (const auto& claim : claims_) {
        if (!claim.has_value() || !same_callback_slot(callback_identity, claim->pass.callback)) {
            continue;
        }
        result.status = claim->pass.callback == callback_identity
                            ? UiTextVifTransportReserveStatus::CallbackAlreadyReserved
                            : UiTextVifTransportReserveStatus::CallbackIdentityMismatch;
        return result;
    }
    if (!valid_pass_order(requests)) {
        result.status = UiTextVifTransportReserveStatus::InvalidPassOrder;
        return result;
    }
    if (result.available_count < requests.size()) {
        result.status = UiTextVifTransportReserveStatus::CapacityExhausted;
        return result;
    }
    if (!claim_ids_available(next_claim_id_, requests.size())) {
        result.status = UiTextVifTransportReserveStatus::ClaimGenerationExhausted;
        return result;
    }
    if (next_reservation_id_ == 0U) {
        result.status = UiTextVifTransportReserveStatus::ReservationGenerationExhausted;
        return result;
    }

    std::array<VifPacketProducerTicketReservationRequest, kCapacity> packet_requests{};
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        packet_requests[index] = requests[index].packet;
    }
    const auto ledger_result = ledger_.try_reserve_stream_passes(
        std::span<const VifPacketProducerTicketReservationRequest>{packet_requests.data(),
                                                                   requests.size()});
    result.ledger_status = ledger_result.status;
    if (!ledger_result.reserved()) {
        result.status = UiTextVifTransportReserveStatus::LedgerRejected;
        return result;
    }

    const UiTextVifTransportCallbackReservation reservation{
        next_reservation_id_, static_cast<std::uint32_t>(requests.size())};
    std::array<UiTextVifTransportClaimId, kCapacity> issued_ids{};
    std::size_t request_index = 0U;
    for (auto& claim : claims_) {
        if (claim.has_value()) {
            continue;
        }
        claim.emplace(Slot{
            UiTextVifTransportClaimId{next_claim_id_ + request_index},
            reservation,
            ledger_result.reservation,
            static_cast<std::uint32_t>(request_index),
            make_pass_identity(requests[request_index].pass),
            requests[request_index].packet});
        issued_ids[request_index] = UiTextVifTransportClaimId{next_claim_id_ + request_index};
        ++request_index;
        if (request_index == requests.size()) {
            break;
        }
    }
    if (request_index != requests.size()) {
        (void)ledger_.cancel_reservation(ledger_result.reservation);
        for (auto& claim : claims_) {
            if (claim.has_value() && claim->reservation == reservation) {
                claim.reset();
            }
        }
        saturating_increment(invariant_failures_);
        result.status = UiTextVifTransportReserveStatus::CapacityExhausted;
        return result;
    }

    next_claim_id_ += requests.size();
    next_reservation_id_ = next_reservation_id_ == std::numeric_limits<std::uint64_t>::max()
                               ? 0U
                               : next_reservation_id_ + 1U;
    occupied_claims_ += requests.size();
    saturating_increment(admitted_callbacks_);
    saturating_add(admitted_claims_, requests.size());
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        claim_ids[index] = issued_ids[index];
    }
    result.status = UiTextVifTransportReserveStatus::Reserved;
    result.reservation = reservation;
    return result;
}

UiTextVifTransportDiagnostics UiTextVifTransportClaimRegistry::diagnostics() const noexcept {
    UiTextVifTransportDiagnostics result{};
    result.occupied_claims = occupied_claims_;
    result.ledger_active_tickets = ledger_.active_count();
    result.ledger_reserved_tickets = ledger_.reserved_count();
    result.admitted_callbacks = admitted_callbacks_;
    result.admitted_claims = admitted_claims_;
    result.published_tickets = published_tickets_;
    result.relocated_tickets = relocated_tickets_;
    result.reconciled_retired_claims = reconciled_retired_claims_;
    result.fail_closed_retire_calls = fail_closed_retire_calls_;
    result.fail_closed_retired_claims = fail_closed_retired_claims_;
    result.cancelled_callbacks = cancelled_callbacks_;
    result.released_claims = released_claims_;
    result.pre_snapshot_candidate_callbacks = pre_snapshot_candidate_callbacks_;
    result.pre_snapshot_candidates_offered = pre_snapshot_candidates_offered_;
    result.pre_snapshot_resolved_callbacks = pre_snapshot_resolved_callbacks_;
    result.marker_armed_claims_total = marker_armed_claims_total_;
    result.incomplete_marker_groups = incomplete_marker_groups_;
    result.marker_reached_claims = marker_reached_claims_;
    result.marker_cancelled_claims = marker_cancelled_claims_;
    result.terminal_provenance_failures = terminal_provenance_failures_;
    result.attachment_resets = attachment_resets_;
    result.attachment_retired_claims = attachment_retired_claims_;
    result.invariant_failures = invariant_failures_;
    for (const auto& claim : claims_) {
        if (!claim.has_value()) {
            continue;
        }
        switch (claim->state) {
            case UiTextVifTransportClaimState::Reserved:
                ++result.reserved_claims;
                break;
            case UiTextVifTransportClaimState::Published:
                ++result.published_claims;
                break;
            case UiTextVifTransportClaimState::MarkerArmed:
                ++result.marker_armed_claims;
                break;
        }
    }
    return result;
}

std::size_t UiTextVifTransportClaimRegistry::on_vif1_dmac_pre_snapshot_candidates(
    const guest::Vif1DmacPreSnapshotObservation& observation,
    const std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept {
    offered_candidate_count_ = 0U;
    offered_observation_ = observation;
    saturating_increment(pre_snapshot_candidate_callbacks_);
    if (candidates.empty() || ledger_.active_count() == 0U) {
        return 0U;
    }
    (void)reconcile_with_ledger();
    const std::size_t capacity = std::min(candidates.size(), kCapacity);
    std::array<const Slot*, kCapacity> group{};
    for (const auto& first : claims_) {
        if (!first.has_value() || first->state != UiTextVifTransportClaimState::Published ||
            first->pass_ordinal != 0U) {
            continue;
        }
        const auto pass_count = static_cast<std::size_t>(first->reservation.pass_count);
        if (pass_count == 0U || pass_count > kCapacity ||
            capacity - offered_candidate_count_ < pass_count) {
            continue;
        }
        bool complete = first->ledger_reservation.pass_count == pass_count;
        for (std::size_t pass_ordinal = 0U; pass_ordinal < pass_count && complete;
             ++pass_ordinal) {
            const Slot* selected = nullptr;
            for (const auto& other : claims_) {
                if (!other.has_value() || other->reservation != first->reservation ||
                    other->pass_ordinal != pass_ordinal) {
                    continue;
                }
                if (selected != nullptr) {
                    complete = false;
                    break;
                }
                selected = &*other;
            }
            if (!complete || selected == nullptr ||
                selected->state != UiTextVifTransportClaimState::Published ||
                selected->ledger_reservation != first->ledger_reservation ||
                selected->pass.callback != first->pass.callback || !role_matches(selected->pass)) {
                complete = false;
                break;
            }
            const auto ticket = ledger_.find(selected->ticket_id);
            if (!ticket.has_value() || ticket->id != selected->ticket_id ||
                !ticket_matches_reservation(*ticket, selected->packet.owner,
                                            selected->ledger_reservation, pass_ordinal)) {
                complete = false;
                break;
            }
            group[pass_ordinal] = selected;
        }
        if (!complete) {
            continue;
        }
        const std::size_t expected = expected_pass_count(first->pass.callback.effect);
        complete = expected != 0U && group[0]->pass.pass_slot == 0U;
        for (std::size_t pass_ordinal = 1U; pass_ordinal < pass_count && complete;
             ++pass_ordinal) {
            const auto& previous = group[pass_ordinal - 1U]->pass;
            const auto& current = group[pass_ordinal]->pass;
            if (current.line_slot == previous.line_slot) {
                complete = current.pass_slot == previous.pass_slot + 1U;
            } else if (current.line_slot > previous.line_slot) {
                complete = previous.pass_slot + 1U == expected && current.pass_slot == 0U;
            } else {
                complete = false;
            }
        }
        complete = complete && group[pass_count - 1U]->pass.pass_slot + 1U == expected;
        if (!complete) {
            saturating_increment(invariant_failures_);
            continue;
        }
        const auto group_offer_begin = offered_candidate_count_;
        for (std::size_t pass_ordinal = 0U; pass_ordinal < pass_count; ++pass_ordinal) {
            const Slot& claim = *group[pass_ordinal];
            const auto ticket = ledger_.find(claim.ticket_id);
            if (!ticket.has_value()) {
                complete = false;
                break;
            }
            offered_candidates_[offered_candidate_count_] =
                OfferedCandidate{claim.id, claim.reservation, claim.pass_ordinal, *ticket};
            candidates[offered_candidate_count_] = guest::Vif1DmacPreSnapshotCandidate{
                claim.id.value, ticket->anchor.first_element.begin,
                ticket->anchor.expected_first_element};
            ++offered_candidate_count_;
        }
        if (!complete) {
            offered_candidate_count_ = group_offer_begin;
            saturating_increment(invariant_failures_);
        }
    }
    saturating_add(pre_snapshot_candidates_offered_, offered_candidate_count_);
    return offered_candidate_count_;
}

void UiTextVifTransportClaimRegistry::on_vif1_path2_source_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    for (auto& claim : claims_) {
        if (!claim.has_value() || claim->state != UiTextVifTransportClaimState::MarkerArmed ||
            claim->marker.generation != marker.generation) {
            continue;
        }
        if (claim->marker.token == claim->id.value && claim->marker.generation != 0U &&
            claim->held_ticket.id == claim->ticket_id &&
            ticket_matches_reservation(claim->held_ticket, claim->packet.owner,
                                       claim->ledger_reservation, claim->pass_ordinal) &&
            claim->held_ticket.anchor.expected_first_element ==
                claim->marker.request.expected_quadword &&
            claim->held_ticket.anchor.expected_first_element == marker.quadword &&
            reached_matches_marker(marker, claim->marker)) {
            saturating_increment(marker_reached_claims_);
        } else {
            saturating_increment(terminal_provenance_failures_);
            saturating_increment(invariant_failures_);
        }
        retire_slot(claim);
        return;
    }
}

void UiTextVifTransportClaimRegistry::on_vif1_path2_source_marker_cancelled(
    const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    for (auto& claim : claims_) {
        if (!claim.has_value() || claim->state != UiTextVifTransportClaimState::MarkerArmed ||
            claim->marker.generation != marker.generation) {
            continue;
        }
        if (claim->marker.token == claim->id.value && claim->marker.generation != 0U &&
            claim->held_ticket.id == claim->ticket_id &&
            ticket_matches_reservation(claim->held_ticket, claim->packet.owner,
                                       claim->ledger_reservation, claim->pass_ordinal) &&
            claim->held_ticket.anchor.expected_first_element ==
                claim->marker.request.expected_quadword &&
            cancelled_matches_marker(marker, claim->marker)) {
            saturating_increment(marker_cancelled_claims_);
        } else {
            saturating_increment(terminal_provenance_failures_);
            saturating_increment(invariant_failures_);
        }
        retire_slot(claim);
        return;
    }
}

void UiTextVifTransportClaimRegistry::on_vif1_dmac_pre_snapshot_markers_resolved(
    const std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
    const std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept {
    for (auto& decision : decisions) {
        decision = guest::Vif1DmacPreSnapshotMarkerDecision::Reject;
    }
    saturating_increment(pre_snapshot_resolved_callbacks_);
    if (decisions.size() != markers.size() || markers.size() > kCapacity ||
        offered_candidate_count_ == 0U) {
        if (!markers.empty()) {
            saturating_increment(invariant_failures_);
        }
        offered_candidate_count_ = 0U;
        offered_observation_ = {};
        return;
    }

    constexpr std::size_t kNoDecision = std::numeric_limits<std::size_t>::max();
    std::array<const guest::Vif1DmacPreSnapshotResolvedMarker*, kCapacity> resolved_by_offer{};
    std::array<std::size_t, kCapacity> decision_by_offer{};
    decision_by_offer.fill(kNoDecision);

    for (std::size_t marker_index = 0U; markers.size() > marker_index; ++marker_index) {
        const auto& marker = markers[marker_index];
        bool duplicate_identity = false;
        for (std::size_t other_index = 0U; markers.size() > other_index; ++other_index) {
            if (other_index == marker_index) {
                continue;
            }
            duplicate_identity = duplicate_identity || markers[other_index].token == marker.token ||
                                 (marker.generation != 0U &&
                                  markers[other_index].generation == marker.generation);
        }
        std::size_t offered_index = offered_candidate_count_;
        for (std::size_t candidate_index = 0U; candidate_index < offered_candidate_count_;
             ++candidate_index) {
            if (offered_candidates_[candidate_index].id.value == marker.token) {
                offered_index = candidate_index;
                break;
            }
        }
        if (duplicate_identity || offered_index == offered_candidate_count_ ||
            marker.generation == 0U) {
            saturating_increment(invariant_failures_);
            continue;
        }
        const auto& offered = offered_candidates_[offered_index];
        Slot* claim = find_slot(offered.id);
        const auto current = claim == nullptr ? std::optional<VifPacketProducerTicket>{}
                                              : ledger_.find(claim->ticket_id);
        const bool matches =
            marker.request.chain_root == offered_observation_.chain_root &&
            marker.candidate_source == offered.ticket.anchor.first_element.begin &&
            marker.request.expected_quadword == offered.ticket.anchor.expected_first_element &&
            claim != nullptr && claim->state == UiTextVifTransportClaimState::Published &&
            claim->id == offered.id && claim->reservation == offered.reservation &&
            claim->pass_ordinal == offered.pass_ordinal && current.has_value() &&
            *current == offered.ticket &&
            ticket_matches_reservation(*current, claim->packet.owner, claim->ledger_reservation,
                                       claim->pass_ordinal);
        if (!matches) {
            saturating_increment(invariant_failures_);
            continue;
        }
        resolved_by_offer[offered_index] = &marker;
        decision_by_offer[offered_index] = marker_index;
    }

    std::array<Slot*, kCapacity> group_slots{};
    std::array<VifPacketProducerTicket, kCapacity> taken_tickets{};
    std::size_t group_begin = 0U;
    while (group_begin < offered_candidate_count_) {
        const UiTextVifTransportCallbackReservation reservation =
            offered_candidates_[group_begin].reservation;
        const auto pass_count = static_cast<std::size_t>(reservation.pass_count);
        bool complete = pass_count != 0U && offered_candidate_count_ - group_begin >= pass_count;
        for (std::size_t offset = 0U; offset < pass_count && complete; ++offset) {
            const auto offered_index = group_begin + offset;
            const auto& offered = offered_candidates_[offered_index];
            Slot* claim = find_slot(offered.id);
            const auto current = claim == nullptr ? std::optional<VifPacketProducerTicket>{}
                                                  : ledger_.find(claim->ticket_id);
            complete = offered.reservation == reservation && offered.pass_ordinal == offset &&
                       resolved_by_offer[offered_index] != nullptr &&
                       decision_by_offer[offered_index] != kNoDecision && claim != nullptr &&
                       claim->state == UiTextVifTransportClaimState::Published &&
                       claim->reservation == reservation && claim->pass_ordinal == offset &&
                       current.has_value() && *current == offered.ticket &&
                       ticket_matches_reservation(*current, claim->packet.owner,
                                                  claim->ledger_reservation, offset);
            group_slots[offset] = claim;
        }
        for (std::size_t offset = 1U; offset < pass_count && complete; ++offset) {
            const auto& previous = resolved_by_offer[group_begin + offset - 1U]->request;
            const auto& current = resolved_by_offer[group_begin + offset]->request;
            complete = current.source_tag_ordinal > previous.source_tag_ordinal ||
                       (current.source_tag_ordinal == previous.source_tag_ordinal &&
                        current.source_payload_word_ordinal > previous.source_payload_word_ordinal);
        }
        if (!complete) {
            saturating_increment(incomplete_marker_groups_);
            group_begin += pass_count != 0U ? pass_count : 1U;
            continue;
        }
        std::size_t taken_count = 0U;
        for (; taken_count < pass_count; ++taken_count) {
            const auto taken =
                ledger_.take_completed(offered_candidates_[group_begin + taken_count].ticket.id);
            if (!taken.taken() ||
                !(*taken.ticket == offered_candidates_[group_begin + taken_count].ticket)) {
                break;
            }
            taken_tickets[taken_count] = *taken.ticket;
        }
        if (taken_count != pass_count) {
            for (std::size_t offset = taken_count; offset < pass_count; ++offset) {
                (void)ledger_.cancel(offered_candidates_[group_begin + offset].ticket.id);
            }
            for (auto& claim : claims_) {
                if (claim.has_value() && claim->reservation == reservation) {
                    retire_slot(claim);
                }
            }
            saturating_increment(invariant_failures_);
            group_begin += pass_count;
            continue;
        }
        for (std::size_t offset = 0U; offset < pass_count; ++offset) {
            const auto offered_index = group_begin + offset;
            Slot& claim = *group_slots[offset];
            claim.state = UiTextVifTransportClaimState::MarkerArmed;
            claim.held_ticket = taken_tickets[offset];
            claim.marker = *resolved_by_offer[offered_index];
            decisions[decision_by_offer[offered_index]] =
                guest::Vif1DmacPreSnapshotMarkerDecision::Accept;
        }
        saturating_add(marker_armed_claims_total_, pass_count);
        group_begin += pass_count;
    }
    offered_candidate_count_ = 0U;
    offered_observation_ = {};
}

} // namespace takedown::runtime::slus_21050
