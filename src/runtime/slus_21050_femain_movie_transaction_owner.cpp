#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"

#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace takedown::runtime {
namespace {

// Same staged GS base which the retail movie path hands to the player receipt.
constexpr std::uint32_t kStagedGsBase = 0x3460U;

// Process-wide owner identities. Zero means exhausted.
std::uint64_t allocate_movie_transaction_owner_identity() noexcept {
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

} // namespace

std::string_view to_string(const Slus21050FemainMovieTransactionStatus status) noexcept {
    switch (status) {
        case Slus21050FemainMovieTransactionStatus::Ready:
            return "ready";
        case Slus21050FemainMovieTransactionStatus::Committed:
            return "committed";
        case Slus21050FemainMovieTransactionStatus::Retained:
            return "retained";
        case Slus21050FemainMovieTransactionStatus::Empty:
            return "empty";
        case Slus21050FemainMovieTransactionStatus::Occupied:
            return "occupied";
        case Slus21050FemainMovieTransactionStatus::NoPreparedFrame:
            return "no-prepared-frame";
        case Slus21050FemainMovieTransactionStatus::ConsumerRejected:
            return "consumer-rejected";
        case Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost:
            return "player-ownership-lost";
        case Slus21050FemainMovieTransactionStatus::RetainFailed:
            return "retain-failed";
        case Slus21050FemainMovieTransactionStatus::CompletionPending:
            return "completion-pending";
        case Slus21050FemainMovieTransactionStatus::AlreadyCommitted:
            return "already-committed";
        case Slus21050FemainMovieTransactionStatus::LedgerRejected:
            return "ledger-rejected";
        case Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch:
            return "ledger-session-mismatch";
        case Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected:
            return "dropped-frame-batch-rejected";
        case Slus21050FemainMovieTransactionStatus::DroppedFrameBoundaryMismatch:
            return "dropped-frame-boundary-mismatch";
        case Slus21050FemainMovieTransactionStatus::Cancelled:
            return "cancelled";
        case Slus21050FemainMovieTransactionStatus::CancellationRejected:
            return "cancellation-rejected";
        default:
            return "unknown";
    }
}

bool validate_slus_21050_femain_movie_publication_packet(
    const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    const auto& reservation = packet.published_reservation;
    const auto& plan = packet.plan;
    const auto& queue = packet.queue;
    const bool lease_valid = plan.player_lease.has_value() &&
                             plan.player_lease->session_generation != 0U &&
                             plan.player_lease->session_generation ==
                                 packet.player_session_generation &&
                             plan.player_lease->lease_generation != 0U;
    if (packet.owner_identity == 0U || packet.publication_serial == 0U ||
        packet.player_identity == 0U || packet.player_session_generation == 0U ||
        packet.ledger_identity == 0U || packet.ledger_session_generation == 0U ||
        packet.player_session_generation != packet.ledger_session_generation ||
        packet.ledger_identity != reservation.ledger_identity ||
        packet.ledger_session_generation != reservation.session_generation ||
        reservation.frame_generation == 0U ||
        reservation.slot >= kSlus21050FemainFrameTransactionSlotCount ||
        plan.layout_generation == 0U || plan.first_buffer == 0U || plan.second_buffer == 0U ||
        plan.buffer_bytes == 0U || plan.frame_generation == 0U || plan.payload_digest == 0U ||
        reservation.frame_generation != plan.frame_generation ||
        plan.media_ordinal != reservation.frame.media_ordinal ||
        plan.source_sequence != reservation.frame.source_sequence ||
        plan.source_checksum != reservation.frame.source_checksum || !lease_valid) {
        return false;
    }
    return queue.queue_read != 0U && queue.queue_end != 0U && queue.queue_base != 0U;
}

Slus21050FemainMovieTransactionOwner::Slus21050FemainMovieTransactionOwner(
    Slus21050MoviePlayer& player) noexcept
    : player_(player), owner_identity_(allocate_movie_transaction_owner_identity()),
      bound_player_identity_(player.identity()) {}

Slus21050FemainMovieTransactionOwner::~Slus21050FemainMovieTransactionOwner() {
    if (state_ == State::Prepared) {
        (void)retain();
    } else if (state_ == State::GuestCommitted) {
        (void)complete_receipt();
    }
}

Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::complete_receipt() noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::CompletionPending,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_};
    }
    const auto ledger_status = committed_ledger_status_;
    if (!frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::CompletionPending,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = ledger_status};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (dropped_frames_.has_value()) {
        const auto batch_status =
            player_.complete_pending_frame_and_acknowledge_dropped_frame_batch(receipt,
                                                                               *dropped_frames_);
        if (batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            return {.status = Slus21050FemainMovieTransactionStatus::CompletionPending,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = ledger_status,
            .dropped_frame_batch_status = batch_status};
        }
        clear();
        return {.status = Slus21050FemainMovieTransactionStatus::Committed,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = ledger_status,
            .dropped_frame_batch_status = batch_status};
    }
    if (!player_.complete_pending_frame(receipt)) {
        return {.status = Slus21050FemainMovieTransactionStatus::CompletionPending,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = ledger_status};
    }
    clear();
    return {.status = Slus21050FemainMovieTransactionStatus::Committed,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = ledger_status};
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::admit(const guest::EeMemory& memory) noexcept {
    return admit_impl(memory, nullptr);
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::admit_impl(
    const guest::EeMemory& memory,
    const Slus21050MovieDroppedFrameBatch* const dropped_frames) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ != State::Empty) {
        return {.status = Slus21050FemainMovieTransactionStatus::Occupied};
    }
    if (dropped_frames != nullptr) {
        const auto batch_status = player_.validate_dropped_frame_batch(*dropped_frames);
        if (batch_status != Slus21050MovieDroppedFrameBatchStatus::Ready) {
            return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                    .dropped_frame_batch_status = batch_status};
        }
    }
    auto frame = player_.take_pending_frame();
    if (!frame.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::NoPreparedFrame};
    }
    if (!player_session_.has_value() || *player_session_ != frame->lease().session_generation) {
        consumer_.reset();
        player_session_ = frame->lease().session_generation;
    }
    if (dropped_frames == nullptr &&
        !player_.can_complete_pending_frame(frame->completion_receipt(kStagedGsBase))) {
        if (!player_.retain_pending_frame(std::move(*frame))) {
            return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
                    .dropped_frame_batch_status =
                        Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                .dropped_frame_batch_status =
                    Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding};
    }
    if (dropped_frames != nullptr &&
        !player_.can_complete_pending_frame_and_acknowledge_dropped_frame_batch(
            frame->completion_receipt(kStagedGsBase), *dropped_frames)) {
        if (!player_.retain_pending_frame(std::move(*frame))) {
            return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
                    .dropped_frame_batch_status =
                        Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                .dropped_frame_batch_status =
                    Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized};
    }
    const auto plan = consumer_.preflight(memory, *frame);
    if (!plan.admitted()) {
        const auto status = plan.status;
        if (!player_.retain_pending_frame(std::move(*frame))) {
            return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
                    .consumer_status = status};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::ConsumerRejected,
                .consumer_status = status};
    }
    const auto handoff = consumer_.preflight_handoff(memory, *plan.plan);
    if (!handoff.admitted()) {
        const auto status = handoff.status;
        if (!player_.retain_pending_frame(std::move(*frame))) {
            return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
                    .consumer_status = status};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::ConsumerRejected,
                .consumer_status = status};
    }
    frame_.emplace(std::move(*frame));
    plan_ = *plan.plan;
    handoff_ = *handoff.token;
    if (dropped_frames != nullptr) {
        dropped_frames_ = *dropped_frames;
    }
    state_ = State::Prepared;
    return {.status = Slus21050FemainMovieTransactionStatus::Ready};
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::admit_with_dropped_frame_batch(
    const guest::EeMemory& memory, const Slus21050MovieDroppedFrameBatch& dropped_frames) noexcept {
    return admit_impl(memory, &dropped_frames);
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::commit(guest::EeMemory& memory,
                                             guest::R5900State& cpu) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestPublished) {
        return publication_pending_result();
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (!plan_.has_value() || !handoff_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (dropped_frames_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                .dropped_frame_batch_status =
                    Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (!player_.can_complete_pending_frame(receipt)) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    const auto status = consumer_.commit_with_cpu_handoff(memory, cpu, *plan_, *frame_, *handoff_);
    if (status != Slus21050FemainMovieConsumerStatus::Ready) {
        return retain_after_rejection(status);
    }
    state_ = State::GuestCommitted;
    return complete_receipt();
}

Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::commit_with_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMovieTransactionBoundaries& boundaries) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestPublished) {
        return publication_pending_result();
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (!plan_.has_value() || !handoff_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (dropped_frames_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                .dropped_frame_batch_status =
                    Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding};
    }
    if (!ledger_session_matches(ledger)) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (!player_.can_complete_pending_frame(receipt)) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    const Slus21050FemainFrameTransactionEvidence evidence{
        .frame = {.media_ordinal = plan_->media_ordinal,
                  .source_sequence = plan_->source_sequence,
                  .source_checksum = plan_->source_checksum,
                  .produced_at = boundaries.produced_at},
        .published_at = boundaries.published_at,
        .graphics_completed_at = boundaries.graphics_completed_at,
        .consumed_at = boundaries.consumed_at,
        .vif1_before_gif = boundaries.vif1_before_gif,
    };
    const auto preflight = ledger.preflight_frame_transaction(evidence);
    if (!preflight.admitted()) {
        return retain_after_ledger_rejection(preflight.status);
    }
    const auto committed = consumer_.commit_with_cpu_handoff_and_ledger(
        memory, cpu, *plan_, *frame_, *handoff_, ledger, *preflight.ticket);
    if (committed.committed()) {
        state_ = State::GuestCommitted;
        committed_ledger_status_ = Slus21050FemainFrameLedgerStatus::Ok;
        return complete_receipt();
    }
    if (committed.ledger_status.has_value() &&
        *committed.ledger_status != Slus21050FemainFrameLedgerStatus::Ok) {
        return retain_after_ledger_rejection(*committed.ledger_status);
    }
    return retain_after_rejection(committed.consumer_status);
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::commit_drop_batch_with_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMovieTransactionBoundaries& current_frame_boundaries) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestPublished) {
        return publication_pending_result();
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (!plan_.has_value() || !handoff_.has_value() || !dropped_frames_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (!ledger_session_matches(ledger)) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (!player_.can_complete_pending_frame_and_acknowledge_dropped_frame_batch(receipt,
                                                                                 *dropped_frames_)) {
        auto batch_status = player_.validate_dropped_frame_batch(*dropped_frames_);
        if (batch_status == Slus21050MovieDroppedFrameBatchStatus::Ready) {
            batch_status = Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized;
        }
        return retain_after_batch_rejection(batch_status);
    }
    const auto frames = dropped_frames_->frames();
    std::array<Slus21050FemainFrameIdentity, kSlus21050FemainFrameTransactionCapacity - 1U>
        ledger_drops{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& dropped = frames[index];
        if (dropped.session_generation != frame_->lease().session_generation ||
            !dropped.dropped_at.has_value()) {
            return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBoundaryMismatch};
        }
        ledger_drops[index] = {.media_ordinal = dropped.timeline_ordinal,
                               .source_sequence = dropped.source_sequence,
                               .source_checksum = dropped.source_checksum,
                               .produced_at = *dropped.dropped_at};
    }
    const Slus21050FemainFrameTransactionEvidence evidence{
        .frame = {.media_ordinal = plan_->media_ordinal,
                  .source_sequence = plan_->source_sequence,
                  .source_checksum = plan_->source_checksum,
                  .produced_at = current_frame_boundaries.produced_at},
        .published_at = current_frame_boundaries.published_at,
        .graphics_completed_at = current_frame_boundaries.graphics_completed_at,
        .consumed_at = current_frame_boundaries.consumed_at,
        .vif1_before_gif = current_frame_boundaries.vif1_before_gif,
    };
    const auto preflight = ledger.preflight_drop_batch_frame_transaction(
        std::span<const Slus21050FemainFrameIdentity>{ledger_drops.data(), frames.size()},
        evidence);
    if (!preflight.admitted()) {
        return retain_after_ledger_rejection(preflight.status);
    }
    const auto committed = consumer_.commit_with_cpu_handoff_and_drop_batch_ledger(
        memory, cpu, *plan_, *frame_, *handoff_, ledger, *preflight.ticket);
    if (committed.committed()) {
        state_ = State::GuestCommitted;
        committed_ledger_status_ = Slus21050FemainFrameLedgerStatus::Ok;
        return complete_receipt();
    }
    if (committed.ledger_status.has_value() &&
        *committed.ledger_status != Slus21050FemainFrameLedgerStatus::Ok) {
        return retain_after_ledger_rejection(*committed.ledger_status);
    }
    return retain_after_rejection(committed.consumer_status);
}

Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::publish_with_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept {
    return publish_impl(memory, cpu, ledger, boundaries);
}

