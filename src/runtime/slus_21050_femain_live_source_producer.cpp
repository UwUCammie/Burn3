#include "takedown/runtime/slus_21050_femain_live_source_producer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace takedown::runtime {
namespace {

[[nodiscard]] bool main_ram_range_contains(const guest::EeMemory& memory,
                                           const guest::GuestAddress range_begin,
                                           const std::size_t range_bytes,
                                           const guest::GuestAddress address,
                                           const std::size_t length) noexcept {
    if (length == 0U || !memory.is_main_ram_range(range_begin, range_bytes) ||
        !memory.is_main_ram_range(address, length)) {
        return false;
    }
    const auto last = static_cast<std::uint64_t>(address) + length - 1U;
    if (last > 0xFFFFFFFFULL) {
        return false;
    }
    return memory.main_ram_ranges_overlap(range_begin, range_bytes, address, 1U) &&
           memory.main_ram_ranges_overlap(range_begin, range_bytes,
                                          static_cast<guest::GuestAddress>(last), 1U);
}

[[nodiscard]] bool request_matches(const guest::Vif1Path2SourceMarkerRequest& request,
                                   const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    return request.source == marker.source && request.chain_root == marker.chain_root &&
           request.source_tag_location == marker.source_tag_location &&
           request.source_tag_ordinal == marker.source_tag_ordinal &&
           request.source_payload_word_ordinal == marker.source_payload_word_ordinal &&
           request.expected_quadword == marker.quadword;
}

[[nodiscard]] bool request_matches(const guest::Vif1Path2SourceMarkerRequest& request,
                                   const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    return request.source == marker.source && request.chain_root == marker.chain_root &&
           request.source_tag_location == marker.source_tag_location &&
           request.source_tag_ordinal == marker.source_tag_ordinal &&
           request.source_payload_word_ordinal == marker.source_payload_word_ordinal;
}

[[nodiscard]] bool same_candidate(const Slus21050FemainLiveSourceCandidate& actual,
                                  const Slus21050FemainLiveSourceCandidate& expected) noexcept {
    return actual.source == expected.source && actual.chain_root == expected.chain_root &&
           actual.source_tag_location == expected.source_tag_location &&
           actual.source_tag_ordinal == expected.source_tag_ordinal &&
           actual.terminal_tag_location == expected.terminal_tag_location &&
           actual.terminal_tag_ordinal == expected.terminal_tag_ordinal &&
           actual.source_tag_words == expected.source_tag_words &&
           actual.terminal_tag_words == expected.terminal_tag_words &&
           actual.source_payload_word_ordinal == expected.source_payload_word_ordinal &&
           actual.source_qwc == expected.source_qwc &&
           actual.expected_quadword == expected.expected_quadword &&
           actual.publication_serial == expected.publication_serial &&
           actual.player_session_generation == expected.player_session_generation &&
           actual.frame_generation == expected.frame_generation &&
           actual.authentication_tag == expected.authentication_tag;
}

} // namespace

std::string_view to_string(const Slus21050FemainLiveSourceProducer::Phase phase) noexcept {
    switch (phase) {
    case Slus21050FemainLiveSourceProducer::Phase::Disabled:
        return "disabled";
    case Slus21050FemainLiveSourceProducer::Phase::Idle:
        return "idle";
    case Slus21050FemainLiveSourceProducer::Phase::PublicationBound:
        return "publication-bound";
    case Slus21050FemainLiveSourceProducer::Phase::MarkerResolved:
        return "marker-resolved";
    case Slus21050FemainLiveSourceProducer::Phase::Armed:
        return "armed";
    case Slus21050FemainLiveSourceProducer::Phase::SourceReached:
        return "source-reached";
    case Slus21050FemainLiveSourceProducer::Phase::CompletionPending:
        return "completion-pending";
    case Slus21050FemainLiveSourceProducer::Phase::SourceCompleted:
        return "source-completed";
    case Slus21050FemainLiveSourceProducer::Phase::Failed:
        return "failed";
    default:
        return "unknown";
    }
}

