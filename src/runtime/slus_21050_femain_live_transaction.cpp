#include "takedown/runtime/slus_21050_femain_live_transaction.hpp"
#include "takedown/runtime/slus_21050_femain_serialized_token.hpp"
#include "takedown/runtime/slus_21050_femain_retail_consume_invocation_barrier.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

std::string_view to_string(const Slus21050FemainLiveTransactionStatus status) noexcept {
    switch (status) {
    case Slus21050FemainLiveTransactionStatus::Disabled:
        return "disabled";
    case Slus21050FemainLiveTransactionStatus::AuthorityUnavailable:
        return "authority-unavailable";
    case Slus21050FemainLiveTransactionStatus::AuthorityRejected:
        return "authority-rejected";
    case Slus21050FemainLiveTransactionStatus::PlayerInactive:
        return "player-inactive";
    case Slus21050FemainLiveTransactionStatus::SessionNotStarted:
        return "session-not-started";
    case Slus21050FemainLiveTransactionStatus::SessionAlreadyStarted:
        return "session-already-started";
    case Slus21050FemainLiveTransactionStatus::SessionMismatch:
        return "session-mismatch";
    case Slus21050FemainLiveTransactionStatus::SessionStarted:
        return "session-started";
    case Slus21050FemainLiveTransactionStatus::PreparationRejected:
        return "preparation-rejected";
    case Slus21050FemainLiveTransactionStatus::OwnerRejected:
        return "owner-rejected";
    case Slus21050FemainLiveTransactionStatus::Published:
        return "published";
    case Slus21050FemainLiveTransactionStatus::RendererAwaitingTerminal:
        return "renderer-awaiting-terminal";
    case Slus21050FemainLiveTransactionStatus::RendererRejected:
        return "renderer-rejected";
    case Slus21050FemainLiveTransactionStatus::RendererPending:
        return "renderer-pending";
    case Slus21050FemainLiveTransactionStatus::RendererReady:
        return "renderer-ready";
    case Slus21050FemainLiveTransactionStatus::Retired:
        return "retired";
    case Slus21050FemainLiveTransactionStatus::Cancelled:
        return "cancelled";
    case Slus21050FemainLiveTransactionStatus::CancellationRejected:
        return "cancellation-rejected";
    }
    return "unknown";
}


Slus21050FemainLiveTransaction::Slus21050FemainLiveTransaction(Slus21050MoviePlayer& player,
                                                               const Authority* authority, bool enabled) noexcept
    : player_(player), owner_(player), authority_(authority), enabled_(enabled) {}

Slus21050FemainLiveTransaction::~Slus21050FemainLiveTransaction() = default;

Slus21050FemainLiveTransaction::Result Slus21050FemainLiveTransaction::make_result(const Status status) const noexcept {
    return Result{.status = status};
}