// RECOVERY: behaviour is exact, but GCC lays out the early failure return after the
// following statement in the original GIMPLE; every source ordering tried puts it first.
Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::publish_impl(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestPublished) {
        return publication_pending_result();
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (!plan_.has_value() || !handoff_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (dropped_frames_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
                .dropped_frame_batch_status =
                    Slus21050MovieDroppedFrameBatchStatus::AuthorizedFrameOutstanding};
    }
    if (!ledger_session_matches(ledger)) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (!player_.can_complete_pending_frame(receipt)) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    const Slus21050FemainFramePublicationEvidence evidence{
        .frame = {.media_ordinal = plan_->media_ordinal,
                  .source_sequence = plan_->source_sequence,
                  .source_checksum = plan_->source_checksum,
                  .produced_at = boundaries.produced_at},
        .published_at = boundaries.published_at,
    };
    const auto preflight = ledger.preflight_frame_publication(evidence);
    if (!preflight.admitted()) {
        return retain_after_ledger_rejection(preflight.status);
    }
    const auto packet =
        make_publication_packet_candidate(preflight.ticket->reservation(), ledger.snapshot());
    if (!packet.has_value()) {
        return retain_after_rejection(Slus21050FemainMovieConsumerStatus::HandoffRangeInvalid);
    }
    const auto committed = consumer_.commit_with_cpu_handoff_and_publication_ledger(
        memory, cpu, *plan_, *frame_, *handoff_, ledger, *preflight.ticket);
    if (!committed.committed()) {
        if (committed.ledger_status.has_value() &&
            *committed.ledger_status != Slus21050FemainFrameLedgerStatus::Ok) {
            return retain_after_ledger_rejection(*committed.ledger_status);
        }
        return retain_after_rejection(committed.consumer_status);
    }
    published_reservation_ = preflight.ticket->reservation();
    published_ledger_ = &ledger;
    const auto published_snapshot = ledger.snapshot();
    published_ledger_identity_ = published_snapshot.ledger_identity;
    published_ledger_session_ = published_snapshot.session_generation;
    commit_publication_packet(*packet);
    committed_ledger_status_ = Slus21050FemainFrameLedgerStatus::Ok;
    state_ = State::GuestPublished;
    return publication_pending_result();
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::publish_drop_batch_with_ledger(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept {
    return publish_drop_batch_impl(memory, cpu, ledger, boundaries);
}

// RECOVERY: behaviour is exact, but GCC lays out the early failure return after the
// following statement in the original GIMPLE; every source ordering tried puts it first.
Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::publish_drop_batch_impl(
    guest::EeMemory& memory, guest::R5900State& cpu,
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestPublished) {
        return publication_pending_result();
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (!plan_.has_value() || !handoff_.has_value() || !dropped_frames_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost};
    }
    if (!ledger_session_matches(ledger)) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (!player_.can_complete_pending_frame_and_acknowledge_dropped_frame_batch(receipt,
                                                                                 *dropped_frames_)) {
        auto batch_status = player_.validate_dropped_frame_batch(*dropped_frames_);
        if (batch_status == Slus21050MovieDroppedFrameBatchStatus::Ready) {
            batch_status = Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized;
        }
        return retain_after_batch_rejection(batch_status);
    }
    const auto frames = dropped_frames_->frames();
    std::array<Slus21050FemainFrameIdentity, kSlus21050FemainFrameTransactionCapacity - 1U>
        ledger_drops{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& dropped = frames[index];
        if (dropped.session_generation != frame_->lease().session_generation ||
            !dropped.dropped_at.has_value()) {
            return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBoundaryMismatch};
        }
        ledger_drops[index] = {.media_ordinal = dropped.timeline_ordinal,
                               .source_sequence = dropped.source_sequence,
                               .source_checksum = dropped.source_checksum,
                               .produced_at = *dropped.dropped_at};
    }
    const Slus21050FemainFramePublicationEvidence evidence{
        .frame = {.media_ordinal = plan_->media_ordinal,
                  .source_sequence = plan_->source_sequence,
                  .source_checksum = plan_->source_checksum,
                  .produced_at = boundaries.produced_at},
        .published_at = boundaries.published_at,
    };
    const auto preflight = ledger.preflight_drop_batch_frame_publication(
        std::span<const Slus21050FemainFrameIdentity>{ledger_drops.data(), frames.size()},
        evidence);
    if (!preflight.admitted()) {
        return retain_after_ledger_rejection(preflight.status);
    }
    const auto packet =
        make_publication_packet_candidate(preflight.ticket->reservation(), ledger.snapshot());
    if (!packet.has_value()) {
        return retain_after_rejection(Slus21050FemainMovieConsumerStatus::HandoffRangeInvalid);
    }
    const auto committed = consumer_.commit_with_cpu_handoff_and_drop_batch_publication_ledger(
        memory, cpu, *plan_, *frame_, *handoff_,
        std::span<const Slus21050FemainFrameIdentity>{ledger_drops.data(), frames.size()}, ledger,
        *preflight.ticket);
    if (!committed.committed()) {
        if (committed.ledger_status.has_value() &&
            *committed.ledger_status != Slus21050FemainFrameLedgerStatus::Ok) {
            return retain_after_ledger_rejection(*committed.ledger_status);
        }
        return retain_after_rejection(committed.consumer_status);
    }
    published_reservation_ = preflight.ticket->reservation();
    published_ledger_ = &ledger;
    const auto published_snapshot = ledger.snapshot();
    published_ledger_identity_ = published_snapshot.ledger_identity;
    published_ledger_session_ = published_snapshot.session_generation;
    commit_publication_packet(*packet);
    committed_ledger_status_ = Slus21050FemainFrameLedgerStatus::Ok;
    state_ = State::GuestPublished;
    return publication_pending_result();
}

Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::retire_with_ledger(
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept {
    return retire_impl(ledger, boundaries);
}

// RECOVERY: behaviour is exact, but GCC lays out the early failure return after the
// following statement in the original GIMPLE; every source ordering tried puts it first.
Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::retire_impl(
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept {
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_};
    }
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ == State::GuestCommitted) {
        return complete_receipt();
    }
    if (state_ != State::GuestPublished) {
        return {.status = Slus21050FemainMovieTransactionStatus::CompletionPending};
    }
    if (!plan_.has_value() || !published_reservation_.has_value() || published_ledger_ == nullptr ||
        !published_ledger_identity_.has_value() || !published_ledger_session_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
                .ledger_status = Slus21050FemainFrameLedgerStatus::StaleTransaction};
    }
    if (!player_session_.has_value() || player_.session_generation() == 0U ||
        player_.session_generation() != *player_session_ ||
        frame_->lease().session_generation != *player_session_) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_};
    }
    if (!published_ledger_matches(ledger)) {
        const auto snapshot = ledger.snapshot();
        if (&ledger != published_ledger_ || !published_ledger_identity_.has_value() ||
            snapshot.ledger_identity != *published_ledger_identity_ ||
            !published_ledger_session_.has_value() ||
            snapshot.session_generation != *published_ledger_session_) {
            return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch,
                    .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
                    .ledger_status = committed_ledger_status_};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
                .ledger_status = Slus21050FemainFrameLedgerStatus::StaleTransaction};
    }
    const auto receipt = frame_->completion_receipt(kStagedGsBase);
    if (dropped_frames_.has_value()) {
        if (!player_.can_complete_pending_frame_and_acknowledge_dropped_frame_batch(
                receipt, *dropped_frames_)) {
            auto batch_status = player_.validate_dropped_frame_batch(*dropped_frames_);
            if (batch_status == Slus21050MovieDroppedFrameBatchStatus::Ready) {
                batch_status = Slus21050MovieDroppedFrameBatchStatus::BatchNotAuthorized;
            }
            return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_,
            .dropped_frame_batch_status = batch_status};
        }
    } else if (!player_.can_complete_pending_frame(receipt)) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_};
    }
    const Slus21050FemainFrameRetirementEvidence evidence{
        .reservation = *published_reservation_,
        .graphics_completed_at = boundaries.graphics_completed_at,
        .consumed_at = boundaries.consumed_at,
        .vif1_before_gif = boundaries.vif1_before_gif,
    };
    const auto preflight = ledger.preflight_frame_retirement(evidence);
    if (!preflight.admitted()) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = preflight.status};
    }
    const auto retired = ledger.commit_frame_retirement(*preflight.ticket);
    if (!retired.retired()) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = retired.status};
    }
    clear_published_ledger_binding();
    state_ = State::GuestCommitted;
    committed_ledger_status_ = Slus21050FemainFrameLedgerStatus::Ok;
    return complete_receipt();
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::cancel_published_with_ledger(
    Slus21050FemainFrameTransactionLedger& ledger,
    const Slus21050FemainFrameLedgerCancelReason reason) noexcept {
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (state_ != State::GuestPublished) {
        return {.status = Slus21050FemainMovieTransactionStatus::CancellationRejected};
    }
    if (!plan_.has_value() || !published_reservation_.has_value() || published_ledger_ == nullptr ||
        !published_ledger_identity_.has_value() || !published_ledger_session_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
                .ledger_status = Slus21050FemainFrameLedgerStatus::StaleReservation};
    }
    if (!published_ledger_matches(ledger)) {
        const auto snapshot = ledger.snapshot();
        if (&ledger != published_ledger_ || !published_ledger_identity_.has_value() ||
            snapshot.ledger_identity != *published_ledger_identity_ ||
            !published_ledger_session_.has_value() ||
            snapshot.session_generation != *published_ledger_session_) {
            return {.status = Slus21050FemainMovieTransactionStatus::LedgerSessionMismatch,
                    .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
                    .ledger_status = committed_ledger_status_};
        }
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
                .ledger_status = Slus21050FemainFrameLedgerStatus::StaleReservation};
    }
    const auto snapshot = ledger.snapshot();
    const auto& reservation = *published_reservation_;
    if (reservation.slot >= snapshot.slots.size() ||
        snapshot.slots[reservation.slot] != Slus21050FemainFrameLedgerSlotState::Published) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
                .ledger_status = Slus21050FemainFrameLedgerStatus::StaleReservation};
    }
    if (!player_identity_matches()) {
        return {.status = Slus21050FemainMovieTransactionStatus::PlayerOwnershipLost,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = committed_ledger_status_};
    }
    if (player_.session_generation() == 0U ||
        player_.session_generation() == reservation.session_generation ||
        !player_session_.has_value() || player_.session_generation() == *player_session_) {
        return {.status = Slus21050FemainMovieTransactionStatus::CancellationRejected};
    }
    const auto cancel_status = ledger.cancel_and_drain(reservation, reason);
    if (cancel_status != Slus21050FemainFrameLedgerStatus::Ok) {
        return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = cancel_status};
    }
    clear();
    return {.status = Slus21050FemainMovieTransactionStatus::Cancelled,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = cancel_status};
}

