#include "takedown/runtime/slus_21050_femain_drop_prefix_epoch_coordinator.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace takedown::runtime {

std::string_view to_string(const Slus21050FemainDropPrefixEpochCoordinatorStatus status) noexcept {
    switch (status) {
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::Ready:
        return "ready";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::Committed:
        return "committed";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::Empty:
        return "empty";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::Occupied:
        return "occupied";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::AlreadyCommitted:
        return "already-committed";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::DueFrameOutstanding:
        return "due-frame-outstanding";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::BatchRejected:
        return "batch-rejected";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::PrefixCountMismatch:
        return "prefix-count-mismatch";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::BoundaryEvidenceMissing:
        return "boundary-evidence-missing";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::CrossSession:
        return "cross-session";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::WrongLedger:
        return "wrong-ledger";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::LedgerRejected:
        return "ledger-rejected";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending:
        return "acknowledgement-pending";
    case Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending:
        return "rollover-pending";
    }
    return "unknown";
}

Slus21050FemainDropPrefixEpochCoordinator::Slus21050FemainDropPrefixEpochCoordinator(
    Slus21050MoviePlayer& player) noexcept
    : player_(player) {}

Slus21050FemainDropPrefixEpochCoordinatorResult Slus21050FemainDropPrefixEpochCoordinator::result(
    const Slus21050FemainDropPrefixEpochCoordinatorStatus status) const noexcept {
    return Slus21050FemainDropPrefixEpochCoordinatorResult{status, {}, {}, prefix_count_, {}};
}

void Slus21050FemainDropPrefixEpochCoordinator::clear_prepared() noexcept {
    ledger_ = nullptr;
    live_batch_.reset();
    expected_suffix_.reset();
    drop_ticket_.reset();
    rollover_ticket_.reset();
    expected_rollover_seal_.reset();
    prefix_count_ = 0;
    player_identity_ = 0;
    player_session_generation_ = 0;
    state_ = State::Empty;
}

Slus21050FemainDropPrefixEpochCoordinatorResult Slus21050FemainDropPrefixEpochCoordinator::admit(
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050MovieDroppedFrameBatch& live_batch,
    const std::size_t prefix_count) noexcept {
    if (state_ == State::Completed) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::AlreadyCommitted);
    }
    if (state_ != State::Empty) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::Occupied);
    }
    if (player_.has_pending_frame() || player_.has_outstanding_prepared_frame()) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::DueFrameOutstanding);
    }
    if (const auto batch_status =
            player_.validate_dropped_frame_batch_acknowledgement(live_batch, prefix_count);
        batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::BatchRejected;
        value.player_status = batch_status;
        value.prefix_count = prefix_count_;
        return value;
    }

    const auto snapshot = ledger.snapshot();
    if (snapshot.session_generation != 0U) {
        for (const auto& identity : live_batch.frames()) {
            if (snapshot.session_generation != identity.session_generation) {
                return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::CrossSession);
            }
        }
    }
    if (prefix_count == 0U || prefix_count > live_batch.frames().size() ||
        snapshot.logical_event_count > 63U || prefix_count != 64U - snapshot.logical_event_count) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::PrefixCountMismatch);
    }

    std::array<Slus21050FemainFrameIdentity, 64U> identities{};
    const auto frames = live_batch.frames();
    for (std::size_t index = 0U; index < prefix_count; ++index) {
        const auto& identity = frames[index];
        if (!identity.dropped_at.has_value()) {
            return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::BoundaryEvidenceMissing);
        }
        identities[index].media_ordinal = identity.timeline_ordinal;
        identities[index].source_sequence = identity.source_sequence;
        identities[index].source_checksum = identity.source_checksum;
        identities[index].produced_at = *identity.dropped_at;
    }

    const auto admitted = ledger.preflight_drop_prefix_transaction(
        std::span<const Slus21050FemainFrameIdentity>(identities.data(), prefix_count));
    if (admitted.status != Slus21050FemainFrameLedgerStatus::Ok || !admitted.ticket.has_value()) {
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::LedgerRejected;
        value.ledger_status = admitted.status;
        value.prefix_count = prefix_count_;
        return value;
    }

    ledger_ = &ledger;
    live_batch_ = live_batch;
    expected_suffix_.reset();
    drop_ticket_ = admitted.ticket;
    rollover_ticket_.reset();
    expected_rollover_seal_.reset();
    prefix_count_ = prefix_count;
    player_identity_ = player_.identity();
    player_session_generation_ = frames[0].session_generation;
    state_ = State::Prepared;
    return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::Ready);
}

