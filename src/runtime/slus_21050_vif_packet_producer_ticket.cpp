#include "takedown/runtime/slus_21050_vif_packet_producer_ticket.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace takedown::runtime::slus_21050 {
namespace {

constexpr unsigned long long kAddressSpaceBytes = 1ULL << 32U;

[[nodiscard]] bool valid_range(const VifPacketQwordRange range) {
    if (range.quadword_count == 0U || (range.begin & 15U) != 0U) {
        return false;
    }
    const unsigned long long byte_count = static_cast<unsigned long long>(range.quadword_count) * 16ULL;
    return byte_count <= kAddressSpaceBytes - static_cast<unsigned long long>(range.begin);
}

[[nodiscard]] unsigned long long range_end(const VifPacketQwordRange range) {
    return static_cast<unsigned long long>(range.begin) +
           static_cast<unsigned long long>(range.quadword_count) * 16ULL;
}

[[nodiscard]] bool contains_range(const VifPacketQwordRange outer, const VifPacketQwordRange inner) {
    return valid_range(outer) && valid_range(inner) && outer.begin <= inner.begin &&
           range_end(inner) <= range_end(outer);
}

[[nodiscard]] bool ranges_overlap(const VifPacketQwordRange first, const VifPacketQwordRange second) {
    return valid_range(first) && valid_range(second) && first.begin < range_end(second) &&
           second.begin < range_end(first);
}

[[nodiscard]] bool valid_anchor_shape(const StreamEnvironmentProducerAnchor& anchor) {
    return valid_range(anchor.element_output) && anchor.first_element.quadword_count == 1U &&
           contains_range(anchor.element_output, anchor.first_element) &&
           anchor.element_output.begin == anchor.first_element.begin;
}

[[nodiscard]] bool valid_packet_outputs(const VifPacketQwordRange packet_output,
                                        const VifPacketQwordRange header_output) {
    return valid_range(packet_output) && header_output.quadword_count == 1U &&
           valid_range(header_output) &&
           (contains_range(packet_output, header_output) ||
            !ranges_overlap(packet_output, header_output));
}

[[nodiscard]] bool valid_reservation_pass(const StreamEnvironmentProducerAnchor& anchor,
                                          const VifPacketQwordRange packet_output,
                                          const VifPacketQwordRange header_output) {
    return valid_anchor_shape(anchor) && valid_packet_outputs(packet_output, header_output) &&
           contains_range(packet_output, anchor.element_output) &&
           !ranges_overlap(anchor.element_output, header_output);
}

[[nodiscard]] std::optional<guest::GsQuadword> read_quadword(const guest::EeMemory& memory,
                                                             const VifPacketQwordRange range) {
    if (range.quadword_count != 1U || !valid_range(range) || !memory.contains(range.begin, 16U)) {
        return std::nullopt;
    }
    return guest::GsQuadword{memory.read_u32_le(range.begin), memory.read_u32_le(range.begin + 4U),
                             memory.read_u32_le(range.begin + 8U),
                             memory.read_u32_le(range.begin + 12U)};
}

template <typename Plan>
[[nodiscard]] std::optional<StreamEnvironmentProducerAnchor>
make_anchor(const Plan& plan) noexcept {
    if (plan.encoded_elements.empty() || !valid_range(plan.element_output) ||
        plan.encoded_elements.size() > std::numeric_limits<std::uint32_t>::max() / 2U ||
        plan.element_output.quadword_count != plan.encoded_elements.size() * 2U) {
        return std::nullopt;
    }
    const auto words = plan.encoded_elements.front().words();
    return StreamEnvironmentProducerAnchor{
        plan.element_output,
        {plan.element_output.begin, 1U},
        {words[0], words[1], words[2], words[3]}};
}

[[nodiscard]] bool valid_request(const VifPacketProducerTicketReservationRequest& request) {
    return request.owner.opaque_value != 0U &&
           valid_reservation_pass(request.anchor, request.packet_output, request.header_output) &&
           request.capacity_mode == StreamEnvironmentPacketCapacityMode::ExistingRoom &&
           !request.has_flush_plan;
}

template <typename Plan>
[[nodiscard]] std::optional<VifPacketProducerTicketReservationPass>
make_reservation_pass(const Plan& plan) noexcept {
    if (plan.capacity_mode != StreamEnvironmentPacketCapacityMode::ExistingRoom ||
        plan.flush_plan.has_value()) {
        return std::nullopt;
    }
    const auto anchor = make_anchor(plan);
    if (!anchor.has_value()) {
        return std::nullopt;
    }
    const VifPacketProducerTicketReservationPass pass{*anchor, plan.packet_output,
                                                      plan.header_output};
    if (!valid_reservation_pass(pass.anchor, pass.packet_output, pass.header_output)) {
        return std::nullopt;
    }
    return pass;
}

[[nodiscard]] bool footprints_overlap(const VifPacketQwordRange left_packet_output,
                                       const VifPacketQwordRange left_header_output,
                                       const VifPacketQwordRange right_packet_output,
                                       const VifPacketQwordRange right_header_output) {
    return ranges_overlap(left_packet_output, right_packet_output) ||
           ranges_overlap(left_packet_output, right_header_output) ||
           ranges_overlap(left_header_output, right_packet_output) ||
           ranges_overlap(left_header_output, right_header_output);
}

// A packet may touch another pass's header only when both passes name that
// same header and this packet is the one that created it.
[[nodiscard]] bool packet_allows_other_header(const VifPacketQwordRange packet_output,
                                              const VifPacketQwordRange own_header_output,
                                              const VifPacketQwordRange other_header_output) {
    return !ranges_overlap(packet_output, other_header_output) ||
           (contains_range(packet_output, own_header_output) &&
            own_header_output == other_header_output);
}

[[nodiscard]] bool compatible_header_use(const VifPacketProducerTicketReservationRequest& left,
                                         const VifPacketProducerTicketReservationRequest& right) {
    if (ranges_overlap(left.packet_output, right.packet_output)) {
        return false;
    }
    if (ranges_overlap(left.header_output, right.header_output) &&
        !(left.header_output == right.header_output)) {
        return false;
    }
    const bool left_creates_header = contains_range(left.packet_output, left.header_output);
    const bool right_creates_header = contains_range(right.packet_output, right.header_output);
    if (left.header_output == right.header_output && right_creates_header &&
        !left_creates_header) {
        return false;
    }
    return packet_allows_other_header(left.packet_output, left.header_output,
                                      right.header_output) &&
           packet_allows_other_header(right.packet_output, right.header_output,
                                      left.header_output);
}

// Zero is the "no ID" sentinel and IDs are never recycled, so a ledger that
// would wrap past the top of the range must refuse the group.
[[nodiscard]] bool can_allocate_ids(const std::uint64_t next_id, const std::size_t count) {
    if (next_id == 0U) {
        return false;
    }
    const unsigned long long remaining = 0U - next_id;
    return count <= remaining;
}

[[nodiscard]] bool result_covers_element_output(const StreamEnvironmentPacketGuestResult& result,
                                                const StreamEnvironmentProducerAnchor& anchor) {
    return result.element_output == anchor.element_output && result.element_count != 0U &&
           static_cast<unsigned long long>(result.element_count) * 2U ==
               anchor.element_output.quadword_count &&
           contains_range(result.packet_output, anchor.element_output);
}

[[nodiscard]] bool commit_matches_reserved_pass(const StreamEnvironmentPacketGuestResult& result,
                                                const StreamEnvironmentProducerAnchor& anchor,
                                                const VifPacketQwordRange packet_output,
                                                const VifPacketQwordRange header_output) {
    return result.capacity_mode == StreamEnvironmentPacketCapacityMode::ExistingRoom &&
           !result.flush_result.has_value() && result.packet_output == packet_output &&
           result.header_output == header_output && result_covers_element_output(result, anchor);
}

[[nodiscard]] VifPacketProducerTicketReservedPublishStatus
published_status_from_preflight(const VifPacketProducerTicketReservationPreflightStatus status) {
    return status != VifPacketProducerTicketReservationPreflightStatus::StaleReservation
               ? (status == VifPacketProducerTicketReservationPreflightStatus::PassOutOfOrder
                      ? VifPacketProducerTicketReservedPublishStatus::PassOutOfOrder
                      : VifPacketProducerTicketReservedPublishStatus::PassAlreadyPublished)
               : VifPacketProducerTicketReservedPublishStatus::StaleReservation;
}

[[nodiscard]] bool overlaps_write(const VifPacketQwordRange range, const guest::GuestAddress begin,
                                  const std::size_t byte_count) {
    if (!valid_range(range)) {
        return false;
    }
    const unsigned long long write_end = static_cast<unsigned long long>(begin) + byte_count;
    return range.begin < write_end && begin < range_end(range);
}

[[nodiscard]] bool is_exempt(const VifPacketProducerTicketReservation* const reservation_exemption,
                             const VifPacketProducerTicketReservationId id,
                             const std::size_t pass_count) {
    return reservation_exemption != nullptr && reservation_exemption->id == id &&
           reservation_exemption->pass_count == pass_count;
}

[[nodiscard]] VifPacketQwordRange relocate_range(const VifPacketQwordRange range,
                                                 const VifPacketQwordRange source,
                                                 const VifPacketQwordRange destination) {
    return {range.begin - source.begin + destination.begin, range.quadword_count};
}

} // namespace