Slus21050FemainMovieTransactionResult Slus21050FemainMovieTransactionOwner::retain() noexcept {
    if (state_ == State::Empty || !frame_.has_value()) {
        return {.status = Slus21050FemainMovieTransactionStatus::Empty};
    }
    if (guest_commit_complete()) {
        return {.status = Slus21050FemainMovieTransactionStatus::AlreadyCommitted};
    }
    if (!player_.retain_pending_frame(std::move(*frame_))) {
        return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed};
    }
    clear();
    return {.status = Slus21050FemainMovieTransactionStatus::Retained};
}

bool Slus21050FemainMovieTransactionOwner::occupied() const noexcept {
    return state_ != State::Empty;
}

bool Slus21050FemainMovieTransactionOwner::guest_commit_complete() const noexcept {
    return state_ == State::GuestPublished || state_ == State::GuestCommitted;
}

std::optional<Slus21050MovieFrameLease> Slus21050FemainMovieTransactionOwner::lease() const noexcept {
    if (!frame_.has_value()) {
        return std::nullopt;
    }
    return frame_->lease();
}

std::uint64_t Slus21050FemainMovieTransactionOwner::player_identity() const noexcept {
    return bound_player_identity_;
}

std::optional<std::uint64_t>
Slus21050FemainMovieTransactionOwner::player_session_generation() const noexcept {
    return player_session_;
}

