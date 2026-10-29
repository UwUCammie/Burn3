#include "takedown/runtime/slus_21050_femain_live_terminal_coordinator.hpp"

#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_producer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace takedown::runtime {

using Coordinator = Slus21050FemainLiveTerminalCoordinator;

namespace {

[[nodiscard]] bool witness_authentic(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept {
    return witness.authentication_tag != 0U &&
           witness.authentication_tag ==
               authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(witness);
}

} // namespace

std::string_view to_string(const Coordinator::Phase phase) noexcept {
    switch (phase) {
    case Coordinator::Phase::Disabled:
        return "disabled";
    case Coordinator::Phase::Collecting:
        return "collecting";
    case Coordinator::Phase::EopReady:
        return "eop-ready";
    case Coordinator::Phase::Committed:
        return "committed";
    case Coordinator::Phase::Consumed:
        return "consumed";
    case Coordinator::Phase::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view to_string(const Coordinator::Failure failure) noexcept {
    switch (failure) {
    case Coordinator::Failure::None:
        return "none";
    case Coordinator::Failure::Disabled:
        return "disabled";
    case Coordinator::Failure::RawPathMismatch:
        return "raw-path-mismatch";
    case Coordinator::Failure::SourceQuadwordMismatch:
        return "source-quadword-mismatch";
    case Coordinator::Failure::SourceMarkerNotPacketStart:
        return "source-marker-not-packet-start";
    case Coordinator::Failure::CommandBeforeRaw:
        return "command-before-raw";
    case Coordinator::Failure::CommandPathMismatch:
        return "command-path-mismatch";
    case Coordinator::Failure::EopMissing:
        return "eop-missing";
    case Coordinator::Failure::EopWrongPath:
        return "eop-wrong-path";
    case Coordinator::Failure::PrimaryStreamUnavailable:
        return "primary-stream-unavailable";
    case Coordinator::Failure::EmptyBatch:
        return "empty-batch";
    case Coordinator::Failure::RawReset:
        return "raw-reset";
    case Coordinator::Failure::GsReset:
        return "gs-reset";
    case Coordinator::Failure::Overflow:
        return "overflow";
    case Coordinator::Failure::AllocationFailure:
        return "allocation-failure";
    case Coordinator::Failure::LateCallback:
        return "late-callback";
    case Coordinator::Failure::Replay:
        return "replay";
    case Coordinator::Failure::SourceNotBound:
        return "source-not-bound";
    case Coordinator::Failure::SourceEvidenceMismatch:
        return "source-evidence-mismatch";
    case Coordinator::Failure::WitnessAuthenticationMismatch:
        return "witness-authentication-mismatch";
    case Coordinator::Failure::PacketIdentityMismatch:
        return "packet-identity-mismatch";
    case Coordinator::Failure::ExecutionIdentityMismatch:
        return "execution-identity-mismatch";
    case Coordinator::Failure::FrameIdentityMismatch:
        return "frame-identity-mismatch";
    case Coordinator::Failure::CountMismatch:
        return "count-mismatch";
    case Coordinator::Failure::DigestMismatch:
        return "digest-mismatch";
    case Coordinator::Failure::ImageOverflow:
        return "image-overflow";
    case Coordinator::Failure::PrimaryEvidenceUnavailable:
        return "primary-evidence-unavailable";
    case Coordinator::Failure::AuthorityRejected:
        return "authority-rejected";
    case Coordinator::Failure::SerializedTokenRejected:
        return "serialized-token-rejected";
    }
    return "unknown";
}

Slus21050FemainLiveTerminalCoordinator::Slus21050FemainLiveTerminalCoordinator(
    const bool enabled) noexcept
    : enabled_(enabled), phase_(enabled ? Phase::Collecting : Phase::Disabled) {}

bool Slus21050FemainLiveTerminalCoordinator::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    phase_ = enabled_ ? Phase::Failed : Phase::Disabled;
    return false;
}

bool Slus21050FemainLiveTerminalCoordinator::accepting_callback() const noexcept {
    return enabled_ && phase_ == Phase::Collecting;
}

bool Slus21050FemainLiveTerminalCoordinator::accepting_command(const guest::GifPath path) noexcept {
    // Commands which arrive before the source producer's marked quadword are
    // ordinary pre-packet traffic and are ignored.
    if (enabled_ && phase_ == Phase::Collecting && source_gate_ != nullptr &&
        !source_gate_->staged_marker_reached()) {
        return false;
    }
    if (!accepting_callback()) {
        if (phase_ == Phase::EopReady) {
            if (source_gate_ == nullptr || !source_gate_->source_completion_ready_for_drain()) {
                static_cast<void>(fail(Failure::LateCallback));
            }
        }
        return false;
    }
    if (!prefix_raw_seen_) {
        static_cast<void>(fail(Failure::CommandBeforeRaw));
        return false;
    }
    if (path != guest::GifPath::Path2 ||
        (packet_path_bound_ && packet_path_ != guest::GifPath::Path2)) {
        static_cast<void>(fail(Failure::CommandPathMismatch));
        return false;
    }
    return true;
}

bool Slus21050FemainLiveTerminalCoordinator::append_command(CommandEvidence command) noexcept {
    if (commands_.size() >= kMaximumCommands) {
        return fail(Failure::Overflow);
    }
    try {
        commands_.push_back(std::move(command));
    } catch (...) {
        return fail(Failure::AllocationFailure);
    }
    return true;
}

void Slus21050FemainLiveTerminalCoordinator::attach(guest::GsCapture& capture) noexcept {
    source_gate_ = nullptr;
    capture.set_terminal_command_observer(this);
    capture.set_terminal_raw_gif_observer(this);
}

void Slus21050FemainLiveTerminalCoordinator::attach(
    guest::GsCapture& capture, Slus21050FemainLiveSourceProducer& source_producer) noexcept {
    source_gate_ = &source_producer;
    source_producer.set_raw_forward_observer(this);
    capture.set_terminal_command_observer(this);
    capture.set_terminal_raw_gif_observer(&source_producer);
}

void Slus21050FemainLiveTerminalCoordinator::detach(guest::GsCapture& capture) noexcept {
    source_gate_ = nullptr;
    capture.set_terminal_raw_gif_observer(nullptr);
    capture.set_terminal_command_observer(nullptr);
}

void Slus21050FemainLiveTerminalCoordinator::detach(
    guest::GsCapture& capture, Slus21050FemainLiveSourceProducer& source_producer) noexcept {
    source_producer.set_raw_forward_observer(nullptr);
    capture.set_terminal_raw_gif_observer(nullptr);
    capture.set_terminal_command_observer(nullptr);
    source_gate_ = nullptr;
}

bool Slus21050FemainLiveTerminalCoordinator::arm_for_source(
    const guest::GsQuadword& expected_quadword) noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::Collecting || raw_count_ != 0U || !commands_.empty() ||
        source_quadword_armed_) {
        return fail(Failure::Replay);
    }
    expected_source_quadword_ = expected_quadword;
    source_quadword_armed_ = true;
    return true;
}