std::optional<StreamEnvironmentProducerAnchor>
make_stream_environment_producer_anchor(const StreamEnvironmentPacketGuestPlan& plan) noexcept {
    return make_anchor(plan);
}

std::optional<StreamEnvironmentProducerAnchor>
make_stream_environment_producer_anchor(
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept {
    return make_anchor(plan);
}

std::optional<VifPacketProducerTicketReservationPass>
make_stream_environment_packet_ticket_reservation_pass(
    const StreamEnvironmentPacketGuestPlan& plan) noexcept {
    return make_reservation_pass(plan);
}

std::optional<VifPacketProducerTicketReservationPass>
make_stream_environment_packet_ticket_reservation_pass(
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept {
    return make_reservation_pass(plan);
}

std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    const VifPacketProducerTicketOwner owner,
    const VifPacketProducerTicketReservationPass& pass) noexcept {
    if (owner.opaque_value == 0U ||
        !valid_reservation_pass(pass.anchor, pass.packet_output, pass.header_output)) {
        return std::nullopt;
    }
    const VifPacketProducerTicketReservationRequest request{owner, pass.anchor, pass.packet_output,
                                                            pass.header_output};
    if (!valid_request(request)) {
        return std::nullopt;
    }
    return request;
}

std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    const VifPacketProducerTicketOwner owner, const StreamEnvironmentPacketGuestPlan& plan) noexcept {
    const auto pass = make_stream_environment_packet_ticket_reservation_pass(plan);
    if (pass.has_value()) {
        return make_stream_environment_packet_ticket_reservation_request(owner, *pass);
    }
    return std::nullopt;
}