std::optional<Slus21050FemainMovieConsumerPlan>
Slus21050FemainMovieTransactionOwner::consumer_plan() const noexcept {
    if (!plan_.has_value()) {
        return std::nullopt;
    }
    return *plan_;
}

std::optional<Slus21050FemainHandoffToken>
Slus21050FemainMovieTransactionOwner::handoff_token() const noexcept {
    if (!handoff_.has_value()) {
        return std::nullopt;
    }
    return *handoff_;
}

bool Slus21050FemainMovieTransactionOwner::publication_pending() const noexcept {
    return state_ == State::GuestPublished;
}

std::optional<Slus21050FemainFrameReservation>
Slus21050FemainMovieTransactionOwner::published_reservation() const noexcept {
    return published_reservation_;
}

std::optional<std::uint64_t>
Slus21050FemainMovieTransactionOwner::published_ledger_identity() const noexcept {
    return published_ledger_identity_;
}

std::optional<std::uint64_t>
Slus21050FemainMovieTransactionOwner::published_ledger_session_generation() const noexcept {
    return published_ledger_session_;
}

bool Slus21050FemainMovieTransactionOwner::ledger_session_matches(
    const Slus21050FemainFrameTransactionLedger& ledger) const noexcept {
    if (!player_identity_matches() || !frame_.has_value() || !player_session_.has_value()) {
        return false;
    }
    const auto player_session = player_.session_generation();
    const auto ledger_session = ledger.snapshot().session_generation;
    return player_session != 0U && *player_session_ == player_session &&
           *player_session_ == frame_->lease().session_generation &&
           *player_session_ == ledger_session;
}