std::string_view to_string(const Slus21050FemainLiveSourceProducer::Failure failure) noexcept {
    switch (failure) {
    case Slus21050FemainLiveSourceProducer::Failure::None:
        return "none";
    case Slus21050FemainLiveSourceProducer::Failure::Disabled:
        return "disabled";
    case Slus21050FemainLiveSourceProducer::Failure::PublicationAlreadyBound:
        return "publication-already-bound";
    case Slus21050FemainLiveSourceProducer::Failure::PublicationRejected:
        return "publication-rejected";
    case Slus21050FemainLiveSourceProducer::Failure::PublicationMismatch:
        return "publication-mismatch";
    case Slus21050FemainLiveSourceProducer::Failure::ExecutionIdentityInvalid:
        return "execution-identity-invalid";
    case Slus21050FemainLiveSourceProducer::Failure::FrameIdentityInvalid:
        return "frame-identity-invalid";
    case Slus21050FemainLiveSourceProducer::Failure::MediaClockInvalid:
        return "media-clock-invalid";
    case Slus21050FemainLiveSourceProducer::Failure::SourceCandidateInvalid:
        return "source-candidate-invalid";
    case Slus21050FemainLiveSourceProducer::Failure::SourceCandidateAmbiguous:
        return "source-candidate-ambiguous";
    case Slus21050FemainLiveSourceProducer::Failure::CandidateCapacityExhausted:
        return "candidate-capacity-exhausted";
    case Slus21050FemainLiveSourceProducer::Failure::CandidateResolutionMissing:
        return "candidate-resolution-missing";
    case Slus21050FemainLiveSourceProducer::Failure::CandidateResolutionAmbiguous:
        return "candidate-resolution-ambiguous";
    case Slus21050FemainLiveSourceProducer::Failure::CandidateResolutionRejected:
        return "candidate-resolution-rejected";
    case Slus21050FemainLiveSourceProducer::Failure::ResolvedMarkerInvalid:
        return "resolved-marker-invalid";
    case Slus21050FemainLiveSourceProducer::Failure::MarkerMissing:
        return "marker-missing";
    case Slus21050FemainLiveSourceProducer::Failure::MarkerMismatch:
        return "marker-mismatch";
    case Slus21050FemainLiveSourceProducer::Failure::MarkerCancelled:
        return "marker-cancelled";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanAllocationFailure:
        return "source-span-allocation-failure";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanPathMismatch:
        return "source-span-path-mismatch";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanQuadwordMismatch:
        return "source-span-quadword-mismatch";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanIncomplete:
        return "source-span-incomplete";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanOverflow:
        return "source-span-overflow";
    case Slus21050FemainLiveSourceProducer::Failure::SourceSpanMarkerNotPacketStart:
        return "source-span-marker-not-packet-start";
    case Slus21050FemainLiveSourceProducer::Failure::RawReset:
        return "raw-reset";
    case Slus21050FemainLiveSourceProducer::Failure::PacketCompletionMissing:
        return "packet-completion-missing";
    case Slus21050FemainLiveSourceProducer::Failure::PacketCompletionAmbiguous:
        return "packet-completion-ambiguous";
    case Slus21050FemainLiveSourceProducer::Failure::PacketCompletionRejected:
        return "packet-completion-rejected";
    case Slus21050FemainLiveSourceProducer::Failure::AuthorityRejected:
        return "authority-rejected";
    case Slus21050FemainLiveSourceProducer::Failure::Replay:
        return "replay";
    default:
        return "unknown";
    }
}

Slus21050FemainLiveSourceProducer::Slus21050FemainLiveSourceProducer(const bool enabled) noexcept
    : enabled_(enabled), phase_(enabled ? Phase::Idle : Phase::Disabled), probe_(enabled) {}

bool Slus21050FemainLiveSourceProducer::fail(const Failure failure) noexcept {
    if (failure_ == Failure::None) {
        failure_ = failure;
    }
    phase_ = enabled_ ? Phase::Failed : Phase::Disabled;
    return false;
}

bool Slus21050FemainLiveSourceProducer::valid_execution(
    const Slus21050FemainSerializedExecutionIdentity& execution) noexcept {
    return execution.emulation_thread_identity != 0U && execution.worker_epoch != 0U &&
           execution.runtime_epoch != 0U;
}