std::optional<VifPacketProducerTicketReservationRequest>
make_stream_environment_packet_ticket_reservation_request(
    const VifPacketProducerTicketOwner owner,
    const detail::StreamEnvironmentPacketProjectedPlan& plan) noexcept {
    const auto pass = make_stream_environment_packet_ticket_reservation_pass(plan);
    if (pass.has_value()) {
        return make_stream_environment_packet_ticket_reservation_request(owner, *pass);
    }
    return std::nullopt;
}

// RECOVERY: identical to the original GIMPLE except one operand order: the packet-output
// range check (InvalidPacketOutput) prints `limit - begin >= byte_count` in the original
// but `byte_count <= limit - begin` here; the dumps are otherwise the same.
VifPacketProducerTicketReservationResult VifPacketProducerTicketLedger::try_reserve_stream_passes(
    const std::span<const VifPacketProducerTicketReservationRequest> requests) noexcept {
    VifPacketProducerTicketReservationResult result{
        .status = VifPacketProducerTicketReservationStatus::EmptyRequest,
        .requested_count = requests.size(),
        .available_count = available_count()};
    if (requests.empty()) {
        return result;
    }
    if (requests.size() > kCapacity) {
        result.status = VifPacketProducerTicketReservationStatus::CapacityExhausted;
        return result;
    }

    for (std::size_t index = 0U; index < requests.size(); ++index) {
        const auto& request = requests[index];
        if (request.owner.opaque_value == 0U) {
            result.status = VifPacketProducerTicketReservationStatus::InvalidOwner;
            return result;
        }
        if (!valid_anchor_shape(request.anchor)) {
            result.status = VifPacketProducerTicketReservationStatus::InvalidAnchor;
            return result;
        }
        if (!valid_range(request.packet_output)) {
            result.status = VifPacketProducerTicketReservationStatus::InvalidPacketOutput;
            return result;
        }
        if (!valid_range(request.header_output) ||
            !valid_packet_outputs(request.packet_output, request.header_output) ||
            ranges_overlap(request.anchor.element_output, request.header_output)) {
            result.status = VifPacketProducerTicketReservationStatus::InvalidHeaderOutput;
            return result;
        }
        if (!contains_range(request.packet_output, request.anchor.element_output)) {
            result.status =
                VifPacketProducerTicketReservationStatus::PacketDoesNotContainElementOutput;
            return result;
        }
        if (request.capacity_mode != StreamEnvironmentPacketCapacityMode::ExistingRoom ||
            request.has_flush_plan) {
            result.status = VifPacketProducerTicketReservationStatus::EmbeddedFlush;
            return result;
        }
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (ranges_overlap(requests[earlier].anchor.element_output, request.anchor.element_output)) {
                result.status =
                    VifPacketProducerTicketReservationStatus::OverlappingElementOutput;
                return result;
            }
            if (ranges_overlap(requests[earlier].packet_output, request.packet_output)) {
                result.status = VifPacketProducerTicketReservationStatus::OverlappingPacketOutput;
                return result;
            }
            if (!compatible_header_use(requests[earlier], request)) {
                result.status =
                    VifPacketProducerTicketReservationStatus::OverlappingPacketHeaderOutput;
                return result;
            }
        }
        for (const auto& reserved : reserved_passes_) {
            if (!reserved.has_value()) {
                continue;
            }
            if (footprints_overlap(request.packet_output, request.header_output,
                                   reserved->packet_output, reserved->header_output)) {
                result.status = VifPacketProducerTicketReservationStatus::
                    OverlappingOutstandingReservationWrite;
                return result;
            }
        }
        for (const auto& claimed : claimed_reservation_passes_) {
            if (!claimed.has_value()) {
                continue;
            }
            if (footprints_overlap(request.packet_output, request.header_output,
                                   claimed->packet_output, claimed->header_output)) {
                result.status = VifPacketProducerTicketReservationStatus::
                    OverlappingOutstandingReservationWrite;
                return result;
            }
        }
    }

    if (requests.size() > result.available_count) {
        result.status = VifPacketProducerTicketReservationStatus::CapacityExhausted;
        return result;
    }
    if (!can_allocate_ids(next_id_, requests.size())) {
        result.status = VifPacketProducerTicketReservationStatus::TicketGenerationExhausted;
        return result;
    }
    if (next_reservation_id_ == 0U) {
        result.status = VifPacketProducerTicketReservationStatus::ReservationGenerationExhausted;
        return result;
    }

    auto group_slot = reservation_groups_.end();
    for (auto group = reservation_groups_.begin(); group != reservation_groups_.end(); ++group) {
        if (!group->has_value()) {
            group_slot = group;
            break;
        }
    }
    if (group_slot == reservation_groups_.end()) {
        result.status = VifPacketProducerTicketReservationStatus::CapacityExhausted;
        return result;
    }

    const VifPacketProducerTicketReservation reservation{
        VifPacketProducerTicketReservationId{next_reservation_id_++}, requests.size()};
    *group_slot = ReservationGroup{reservation.id, requests.size(), 0U};

    std::size_t request_index = 0U;
    for (std::size_t slot = 0U; slot < kCapacity; ++slot) {
        if (tickets_[slot].has_value() || reserved_passes_[slot].has_value()) {
            continue;
        }
        const auto& request = requests[request_index];
        reserved_passes_[slot] =
            ReservedPass{VifPacketProducerTicketId{next_id_++}, reservation.id, request_index,
                         requests.size(), request.owner, request.anchor, request.packet_output,
                         request.header_output};
        ++reserved_count_;
        ++request_index;
        if (request_index == requests.size()) {
            break;
        }
    }
    if (request_index != requests.size()) {
        static_cast<void>(abort_reservation_group(reservation));
        result.status = VifPacketProducerTicketReservationStatus::CapacityExhausted;
        return result;
    }

    result.status = VifPacketProducerTicketReservationStatus::Reserved;
    result.reservation = reservation;
    return result;
}