bool Slus21050FemainLiveTransaction::ledger_session_matches_player() const noexcept {
    if (!session_started_) {
        return false;
    }
    if (player_.active()) {
        const Slus21050FemainFrameLedgerSnapshot snapshot = ledger_.snapshot();
        if (snapshot.state == Slus21050FemainFrameLedgerState::Active) {
            if (snapshot.session_generation != 0U) {
                if (snapshot.session_generation == player_.session_generation()) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Slus21050FemainLiveTransaction::adapter_can_be_replaced() const noexcept {
    if (adapter_.has_value()) {
        if (adapter_->phase() == Adapter::Phase::Retired) {
            return true;
        }
        if (adapter_->phase() == Adapter::Phase::Failed) {
            return true;
        }
        return false;
    }
    return true;
}

void Slus21050FemainLiveTransaction::reset_adapter_if_finished() noexcept {
    if (adapter_.has_value()) {
        if (adapter_->phase() == Adapter::Phase::Retired || adapter_->phase() == Adapter::Phase::Failed) {
            adapter_.reset();
        }
    }
}


Slus21050FemainLiveTransaction::Result Slus21050FemainLiveTransaction::begin_session() noexcept {
    if (!enabled_) {
        return make_result(Status::Disabled);
    }
    if (!player_.active()) {
        return make_result(Status::PlayerInactive);
    }
    if (session_started_) {
        return make_result(Status::SessionAlreadyStarted);
    }
    const Slus21050FemainFrameLedgerBeginResult begin = ledger_.begin_session(player_.session_generation());
    Slus21050FemainLiveTransactionResult result;
    if (begin.status != Slus21050FemainFrameLedgerStatus::Ok) {
        result.status = Status::SessionMismatch;
        result.ledger_status = begin.status;
        return result;
    }
    session_started_ = true;
    result.status = Status::SessionStarted;
    result.ledger_status = begin.status;
    return result;
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::submit_renderer(const Context& context) noexcept {
    if (enabled_ && adapter_.has_value()) {
        const Slus21050FemainRendererRetirementAdapterResult submitted = adapter_->submit(context);
        const Slus21050FemainLiveTransactionStatus status = submitted.status == Slus21050FemainRendererRetirementAdapterStatus::Pending ? Status::RendererPending
                                                                            : Status::RendererRejected;
        Slus21050FemainLiveTransactionResult result;
        result.status = status;
        result.renderer = submitted;
        return result;
    }
    return make_result(Status::RendererRejected);
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::poll_renderer(const Context& context) noexcept {
    if (enabled_ && adapter_.has_value()) {
        const Slus21050FemainRendererRetirementAdapterResult polled = adapter_->poll(context);
        Slus21050FemainLiveTransactionStatus status = Status::RendererRejected;
        if (polled.status == Slus21050FemainRendererRetirementAdapterStatus::Pending) {
            status = Status::RendererPending;
        } else if (polled.status == Slus21050FemainRendererRetirementAdapterStatus::Ok) {
            status = Status::RendererReady;
        }
        Slus21050FemainLiveTransactionResult result;
        result.status = status;
        result.renderer = polled;
        return result;
    }
    return make_result(Status::RendererRejected);
}

Slus21050FemainLiveTransaction::Adapter::Phase Slus21050FemainLiveTransaction::renderer_phase() const noexcept {
    if (adapter_.has_value()) {
        return adapter_->phase();
    }
    return Adapter::Phase::Empty;
}


Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::bind_renderer(const Context& context, const Witness& witness) noexcept {
    if (enabled_ && session_started_ && owner_.publication_pending() && adapter_.has_value()) {
        const Slus21050FemainRendererRetirementAdapterResult bound = adapter_->bind(context, witness);
        Slus21050FemainLiveTransactionResult result;
        const Status status = bound.status == Slus21050FemainRendererRetirementAdapterStatus::Ok
                                  ? Status::RendererReady
                                  : Status::RendererRejected;
        result.status = status;
        result.renderer = bound;
        return result;
    }
    return make_result(Status::RendererRejected);
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::bind_renderer(const Context& context,
                                              const Slus21050FemainSerializedSourceTerminalToken& token) noexcept {
    if (token.published_reservation.has_value() && token.authentication_tag != 0 &&
        token.authentication_tag == authenticate_slus_21050_femain_serialized_source_terminal_token(token) &&
        token.source.source_evidence.resolver_proof.has_value()) {
        const Slus21050FemainSerializedTokenResult rebuilt =
            make_slus_21050_femain_serialized_source_terminal_token(token.source, token.terminal);
        if (rebuilt.failure == Slus21050FemainSerializedTokenFailure::None && rebuilt.token.has_value() &&
            rebuilt.token->authentication_tag == token.authentication_tag &&
            rebuilt.token->published_reservation.has_value() && token.published_reservation.has_value()) {
            const Slus21050FemainFrameReservation& reserved = *token.published_reservation;
            const Slus21050FemainFrameReservation& rebuilt_reserved = *rebuilt.token->published_reservation;
            if (rebuilt_reserved.ledger_identity == reserved.ledger_identity &&
                rebuilt_reserved.session_generation == reserved.session_generation &&
                rebuilt_reserved.frame_generation == reserved.frame_generation &&
                rebuilt_reserved.slot == reserved.slot &&
                rebuilt_reserved.frame.media_ordinal == reserved.frame.media_ordinal &&
                rebuilt_reserved.frame.source_sequence == reserved.frame.source_sequence &&
                rebuilt_reserved.frame.source_checksum == reserved.frame.source_checksum &&
                rebuilt_reserved.frame.produced_at.guest_vblank == reserved.frame.produced_at.guest_vblank &&
                rebuilt_reserved.frame.produced_at.audio_sample == reserved.frame.produced_at.audio_sample) {
                return bind_renderer(context, token.terminal.terminal_witness);
            }
        }
    }
    return make_result(Status::RendererRejected);
}

bool Slus21050FemainLiveTransaction::consume_witness_matches(
    const Slus21050FemainRetailConsumeCompletionWitness& witness) const noexcept {
    return active_publication_packet_.has_value() && witness.valid() && witness.identity().valid() &&
           witness.publication_packet() == *active_publication_packet_ &&
           witness.binding().owner.owner_identity == active_publication_packet_->owner_identity &&
           witness.binding().owner.player_identity == active_publication_packet_->player_identity &&
           witness.binding().owner.player_session_generation == active_publication_packet_->player_session_generation &&
           witness.binding().owner.ledger_identity == active_publication_packet_->ledger_identity &&
           witness.binding().owner.ledger_session_generation == active_publication_packet_->ledger_session_generation &&
           witness.binding().published_reservation == active_publication_packet_->published_reservation &&
           witness.binding().publication_serial == active_publication_packet_->publication_serial &&
           witness.binding().runtime_epoch == witness.identity().runtime_epoch &&
           witness.binding().observation_epoch == witness.identity().observation_epoch &&
           witness.binding().invocation_serial == witness.identity().invocation_serial &&
           witness.binding().guest_execution_context_identity == witness.identity().guest_execution_context_identity &&
           witness.binding().published_queue_read == active_publication_packet_->queue.queue_read &&
           witness.binding().published_queue_end == active_publication_packet_->queue.queue_end &&
           witness.binding().published_buffer_index == active_publication_packet_->queue.buffer_index &&
           witness.binding().published_queue_base == active_publication_packet_->queue.queue_base;
}

Slus21050FemainLiveTransaction::Result Slus21050FemainLiveTransaction::step(
    guest::EeMemory& memory, guest::R5900State& cpu, std::uint64_t elapsed_nanoseconds,
    Slus21050FemainFrameGuestBoundary dropped_at,
    const Slus21050FemainMoviePublicationBoundaries& boundaries) noexcept {
    if (!enabled_) {
        return make_result(Status::Disabled);
    }
    if (authority_ == nullptr || !authority_->ready_for_publication()) {
        return make_result(Status::AuthorityUnavailable);
    }
    if (!player_.active()) {
        return make_result(Status::PlayerInactive);
    }
    if (!session_started_) {
        return make_result(Status::SessionNotStarted);
    }
    if (!ledger_session_matches_player()) {
        return make_result(Status::SessionMismatch);
    }
    if (owner_.occupied()) {
        return make_result(Status::OwnerRejected);
    }
    if (!adapter_can_be_replaced()) {
        return make_result(Status::OwnerRejected);
    }
    reset_adapter_if_finished();
    const Slus21050MoviePlayerStatus prepared =
        player_.prepare_at_media_time_with_guest_boundary(elapsed_nanoseconds, dropped_at);
    if (prepared != Slus21050MoviePlayerStatus::FrameStaged || !player_.has_pending_frame()) {
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::PreparationRejected;
        result.player_status = prepared;
        return result;
    }
    const Slus21050FemainMovieTransactionResult admitted = owner_.admit(memory);
    if (admitted.status != Slus21050FemainMovieTransactionStatus::Ready) {
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::OwnerRejected;
        result.player_status = Slus21050MoviePlayerStatus::FrameStaged;
        result.transaction = admitted;
        return result;
    }
    if (!authority_->authorize_publication(owner_, ledger_, memory)) {
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::AuthorityRejected;
        result.player_status = Slus21050MoviePlayerStatus::FrameStaged;
        result.transaction = owner_.retain();
        return result;
    }
    const Slus21050FemainMovieTransactionResult published =
        owner_.publish_with_ledger(memory, cpu, ledger_, boundaries);
    if (published.status != Slus21050FemainMovieTransactionStatus::CompletionPending) {
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::OwnerRejected;
        result.player_status = Slus21050MoviePlayerStatus::FrameStaged;
        result.transaction = published;
        return result;
    }
    adapter_.emplace();
    Slus21050FemainLiveTransactionResult result;
    result.status = Status::Published;
    result.player_status = Slus21050MoviePlayerStatus::FrameStaged;
    result.transaction = published;
    active_publication_packet_ = owner_.publication_packet();
    return result;
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::retire_renderer(const Context& context,
                                                const Slus21050FemainMovieRetirementBoundaries& boundaries) noexcept {
    return make_result(Status::RendererRejected);
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::retire_renderer(const Context& context,
                                                const Slus21050FemainMovieRetirementBoundaries& boundaries,
                                                const Slus21050FemainRetailConsumeCompletionWitness& consume_witness) noexcept {
    if (enabled_ && adapter_.has_value()) {
        if (!consume_witness_matches(consume_witness)) {
            return make_result(Status::RendererRejected);
        }
        const std::optional<Slus21050FemainMoviePublicationPacket> retired_packet = active_publication_packet_;
        const Slus21050FemainRendererRetirementAdapterResult retired = adapter_->retire(context, boundaries);
        const Status status = retired.status == Slus21050FemainRendererRetirementAdapterStatus::Pending
                                  ? Status::RendererPending
                                  : (retired.status == Slus21050FemainRendererRetirementAdapterStatus::Ok
                                         ? Status::Retired
                                         : Status::RendererRejected);
        Slus21050FemainLiveTransactionResult result;
        result.status = status;
        result.player_status = Slus21050MoviePlayerStatus{};
        result.renderer = retired;
        if (retired.retirement.has_value()) {
            result.transaction = retired.retirement;
        }
        if (status == Status::Retired) {
            if (retired_packet.has_value()) {
                result.retirement_witness = Slus21050FemainLiveRetirementWitness(*retired_packet);
                active_publication_packet_.reset();
            }
        }
        return result;
    }
    return make_result(Status::RendererRejected);
}

Slus21050FemainLiveTransaction::Result Slus21050FemainLiveTransaction::advance_renderer(
    const Context& context, const Slus21050FemainSerializedSourceTerminalToken* token,
    const Slus21050FemainMovieRetirementBoundaries* retirement_boundaries) noexcept {
    return advance_renderer(context, token, nullptr, retirement_boundaries);
}

Slus21050FemainLiveTransaction::Result Slus21050FemainLiveTransaction::advance_renderer(
    const Context& context, const Slus21050FemainSerializedSourceTerminalToken* token,
    const Slus21050FemainRetailConsumeCompletionWitness* consume_witness,
    const Slus21050FemainMovieRetirementBoundaries* retirement_boundaries) noexcept {
    if (!enabled_ || !session_started_) {
        return make_result(Status::RendererRejected);
    }
    if (consume_witness != nullptr) {
        if (!adapter_.has_value()) {
            return make_result(Status::RendererRejected);
        }
        if (adapter_->phase() != Adapter::Phase::CompletionReady &&
            adapter_->phase() != Adapter::Phase::ReceiptPending) {
            return make_result(Status::RendererRejected);
        }
    }
    if (adapter_.has_value() && adapter_->phase() != Adapter::Phase::Empty) {
        if (token != nullptr) {
            return make_result(Status::RendererRejected);
        }
    } else {
        if (!owner_.publication_pending()) {
            return make_result(Status::RendererRejected);
        }
        if (token == nullptr) {
            return make_result(Status::RendererAwaitingTerminal);
        }
        const Result bound = bind_renderer(context, *token);
        if (bound.status == Status::RendererReady) {
            return bound;
        }
    }
    switch (adapter_->phase()) {
    case Adapter::Phase::Empty:
        return make_result(Status::RendererRejected);
    case Adapter::Phase::Bound:
        if (enabled_ && adapter_.has_value()) {
            return submit_renderer(context);
        }
        return make_result(Status::RendererRejected);
    case Adapter::Phase::Submitted:
        if (enabled_ && adapter_.has_value()) {
            return poll_renderer(context);
        }
        return make_result(Status::RendererRejected);
    case Adapter::Phase::CompletionReady:
        if (retirement_boundaries == nullptr) {
            return make_result(Status::RendererReady);
        }
        if (consume_witness == nullptr) {
            return make_result(Status::RendererRejected);
        }
        if (enabled_ && adapter_.has_value()) {
            return retire_renderer(context, *retirement_boundaries, *consume_witness);
        }
        return make_result(Status::RendererRejected);
    case Adapter::Phase::ReceiptPending:
        if (retirement_boundaries == nullptr) {
            return make_result(Status::RendererPending);
        }
        if (consume_witness == nullptr) {
            return make_result(Status::RendererRejected);
        }
        if (enabled_ && adapter_.has_value()) {
            return retire_renderer(context, *retirement_boundaries, *consume_witness);
        }
        return make_result(Status::RendererRejected);
    case Adapter::Phase::Retired:
        return make_result(Status::Retired);
    case Adapter::Phase::Failed:
    default:
        return make_result(Status::RendererRejected);
    }
}

Slus21050FemainLiveTransaction::Result
Slus21050FemainLiveTransaction::cancel_after_player_close(Slus21050FemainFrameLedgerCancelReason reason) noexcept {
    if (!enabled_) {
        return make_result(Status::Disabled);
    }
    if (!session_started_) {
        return make_result(Status::SessionNotStarted);
    }
    if (owner_.publication_pending()) {
        const std::optional<Slus21050FemainMoviePublicationPacket> publication_packet = owner_.publication_packet();
        const Slus21050FemainMovieTransactionResult cancelled = owner_.cancel_published_with_ledger(ledger_, reason);
        if (cancelled.status != Slus21050FemainMovieTransactionStatus::Cancelled) {
            Slus21050FemainLiveTransactionResult result;
            result.status = Status::CancellationRejected;
            result.player_status = Slus21050MoviePlayerStatus{};
            result.transaction = cancelled;
            result.ledger_status = cancelled.ledger_status;
            return result;
        }
        adapter_.reset();
        active_publication_packet_.reset();
        session_started_ = false;
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::Cancelled;
        result.player_status = Slus21050MoviePlayerStatus{};
        result.transaction = cancelled;
        result.ledger_status = cancelled.ledger_status;
        if (publication_packet.has_value()) {
            result.cancellation_witness = Slus21050FemainLiveCancellationWitness(*publication_packet);
        }
        return result;
    }
    if (owner_.occupied()) {
        return make_result(Status::CancellationRejected);
    }
    const Slus21050FemainFrameLedgerSnapshot snapshot = ledger_.snapshot();
    if (snapshot.state != Slus21050FemainFrameLedgerState::Inactive) {
        const Slus21050FemainFrameLedgerStatus ledger_status = ledger_.cancel_and_drain(reason);
        if (ledger_status != Slus21050FemainFrameLedgerStatus::Ok) {
            Slus21050FemainLiveTransactionResult result;
            result.status = Status::CancellationRejected;
            result.player_status = Slus21050MoviePlayerStatus{};
            result.ledger_status = ledger_status;
            return result;
        }
        adapter_.reset();
        session_started_ = false;
        Slus21050FemainLiveTransactionResult result;
        result.status = Status::Cancelled;
        result.player_status = Slus21050MoviePlayerStatus{};
        result.ledger_status = Slus21050FemainFrameLedgerStatus::Ok;
        return result;
    }
    adapter_.reset();
    session_started_ = false;
    return make_result(Status::Cancelled);
}

} // namespace takedown::runtime