bool Slus21050FemainLiveTerminalCoordinator::arm_for_source(
    const Slus21050FemainLiveSourceCandidate& candidate) noexcept {
    if (!candidate.valid() ||
        candidate.authentication_tag !=
            authenticate_slus_21050_femain_live_source_candidate(candidate)) {
        return fail(Failure::SourceEvidenceMismatch);
    }
    if (!arm_for_source(candidate.expected_quadword)) {
        return false;
    }
    if (candidate.source_qwc > kMaximumRawQuadwords) {
        return fail(Failure::Overflow);
    }
    try {
        resolver_raw_quadwords_.reserve(candidate.source_qwc);
    } catch (...) {
        return fail(Failure::AllocationFailure);
    }
    resolver_expected_raw_count_ = candidate.source_qwc;
    return true;
}

void Slus21050FemainLiveTerminalCoordinator::set_raw_forward_observer(
    guest::GsRawGifObserver* const observer) noexcept {
    raw_forward_ = observer == static_cast<guest::GsRawGifObserver*>(this) ? nullptr : observer;
}

// RECOVERY: functionally equivalent; branch-probability/layout may differ from the original object.
void Slus21050FemainLiveTerminalCoordinator::on_gs_gif_quadword(
    const guest::GifPath path, const guest::GsQuadword& quadword) {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_quadword(path, quadword);
    }
    if (!enabled_ || phase_ == Phase::Committed || phase_ == Phase::Consumed ||
        phase_ == Phase::Failed) {
        return;
    }
    if (phase_ == Phase::EopReady) {
        if (source_gate_ == nullptr || !source_gate_->source_completion_ready_for_drain()) {
            static_cast<void>(fail(Failure::LateCallback));
        }
        return;
    }
    if (phase_ != Phase::Collecting) {
        return;
    }
    if (source_gate_ != nullptr && !source_gate_->staged_marker_reached()) {
        return;
    }
    if (path != guest::GifPath::Path2) {
        static_cast<void>(fail(Failure::RawPathMismatch));
        return;
    }
    if (source_gate_ != nullptr && !source_gate_->staged_marker_at_packet_start()) {
        static_cast<void>(fail(Failure::SourceMarkerNotPacketStart));
        return;
    }
    prefix_raw_seen_ = true;
    if (source_quadword_armed_ && !source_raw_started_) {
        if (quadword != expected_source_quadword_) {
            static_cast<void>(fail(Failure::SourceQuadwordMismatch));
            return;
        }
        source_raw_started_ = true;
    }
    if (raw_count_ >= kMaximumRawQuadwords) {
        static_cast<void>(fail(Failure::Overflow));
        return;
    }
    if (!packet_path_bound_) {
        packet_path_ = path;
        packet_path_bound_ = true;
    } else if (packet_path_ != path) {
        static_cast<void>(fail(Failure::RawPathMismatch));
        return;
    }
    if (!first_raw_quadword_.has_value()) {
        first_raw_quadword_ = RawQuadwordEvidence{path, quadword};
    }
    if (resolver_expected_raw_count_ != 0U) {
        if (resolver_raw_quadwords_.size() >= resolver_expected_raw_count_) {
            static_cast<void>(fail(Failure::Overflow));
            return;
        }
        resolver_raw_quadwords_.push_back(quadword);
    }
    ++raw_count_;
}