VifPacketProducerTicketLedger::ReservationGroup* VifPacketProducerTicketLedger::find_reservation_group(
    const VifPacketProducerTicketReservation& reservation) noexcept {
    if (reservation.id.value == 0U || reservation.pass_count == 0U) {
        return nullptr;
    }
    for (auto& group : reservation_groups_) {
        if (group.has_value() && group->id == reservation.id &&
            group->pass_count == reservation.pass_count) {
            return &*group;
        }
    }
    return nullptr;
}

VifPacketProducerTicketReservationPreflightResult
VifPacketProducerTicketLedger::can_publish_reserved_pass(
    const VifPacketProducerTicketReservation& reservation,
    const std::size_t pass_ordinal) const noexcept {
    if (reservation.id.value == 0U || reservation.pass_count == 0U) {
        return {VifPacketProducerTicketReservationPreflightStatus::StaleReservation};
    }
    for (const auto& group : reservation_groups_) {
        if (!group.has_value() || !(group->id == reservation.id) ||
            group->pass_count != reservation.pass_count) {
            continue;
        }
        if (group->next_pass_ordinal > pass_ordinal) {
            return {VifPacketProducerTicketReservationPreflightStatus::PassAlreadyPublished};
        }
        if (group->next_pass_ordinal < pass_ordinal) {
            return {VifPacketProducerTicketReservationPreflightStatus::PassOutOfOrder};
        }
        return {VifPacketProducerTicketReservationPreflightStatus::Ready};
    }
    return {VifPacketProducerTicketReservationPreflightStatus::StaleReservation};
}

VifPacketProducerTicketLedger::ReservationAbortCounts
VifPacketProducerTicketLedger::abort_reservation_group(
    const VifPacketProducerTicketReservation& reservation) noexcept {
    ReservationAbortCounts counts;
    if (reservation.id.value == 0U || reservation.pass_count == 0U) {
        return counts;
    }
    for (auto& group : reservation_groups_) {
        if (!group.has_value() || !(group->id == reservation.id) ||
            group->pass_count != reservation.pass_count) {
            continue;
        }
        group.reset();
        counts.cancelled = true;
        break;
    }
    for (auto& pass : reserved_passes_) {
        if (!pass.has_value() || !(pass->reservation_id == reservation.id) ||
            pass->pass_count != reservation.pass_count) {
            continue;
        }
        pass.reset();
        --reserved_count_;
        ++counts.released_reservations;
        counts.cancelled = true;
    }
    clear_claimed_reservation_passes(reservation);
    for (auto& ticket : tickets_) {
        if (!ticket.has_value() || !ticket->reservation_id.has_value() ||
            !(*ticket->reservation_id == reservation.id) ||
            ticket->reservation_pass_count != reservation.pass_count) {
            continue;
        }
        ticket.reset();
        --active_count_;
        ++counts.retired_tickets;
        counts.cancelled = true;
    }
    return counts;
}