bool Slus21050FemainMovieTransactionOwner::published_ledger_matches(
    const Slus21050FemainFrameTransactionLedger& ledger) const noexcept {
    if (published_ledger_ != &ledger || !published_reservation_.has_value() ||
        !published_ledger_identity_.has_value() || !published_ledger_session_.has_value() ||
        !frame_.has_value() || !plan_.has_value()) {
        return false;
    }
    const auto snapshot = ledger.snapshot();
    const auto& reservation = *published_reservation_;
    return snapshot.ledger_identity == *published_ledger_identity_ &&
           snapshot.session_generation == *published_ledger_session_ &&
           reservation.ledger_identity == *published_ledger_identity_ &&
           reservation.session_generation == *published_ledger_session_ &&
           reservation.frame.media_ordinal == plan_->media_ordinal &&
           reservation.frame.source_sequence == plan_->source_sequence &&
           reservation.frame.source_checksum == plan_->source_checksum &&
           frame_->lease().session_generation == *published_ledger_session_;
}

bool Slus21050FemainMovieTransactionOwner::player_identity_matches() const noexcept {
    return bound_player_identity_ != 0U && player_.identity() == bound_player_identity_;
}

std::optional<Slus21050FemainMoviePublicationPacket>
Slus21050FemainMovieTransactionOwner::publication_packet() const noexcept {
    if (state_ != State::GuestPublished || !publication_packet_.has_value()) {
        return std::nullopt;
    }
    return publication_packet_;
}