// RECOVERY: functionally equivalent; branch-probability/layout of the EopMissing fail block differs from the original object.
void Slus21050FemainLiveTerminalCoordinator::on_gs_gif_packet_eop(
    const guest::GifPath path, const bool primary_command_stream_available) noexcept {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
    if (!enabled_ || phase_ == Phase::Committed || phase_ == Phase::Consumed ||
        phase_ == Phase::Failed) {
        return;
    }
    if (phase_ == Phase::EopReady) {
        if (source_gate_ == nullptr || !source_gate_->source_completion_ready_for_drain()) {
            static_cast<void>(fail(Failure::Replay));
        }
        return;
    }
    if (source_gate_ != nullptr && !source_gate_->staged_marker_reached()) {
        return;
    }
    if (path != guest::GifPath::Path2) {
        static_cast<void>(fail(Failure::EopWrongPath));
        return;
    }
    if (phase_ != Phase::Collecting || raw_count_ == 0U ||
        (source_quadword_armed_ && !source_raw_started_)) {
        static_cast<void>(fail(Failure::EopMissing));
        return;
    }
    if (commands_.empty()) {
        static_cast<void>(fail(Failure::EmptyBatch));
        return;
    }
    if (!primary_command_stream_available) {
        static_cast<void>(fail(Failure::PrimaryStreamUnavailable));
        return;
    }
    eop_received_ = true;
    primary_command_stream_available_ = true;
    phase_ = Phase::EopReady;
}

void Slus21050FemainLiveTerminalCoordinator::on_gs_gif_reset() noexcept {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_reset();
    }
    if (!enabled_ || phase_ == Phase::Committed || phase_ == Phase::Consumed ||
        phase_ == Phase::Failed) {
        return;
    }
    if (phase_ == Phase::EopReady && source_gate_ != nullptr &&
        source_gate_->source_completion_ready_for_drain()) {
        return;
    }
    static_cast<void>(fail(Failure::RawReset));
}

void Slus21050FemainLiveTerminalCoordinator::on_gs_gif_tag_begin(const guest::GifPath path) {
    if (!accepting_command(path)) {
        return;
    }
    CommandEvidence command{
        .kind = CommandKind::GifTagBegin,
        .path = path,
    };
    static_cast<void>(append_command(std::move(command)));
}