void VifPacketProducerTicketLedger::clear_claimed_reservation_passes(
    const VifPacketProducerTicketReservation& reservation) noexcept {
    if (reservation.id.value == 0U || reservation.pass_count == 0U) {
        return;
    }
    for (auto& claimed : claimed_reservation_passes_) {
        if (claimed.has_value() && claimed->reservation_id == reservation.id &&
            claimed->pass_count == reservation.pass_count) {
            claimed.reset();
        }
    }
}

VifPacketProducerTicketReservedPublishResult
VifPacketProducerTicketLedger::publish_reserved_after_stream_commit(
    const guest::EeMemory& memory, const VifPacketProducerTicketReservation& reservation,
    const std::size_t pass_ordinal, const StreamEnvironmentPacketGuestResult& result) noexcept {
    const auto preflight = can_publish_reserved_pass(reservation, pass_ordinal);
    if (!preflight.ready()) {
        // After the guest packet write, any ordering or authentication failure leaves the
        // sidecar untrustworthy, so every failure clears the whole ledger.
        clear();
        return {published_status_from_preflight(preflight.status), VifPacketProducerTicketId{}, true,
                true};
    }

    auto reserved_slot = reserved_passes_.end();
    for (auto reserved = reserved_passes_.begin(); reserved != reserved_passes_.end();
         ++reserved) {
        if (reserved->has_value() && (*reserved)->reservation_id == reservation.id &&
            (*reserved)->pass_count == reservation.pass_count &&
            (*reserved)->pass_ordinal == pass_ordinal) {
            reserved_slot = reserved;
            break;
        }
    }
    if (reserved_slot == reserved_passes_.end()) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::StaleReservation, VifPacketProducerTicketId{}, true, true};
    }

    const ReservedPass reserved = **reserved_slot;
    if (!commit_matches_reserved_pass(result, reserved.anchor, reserved.packet_output,
                                      reserved.header_output)) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::CommitResultMismatch, reserved.ticket_id, true, true};
    }

    const auto write_retirement = invalidate_stream_packet_memory_writes_impl(
        reserved.packet_output, reserved.header_output, &reservation);
    auto* const group = find_reservation_group(reservation);
    if (write_retirement.status != VifPacketProducerTicketInvalidationStatus::Applied ||
        group == nullptr) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::ReservationInvalidated, reserved.ticket_id, true, true};
    }

    const unsigned long long element_bytes =
        static_cast<unsigned long long>(reserved.anchor.element_output.quadword_count) * 16U;
    if (!memory.contains(reserved.anchor.element_output.begin, element_bytes)) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::SourceUnavailable, reserved.ticket_id, true, true};
    }
    const auto current = read_quadword(memory, reserved.anchor.first_element);
    if (!current.has_value()) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::SourceUnavailable, reserved.ticket_id, true, true};
    }
    if (*current != reserved.anchor.expected_first_element) {
        clear();
        return {VifPacketProducerTicketReservedPublishStatus::SourceContentsMismatch, reserved.ticket_id, true, true};
    }

    const auto slot = static_cast<std::size_t>(reserved_slot - reserved_passes_.begin());
    reserved_passes_[slot].reset();
    --reserved_count_;
    claimed_reservation_passes_[slot] = ClaimedReservationPass{
        reserved.reservation_id, reserved.pass_count, reserved.packet_output,
        reserved.header_output};
    tickets_[slot] = VifPacketProducerTicket{reserved.ticket_id, reserved.owner, reserved.anchor,
                                             reserved.reservation_id, reserved.pass_ordinal,
                                             reserved.pass_count};
    ++active_count_;

    ++group->next_pass_ordinal;
    if (group->next_pass_ordinal == group->pass_count) {
        clear_claimed_reservation_passes(reservation);
        for (auto& stored_group : reservation_groups_) {
            if (stored_group.has_value() && stored_group->id == reservation.id &&
                stored_group->pass_count == reservation.pass_count) {
                stored_group.reset();
                break;
            }
        }
    }
    return {VifPacketProducerTicketReservedPublishStatus::Published, reserved.ticket_id, false,
            false};
}

VifPacketProducerTicketReservationCancelResult
VifPacketProducerTicketLedger::cancel_reservation(
    const VifPacketProducerTicketReservation& reservation) noexcept {
    const auto counts = abort_reservation_group(reservation);
    return {counts.cancelled, counts.retired_tickets, counts.released_reservations};
}