guest::GsQuadword Slus21050FemainLiveSourceProducer::read_quadword(const guest::EeMemory& memory,
                                    const guest::GuestAddress address) noexcept {
    return {memory.read_u32_le(address), memory.read_u32_le(address + 4U),
            memory.read_u32_le(address + 8U), memory.read_u32_le(address + 12U)};
}

std::uint64_t Slus21050FemainLiveSourceProducer::allocate_candidate_token() noexcept {
    static std::atomic<std::uint64_t> next{1U};
    auto token = next.fetch_add(1U, std::memory_order_relaxed);
    if (token == 0U) {
        token = next.fetch_add(1U, std::memory_order_relaxed);
    }
    return token;
}

bool Slus21050FemainLiveSourceProducer::forwarded_source_aliases_candidate() const noexcept {
    if (memory_ == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < forwarded_candidate_count_; ++index) {
        if (memory_->main_ram_ranges_overlap(source_candidate_, 16U,
                                             forwarded_candidate_sources_[index], 16U)) {
            return true;
        }
    }
    return false;
}

bool Slus21050FemainLiveSourceProducer::copy_resolver_source_span(
    const Slus21050FemainLiveSourceCandidate& candidate) noexcept {
    if (memory_ == nullptr || candidate.source_qwc == 0U || candidate.source_qwc > 65536U ||
        candidate.source_qwc > source_span_bytes_ / 16U) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const auto source_bytes = static_cast<unsigned long long>(candidate.source_qwc) * 16U;
    if (!main_ram_range_contains(*memory_, source_buffer_, source_span_bytes_, candidate.source,
                                 source_bytes)) {
        return fail(Failure::SourceCandidateInvalid);
    }
    std::vector<guest::GsQuadword> copied;
    try {
        copied.resize(candidate.source_qwc);
    } catch (...) {
        return fail(Failure::SourceSpanAllocationFailure);
    }
    for (std::size_t index = 0U; index < copied.size(); ++index) {
        const auto address = static_cast<std::uint64_t>(candidate.source) + index * 16U;
        if (address > 0xFFFFFFFFULL) {
            return fail(Failure::SourceCandidateInvalid);
        }
        copied[index] = read_quadword(*memory_, static_cast<guest::GuestAddress>(address));
    }
    expected_source_span_ = std::move(copied);
    source_span_quadword_count_ = 0U;
    return true;
}

bool Slus21050FemainLiveSourceProducer::bind_publication(
    Slus21050FemainLiveTerminalAuthority& authority, Slus21050FemainMovieTransactionOwner& owner,
    Slus21050FemainFrameTransactionLedger& ledger, Slus21050MoviePlayer& player,
    guest::EeMemory& memory, const Slus21050FemainMoviePublicationPacket& packet,
    const Slus21050FemainSerializedExecutionIdentity& execution, const std::uint64_t frame_id,
    const std::uint64_t media_clock_nanoseconds, const std::uint64_t media_clock_generation,
    const Slus21050FemainLiveSourceCandidate& candidate) noexcept {
    if (!candidate.valid() ||
        candidate.authentication_tag !=
            authenticate_slus_21050_femain_live_source_candidate(candidate) ||
        candidate.publication_serial != packet.publication_serial ||
        candidate.player_session_generation != packet.player_session_generation ||
        candidate.frame_generation != packet.plan.frame_generation || candidate.source_qwc == 0U ||
        candidate.source_qwc > 65536U) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const auto source_bytes = static_cast<unsigned long long>(candidate.source_qwc) * 16U;
    const auto plan = owner.consumer_plan();
    if (!plan.has_value() || plan->frame_generation != candidate.frame_generation ||
        plan->frame_generation != packet.plan.frame_generation ||
        (!main_ram_range_contains(memory, plan->first_buffer, plan->buffer_bytes, candidate.source,
                                  source_bytes) &&
         !main_ram_range_contains(memory, plan->second_buffer, plan->buffer_bytes,
                                  candidate.source, source_bytes))) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const auto expected = read_quadword(memory, candidate.source);
    if (expected != candidate.expected_quadword) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const bool bound = bind_publication(authority, owner, ledger, player, memory, packet, execution,
                                        frame_id, media_clock_nanoseconds,
                                        media_clock_generation, candidate.source);
    if (bound) {
        resolver_candidate_ = candidate;
    }
    return bound;
}