void Slus21050FemainLiveTerminalCoordinator::on_gs_reset() {
    if (!enabled_ || phase_ == Phase::Committed || phase_ == Phase::Consumed ||
        phase_ == Phase::Failed) {
        return;
    }
    if (phase_ == Phase::EopReady && source_gate_ != nullptr &&
        source_gate_->source_completion_ready_for_drain()) {
        return;
    }
    static_cast<void>(fail(Failure::GsReset));
}

void Slus21050FemainLiveTerminalCoordinator::on_gs_register_write(
    const guest::GsRegisterWrite& write) {
    if (!accepting_command(write.path)) {
        return;
    }
    CommandEvidence command{
        .kind = CommandKind::RegisterWrite,
        .path = write.path,
        .guest_sequence = write.sequence,
        .register_write = write,
    };
    static_cast<void>(append_command(std::move(command)));
}

// RECOVERY: functionally equivalent; fail-block layout and jump polarity of the overflow check differ from the original object.
void Slus21050FemainLiveTerminalCoordinator::on_gs_image_transfer(
    const guest::GsImageTransfer& transfer) {
    if (!accepting_command(transfer.path)) {
        return;
    }
    const std::size_t bytes = transfer.pixels.size();
    if (!(bytes > kMaximumImageBytes || image_bytes_ > kMaximumImageBytes - bytes)) {
        try {
            CommandEvidence command{
                .kind = CommandKind::ImageTransfer,
                .path = transfer.path,
                .guest_sequence = transfer.sequence,
                .image_transfer = transfer,
            };
            if (append_command(std::move(command))) {
                image_bytes_ += transfer.pixels.size();
            }
        } catch (...) {
            static_cast<void>(fail(Failure::AllocationFailure));
        }
    } else {
        static_cast<void>(fail(Failure::ImageOverflow));
    }
}

// RECOVERY: functionally equivalent; fail-block layout and jump polarity of the overflow check differ from the original object.
void Slus21050FemainLiveTerminalCoordinator::on_gs_image_transfer(
    guest::GsImageTransfer&& transfer) {
    on_gs_image_transfer(static_cast<const guest::GsImageTransfer&>(transfer));
}

std::uint64_t Slus21050FemainLiveTerminalCoordinator::primary_stream_digest() const noexcept {
    return primary_stream_digest(1U);
}

std::uint64_t Slus21050FemainLiveTerminalCoordinator::primary_stream_digest(
    const std::uint64_t primary_first_ordinal) const noexcept {
    if (commands_.empty() || commands_.size() > kMaximumCommands || primary_first_ordinal == 0U) {
        return 0U;
    }
    return digest_slus_21050_femain_primary_stream(commands_, primary_first_ordinal);
}

std::optional<Slus21050FemainLiveTerminalCoordinator::CompleteBatch>
Slus21050FemainLiveTerminalCoordinator::complete_batch(
    const std::uint64_t primary_first_ordinal) const noexcept {
    if ((phase_ != Phase::EopReady && phase_ != Phase::Committed && phase_ != Phase::Consumed) ||
        !eop_received_ || !primary_command_stream_available_ || commands_.empty() ||
        !first_raw_quadword_.has_value() || raw_count_ == 0U || primary_first_ordinal == 0U) {
        return std::nullopt;
    }
    if (phase_ == Phase::Committed || phase_ == Phase::Consumed) {
        if (committed_primary_first_ordinal_ == 0U ||
            committed_primary_first_ordinal_ != primary_first_ordinal) {
            return std::nullopt;
        }
    }
    const auto digest = primary_stream_digest(primary_first_ordinal);
    if (digest == 0U) {
        return std::nullopt;
    }
    CompleteBatch batch{};
    batch.commands_ = std::span<const CommandEvidence>(commands_.data(), commands_.size());
    batch.first_raw_quadword_ = first_raw_quadword_;
    batch.packet_path_ = packet_path_;
    batch.raw_quadword_count_ = raw_count_;
    if (resolver_expected_raw_count_ != 0U &&
        resolver_raw_quadwords_.size() == resolver_expected_raw_count_) {
        batch.raw_stream_digest_ = digest_slus_21050_femain_live_source_span(resolver_raw_quadwords_);
    }
    batch.eop_received_ = eop_received_;
    batch.primary_command_stream_available_ = primary_command_stream_available_;
    batch.primary_first_ordinal_ = primary_first_ordinal;
    batch.primary_stream_digest_ = digest;
    return batch;
}