VifPacketProducerTicketInvalidationResult VifPacketProducerTicketLedger::invalidate_written_range_impl(
    const guest::GuestAddress begin, const std::size_t byte_count,
    const VifPacketProducerTicketReservation* const reservation_exemption) noexcept {
    if (byte_count == 0U || byte_count > kAddressSpaceBytes - static_cast<unsigned long long>(begin)) {
        return {};
    }

    VifPacketProducerTicketInvalidationResult result;
    std::array<VifPacketProducerTicketReservation, kCapacity> affected_reservations{};
    std::size_t affected_count = 0U;
    // Several tickets, claims and reserved passes can belong to one callback; the
    // callback is aborted once, as a unit, after every footprint has been examined.
    const auto add_affected = [&affected_reservations, &affected_count](
                                  const VifPacketProducerTicketReservationId id,
                                  const std::size_t pass_count) {
        for (std::size_t index = 0U; index < affected_count; ++index) {
            if (affected_reservations[index].id == id &&
                affected_reservations[index].pass_count == pass_count) {
                return;
            }
        }
        if (affected_count < affected_reservations.size()) {
            affected_reservations[affected_count++] = {id, pass_count};
        }
    };

    for (auto& ticket : tickets_) {
        if (!ticket.has_value() ||
            !overlaps_write(ticket->anchor.element_output, begin, byte_count)) {
            continue;
        }
        if (ticket->reservation_id.has_value()) {
            if (!is_exempt(reservation_exemption, *ticket->reservation_id,
                           ticket->reservation_pass_count)) {
                add_affected(*ticket->reservation_id, ticket->reservation_pass_count);
            }
            continue;
        }
        ticket.reset();
        --active_count_;
        ++result.retired;
    }
    for (const auto& claimed : claimed_reservation_passes_) {
        if (!claimed.has_value() ||
            (!overlaps_write(claimed->packet_output, begin, byte_count) &&
             !overlaps_write(claimed->header_output, begin, byte_count)) ||
            is_exempt(reservation_exemption, claimed->reservation_id, claimed->pass_count)) {
            continue;
        }
        add_affected(claimed->reservation_id, claimed->pass_count);
    }
    for (const auto& reserved : reserved_passes_) {
        if (!reserved.has_value() ||
            (!overlaps_write(reserved->packet_output, begin, byte_count) &&
             !overlaps_write(reserved->header_output, begin, byte_count)) ||
            is_exempt(reservation_exemption, reserved->reservation_id, reserved->pass_count)) {
            continue;
        }
        add_affected(reserved->reservation_id, reserved->pass_count);
    }

    for (std::size_t index = 0U; index < affected_count; ++index) {
        const auto counts = abort_reservation_group(affected_reservations[index]);
        if (!counts.cancelled) {
            continue;
        }
        ++result.aborted_reservations;
        result.retired += counts.retired_tickets;
        result.released_reservations += counts.released_reservations;
    }
    result.status = VifPacketProducerTicketInvalidationStatus::Applied;
    return result;
}

VifPacketProducerTicketInvalidationResult
VifPacketProducerTicketLedger::invalidate_stream_packet_memory_writes_impl(
    const VifPacketQwordRange packet_output, const VifPacketQwordRange header_output,
    const VifPacketProducerTicketReservation* const reservation_exemption) noexcept {
    if (!valid_packet_outputs(packet_output, header_output)) {
        return {};
    }
    auto result = invalidate_written_range_impl(
        packet_output.begin, static_cast<std::size_t>(packet_output.quadword_count) * 16U,
        reservation_exemption);
    if (result.status != VifPacketProducerTicketInvalidationStatus::Applied ||
        contains_range(packet_output, header_output)) {
        return result;
    }
    const auto header_result = invalidate_written_range_impl(
        header_output.begin, static_cast<std::size_t>(header_output.quadword_count) * 16U,
        reservation_exemption);
    if (header_result.status != VifPacketProducerTicketInvalidationStatus::Applied) {
        return header_result;
    }
    result.retired += header_result.retired;
    result.aborted_reservations += header_result.aborted_reservations;
    result.released_reservations += header_result.released_reservations;
    return result;
}