bool Slus21050FemainLiveSourceProducer::bind_publication(
    Slus21050FemainLiveTerminalAuthority& authority, Slus21050FemainMovieTransactionOwner& owner,
    Slus21050FemainFrameTransactionLedger& ledger, Slus21050MoviePlayer& player,
    guest::EeMemory& memory, const Slus21050FemainMoviePublicationPacket& packet,
    const Slus21050FemainSerializedExecutionIdentity& execution, const std::uint64_t frame_id,
    const std::uint64_t media_clock_nanoseconds, const std::uint64_t media_clock_generation,
    const guest::GuestAddress source_candidate) noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (phase_ != Phase::Idle) {
        return fail(Failure::PublicationAlreadyBound);
    }
    if (!valid_execution(execution)) {
        return fail(Failure::ExecutionIdentityInvalid);
    }
    if (frame_id == 0U) {
        return fail(Failure::FrameIdentityInvalid);
    }
    if (media_clock_generation == 0U) {
        return fail(Failure::MediaClockInvalid);
    }
    if (!owner.occupied() || !owner.publication_pending() ||
        !owner.publication_packet_matches(packet) || !owner.published_ledger_matches(ledger) ||
        !player.active() || player.identity() == 0U || player.session_generation() == 0U ||
        owner.player_identity() != player.identity() ||
        owner.player_session_generation() != player.session_generation()) {
        return fail(Failure::PublicationMismatch);
    }
    const auto plan = owner.consumer_plan();
    if (!plan.has_value() || packet.plan.layout_generation == 0U ||
        packet.plan.first_buffer == 0U || packet.plan.second_buffer == 0U ||
        packet.plan.buffer_bytes == 0U ||
        packet.plan != Slus21050FemainMoviePublicationPlanIdentity{
                           plan->layout.generation, plan->first_buffer, plan->second_buffer,
                           plan->buffer_bytes, plan->frame_generation, plan->media_ordinal,
                           plan->source_sequence, plan->source_checksum, plan->payload_digest,
                           plan->player_lease, plan->timeline_ordinal}) {
        return fail(Failure::PublicationMismatch);
    }
    if ((source_candidate & 15U) != 0U || !memory.is_main_ram_range(source_candidate, 16U)) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const bool in_first = main_ram_range_contains(memory, plan->first_buffer, plan->buffer_bytes,
                                                  source_candidate, 16U);
    const bool in_second = main_ram_range_contains(memory, plan->second_buffer,
                                                   plan->buffer_bytes, source_candidate, 16U);
    if (in_first == in_second) {
        return fail(Failure::SourceCandidateInvalid);
    }
    const auto candidate_token = allocate_candidate_token();
    if (candidate_token == 0U) {
        return fail(Failure::SourceCandidateInvalid);
    }
    if (!authority.bind_publication(owner, ledger, packet, execution, frame_id)) {
        return fail(Failure::PublicationRejected);
    }
    authority_ = &authority;
    owner_ = &owner;
    ledger_ = &ledger;
    player_ = &player;
    memory_ = &memory;
    packet_ = packet;
    execution_ = execution;
    frame_id_ = frame_id;
    media_clock_nanoseconds_ = media_clock_nanoseconds;
    media_clock_generation_ = media_clock_generation;
    plan_ = *plan;
    source_buffer_ = in_first ? plan->first_buffer : plan->second_buffer;
    source_span_bytes_ = plan->buffer_bytes;
    source_candidate_ = source_candidate;
    expected_quadword_ = read_quadword(memory, source_candidate);
    candidate_token_ = candidate_token;
    candidate_offered_ = false;
    candidate_merge_ambiguous_ = false;
    forwarded_candidate_sources_.fill(0U);
    forwarded_candidate_count_ = 0U;
    staged_marker_.reset();
    raw_packet_open_before_marker_ = false;
    marker_at_packet_start_ = true;
    completion_pending_ = false;
    pending_completion_path_ = guest::GifPath::Path1;
    pending_completion_primary_available_ = false;
    source_completion_delivered_ = false;
    resolved_marker_.reset();
    resolver_candidate_.reset();
    expected_source_span_.clear();
    source_span_quadword_count_ = 0U;
    phase_ = Phase::PublicationBound;
    failure_ = Failure::None;
    return true;
}