Slus21050FemainDropPrefixEpochCoordinatorResult Slus21050FemainDropPrefixEpochCoordinator::commit(
    Slus21050FemainFrameTransactionLedger& ledger) noexcept {
    if (state_ == State::Empty) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::Empty);
    }
    if (state_ == State::Completed) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::AlreadyCommitted);
    }
    if (ledger_ != &ledger) {
        return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::WrongLedger);
    }

    if (state_ == State::Prepared) {
        if (!drop_ticket_.has_value() || !live_batch_.has_value()) {
            clear_prepared();
            return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::LedgerRejected);
        }
        if (player_.has_pending_frame() || player_.has_outstanding_prepared_frame()) {
            const auto saved_prefix = prefix_count_;
            clear_prepared();
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::DueFrameOutstanding;
            value.prefix_count = saved_prefix;
            return value;
        }
        if (const auto batch_status = player_.validate_dropped_frame_batch_acknowledgement(
                *live_batch_, prefix_count_);
            batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            const auto saved_prefix = prefix_count_;
            clear_prepared();
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::BatchRejected;
            value.player_status = batch_status;
            value.prefix_count = saved_prefix;
            return value;
        }
        const auto committed = ledger.commit_drop_prefix_transaction(*drop_ticket_);
        if (committed.status != Slus21050FemainFrameLedgerStatus::Ok) {
            const auto saved_prefix = prefix_count_;
            clear_prepared();
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::LedgerRejected;
            value.ledger_status = committed.status;
            value.prefix_count = saved_prefix;
            return value;
        }
        drop_ticket_.reset();
        state_ = State::AcknowledgementPending;
        const auto rollover = ledger.preflight_epoch_rollover();
        if (rollover.status == Slus21050FemainFrameLedgerStatus::Ok && rollover.ticket.has_value()) {
            expected_rollover_seal_ = rollover.ticket->seal();
            rollover_ticket_ = rollover.ticket;
        } else {
            expected_rollover_seal_.reset();
            rollover_ticket_.reset();
        }
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
        value.ledger_status = rollover.status;
        value.prefix_count = prefix_count_;
        return value;
    }

    if (state_ == State::AcknowledgementPending) {
        if (!live_batch_.has_value()) {
            return result(Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending);
        }
        if (player_.identity() != player_identity_ ||
            player_.session_generation() != player_session_generation_) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
            value.player_status = static_cast<Slus21050MovieDroppedFrameBatchStatus>(3U);
            value.prefix_count = prefix_count_;
            return value;
        }
        if (!rollover_ticket_.has_value() ||
            ledger.validate_epoch_rollover(*rollover_ticket_) !=
                Slus21050FemainFrameLedgerStatus::Ok) {
            const auto admitted = ledger.preflight_epoch_rollover();
            Slus21050FemainFrameLedgerStatus rollover_status = admitted.status;
            if (admitted.status == Slus21050FemainFrameLedgerStatus::Ok && admitted.ticket.has_value()) {
                if (expected_rollover_seal_.has_value() &&
                    admitted.ticket->seal() == *expected_rollover_seal_) {
                    rollover_ticket_ = admitted.ticket;
                } else {
                    rollover_status = static_cast<Slus21050FemainFrameLedgerStatus>(17U);
                }
            }
            if (rollover_status != Slus21050FemainFrameLedgerStatus::Ok) {
                Slus21050FemainDropPrefixEpochCoordinatorResult value{};
                value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
                value.ledger_status = rollover_status;
                value.prefix_count = prefix_count_;
                return value;
            }
        }
        if (!rollover_ticket_.has_value()) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
            value.ledger_status = Slus21050FemainFrameLedgerStatus::Ok;
            value.prefix_count = prefix_count_;
            return value;
        }
        if (const auto batch_status =
                player_.validate_dropped_frame_batch_acknowledgement(*live_batch_, prefix_count_);
            batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
            value.player_status = batch_status;
            value.prefix_count = prefix_count_;
            return value;
        }
        if (const auto ack_status =
                player_.acknowledge_dropped_frame_batch(*live_batch_, prefix_count_);
            ack_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::AcknowledgementPending;
            value.player_status = ack_status;
            value.prefix_count = prefix_count_;
            return value;
        }
        live_batch_.reset();
        state_ = State::RolloverPending;
        const auto suffix = player_.dropped_frame_batch();
        if (suffix.status == Slus21050MovieDroppedFrameBatchStatus::Ready && suffix.batch.has_value()) {
            for (const auto& identity : suffix.batch->frames()) {
                if (identity.session_generation != player_session_generation_) {
                    Slus21050FemainDropPrefixEpochCoordinatorResult value{};
                    value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
                    value.player_status = static_cast<Slus21050MovieDroppedFrameBatchStatus>(3U);
                    value.prefix_count = prefix_count_;
                    return value;
                }
            }
            expected_suffix_ = suffix.batch;
        }
        if (suffix.status == Slus21050MovieDroppedFrameBatchStatus::Ready ||
            suffix.status == Slus21050MovieDroppedFrameBatchStatus::Empty) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
            value.ledger_status = Slus21050FemainFrameLedgerStatus::Ok;
            value.player_status = Slus21050MovieDroppedFrameBatchStatus::Ready;
            value.prefix_count = prefix_count_;
            return value;
        }
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
        value.player_status = suffix.status;
        value.prefix_count = prefix_count_;
        return value;
    }

    // RolloverPending.
    if (player_.identity() != player_identity_ ||
        player_.session_generation() != player_session_generation_) {
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
        value.player_status = static_cast<Slus21050MovieDroppedFrameBatchStatus>(3U);
        value.prefix_count = prefix_count_;
        return value;
    }
    if (expected_suffix_.has_value()) {
        if (const auto batch_status = player_.validate_dropped_frame_batch(*expected_suffix_);
            batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
            value.player_status = batch_status;
            value.prefix_count = prefix_count_;
            return value;
        }
    } else {
        const auto suffix = player_.dropped_frame_batch();
        if (suffix.status != Slus21050MovieDroppedFrameBatchStatus::Empty) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
            value.player_status = suffix.status;
            value.prefix_count = prefix_count_;
            return value;
        }
    }

    if (!rollover_ticket_.has_value()) {
        const auto admitted = ledger.preflight_epoch_rollover();
        Slus21050FemainFrameLedgerStatus rollover_status = admitted.status;
        if (admitted.status == Slus21050FemainFrameLedgerStatus::Ok && admitted.ticket.has_value()) {
            if (expected_rollover_seal_.has_value() &&
                admitted.ticket->seal() == *expected_rollover_seal_) {
                rollover_ticket_ = admitted.ticket;
            } else {
                rollover_status = static_cast<Slus21050FemainFrameLedgerStatus>(17U);
            }
        }
        if (!rollover_ticket_.has_value()) {
            Slus21050FemainDropPrefixEpochCoordinatorResult value{};
            value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
            value.ledger_status = rollover_status;
            value.prefix_count = prefix_count_;
            return value;
        }
    }

    const auto rolled = ledger.commit_epoch_rollover(*rollover_ticket_);
    if (rolled.status != Slus21050FemainFrameLedgerStatus::Ok || !rolled.sealed_epoch.has_value()) {
        Slus21050FemainDropPrefixEpochCoordinatorResult value{};
        value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::RolloverPending;
        value.ledger_status = rolled.status;
        value.prefix_count = prefix_count_;
        return value;
    }
    rollover_ticket_.reset();
    ledger_ = nullptr;
    state_ = State::Completed;
    Slus21050FemainDropPrefixEpochCoordinatorResult value{};
    value.status = Slus21050FemainDropPrefixEpochCoordinatorStatus::Committed;
    value.ledger_status = Slus21050FemainFrameLedgerStatus::Ok;
    value.player_status = expected_suffix_.has_value() ? Slus21050MovieDroppedFrameBatchStatus::Ready
                                                       : Slus21050MovieDroppedFrameBatchStatus::Empty;
    value.prefix_count = prefix_count_;
    if (expected_suffix_.has_value()) {
        value.fresh_suffix = expected_suffix_;
    }
    return value;
}

bool Slus21050FemainDropPrefixEpochCoordinator::occupied() const noexcept {
    return state_ != State::Empty && state_ != State::Completed;
}

bool Slus21050FemainDropPrefixEpochCoordinator::acknowledgement_pending() const noexcept {
    return state_ == State::AcknowledgementPending;
}

bool Slus21050FemainDropPrefixEpochCoordinator::rollover_pending() const noexcept {
    return state_ == State::RolloverPending;
}

bool Slus21050FemainDropPrefixEpochCoordinator::completed() const noexcept {
    return state_ == State::Completed;
}

} // namespace takedown::runtime