VifPacketProducerTicketPublishResult VifPacketProducerTicketLedger::try_publish_after_stream_commit(
    const guest::EeMemory& memory, const VifPacketProducerTicketOwner owner,
    const StreamEnvironmentProducerAnchor& anchor,
    const StreamEnvironmentPacketGuestResult& result) noexcept {
    if (owner.opaque_value == 0U) {
        return {.status = VifPacketProducerTicketPublishStatus::InvalidOwner};
    }
    if (!valid_anchor_shape(anchor)) {
        return {.status = VifPacketProducerTicketPublishStatus::InvalidAnchor};
    }
    if (!result_covers_element_output(result, anchor)) {
        return {.status = VifPacketProducerTicketPublishStatus::CommitResultMismatch};
    }

    const unsigned long long element_bytes =
        static_cast<unsigned long long>(anchor.element_output.quadword_count) * 16U;
    if (!memory.contains(anchor.element_output.begin, element_bytes)) {
        return {.status = VifPacketProducerTicketPublishStatus::SourceUnavailable};
    }
    const auto current = read_quadword(memory, anchor.first_element);
    if (!current.has_value()) {
        return {.status = VifPacketProducerTicketPublishStatus::SourceUnavailable};
    }
    if (*current != anchor.expected_first_element) {
        return {.status = VifPacketProducerTicketPublishStatus::SourceContentsMismatch};
    }
    if (occupied_count() >= kCapacity) {
        return {.status = VifPacketProducerTicketPublishStatus::CapacityExhausted};
    }
    if (next_id_ == 0U) {
        return {.status = VifPacketProducerTicketPublishStatus::GenerationExhausted};
    }
    for (std::size_t slot = 0U; slot < kCapacity; ++slot) {
        if (tickets_[slot].has_value() || reserved_passes_[slot].has_value()) {
            continue;
        }
        const VifPacketProducerTicketId id{next_id_++};
        tickets_[slot] = VifPacketProducerTicket{id, owner, anchor};
        ++active_count_;
        return {.status = VifPacketProducerTicketPublishStatus::Published, .id = id};
    }
    return {.status = VifPacketProducerTicketPublishStatus::CapacityExhausted};
}

VifPacketProducerTicketRelocationResult VifPacketProducerTicketLedger::relocate_after_copy(
    const guest::EeMemory& memory, const VifPacketQwordRange source,
    const VifPacketQwordRange destination) noexcept {
    if (!valid_range(source) || !valid_range(destination) ||
        source.quadword_count != destination.quadword_count ||
        ranges_overlap(source, destination)) {
        return {};
    }

    const auto destination_retirement = invalidate_written_range_impl(
        destination.begin, static_cast<std::size_t>(destination.quadword_count) * 16U, nullptr);
    if (destination_retirement.status != VifPacketProducerTicketInvalidationStatus::Applied) {
        return {};
    }

    VifPacketProducerTicketRelocationResult result;
    result.retired_destination_overwrite = destination_retirement.retired;
    result.aborted_reservations = destination_retirement.aborted_reservations;
    result.released_reservations = destination_retirement.released_reservations;

    for (std::size_t slot = 0U; slot < kCapacity; ++slot) {
        auto& ticket = tickets_[slot];
        if (!ticket.has_value()) {
            continue;
        }
        const bool source_overlap = ranges_overlap(ticket->anchor.element_output, source);
        if (!source_overlap) {
            continue;
        }
        const auto& claimed = claimed_reservation_passes_[slot];
        if (claimed.has_value()) {
            const VifPacketProducerTicketReservation reservation{(*claimed).reservation_id,
                                                                 (*claimed).pass_count};
            const auto counts = abort_reservation_group(reservation);
            if (counts.cancelled) {
                result.retired_incomplete_reservation += counts.retired_tickets;
                ++result.aborted_reservations;
                result.released_reservations += counts.released_reservations;
                continue;
            }
            claimed_reservation_passes_[slot].reset();
            ticket.reset();
            --active_count_;
            ++result.retired_incomplete_reservation;
            continue;
        }
        if (!contains_range(source, ticket->anchor.element_output)) {
            if (ticket->reservation_id.has_value()) {
                const VifPacketProducerTicketReservation reservation{*ticket->reservation_id,
                                                                     ticket->reservation_pass_count};
                const auto counts = abort_reservation_group(reservation);
                if (counts.cancelled) {
                    result.retired_partial_source += counts.retired_tickets;
                    ++result.aborted_reservations;
                    result.released_reservations += counts.released_reservations;
                    continue;
                }
            }
            ticket.reset();
            --active_count_;
            ++result.retired_partial_source;
            continue;
        }
        auto relocated_anchor = ticket->anchor;
        relocated_anchor.element_output =
            relocate_range(ticket->anchor.element_output, source, destination);
        relocated_anchor.first_element =
            relocate_range(ticket->anchor.first_element, source, destination);
        const auto current = read_quadword(memory, relocated_anchor.first_element);
        if (!current.has_value() || *current != relocated_anchor.expected_first_element) {
            if (ticket->reservation_id.has_value()) {
                const VifPacketProducerTicketReservation reservation{
                    *ticket->reservation_id, ticket->reservation_pass_count};
                const auto counts = abort_reservation_group(reservation);
                if (counts.cancelled) {
                    result.retired_destination_contents_mismatch += counts.retired_tickets;
                    ++result.aborted_reservations;
                    result.released_reservations += counts.released_reservations;
                    continue;
                }
            }
            ticket.reset();
            --active_count_;
            ++result.retired_destination_contents_mismatch;
            continue;
        }
        ticket->anchor = relocated_anchor;
        ++result.relocated;
    }
    result.status = VifPacketProducerTicketRelocationStatus::Applied;
    return result;
}