void Slus21050FemainLiveSourceProducer::set_marker_forward_observer(
    guest::Vif1Path2SourceMarkerObserver* const observer) noexcept {
    marker_forward_ = observer == this ? nullptr : observer;
}

void Slus21050FemainLiveSourceProducer::set_raw_forward_observer(
    guest::GsRawGifObserver* const observer) noexcept {
    raw_forward_ = observer == this ? nullptr : observer;
}

bool Slus21050FemainLiveSourceProducer::has_vif1_dmac_pre_snapshot_candidates() const noexcept {
    const bool forwarded =
        marker_forward_ != nullptr && marker_forward_->has_vif1_dmac_pre_snapshot_candidates();
    return (phase_ == Phase::PublicationBound && !candidate_offered_ && failure_ == Failure::None) ||
           forwarded;
}

// RECOVERY: GIMPLE matches except GCC canonicalises the loop/clamp comparisons against the
// clamped candidate count the other way round (SSA name order); the count is a plain local here.
std::size_t Slus21050FemainLiveSourceProducer::on_vif1_dmac_pre_snapshot_candidates(
    const guest::Vif1DmacPreSnapshotObservation& observation,
    const std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept {
    std::size_t forwarded_count = 0U;
    if (marker_forward_ != nullptr) {
        forwarded_count = marker_forward_->on_vif1_dmac_pre_snapshot_candidates(observation, candidates);
    }
    const auto count = std::min(forwarded_count, candidates.size());
    if (phase_ == Phase::Failed || failure_ != Failure::None) {
        return count;
    }
    if (phase_ != Phase::PublicationBound) {
        if (candidate_token_ != 0U && candidate_offered_) {
            static_cast<void>(fail(Failure::Replay));
        }
        return count;
    }
    if (candidate_offered_) {
        static_cast<void>(fail(Failure::Replay));
        return count;
    }
    if (count > guest::kVif1DmacPreSnapshotCandidateCapacity) {
        static_cast<void>(fail(Failure::CandidateCapacityExhausted));
        return count;
    }
    forwarded_candidate_count_ = count;
    for (std::size_t index = 0U; count > index; ++index) {
        forwarded_candidate_sources_[index] = candidates[index].source;
    }
    if (count >= candidates.size() ||
        count == guest::kVif1DmacPreSnapshotCandidateCapacity) {
        static_cast<void>(fail(Failure::CandidateCapacityExhausted));
        return count;
    }
    for (std::size_t index = 0U; count > index; ++index) {
        if (candidates[index].token == candidate_token_) {
            candidate_merge_ambiguous_ = true;
            static_cast<void>(fail(Failure::SourceCandidateAmbiguous));
            return count;
        }
    }
    candidates[count] = guest::Vif1DmacPreSnapshotCandidate{
        candidate_token_, source_candidate_, expected_quadword_};
    candidate_offered_ = true;
    return count + 1U;
}

void Slus21050FemainLiveSourceProducer::on_vif1_dmac_pre_snapshot_markers_resolved(
    const std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
    const std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept {
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_dmac_pre_snapshot_markers_resolved(markers, decisions);
    }
    if (phase_ == Phase::Failed || failure_ != Failure::None) {
        return;
    }
    if (phase_ != Phase::PublicationBound) {
        if (candidate_token_ != 0U) {
            for (const auto& marker : markers) {
                if (marker.token == candidate_token_) {
                    static_cast<void>(fail(Failure::Replay));
                    return;
                }
            }
        }
        return;
    }
    if (candidate_merge_ambiguous_ || !candidate_offered_) {
        return;
    }
    std::size_t own_count = 0U;
    std::size_t own_index = 0U;
    for (std::size_t index = 0U; markers.size() > index; ++index) {
        if (markers[index].token != candidate_token_) {
            continue;
        }
        ++own_count;
        own_index = index;
    }
    if (own_count == 0U) {
        return;
    }
    if (own_count != 1U || own_index >= decisions.size()) {
        static_cast<void>(fail(Failure::CandidateResolutionAmbiguous));
        return;
    }
    const auto& marker = markers[own_index];
    if (marker.generation == 0U || marker.candidate_source != source_candidate_ ||
        marker.request.expected_quadword != expected_quadword_ || marker.request.source == 0U ||
        marker.request.chain_root == 0U || marker.request.source_tag_location == 0U ||
        !resolver_marker_matches(marker)) {
        static_cast<void>(fail(Failure::ResolvedMarkerInvalid));
        return;
    }
    decisions[own_index] = guest::Vif1DmacPreSnapshotMarkerDecision::Accept;
    resolved_marker_ = marker;
    phase_ = Phase::MarkerResolved;
}

bool Slus21050FemainLiveSourceProducer::marker_matches(
    const guest::Vif1Path2SourceMarkerReached& marker) const noexcept {
    return resolved_marker_.has_value() && resolved_marker_->generation == marker.generation &&
           request_matches(resolved_marker_->request, marker);
}

bool Slus21050FemainLiveSourceProducer::marker_matches(
    const guest::Vif1Path2SourceMarkerCancelled& marker) const noexcept {
    return resolved_marker_.has_value() && resolved_marker_->generation == marker.generation &&
           request_matches(resolved_marker_->request, marker);
}

bool Slus21050FemainLiveSourceProducer::resolver_marker_matches(
    const guest::Vif1DmacPreSnapshotResolvedMarker& marker) const noexcept {
    if (!resolver_candidate_.has_value()) {
        return true;
    }
    const auto& candidate = *resolver_candidate_;
    return candidate.valid() &&
           candidate.authentication_tag ==
               authenticate_slus_21050_femain_live_source_candidate(candidate) &&
           marker.candidate_source == candidate.source && marker.request.source == candidate.source &&
           marker.request.chain_root == candidate.chain_root &&
           marker.request.source_tag_location == candidate.source_tag_location &&
           marker.request.source_tag_ordinal == candidate.source_tag_ordinal &&
           marker.request.source_payload_word_ordinal == candidate.source_payload_word_ordinal &&
           marker.request.expected_quadword == candidate.expected_quadword;
}

bool Slus21050FemainLiveSourceProducer::drain_resolved_marker() noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (phase_ == Phase::Armed || phase_ == Phase::SourceReached ||
        phase_ == Phase::CompletionPending || phase_ == Phase::SourceCompleted) {
        return fail(Failure::Replay);
    }
    if (forwarded_source_aliases_candidate()) {
        candidate_merge_ambiguous_ = true;
        return fail(Failure::SourceCandidateAmbiguous);
    }
    if (phase_ != Phase::MarkerResolved || !resolved_marker_.has_value() ||
        candidate_merge_ambiguous_) {
        return fail(resolved_marker_.has_value() ? Failure::CandidateResolutionRejected
                                                 : Failure::CandidateResolutionMissing);
    }
    if (resolver_candidate_.has_value()) {
        const Slus21050FemainLiveSourceCandidateResolver resolver{true};
        const auto resolved = resolver.resolve(*memory_, packet_);
        if (!resolved.resolved() || !same_candidate(*resolved.candidate, *resolver_candidate_)) {
            return fail(Failure::CandidateResolutionRejected);
        }
        if (!copy_resolver_source_span(*resolved.candidate)) {
            return false;
        }
    }
    const Slus21050FemainFrameGraphicsSourceContext context{
        .source = kSlus21050FemainLiveExpectedSource,
        .emulation_thread_identity = execution_.emulation_thread_identity,
        .media_clock_nanoseconds = media_clock_nanoseconds_,
        .media_clock_generation = media_clock_generation_,
        .transaction_owner = owner_,
        .player = player_,
        .consumer_plan = &plan_,
        .ledger = ledger_,
        .memory = memory_,
    };
    const auto captured = capture_slus_21050_femain_frame_graphics_source_token(
        context, *resolved_marker_, source_buffer_, source_span_bytes_);
    if (!captured.captured()) {
        return fail(Failure::ResolvedMarkerInvalid);
    }
    const auto armed = probe_.arm(*captured.token, context);
    if (armed.failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None ||
        probe_.phase() != Slus21050FemainFrameGraphicsSourceProbe::Phase::Armed) {
        return fail(Failure::ResolvedMarkerInvalid);
    }
    raw_packet_open_before_marker_ = false;
    if (!staged_marker_.has_value()) {
        marker_at_packet_start_ = true;
    }
    phase_ = staged_marker_.has_value() ? Phase::SourceReached : Phase::Armed;
    return true;
}