bool Slus21050FemainMovieTransactionOwner::publication_packet_matches(
    const Slus21050FemainMoviePublicationPacket& packet) const noexcept {
    return state_ == State::GuestPublished && publication_packet_.has_value() &&
           *publication_packet_ == packet && player_identity_matches() && player_.active() &&
           player_.session_generation() == packet.player_session_generation;
}

void Slus21050FemainMovieTransactionOwner::clear() noexcept {
    committed_ledger_status_.reset();
    clear_published_ledger_binding();
    dropped_frames_.reset();
    handoff_.reset();
    plan_.reset();
    frame_.reset();
    state_ = State::Empty;
}

void Slus21050FemainMovieTransactionOwner::clear_published_ledger_binding() noexcept {
    publication_packet_.reset();
    published_ledger_session_.reset();
    published_ledger_identity_.reset();
    published_reservation_.reset();
    published_ledger_ = nullptr;
}

std::optional<Slus21050FemainMoviePublicationPacket>
Slus21050FemainMovieTransactionOwner::make_publication_packet_candidate(
    const Slus21050FemainFrameReservation& reservation,
    const Slus21050FemainFrameLedgerSnapshot& ledger_snapshot) const noexcept {
    if (next_publication_serial_ == 0U || owner_identity_ == 0U || bound_player_identity_ == 0U ||
        !player_session_.has_value() || !plan_.has_value() || !frame_.has_value() ||
        !handoff_.has_value() || ledger_snapshot.ledger_identity == 0U ||
        ledger_snapshot.session_generation == 0U ||
        ledger_snapshot.ledger_identity != reservation.ledger_identity ||
        ledger_snapshot.session_generation != reservation.session_generation) {
        return std::nullopt;
    }
    const auto next_index = static_cast<std::uint8_t>(handoff_->buffer_index_before ^ 1U);
    return Slus21050FemainMoviePublicationPacket{
        .owner_identity = owner_identity_,
        .publication_serial = next_publication_serial_,
        .player_identity = bound_player_identity_,
        .player_session_generation = *player_session_,
        .ledger_identity = ledger_snapshot.ledger_identity,
        .ledger_session_generation = ledger_snapshot.session_generation,
        .published_reservation = reservation,
        .plan = {.layout_generation = plan_->layout.generation,
                 .first_buffer = plan_->first_buffer,
                 .second_buffer = plan_->second_buffer,
                 .buffer_bytes = plan_->buffer_bytes,
                 .frame_generation = plan_->frame_generation,
                 .media_ordinal = plan_->media_ordinal,
                 .source_sequence = plan_->source_sequence,
                 .source_checksum = plan_->source_checksum,
                 .payload_digest = plan_->payload_digest,
                 .player_lease = plan_->player_lease,
                 .timeline_ordinal = plan_->timeline_ordinal},
        .queue = {.queue_read = handoff_->queue_table[handoff_->buffer_index_before],
                  .queue_end = handoff_->pending_end,
                  .transfers_active = handoff_->busy_before,
                  .buffer_index = next_index,
                  .queue_base = handoff_->buffer_address_base},
    };
}