VifPacketProducerTicketRelocationResult VifPacketProducerTicketLedger::relocate_after_ring_flush(
    const guest::EeMemory& memory, const VifPacketRingFlushResult& flush) noexcept {
    return relocate_after_copy(memory, flush.drained, flush.output);
}

VifPacketProducerTicketInvalidationResult VifPacketProducerTicketLedger::invalidate_written_range(
    const guest::GuestAddress begin, const std::size_t byte_count) noexcept {
    return invalidate_written_range_impl(begin, byte_count, nullptr);
}

bool VifPacketProducerTicketLedger::cancel(const VifPacketProducerTicketId id) noexcept {
    if (id.value == 0U) {
        return false;
    }
    for (auto& ticket : tickets_) {
        if (!ticket.has_value() || !(ticket->id == id)) {
            continue;
        }
        if (ticket->reservation_id.has_value()) {
            const VifPacketProducerTicketReservation reservation{*ticket->reservation_id,
                                                                 ticket->reservation_pass_count};
            if (abort_reservation_group(reservation).cancelled) {
                return true;
            }
        }
        ticket.reset();
        --active_count_;
        return true;
    }
    return false;
}

VifPacketProducerTicketTakeResult VifPacketProducerTicketLedger::take_completed(
    const VifPacketProducerTicketId id) noexcept {
    if (id.value == 0U) {
        return {.status = VifPacketProducerTicketTakeStatus::InvalidId};
    }
    for (auto& ticket : tickets_) {
        if (!ticket.has_value() || !(ticket->id == id)) {
            continue;
        }
        if (ticket->reservation_id.has_value()) {
            for (const auto& group : reservation_groups_) {
                if (group.has_value() && group->id == *ticket->reservation_id &&
                    group->pass_count == ticket->reservation_pass_count) {
                    return {.status = VifPacketProducerTicketTakeStatus::ReservationIncomplete};
                }
            }
        }
        VifPacketProducerTicket taken = *ticket;
        ticket.reset();
        --active_count_;
        return {VifPacketProducerTicketTakeStatus::Taken, std::move(taken)};
    }
    return {.status = VifPacketProducerTicketTakeStatus::TicketUnavailable};
}

void VifPacketProducerTicketLedger::clear() noexcept {
    for (auto& ticket : tickets_) {
        ticket.reset();
    }
    for (auto& reserved : reserved_passes_) {
        reserved.reset();
    }
    for (auto& claimed : claimed_reservation_passes_) {
        claimed.reset();
    }
    for (auto& group : reservation_groups_) {
        group.reset();
    }
    active_count_ = 0U;
    reserved_count_ = 0U;
}

std::optional<VifPacketProducerTicket> VifPacketProducerTicketLedger::find(
    const VifPacketProducerTicketId id) const noexcept {
    if (id.value == 0U) {
        return std::nullopt;
    }
    for (const auto& ticket : tickets_) {
        if (ticket.has_value() && ticket->id == id) {
            return *ticket;
        }
    }
    return std::nullopt;
}

StreamEnvironmentPacketProducerTicketCommitResult commit_stream_environment_packet_with_producer_ticket(
    guest::EeMemory& memory, const StreamEnvironmentPacketGuestPlan& plan,
    VifPacketProducerTicketLedger& ledger, const VifPacketProducerTicketOwner owner) {
    const auto anchor = make_stream_environment_producer_anchor(plan);
    auto stream = commit_stream_environment_packet(memory, plan);
    if (stream.flush_result.has_value()) {
        static_cast<void>(ledger.relocate_after_ring_flush(memory, *stream.flush_result));
    }
    if (valid_range(stream.packet_output)) {
        static_cast<void>(ledger.invalidate_written_range(
            stream.packet_output.begin,
            static_cast<std::size_t>(stream.packet_output.quadword_count) * 16U));
    }
    if (valid_packet_outputs(stream.packet_output, stream.header_output) &&
        !contains_range(stream.packet_output, stream.header_output)) {
        static_cast<void>(ledger.invalidate_written_range(
            stream.header_output.begin,
            static_cast<std::size_t>(stream.header_output.quadword_count) * 16U));
    }
    VifPacketProducerTicketPublishResult ticket{};
    if (anchor.has_value()) {
        ticket = ledger.try_publish_after_stream_commit(memory, owner, *anchor, stream);
    }
    return {stream, ticket};
}

VifPacketProducerTicketRelocationResult copy_vif_packet_quadwords_with_producer_tickets(
    guest::EeMemory& memory, const VifPacketQwordRange destination, const VifPacketQwordRange source,
    VifPacketProducerTicketLedger& ledger) {
    copy_vif_packet_quadwords(memory, destination, source);
    return ledger.relocate_after_copy(memory, source, destination);
}

} // namespace takedown::runtime::slus_21050