void Slus21050FemainLiveSourceProducer::on_vif1_path2_source_marker_memory_cleared() noexcept {
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_memory_cleared();
    }
    if (source_completion_ready_for_drain()) {
        return;
    }
    if (phase_ != Phase::Disabled && phase_ != Phase::Idle && phase_ != Phase::Failed &&
        phase_ != Phase::SourceCompleted) {
        static_cast<void>(fail(Failure::RawReset));
    }
}

void Slus21050FemainLiveSourceProducer::on_vif1_path2_source_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_reached(marker);
    }
    if (source_completion_ready_for_drain()) {
        return;
    }
    if (!marker_matches(marker)) {
        return;
    }
    if (staged_marker_.has_value()) {
        static_cast<void>(fail(Failure::Replay));
        return;
    }
    if (phase_ != Phase::MarkerResolved && phase_ != Phase::Armed) {
        static_cast<void>(fail(phase_ == Phase::SourceCompleted ? Failure::Replay
                                                                : Failure::MarkerMissing));
        return;
    }
    if (raw_packet_open_before_marker_) {
        marker_at_packet_start_ = false;
    }
    staged_marker_ = marker;
    if (phase_ == Phase::Armed) {
        phase_ = Phase::SourceReached;
    }
}

void Slus21050FemainLiveSourceProducer::on_vif1_path2_source_marker_cancelled(
    const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    if (marker_forward_ != nullptr) {
        marker_forward_->on_vif1_path2_source_marker_cancelled(marker);
    }
    if (source_completion_ready_for_drain()) {
        return;
    }
    if (!marker_matches(marker)) {
        return;
    }
    static_cast<void>(fail(Failure::MarkerCancelled));
}