void Slus21050FemainMovieTransactionOwner::commit_publication_packet(
    const Slus21050FemainMoviePublicationPacket& packet) noexcept {
    publication_packet_ = packet;
    next_publication_serial_ = packet.publication_serial == std::numeric_limits<std::uint64_t>::max()
                                   ? 0U
                                   : packet.publication_serial + 1U;
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::retain_after_rejection(
    const Slus21050FemainMovieConsumerStatus status) noexcept {
    if (!player_.retain_pending_frame(std::move(*frame_))) {
        return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
                .consumer_status = status};
    }
    clear();
    return {.status = Slus21050FemainMovieTransactionStatus::ConsumerRejected,
            .consumer_status = status};
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::retain_after_ledger_rejection(
    const Slus21050FemainFrameLedgerStatus status) noexcept {
    if (!player_.retain_pending_frame(std::move(*frame_))) {
        return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = status};
    }
    clear();
    return {.status = Slus21050FemainMovieTransactionStatus::LedgerRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .ledger_status = status};
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::retain_after_batch_rejection(
    const Slus21050MovieDroppedFrameBatchStatus status) noexcept {
    if (frame_.has_value() && player_.retain_pending_frame(std::move(*frame_))) {
        clear();
        return {.status = Slus21050FemainMovieTransactionStatus::DroppedFrameBatchRejected,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .dropped_frame_batch_status = status};
    }
    return {.status = Slus21050FemainMovieTransactionStatus::RetainFailed,
            .consumer_status = Slus21050FemainMovieConsumerStatus::Ready,
            .dropped_frame_batch_status = status};
}

Slus21050FemainMovieTransactionResult
Slus21050FemainMovieTransactionOwner::publication_pending_result() const noexcept {
    Slus21050FemainMovieTransactionResult result{
        .status = Slus21050FemainMovieTransactionStatus::CompletionPending,
        .ledger_status = committed_ledger_status_,
        .publication_packet = publication_packet_};
    if (dropped_frames_.has_value()) {
        result.dropped_frame_batch_status = Slus21050MovieDroppedFrameBatchStatus::Ready;
    }
    return result;
}

} // namespace takedown::runtime
