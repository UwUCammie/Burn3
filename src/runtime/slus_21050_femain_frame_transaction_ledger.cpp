#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"

#include <algorithm>
#include <atomic>

namespace takedown::runtime {
namespace {

// Ledger identities bind tickets and reservations to one ledger object. Zero
// is reserved for "no identity", so the counter saturates instead of wrapping.
std::uint64_t allocate_ledger_identity() noexcept {
    static std::atomic<std::uint64_t> next_identity{1U};
    auto candidate = next_identity.load(std::memory_order_relaxed);
    while (candidate != std::numeric_limits<std::uint64_t>::max()) {
        if (next_identity.compare_exchange_weak(candidate, candidate + 1U,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0U;
}

// Tag mixed into the digest at the start of every session ("FEMAINL1").
constexpr std::uint64_t kSessionDigestTag = 0x46454D41494E4C31ULL;

} // namespace

Slus21050FemainFrameTransactionLedger::Slus21050FemainFrameTransactionLedger() noexcept
    : ledger_identity_(allocate_ledger_identity()) {}

std::string_view to_string(const Slus21050FemainFrameLedgerStatus status) noexcept {
    switch (status) {
    case Slus21050FemainFrameLedgerStatus::Ok:
        return "ok";
    case Slus21050FemainFrameLedgerStatus::Inactive:
        return "inactive";
    case Slus21050FemainFrameLedgerStatus::AlreadyActive:
        return "already-active";
    case Slus21050FemainFrameLedgerStatus::Paused:
        return "paused";
    case Slus21050FemainFrameLedgerStatus::Terminal:
        return "terminal";
    case Slus21050FemainFrameLedgerStatus::NoFreeSlot:
        return "no-free-slot";
    case Slus21050FemainFrameLedgerStatus::CapacityExhausted:
        return "capacity-exhausted";
    case Slus21050FemainFrameLedgerStatus::Overflowed:
        return "overflowed";
    case Slus21050FemainFrameLedgerStatus::SessionGenerationExhausted:
        return "session-generation-exhausted";
    case Slus21050FemainFrameLedgerStatus::StaleSessionGeneration:
        return "stale-session-generation";
    case Slus21050FemainFrameLedgerStatus::FrameGenerationExhausted:
        return "frame-generation-exhausted";
    case Slus21050FemainFrameLedgerStatus::StaleReservation:
        return "stale-reservation";
    case Slus21050FemainFrameLedgerStatus::AcknowledgementTooEarly:
        return "acknowledgement-too-early";
    case Slus21050FemainFrameLedgerStatus::DuplicateAcknowledgement:
        return "duplicate-acknowledgement";
    case Slus21050FemainFrameLedgerStatus::OutOfOrder:
        return "out-of-order";
    case Slus21050FemainFrameLedgerStatus::InvalidBoundary:
        return "invalid-boundary";
    case Slus21050FemainFrameLedgerStatus::NoPublishedFrame:
        return "no-published-frame";
    case Slus21050FemainFrameLedgerStatus::NotDrained:
        return "not-drained";
    case Slus21050FemainFrameLedgerStatus::StaleTransaction:
        return "stale-transaction";
    case Slus21050FemainFrameLedgerStatus::TransactionGenerationExhausted:
        return "transaction-generation-exhausted";
    case Slus21050FemainFrameLedgerStatus::EpochNotFull:
        return "epoch-not-full";
    case Slus21050FemainFrameLedgerStatus::EpochGenerationExhausted:
        return "epoch-generation-exhausted";
    case Slus21050FemainFrameLedgerStatus::MediaOrdinalExhausted:
        return "media-ordinal-exhausted";
    case Slus21050FemainFrameLedgerStatus::LogicalEventGenerationExhausted:
        return "logical-event-generation-exhausted";
    case Slus21050FemainFrameLedgerStatus::EmptyDropBatch:
        return "empty-drop-batch";
    }
    return "unknown";
}

Slus21050FemainFrameLedgerBeginResult Slus21050FemainFrameTransactionLedger::begin_session() noexcept {
    return begin_session(next_session_generation_);
}

Slus21050FemainFrameLedgerBeginResult Slus21050FemainFrameTransactionLedger::begin_session(
    const std::uint64_t requested_session_generation) noexcept {
    if (state_ == State::Active || state_ == State::Paused || state_ == State::EosDraining) {
        return {.status = Status::AlreadyActive};
    }
    if (state_ == State::Overflowed) {
        return {.status = Status::Overflowed};
    }
    if (requested_session_generation == 0U || next_session_generation_ == 0U) {
        return {.status = Status::SessionGenerationExhausted};
    }
    if (requested_session_generation < next_session_generation_) {
        return {.status = Status::StaleSessionGeneration};
    }
    const Slus21050FemainFrameLedgerStatus headroom_status = check_lifecycle_control_headroom();
    if (headroom_status != Status::Ok) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    if (requested_session_generation == std::numeric_limits<std::uint64_t>::max()) {
        next_session_generation_ = 0U;
    } else {
        next_session_generation_ = requested_session_generation + 1U;
    }
    reset_session_records();
    session_generation_ = requested_session_generation;
    state_ = State::Active;
    proof_valid_ = true;
    overflowed_ = false;
    digest_ = kFnvOffset;
    hash_u64(kSessionDigestTag);
    hash_u64(requested_session_generation);
    epoch_generation_ = 1U;
    epoch_start_digest_ = digest_;
    record_transaction_mutation();
    return {Status::Ok, requested_session_generation};
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::check_lifecycle_control_headroom() const noexcept {
    if (ledger_identity_ == 0U || transaction_revision_ == 0U) {
        return Status::TransactionGenerationExhausted;
    }
    const std::size_t mutations = remaining_acknowledgement_mutations();
    if (transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 1U - mutations) {
        return Status::TransactionGenerationExhausted;
    }
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::pause() noexcept {
    if (state_ == State::Active) {
        const Slus21050FemainFrameLedgerStatus headroom_status = check_lifecycle_control_headroom();
        if (headroom_status != Status::Ok) {
            return headroom_status;
        }
        state_ = State::Paused;
        record_transaction_mutation();
        return Status::Ok;
    }
    if (state_ == State::Paused) {
        return Status::Paused;
    }
    return terminal_status();
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::resume() noexcept {
    if (state_ == State::Paused) {
        const Slus21050FemainFrameLedgerStatus headroom_status = check_lifecycle_control_headroom();
        if (headroom_status != Status::Ok) {
            return headroom_status;
        }
        state_ = State::Active;
        record_transaction_mutation();
        return Status::Ok;
    }
    if (state_ == State::Active) {
        return Status::AlreadyActive;
    }
    return terminal_status();
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::request_eos() noexcept {
    if (state_ == State::Active || state_ == State::Paused) {
        const Slus21050FemainFrameLedgerStatus headroom_status = check_lifecycle_control_headroom();
        if (headroom_status != Status::Ok) {
            return headroom_status;
        }
        state_ = State::EosDraining;
        record_transaction_mutation();
        return Status::Ok;
    }
    return terminal_status();
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::finish_eos() noexcept {
    if (state_ != State::EosDraining) {
        return terminal_status();
    }
    if (has_outstanding_frames()) {
        return Status::NotDrained;
    }
    const Slus21050FemainFrameLedgerStatus headroom_status = check_lifecycle_control_headroom();
    if (headroom_status != Status::Ok) {
        return headroom_status;
    }
    state_ = State::Inactive;
    record_transaction_mutation();
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::cancel_and_drain(
    const Slus21050FemainFrameLedgerCancelReason reason) noexcept {
    if (state_ == State::Inactive) {
        return Status::Inactive;
    }
    if (transaction_revision_ == 0U ||
        transaction_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::TransactionGenerationExhausted;
    }
    for (std::size_t index = 0U; index < frame_record_count_; ++index) {
        FrameRecord& record = records_[index];
        if (!outstanding_record(record.state)) {
            continue;
        }
        record.state = RecordState::Cancelled;
        ++cancelled_frames_;
        if (record.reservation.slot < slot_records_.size()) {
            slot_records_[record.reservation.slot] = -1;
        }
        hash_u8(7U);
        hash_u8(static_cast<std::uint8_t>(reason));
        hash_reservation(record.reservation);
    }
    proof_valid_ = false;
    state_ = State::Inactive;
    record_transaction_mutation();
    return Status::Ok;
}

void Slus21050FemainFrameTransactionLedger::hash_reservation(
    const Slus21050FemainFrameReservation& reservation) noexcept {
    hash_u64(reservation.session_generation);
    hash_u64(reservation.frame_generation);
    hash_u8(reservation.slot);
    hash_identity(reservation.frame);
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::cancel_and_drain(
    const Slus21050FemainFrameReservation& reservation,
    const Slus21050FemainFrameLedgerCancelReason reason) noexcept {
    if (state_ == State::Inactive) {
        return Status::Inactive;
    }
    const FrameRecord* const record = find_record(reservation);
    if (record == nullptr) {
        return Status::StaleReservation;
    }
    if (!outstanding_record(record->state)) {
        return record->state == RecordState::Cancelled ? Status::StaleReservation
                                                       : Status::DuplicateAcknowledgement;
    }
    return cancel_and_drain(reason);
}

Slus21050FemainFrameLedgerReservationResult
Slus21050FemainFrameTransactionLedger::reserve(const Slus21050FemainFrameIdentity& frame) noexcept {
    if (const Status active = require_active(); active != Status::Ok) {
        return {.status = active};
    }
    if (const Status ordinal = require_source_ordinal(frame.media_ordinal); ordinal != Status::Ok) {
        return {.status = ordinal};
    }
    if (has_outstanding_frames()) {
        return {.status = Status::NoFreeSlot};
    }
    if (has_source_boundary_ && !boundary_not_before(last_source_boundary_, frame.produced_at)) {
        return {.status = Status::InvalidBoundary};
    }
    if (check_event_capacity() != Status::Ok) {
        return {.status = overflow()};
    }
    if (next_media_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        return {.status = Status::MediaOrdinalExhausted};
    }
    if (total_logical_event_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return {.status = Status::LogicalEventGenerationExhausted};
    }
    auto slot = next_slot_;
    if (slot >= slot_records_.size() || slot_records_[slot] >= 0) {
        slot = find_free_slot();
    }
    if (slot == slot_records_.size()) {
        return {.status = Status::NoFreeSlot};
    }
    if (frame_record_count_ >= records_.size()) {
        return {.status = overflow()};
    }
    if (next_frame_generation_ == 0U) {
        return {.status = Status::FrameGenerationExhausted};
    }
    const Slus21050FemainFrameReservation reservation{
        .ledger_identity = ledger_identity_,
        .session_generation = session_generation_,
        .frame_generation = next_frame_generation_,
        .slot = static_cast<std::uint8_t>(slot),
        .frame = frame,
    };
    FrameRecord& record = records_[frame_record_count_];
    record = {};
    record.reservation = reservation;
    record.state = RecordState::Reserved;
    slot_records_[slot] = static_cast<std::int16_t>(frame_record_count_);
    ++frame_record_count_;
    ++logical_event_count_;
    ++total_logical_event_count_;
    ++next_media_ordinal_;
    has_source_boundary_ = true;
    last_source_boundary_ = frame.produced_at;
    next_slot_ = (slot + 1U) % slot_records_.size();
    if (next_frame_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        next_frame_generation_ = 0U;
    } else {
        ++next_frame_generation_;
    }
    hash_u8(1U);
    hash_reservation(reservation);
    record_transaction_mutation();
    return {Status::Ok, reservation};
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::record_drop(
    const Slus21050FemainFrameIdentity& frame) noexcept {
    if (const Status active = require_active(); active != Status::Ok) {
        return active;
    }
    if (const Status ordinal = require_source_ordinal(frame.media_ordinal); ordinal != Status::Ok) {
        return ordinal;
    }
    if (has_source_boundary_ && !boundary_not_before(last_source_boundary_, frame.produced_at)) {
        return Status::InvalidBoundary;
    }
    if (has_outstanding_frames() || next_publication_ordinal_ != next_media_ordinal_ ||
        next_graphics_ordinal_ != next_media_ordinal_ ||
        next_consumption_ordinal_ != next_media_ordinal_) {
        return Status::OutOfOrder;
    }
    if (check_event_capacity() != Status::Ok) {
        return overflow();
    }
    if (next_media_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::MediaOrdinalExhausted;
    }
    if (total_logical_event_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::LogicalEventGenerationExhausted;
    }
    ++logical_event_count_;
    ++total_logical_event_count_;
    ++dropped_frames_;
    ++next_media_ordinal_;
    ++next_publication_ordinal_;
    ++next_graphics_ordinal_;
    ++next_consumption_ordinal_;
    has_source_boundary_ = true;
    last_source_boundary_ = frame.produced_at;
    hash_u8(5U);
    hash_identity(frame);
    record_transaction_mutation();
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::record_repeat(
    const Slus21050FemainFrameGuestBoundary& boundary) noexcept {
    if (const Status active = require_active(); active != Status::Ok) {
        return active;
    }
    if (!has_published_frame_) {
        return Status::NoPublishedFrame;
    }
    if (!has_consumed_frame_ || !(last_consumed_frame_ == last_published_frame_) ||
        !boundary_not_before(last_consumed_frame_boundary_, boundary)) {
        return Status::InvalidBoundary;
    }
    if (has_presentation_boundary_ &&
        (!boundary_not_before(last_presentation_boundary_, boundary) ||
         boundary.guest_vblank == last_presentation_boundary_.guest_vblank)) {
        return Status::InvalidBoundary;
    }
    if (check_event_capacity() != Status::Ok) {
        return overflow();
    }
    if (total_logical_event_count_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::LogicalEventGenerationExhausted;
    }
    ++logical_event_count_;
    ++total_logical_event_count_;
    ++repeated_frames_;
    has_presentation_boundary_ = true;
    last_presentation_boundary_ = boundary;
    hash_u8(6U);
    hash_identity(last_published_frame_);
    hash_boundary(boundary);
    record_transaction_mutation();
    return Status::Ok;
}

// RECOVERY: publish body matches the original GIMPLE, but GCC splits it into publish.part.0 here
// (the original stayed whole), which makes its callers differ; the cause is unknown.
Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::publish(
    const Slus21050FemainFrameReservation& reservation,
    const Slus21050FemainFrameGuestBoundary& published_at) noexcept {
    if (state_ == State::Paused) {
        return Status::Paused;
    }
    if (state_ != State::Active && state_ != State::EosDraining) {
        return terminal_status();
    }
    FrameRecord* const record = find_record(reservation);
    if (record == nullptr) {
        return Status::StaleReservation;
    }
    if (record->state == RecordState::Published || record->state == RecordState::GraphicsComplete ||
        record->state == RecordState::Consumed) {
        return Status::DuplicateAcknowledgement;
    }
    if (record->state == RecordState::Cancelled) {
        return Status::StaleReservation;
    }
    if (record->reservation.frame.media_ordinal != next_publication_ordinal_) {
        return Status::OutOfOrder;
    }
    if (!boundary_not_before(record->reservation.frame.produced_at, published_at)) {
        return Status::InvalidBoundary;
    }
    if (has_presentation_boundary_ &&
        (!boundary_not_before(last_presentation_boundary_, published_at) ||
         published_at.guest_vblank == last_presentation_boundary_.guest_vblank)) {
        return Status::InvalidBoundary;
    }
    record->state = RecordState::Published;
    record->published_at = published_at;
    record->published_boundary_valid = true;
    ++published_frames_;
    ++next_publication_ordinal_;
    last_published_frame_ = record->reservation.frame;
    has_published_frame_ = true;
    has_presentation_boundary_ = true;
    last_presentation_boundary_ = published_at;
    hash_u8(2U);
    hash_reservation(reservation);
    hash_boundary(published_at);
    record_transaction_mutation();
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::complete_graphics(
    const Slus21050FemainFrameGraphicsCompletion& completion) noexcept {
    if (state_ != State::Active && state_ != State::Paused && state_ != State::EosDraining) {
        return terminal_status();
    }
    FrameRecord* const record = find_record(completion.reservation);
    if (record == nullptr) {
        return Status::StaleReservation;
    }
    if (record->state == RecordState::GraphicsComplete || record->state == RecordState::Consumed) {
        return Status::DuplicateAcknowledgement;
    }
    if (record->state == RecordState::Reserved) {
        return Status::AcknowledgementTooEarly;
    }
    if (record->state == RecordState::Cancelled) {
        return Status::StaleReservation;
    }
    if (record->reservation.frame.media_ordinal != next_graphics_ordinal_) {
        return Status::OutOfOrder;
    }
    if (!completion.vif1_before_gif || !record->published_boundary_valid ||
        !boundary_not_before(record->published_at, completion.completed_at)) {
        return Status::InvalidBoundary;
    }
    if (has_graphics_boundary_ &&
        !boundary_not_before(last_graphics_boundary_, completion.completed_at)) {
        return Status::InvalidBoundary;
    }
    record->state = RecordState::GraphicsComplete;
    record->completed_at = completion.completed_at;
    record->completed_boundary_valid = true;
    ++graphics_completed_frames_;
    ++next_graphics_ordinal_;
    has_graphics_boundary_ = true;
    last_graphics_boundary_ = completion.completed_at;
    hash_u8(3U);
    hash_reservation(completion.reservation);
    hash_boundary(completion.completed_at);
    hash_u8(completion.vif1_before_gif ? 1U : 0U);
    record_transaction_mutation();
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::consume(
    const Slus21050FemainFrameConsumption& consumption) noexcept {
    if (state_ != State::Active && state_ != State::Paused && state_ != State::EosDraining) {
        return terminal_status();
    }
    FrameRecord* const record = find_record(consumption.reservation);
    if (record == nullptr) {
        return Status::StaleReservation;
    }
    if (record->state == RecordState::Consumed) {
        return Status::DuplicateAcknowledgement;
    }
    if (record->state == RecordState::Reserved || record->state == RecordState::Published) {
        return Status::AcknowledgementTooEarly;
    }
    if (record->state == RecordState::Cancelled) {
        return Status::StaleReservation;
    }
    if (record->reservation.frame.media_ordinal != next_consumption_ordinal_) {
        return Status::OutOfOrder;
    }
    if (!record->completed_boundary_valid ||
        !boundary_not_before(record->completed_at, consumption.consumed_at)) {
        return Status::InvalidBoundary;
    }
    if (has_consumption_boundary_ &&
        !boundary_not_before(last_consumption_boundary_, consumption.consumed_at)) {
        return Status::InvalidBoundary;
    }
    record->state = RecordState::Consumed;
    record->consumed_at = consumption.consumed_at;
    record->consumed_boundary_valid = true;
    slot_records_[record->reservation.slot] = -1;
    ++consumed_frames_;
    ++next_consumption_ordinal_;
    has_consumed_frame_ = true;
    last_consumed_frame_ = record->reservation.frame;
    last_consumed_frame_boundary_ = consumption.consumed_at;
    has_consumption_boundary_ = true;
    last_consumption_boundary_ = consumption.consumed_at;
    hash_u8(4U);
    hash_reservation(consumption.reservation);
    hash_boundary(consumption.consumed_at);
    record_transaction_mutation();
    return Status::Ok;
}

Slus21050FemainFrameTransactionPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_frame_transaction(
    const Slus21050FemainFrameTransactionEvidence& evidence) const noexcept {
    // Reserve, publish, graphics and consume each consume one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 4U) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_frame_transaction(evidence, reservation);
    if (status != Status::Ok) {
        if (status == Status::Overflowed && check_event_capacity() == Status::CapacityExhausted) {
            // The candidate overflowed; the live ledger merely ran out of room.
            return {.status = Status::CapacityExhausted};
        }
        return {.status = status};
    }
    Slus21050FemainFrameTransactionTicket ticket{};
    ticket.before_ = snapshot();
    ticket.evidence_ = evidence;
    ticket.reservation_ = reservation;
    ticket.owner_ = this;
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}

Slus21050FemainFrameTransactionCommitResult
Slus21050FemainFrameTransactionLedger::commit_frame_transaction(
    const Slus21050FemainFrameTransactionTicket& ticket) noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot())) {
        return {.status = Status::StaleTransaction};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_frame_transaction(ticket.evidence_, reservation);
    if (status != Status::Ok) {
        return {.status = status};
    }
    if (!(reservation == ticket.reservation_)) {
        return {.status = Status::StaleTransaction};
    }
    *this = candidate;
    return {Status::Ok, reservation};
}

Slus21050FemainFramePublicationPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_frame_publication(
    const Slus21050FemainFramePublicationEvidence& evidence) const noexcept {
    // Reserve, publish, graphics and consume each consume one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 4U) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_frame_publication(evidence, reservation);
    if (status != Status::Ok) {
        if (status == Status::Overflowed && check_event_capacity() == Status::CapacityExhausted) {
            // The candidate overflowed; the live ledger merely ran out of room.
            return {.status = Status::CapacityExhausted};
        }
        return {.status = status};
    }
    Slus21050FemainFramePublicationTicket ticket{};
    ticket.before_ = snapshot();
    ticket.evidence_ = evidence;
    ticket.reservation_ = reservation;
    ticket.owner_ = this;
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_frame_publication(
    const Slus21050FemainFramePublicationEvidence& evidence,
    Slus21050FemainFrameReservation& reservation) noexcept {
    const auto reserved = reserve(evidence.frame);
    if (!reserved.reserved()) {
        return reserved.status;
    }
    reservation = reserved.reservation;
    return publish(reservation, evidence.published_at);
}

Slus21050FemainFramePublicationCommitResult
Slus21050FemainFrameTransactionLedger::commit_frame_publication(
    const Slus21050FemainFramePublicationTicket& ticket) noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot()) ||
        ticket.before_.ledger_identity != ledger_identity_ ||
        ticket.reservation_.ledger_identity != ledger_identity_ ||
        !(ticket.reservation_.frame == ticket.evidence_.frame)) {
        return {.status = Status::StaleTransaction};
    }
    if (transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 4U) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_frame_publication(ticket.evidence_, reservation);
    if (status != Status::Ok) {
        return {.status = status};
    }
    if (!(reservation == ticket.reservation_)) {
        return {.status = Status::StaleTransaction};
    }
    *this = candidate;
    return {Status::Ok, reservation};
}

Slus21050FemainFrameDropBatchPublicationPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_drop_batch_frame_publication(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames,
    const Slus21050FemainFramePublicationEvidence& evidence) const noexcept {
    if (dropped_frames.size() > kSlus21050FemainFrameTransactionCapacity - 1U) {
        return {.status = Status::CapacityExhausted};
    }
    // The drops plus reserve, publish, graphics and consume each consume one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ >
            std::numeric_limits<std::uint64_t>::max() - 4U - dropped_frames.size()) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_drop_batch_frame_publication(dropped_frames, evidence,
                                                                     reservation);
    if (status != Status::Ok) {
        if (status == Status::Overflowed && state_ == State::Active &&
            logical_event_count_ + dropped_frames.size() + 1U >
                kSlus21050FemainFrameTransactionCapacity) {
            return {.status = Status::CapacityExhausted};
        }
        return {.status = status};
    }
    Slus21050FemainFrameDropBatchPublicationTicket ticket{};
    ticket.owner_ = this;
    ticket.before_ = snapshot();
    std::copy(dropped_frames.begin(), dropped_frames.end(), ticket.dropped_frames_.begin());
    ticket.dropped_frame_count_ = dropped_frames.size();
    ticket.evidence_ = evidence;
    ticket.reservation_ = reservation;
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}

Slus21050FemainFrameDropBatchPublicationCommitResult
Slus21050FemainFrameTransactionLedger::commit_drop_batch_frame_publication(
    const Slus21050FemainFrameDropBatchPublicationTicket& ticket) noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot()) ||
        ticket.before_.ledger_identity != ledger_identity_ ||
        ticket.dropped_frame_count_ > kSlus21050FemainFrameTransactionCapacity - 1U ||
        ticket.reservation_.ledger_identity != ledger_identity_ ||
        !(ticket.reservation_.frame == ticket.evidence_.frame)) {
        return {.status = Status::StaleTransaction};
    }
    if (transaction_revision_ >
        std::numeric_limits<std::uint64_t>::max() - 4U - ticket.dropped_frame_count_) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_drop_batch_frame_publication(ticket.dropped_frames(),
                                                                     ticket.evidence_, reservation);
    if (status != Status::Ok) {
        return {.status = status};
    }
    if (!(reservation == ticket.reservation_)) {
        return {.status = Status::StaleTransaction};
    }
    *this = candidate;
    return {Status::Ok, reservation};
}

Slus21050FemainFrameRetirementPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_frame_retirement(
    const Slus21050FemainFrameRetirementEvidence& evidence) const noexcept {
    // Graphics completion and consume each consume one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 2U) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    const auto status = candidate.apply_frame_retirement(evidence);
    if (status != Status::Ok) {
        return {.status = status};
    }
    Slus21050FemainFrameRetirementTicket ticket{};
    // RECOVERY: the original inlined a local named `snapshot` here (no begin-of-scope clobber);
    // the source form that produces exactly that is unknown.
    ticket.before_ = snapshot();
    ticket.evidence_ = evidence;
    ticket.reservation_ = evidence.reservation;
    ticket.owner_ = this;
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}

Slus21050FemainFrameRetirementCommitResult
Slus21050FemainFrameTransactionLedger::commit_frame_retirement(
    const Slus21050FemainFrameRetirementTicket& ticket) noexcept {
    const auto& reservation = ticket.reservation_;
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot()) ||
        ticket.before_.ledger_identity != ledger_identity_ ||
        ticket.reservation().ledger_identity != ledger_identity_ ||
        !(ticket.evidence_.reservation == reservation)) {
        return {.status = Status::StaleTransaction};
    }
    if (transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - 2U) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    const auto status = candidate.apply_frame_retirement(ticket.evidence_);
    if (status != Status::Ok) {
        return {.status = status};
    }
    *this = candidate;
    return {Status::Ok, reservation};
}
Slus21050FemainFrameDropBatchTransactionPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_drop_batch_frame_transaction(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames,
    const Slus21050FemainFrameTransactionEvidence& evidence) const noexcept {
    if (dropped_frames.size() > kSlus21050FemainFrameTransactionCapacity - 1U) {
        return {.status = Status::CapacityExhausted};
    }
    // The drops plus reserve, publish, graphics and consume each consume one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ >
            std::numeric_limits<std::uint64_t>::max() - 4U - dropped_frames.size()) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_drop_batch_frame_transaction(dropped_frames, evidence,
                                                                     reservation);
    if (status != Status::Ok) {
        if (status == Status::Overflowed && state_ == State::Active &&
            logical_event_count_ + dropped_frames.size() + 1U >
                kSlus21050FemainFrameTransactionCapacity) {
            return {.status = Status::CapacityExhausted};
        }
        return {.status = status};
    }
    Slus21050FemainFrameDropBatchTransactionTicket ticket{};
    ticket.owner_ = this;
    ticket.before_ = snapshot();
    std::copy(dropped_frames.begin(), dropped_frames.end(), ticket.dropped_frames_.begin());
    ticket.dropped_frame_count_ = dropped_frames.size();
    ticket.evidence_ = evidence;
    ticket.reservation_ = reservation;
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}
Slus21050FemainFrameTransactionCommitResult
Slus21050FemainFrameTransactionLedger::commit_drop_batch_frame_transaction(
    const Slus21050FemainFrameDropBatchTransactionTicket& ticket) noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot())) {
        return {.status = Status::StaleTransaction};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    Slus21050FemainFrameReservation reservation{};
    const auto status = candidate.apply_drop_batch_frame_transaction(ticket.dropped_frames(),
                                                                     ticket.evidence_, reservation);
    if (status != Status::Ok) {
        return {.status = status};
    }
    if (!(reservation == ticket.reservation_)) {
        return {.status = Status::StaleTransaction};
    }
    *this = candidate;
    return {Status::Ok, reservation};
}
// RECOVERY: branch probability of the state_ == Active test differs (34% vs 20.97%); CFG is identical.
Slus21050FemainFrameDropPrefixTransactionPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_drop_prefix_transaction(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames) const noexcept {
    if (dropped_frames.empty()) {
        return {.status = Status::EmptyDropBatch};
    }
    if (dropped_frames.size() > kSlus21050FemainFrameTransactionCapacity) {
        return {.status = Status::CapacityExhausted};
    }
    // Each drop consumes one revision.
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ > std::numeric_limits<std::uint64_t>::max() - dropped_frames.size()) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    const auto status = candidate.apply_drop_prefix_transaction(dropped_frames);
    if (status != Status::Ok) {
        if (status == Status::Overflowed) {
            if (state_ == State::Active) {
                if (logical_event_count_ + dropped_frames.size() >
                    kSlus21050FemainFrameTransactionCapacity) {
                    return {.status = Status::CapacityExhausted};
                }
            }
        }
        return {.status = status};
    }
    Slus21050FemainFrameDropPrefixTransactionTicket ticket{};
    ticket.owner_ = this;
    ticket.before_ = snapshot();
    std::copy(dropped_frames.begin(), dropped_frames.end(), ticket.dropped_frames_.begin());
    ticket.dropped_frame_count_ = dropped_frames.size();
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}
Slus21050FemainFrameDropPrefixTransactionCommitResult
Slus21050FemainFrameTransactionLedger::commit_drop_prefix_transaction(
    const Slus21050FemainFrameDropPrefixTransactionTicket& ticket) noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot())) {
        return {.status = Status::StaleTransaction};
    }
    Slus21050FemainFrameTransactionLedger candidate = *this;
    const auto status = candidate.apply_drop_prefix_transaction(ticket.dropped_frames());
    if (status != Status::Ok) {
        return {.status = status};
    }
    *this = candidate;
    return {.status = Status::Ok};
}
Slus21050FemainFrameLedgerEpochRolloverPreflightResult
Slus21050FemainFrameTransactionLedger::preflight_epoch_rollover() const noexcept {
    if (ledger_identity_ == 0U || transaction_revision_ == 0U ||
        transaction_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return {.status = Status::TransactionGenerationExhausted};
    }
    if (state_ != State::Active && state_ != State::Paused) {
        return {.status = terminal_status()};
    }
    if (!proof_valid_ || overflowed_) {
        return {.status = Status::Overflowed};
    }
    if (logical_event_count_ != kSlus21050FemainFrameTransactionCapacity) {
        return {.status = Status::EpochNotFull};
    }
    if (has_outstanding_frames()) {
        return {.status = Status::NotDrained};
    }
    if (next_publication_ordinal_ != next_media_ordinal_ ||
        next_graphics_ordinal_ != next_media_ordinal_ ||
        next_consumption_ordinal_ != next_media_ordinal_) {
        return {.status = Status::OutOfOrder};
    }
    if (epoch_generation_ == 0U ||
        epoch_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return {.status = Status::EpochGenerationExhausted};
    }
    Slus21050FemainFrameLedgerEpochRolloverTicket ticket{};
    ticket.before_ = snapshot();
    ticket.owner_ = this;
    ticket.seal_ = current_epoch_seal();
    ticket.ledger_identity_ = ledger_identity_;
    ticket.revision_ = transaction_revision_;
    return {Status::Ok, ticket};
}
Slus21050FemainFrameLedgerEpochRolloverCommitResult
Slus21050FemainFrameTransactionLedger::commit_epoch_rollover(
    const Slus21050FemainFrameLedgerEpochRolloverTicket& ticket) noexcept {
    const auto validation = validate_epoch_rollover(ticket);
    if (validation != Status::Ok) {
        return {.status = validation};
    }
    const auto sealed_epoch = ticket.seal_;
    clear_epoch_records();
    ++epoch_generation_;
    epoch_first_media_ordinal_ = next_media_ordinal_;
    hash_u8(8U);
    hash_u64(sealed_epoch.terminal_digest);
    hash_u64(session_generation_);
    hash_u64(epoch_generation_);
    hash_u64(total_logical_event_count_);
    hash_u64(next_media_ordinal_);
    hash_u64(next_publication_ordinal_);
    hash_u64(next_graphics_ordinal_);
    hash_u64(next_consumption_ordinal_);
    hash_u64(next_frame_generation_);
    hash_u64(next_slot_);
    epoch_start_digest_ = digest_;
    record_transaction_mutation();
    return {Status::Ok, sealed_epoch};
}
Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::validate_epoch_rollover(
    const Slus21050FemainFrameLedgerEpochRolloverTicket& ticket) const noexcept {
    if (ticket.owner_ != this || ticket.ledger_identity_ == 0U ||
        ticket.ledger_identity_ != ledger_identity_ || ticket.revision_ == 0U ||
        ticket.revision_ != transaction_revision_ || !(ticket.before_ == snapshot()) ||
        !(ticket.seal_ == current_epoch_seal())) {
        return Status::StaleTransaction;
    }
    const auto admitted = preflight_epoch_rollover();
    if (!admitted.admitted()) {
        return admitted.status;
    }
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_frame_retirement(
    const Slus21050FemainFrameRetirementEvidence& evidence) noexcept {
    if (const auto status = complete_graphics(
            {evidence.reservation, evidence.graphics_completed_at, evidence.vif1_before_gif});
        status != Status::Ok) {
        return status;
    }
    return consume({evidence.reservation, evidence.consumed_at});
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_drop_batch_frame_publication(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames,
    const Slus21050FemainFramePublicationEvidence& evidence,
    Slus21050FemainFrameReservation& reservation) noexcept {
    for (const auto& dropped : dropped_frames) {
        if (const auto status = record_drop(dropped); status != Status::Ok) {
            return status;
        }
    }
    const auto reserved = reserve(evidence.frame);
    if (!reserved.reserved()) {
        return reserved.status;
    }
    reservation = reserved.reservation;
    return publish(reservation, evidence.published_at);
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_frame_transaction(
    const Slus21050FemainFrameTransactionEvidence& evidence,
    Slus21050FemainFrameReservation& reservation) noexcept {
    const Slus21050FemainFramePublicationEvidence publication{evidence.frame, evidence.published_at};
    Slus21050FemainFrameLedgerStatus status = apply_frame_publication(publication, reservation);
    if (status != Status::Ok) {
        return status;
    }
    return apply_frame_retirement(Slus21050FemainFrameRetirementEvidence{
        reservation, evidence.graphics_completed_at, evidence.consumed_at,
        evidence.vif1_before_gif});
}
Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_drop_batch_frame_transaction(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames,
    const Slus21050FemainFrameTransactionEvidence& evidence,
    Slus21050FemainFrameReservation& reservation) noexcept {
    for (const auto& dropped : dropped_frames) {
        if (const auto status = record_drop(dropped); status != Status::Ok) {
            return status;
        }
    }
    return apply_frame_transaction(evidence, reservation);
}
Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::apply_drop_prefix_transaction(
    const std::span<const Slus21050FemainFrameIdentity> dropped_frames) noexcept {
    for (const auto& dropped : dropped_frames) {
        if (const auto status = record_drop(dropped); status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}

Slus21050FemainFrameLedgerSnapshot Slus21050FemainFrameTransactionLedger::snapshot() const noexcept {
    Slus21050FemainFrameLedgerSnapshot result{};
    result.state = state_;
    result.session_generation = session_generation_;
    result.next_frame_generation = next_frame_generation_;
    result.next_media_ordinal = next_media_ordinal_;
    result.next_publication_ordinal = next_publication_ordinal_;
    result.next_graphics_ordinal = next_graphics_ordinal_;
    result.next_consumption_ordinal = next_consumption_ordinal_;
    result.logical_event_count = logical_event_count_;
    result.frame_record_count = frame_record_count_;
    result.in_flight_count = in_flight_count();
    result.published_frames = published_frames_;
    result.graphics_completed_frames = graphics_completed_frames_;
    result.consumed_frames = consumed_frames_;
    result.repeated_frames = repeated_frames_;
    result.dropped_frames = dropped_frames_;
    result.cancelled_frames = cancelled_frames_;
    result.overflowed = overflowed_;
    result.proof_valid = proof_valid_;
    result.digest = digest_;
    result.total_logical_event_count = total_logical_event_count_;
    result.epoch_generation = epoch_generation_;
    result.epoch_first_media_ordinal = epoch_first_media_ordinal_;
    result.epoch_start_digest = epoch_start_digest_;
    result.ledger_identity = ledger_identity_;
    for (std::size_t slot = 0U; slot < result.slots.size(); ++slot) {
        const auto record_index = slot_records_[slot];
        if (record_index < 0 || static_cast<std::size_t>(record_index) >= frame_record_count_) {
            result.slots[slot] = SlotState::Free;
            continue;
        }
        switch (records_[static_cast<std::size_t>(record_index)].state) {
        case RecordState::Reserved:
            result.slots[slot] = SlotState::Reserved;
            break;
        case RecordState::Published:
            result.slots[slot] = SlotState::Published;
            break;
        case RecordState::GraphicsComplete:
            result.slots[slot] = SlotState::GraphicsComplete;
            break;
        case RecordState::Consumed:
        case RecordState::Cancelled:
            result.slots[slot] = SlotState::Free;
            break;
        }
    }
    return result;
}

bool Slus21050FemainFrameTransactionLedger::has_outstanding_frames() const noexcept {
    return in_flight_count() != 0U;
}

bool Slus21050FemainFrameTransactionLedger::drained() const noexcept {
    return in_flight_count() == 0U;
}

Slus21050FemainFrameTransactionLedger::FrameRecord*
Slus21050FemainFrameTransactionLedger::find_record(
    const Slus21050FemainFrameReservation& reservation) noexcept {
    for (std::size_t index = 0U; index < frame_record_count_; ++index) {
        if (records_[index].reservation == reservation) {
            return &records_[index];
        }
    }
    return nullptr;
}

const Slus21050FemainFrameTransactionLedger::FrameRecord*
Slus21050FemainFrameTransactionLedger::find_record(
    const Slus21050FemainFrameReservation& reservation) const noexcept {
    for (std::size_t index = 0U; index < frame_record_count_; ++index) {
        if (records_[index].reservation == reservation) {
            return &records_[index];
        }
    }
    return nullptr;
}

std::size_t Slus21050FemainFrameTransactionLedger::find_free_slot() const noexcept {
    for (std::size_t slot = 0U; slot < slot_records_.size(); ++slot) {
        if (slot_records_[slot] < 0) {
            return slot;
        }
    }
    return slot_records_.size();
}

std::size_t Slus21050FemainFrameTransactionLedger::in_flight_count() const noexcept {
    std::size_t count = 0U;
    for (std::size_t index = 0U; frame_record_count_ > index; ++index) {
        if (outstanding_record(records_[index].state)) {
            ++count;
        }
    }
    return count;
}

bool Slus21050FemainFrameTransactionLedger::outstanding_record(const RecordState state) noexcept {
    return state == RecordState::Reserved || state == RecordState::Published ||
           state == RecordState::GraphicsComplete;
}

std::size_t Slus21050FemainFrameTransactionLedger::remaining_acknowledgement_mutations() const noexcept {
    std::size_t mutations = 0U;
    for (std::size_t index = 0U; frame_record_count_ > index; ++index) {
        switch (records_[index].state) {
        case RecordState::Reserved:
            mutations += 3U;
            break;
        case RecordState::Published:
            mutations += 2U;
            break;
        case RecordState::GraphicsComplete:
            mutations += 1U;
            break;
        case RecordState::Consumed:
        case RecordState::Cancelled:
            break;
        }
    }
    return mutations;
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::terminal_status() const noexcept {
    switch (state_) {
    case State::Active:
        return Status::Ok;
    case State::Paused:
        return Status::Paused;
    case State::EosDraining:
        return Status::Terminal;
    case State::Overflowed:
        return Status::Overflowed;
    case State::Inactive:
        break;
    }
    return Status::Inactive;
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::check_event_capacity() const noexcept {
    if (state_ == State::Overflowed || overflowed_) {
        return Status::Overflowed;
    }
    if (logical_event_count_ >= kSlus21050FemainFrameTransactionCapacity) {
        return Status::CapacityExhausted;
    }
    return Status::Ok;
}

Slus21050FemainFrameTransactionLedger::Status Slus21050FemainFrameTransactionLedger::overflow() noexcept {
    state_ = State::Overflowed;
    overflowed_ = true;
    proof_valid_ = false;
    record_transaction_mutation();
    return Status::Overflowed;
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::require_active() const noexcept {
    if (state_ == State::Active) {
        return Status::Ok;
    }
    if (state_ == State::Paused) {
        return Status::Paused;
    }
    if (state_ == State::EosDraining) {
        return Status::Terminal;
    }
    return terminal_status();
}

Slus21050FemainFrameTransactionLedger::Status
Slus21050FemainFrameTransactionLedger::require_source_ordinal(
    const std::uint64_t media_ordinal) const noexcept {
    return next_media_ordinal_ == media_ordinal ? Status::Ok : Status::OutOfOrder;
}

bool Slus21050FemainFrameTransactionLedger::boundary_not_before(
    const Slus21050FemainFrameGuestBoundary& first,
    const Slus21050FemainFrameGuestBoundary& second) const noexcept {
    return second.guest_vblank >= first.guest_vblank && second.audio_sample >= first.audio_sample;
}

void Slus21050FemainFrameTransactionLedger::reset_session_records() noexcept {
    state_ = State::Inactive;
    next_media_ordinal_ = 0U;
    next_publication_ordinal_ = 0U;
    next_graphics_ordinal_ = 0U;
    next_consumption_ordinal_ = 0U;
    logical_event_count_ = 0U;
    total_logical_event_count_ = 0U;
    epoch_generation_ = 0U;
    epoch_first_media_ordinal_ = 0U;
    epoch_start_digest_ = 0U;
    frame_record_count_ = 0U;
    published_frames_ = 0U;
    graphics_completed_frames_ = 0U;
    consumed_frames_ = 0U;
    repeated_frames_ = 0U;
    dropped_frames_ = 0U;
    cancelled_frames_ = 0U;
    digest_ = kFnvOffset;
    overflowed_ = false;
    proof_valid_ = false;
    has_published_frame_ = false;
    last_published_frame_ = {};
    has_consumed_frame_ = false;
    last_consumed_frame_ = {};
    last_consumed_frame_boundary_ = {};
    has_source_boundary_ = false;
    has_presentation_boundary_ = false;
    has_graphics_boundary_ = false;
    has_consumption_boundary_ = false;
    last_source_boundary_ = {};
    last_presentation_boundary_ = {};
    last_graphics_boundary_ = {};
    last_consumption_boundary_ = {};
    next_slot_ = 0U;
    records_.fill(FrameRecord{});
    slot_records_.fill(-1);
}

void Slus21050FemainFrameTransactionLedger::clear_epoch_records() noexcept {
    logical_event_count_ = 0U;
    frame_record_count_ = 0U;
    records_.fill(FrameRecord{});
    slot_records_.fill(-1);
}

Slus21050FemainFrameLedgerEpochSeal
Slus21050FemainFrameTransactionLedger::current_epoch_seal() const noexcept {
    Slus21050FemainFrameLedgerEpochSeal seal{};
    seal.session_generation = session_generation_;
    seal.epoch_generation = epoch_generation_;
    seal.logical_event_count = logical_event_count_;
    seal.total_logical_event_count = total_logical_event_count_;
    seal.first_media_ordinal = epoch_first_media_ordinal_;
    seal.next_media_ordinal = next_media_ordinal_;
    seal.next_publication_ordinal = next_publication_ordinal_;
    seal.next_graphics_ordinal = next_graphics_ordinal_;
    seal.next_consumption_ordinal = next_consumption_ordinal_;
    seal.next_frame_generation = next_frame_generation_;
    seal.next_slot = next_slot_;
    seal.start_digest = epoch_start_digest_;
    seal.terminal_digest = digest_;
    return seal;
}

void Slus21050FemainFrameTransactionLedger::hash_u8(const std::uint8_t value) noexcept {
    digest_ ^= value;
    digest_ *= kFnvPrime;
}

void Slus21050FemainFrameTransactionLedger::hash_u32(const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        hash_u8(static_cast<std::uint8_t>(value >> shift));
    }
}

void Slus21050FemainFrameTransactionLedger::hash_u64(const std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        hash_u8(static_cast<std::uint8_t>(value >> shift));
    }
}

void Slus21050FemainFrameTransactionLedger::hash_boundary(
    const Slus21050FemainFrameGuestBoundary& boundary) noexcept {
    hash_u64(boundary.guest_vblank);
    hash_u64(boundary.audio_sample);
}

void Slus21050FemainFrameTransactionLedger::hash_identity(
    const Slus21050FemainFrameIdentity& identity) noexcept {
    hash_u64(identity.media_ordinal);
    hash_u64(identity.source_sequence);
    hash_u32(identity.source_checksum);
    hash_boundary(identity.produced_at);
}

void Slus21050FemainFrameTransactionLedger::record_transaction_mutation() noexcept {
    if (transaction_revision_ == 0U) {
        return;
    }
    if (transaction_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        transaction_revision_ = 0U;
        return;
    }
    ++transaction_revision_;
}
} // namespace takedown::runtime