void Slus21050FemainLiveSourceProducer::on_gs_gif_quadword(const guest::GifPath path,
                                                           const guest::GsQuadword& quadword) {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_quadword(path, quadword);
    }
    if (phase_ == Phase::SourceCompleted || source_completion_ready_for_drain()) {
        return;
    }
    const bool collecting = phase_ == Phase::Armed || phase_ == Phase::SourceReached ||
                            phase_ == Phase::CompletionPending;
    if (collecting && !staged_marker_.has_value() && path == guest::GifPath::Path2) {
        raw_packet_open_before_marker_ = true;
    }
    if (!staged_marker_.has_value() || !collecting) {
        return;
    }
    if (completion_pending_) {
        static_cast<void>(fail(Failure::PacketCompletionAmbiguous));
        return;
    }
    if (resolver_candidate_.has_value()) {
        if (path != guest::GifPath::Path2) {
            static_cast<void>(fail(Failure::SourceSpanPathMismatch));
            return;
        }
        if (source_span_quadword_count_ >= expected_source_span_.size()) {
            static_cast<void>(fail(Failure::SourceSpanOverflow));
            return;
        }
        if (quadword != expected_source_span_[source_span_quadword_count_]) {
            static_cast<void>(fail(Failure::SourceSpanQuadwordMismatch));
            return;
        }
        ++source_span_quadword_count_;
    } else if (staged_raw_quadword_.has_value()) {
        return;
    }
    if (!staged_raw_quadword_.has_value()) {
        staged_raw_quadword_ = StagedRawQuadword{path, quadword};
    }
}

void Slus21050FemainLiveSourceProducer::on_gs_gif_packet_eop(
    const guest::GifPath path, const bool primary_command_stream_available) noexcept {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_packet_eop(path, primary_command_stream_available);
    }
    if (completion_pending_) {
        if (!source_completion_ready_for_drain()) {
            static_cast<void>(fail(Failure::PacketCompletionAmbiguous));
        }
    } else {
        if (resolver_candidate_.has_value() && staged_marker_.has_value() &&
            path != guest::GifPath::Path2 && phase_ != Phase::Disabled && phase_ != Phase::Idle &&
            phase_ != Phase::Failed && phase_ != Phase::SourceCompleted) {
            static_cast<void>(fail(Failure::SourceSpanPathMismatch));
            return;
        }
        if (path != guest::GifPath::Path2 || phase_ == Phase::Disabled || phase_ == Phase::Idle ||
            phase_ == Phase::Failed || phase_ == Phase::SourceCompleted) {
            return;
        }
        if (completion_pending_ || source_completion_delivered_) {
            static_cast<void>(fail(source_completion_delivered_ ? Failure::Replay
                                                                : Failure::PacketCompletionAmbiguous));
            return;
        }
        raw_packet_open_before_marker_ = false;
        pending_completion_path_ = guest::GifPath::Path2;
        pending_completion_primary_available_ = primary_command_stream_available;
        completion_pending_ = true;
        phase_ = Phase::CompletionPending;
    }
}