bool Slus21050FemainLiveTerminalCoordinator::packet_identity_matches(
    const Slus21050FemainSerializedSourceBoundary& source) const noexcept {
    if (!packet_path_bound_ || packet_path_ != guest::GifPath::Path2 || raw_count_ == 0U ||
        !eop_received_ || !primary_command_stream_available_) {
        return false;
    }
    if (!first_raw_quadword_.has_value() || first_raw_quadword_->path != guest::GifPath::Path2 ||
        first_raw_quadword_->quadword !=
            source.source_evidence.token.source_marker.request.expected_quadword) {
        return false;
    }
    return true;
}

bool Slus21050FemainLiveTerminalCoordinator::witness_matches(
    const Slus21050FemainSerializedSourceBoundary& source,
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) const noexcept {
    if (!witness_authentic(witness)) {
        return false;
    }
    if (!(witness.source_evidence == source.source_evidence)) {
        return false;
    }
    if (witness.frame_id != source.frame_id ||
        witness.emulation_thread_identity != source.execution.emulation_thread_identity ||
        witness.worker_epoch != source.execution.worker_epoch ||
        witness.runtime_epoch != source.execution.runtime_epoch) {
        return false;
    }
    return validate_slus_21050_femain_renderer_terminal_primary_delivery_range(
        witness, source.source_evidence);
}

bool Slus21050FemainLiveTerminalCoordinator::commit(
    Slus21050FemainLiveTerminalAuthority& authority,
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept {
    if (!enabled_ || phase_ == Phase::Disabled) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::EopReady) {
        return fail(complete() ? Failure::Replay : Failure::EopMissing);
    }
    if (authority.phase() != Slus21050FemainLiveTerminalAuthority::Phase::SourceBound) {
        return fail(Failure::SourceNotBound);
    }
    const auto source = authority.source_boundary();
    if (!source.has_value()) {
        return fail(Failure::SourceNotBound);
    }
    if (!packet_identity_matches(*source)) {
        return fail(Failure::PacketIdentityMismatch);
    }
    if (!witness_matches(*source, witness)) {
        if (!(witness.source_evidence == source->source_evidence)) {
            return fail(Failure::SourceEvidenceMismatch);
        }
        if (!witness_authentic(witness)) {
            return fail(Failure::WitnessAuthenticationMismatch);
        }
        if (witness.frame_id != source->frame_id) {
            return fail(Failure::FrameIdentityMismatch);
        }
        if (witness.emulation_thread_identity != source->execution.emulation_thread_identity ||
            witness.worker_epoch != source->execution.worker_epoch ||
            witness.runtime_epoch != source->execution.runtime_epoch) {
            return fail(Failure::ExecutionIdentityMismatch);
        }
        return fail(Failure::PrimaryEvidenceUnavailable);
    }
    if (commands_.empty() || commands_.size() > kMaximumCommands ||
        witness.primary_command_count != commands_.size()) {
        return fail(Failure::CountMismatch);
    }
    if (witness.primary_stream_digest != primary_stream_digest(witness.primary_first_ordinal)) {
        return fail(Failure::DigestMismatch);
    }
    for (std::size_t index = 0U; index < commands_.size(); ++index) {
        if (!authority.on_primary_command_delivery()) {
            return fail(Failure::AuthorityRejected);
        }
    }
    if (!authority.on_terminal_primary_delivery(witness)) {
        return fail(Failure::AuthorityRejected);
    }
    auto token = authority.take_serialized_token();
    if (!token.has_value()) {
        return fail(Failure::SerializedTokenRejected);
    }
    serialized_token_ = token;
    committed_primary_first_ordinal_ = witness.primary_first_ordinal;
    phase_ = Phase::Committed;
    return true;
}

std::optional<Slus21050FemainSerializedSourceTerminalToken>
Slus21050FemainLiveTerminalCoordinator::take_serialized_token() noexcept {
    if (phase_ != Phase::Committed || !serialized_token_.has_value()) {
        if (complete()) {
            static_cast<void>(fail(Failure::Replay));
        }
        return std::nullopt;
    }
    auto token = std::move(serialized_token_);
    serialized_token_.reset();
    phase_ = Phase::Consumed;
    return token;
}

} // namespace takedown::runtime