void Slus21050FemainLiveSourceProducer::on_gs_gif_reset() noexcept {
    if (raw_forward_ != nullptr) {
        raw_forward_->on_gs_gif_reset();
    }
    if (source_completion_ready_for_drain()) {
        return;
    }
    if (phase_ != Phase::Disabled && phase_ != Phase::Idle && phase_ != Phase::Failed &&
        phase_ != Phase::SourceCompleted) {
        static_cast<void>(fail(Failure::RawReset));
    }
}

bool Slus21050FemainLiveSourceProducer::drain_source_completion() noexcept {
    if (!enabled_) {
        return fail(Failure::Disabled);
    }
    if (source_completion_delivered_) {
        return fail(Failure::Replay);
    }
    if (!completion_pending_) {
        return fail(Failure::PacketCompletionMissing);
    }
    source_completion_delivered_ = true;
    completion_pending_ = false;
    if (pending_completion_path_ != guest::GifPath::Path2 || phase_ == Phase::Failed) {
        return fail(Failure::PacketCompletionRejected);
    }
    if (!staged_marker_.has_value()) {
        return fail(Failure::MarkerMissing);
    }
    if (resolver_candidate_.has_value() && !marker_at_packet_start_) {
        return fail(Failure::SourceSpanMarkerNotPacketStart);
    }
    if (resolver_candidate_.has_value() &&
        source_span_quadword_count_ != expected_source_span_.size()) {
        return fail(Failure::SourceSpanIncomplete);
    }
    const auto marker_observed = probe_.observe_vif1_path2_source_marker_reached(*staged_marker_);
    if (marker_observed.failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(Failure::MarkerMismatch);
    }
    if (staged_raw_quadword_.has_value()) {
        probe_.on_gs_gif_quadword(staged_raw_quadword_->path, staged_raw_quadword_->quadword);
        if (probe_.phase() == Slus21050FemainFrameGraphicsSourceProbe::Phase::Failed) {
            return fail(Failure::PacketCompletionRejected);
        }
    }
    probe_.on_gs_gif_packet_eop(pending_completion_path_, pending_completion_primary_available_);
    if (probe_.phase() != Slus21050FemainFrameGraphicsSourceProbe::Phase::Completed) {
        return fail(Failure::PacketCompletionRejected);
    }
    auto evidence = probe_.take_evidence();
    if (evidence.has_value() && resolver_candidate_.has_value()) {
        if (expected_source_span_.size() != resolver_candidate_->source_qwc ||
            expected_source_span_.size() > std::numeric_limits<std::uint32_t>::max()) {
            return fail(Failure::SourceSpanIncomplete);
        }
        evidence->resolver_proof = Slus21050FemainLiveResolverSourceProof{
            .candidate = *resolver_candidate_,
            .publication_queue = packet_.queue,
            .matched_source_qwc = static_cast<std::uint32_t>(expected_source_span_.size()),
            .matched_span_digest =
                digest_slus_21050_femain_live_source_span(expected_source_span_),
        };
        if (!validate_slus_21050_femain_live_resolver_source_proof(*evidence->resolver_proof)) {
            return fail(Failure::SourceSpanIncomplete);
        }
        evidence->authentication_tag =
            authenticate_slus_21050_femain_frame_graphics_source_evidence(*evidence);
    }
    if (!evidence.has_value() || authority_ == nullptr ||
        !authority_->on_source_packet_completed(*evidence)) {
        return fail(Failure::AuthorityRejected);
    }
    phase_ = Phase::SourceCompleted;
    return true;
}

} // namespace takedown::runtime
