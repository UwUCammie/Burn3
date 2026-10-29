#include "takedown/runtime/slus_21050_femain_graphics_source_probe.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace takedown::runtime {

void Slus21050FemainFrameGraphicsSourceProbe::on_gs_gif_quadword(const guest::GifPath path, const guest::GsQuadword& quadword) {
    if (diagnostic_event_domain_identity_ != 0U) {
        // The shared diagnostic callbacks own the sequence; an unmarked raw word is out of order.
        if (phase_ == Phase::SourceReached) {
            static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionReordered));
        }
        return;
    }
    observe_raw_quadword_impl(path, quadword);
}

void Slus21050FemainFrameGraphicsSourceProbe::observe_raw_quadword_impl(const guest::GifPath path,
                                      const guest::GsQuadword& quadword) noexcept {
    if (!enabled_ || phase_ == Phase::Idle || phase_ == Phase::Failed ||
        (phase_ == Phase::Armed || phase_ == Phase::SourceSubmitted) ||
        phase_ == Phase::Completed) {
        return;
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        static_cast<void>(fail(context_failure));
        return;
    }
    if (path != guest::GifPath::Path2) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawPathMismatch));
        return;
    }
    if (quadword != token_.source_marker.request.expected_quadword) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawQuadwordMismatch));
        return;
    }
    if (event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionReordered));
        return;
    }
    ++event_ordinal_;
    raw_submission_event_ordinal_ = event_ordinal_;
    phase_ = Phase::SourceSubmitted;
}

namespace {

constexpr guest::GuestAddress kMovieWorkAddress = 0x0051a870U;
constexpr std::size_t kMovieWorkBytes = 0x38U;
constexpr guest::GuestAddress kHandoffAddress = 0x004e27f8U;
constexpr std::size_t kHandoffBytes = 72U;
constexpr std::size_t kBufferRangeBytes = 0x1b8800U;

[[nodiscard]] bool plan_matches_token(const Slus21050FemainMovieConsumerPlan& plan,
                                      const Slus21050FemainFrameGraphicsSourceToken& token) noexcept {
    if (plan.layout == token.layout && plan.frame_generation == token.frame_generation &&
        plan.media_ordinal == token.media_ordinal &&
        plan.source_sequence == token.source_sequence &&
        plan.source_checksum == token.source_checksum &&
        plan.payload_digest == token.payload_digest && plan.player_lease == token.frame_lease &&
        plan.timeline_ordinal == token.timeline_ordinal &&
        (token.source_range.buffer == plan.first_buffer ||
         token.source_range.buffer == plan.second_buffer) &&
        token.source_range.span_bytes != 0U && token.source_range.span_bytes <= plan.buffer_bytes) {
        if (token.published_reservation.has_value()) {
            // A post-publication token carries mutation tokens recaptured after the guest write.
            return true;
        }
        return plan.work_mutation == token.work_mutation &&
               plan.buffer_mutation == token.buffer_mutation;
    }
    return false;
}

// The marked quadword must lie wholly inside the retained source span.
[[nodiscard]] inline bool contains_span(const guest::EeMemory& memory,
                                        const guest::GuestAddress range_begin,
                                        const std::size_t range_bytes,
                                        const guest::GuestAddress address,
                                        const std::size_t address_bytes) noexcept {
    if (!memory.is_main_ram_range(range_begin, range_bytes) ||
        !memory.is_main_ram_range(address, address_bytes)) {
        return false;
    }
    const auto last = static_cast<std::uint64_t>(address) + address_bytes - 1U;
    if (last > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    return memory.main_ram_ranges_overlap(range_begin, range_bytes, address, 1U) &&
           memory.main_ram_ranges_overlap(range_begin, range_bytes,
                                          static_cast<guest::GuestAddress>(last), 1U);
}

[[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeFailure marker_failure(const guest::Vif1DmacPreSnapshotResolvedMarker& marker,
                                          const Slus21050FemainFrameGraphicsSourceRange& range,
                                          const guest::EeMemory& memory) noexcept {
    if (marker.token == 0U || marker.generation == 0U || marker.candidate_source == 0U ||
        marker.request.source == 0U || marker.request.chain_root == 0U ||
        marker.request.source_tag_location == 0U ||
        !contains_span(memory, range.buffer, range.span_bytes, marker.candidate_source, 16U) ||
        !contains_span(memory, range.buffer, range.span_bytes, marker.request.source, 16U) ||
        !memory.is_main_ram_range(range.buffer, range.span_bytes)) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerInvalid;
    }
    return Slus21050FemainFrameGraphicsSourceProbeFailure::None;
}

[[nodiscard]] bool source_complete(const Slus21050FemainLiveSourceIdentity& source) noexcept {
    return !source.revision.empty() && !source.function.empty() && source.function_address != 0U &&
           source.function_bytes != 0U && source.function_fingerprint != 0U &&
           source.executable_fingerprint != 0U;
}

[[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeFailure source_failure(const Slus21050FemainLiveSourceIdentity& source) noexcept {
    if (!source_complete(source)) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::SourceIdentityInvalid;
    }
    if (source != kSlus21050FemainLiveExpectedSource) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::SourceIdentityMismatch;
    }
    return Slus21050FemainFrameGraphicsSourceProbeFailure::None;
}

[[nodiscard]] bool consumer_plans_equal(const Slus21050FemainMovieConsumerPlan& left,
                                        const Slus21050FemainMovieConsumerPlan& right) noexcept {
    return left.layout == right.layout && left.first_buffer == right.first_buffer &&
           left.second_buffer == right.second_buffer && left.buffer_bytes == right.buffer_bytes &&
           left.frame_generation == right.frame_generation &&
           left.media_ordinal == right.media_ordinal &&
           left.source_sequence == right.source_sequence &&
           left.source_checksum == right.source_checksum &&
           left.payload_digest == right.payload_digest &&
           left.work_mutation == right.work_mutation &&
           left.buffer_mutation == right.buffer_mutation &&
           left.player_lease == right.player_lease &&
           left.timeline_ordinal == right.timeline_ordinal;
}

// Read-only participant revalidation. A null token checks a Prepared frame
// before capture; a token also binds the retained publication evidence.
[[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeFailure context_failure(const Slus21050FemainFrameGraphicsSourceContext& context,
                                           const Slus21050FemainFrameGraphicsSourceToken* const token) noexcept {
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = source_failure(context.source); failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return failure;
    }
    if (context.emulation_thread_identity == 0U) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::InvalidEmulationThreadIdentity;
    }
    if (context.media_clock_generation == 0U) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing;
    }
    if (context.transaction_owner == nullptr || context.player == nullptr ||
        context.consumer_plan == nullptr || context.ledger == nullptr) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing;
    }
    if (context.memory == nullptr) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMissing;
    }
    const Slus21050FemainMovieTransactionOwner& owner = *context.transaction_owner;
    const Slus21050MoviePlayer& player = *context.player;
    const Slus21050FemainMovieConsumerPlan& plan = *context.consumer_plan;
    const bool post_publication =
        token != nullptr ? token->published_reservation.has_value() : owner.publication_pending();
    if (!player.active()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerInactive;
    }
    if (player.identity() == 0U) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerIdentityMismatch;
    }
    if (player.session_generation() == 0U) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerSessionMismatch;
    }
    if (!owner.player_identity_matches() || owner.player_identity() != player.identity()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerPlayerMismatch;
    }
    if (token != nullptr && token->owner_identity != owner.identity()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerPlayerMismatch;
    }
    if (!owner.occupied()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerNotPrepared;
    }
    if (post_publication) {
        if (!owner.publication_pending()) {
            if (owner.guest_commit_complete()) {
                return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerGuestCommitted;
            }
            return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerNotPrepared;
        }
        const auto published = owner.published_reservation();
        if (!published.has_value() || published->ledger_identity == 0U ||
            published->session_generation == 0U || published->frame_generation == 0U ||
            (token != nullptr && (!token->published_reservation.has_value() ||
                                  !(*token->published_reservation == *published)))) {
            return Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch;
        }
    } else if (owner.guest_commit_complete()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerGuestCommitted;
    }
    const auto owner_session = owner.player_session_generation();
    if (!owner_session.has_value() || *owner_session != player.session_generation()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerPlayerSessionMismatch;
    }
    if (!player.has_outstanding_prepared_frame()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::PreparedFrameMissing;
    }
    const auto lease = owner.lease();
    const auto owner_plan = owner.consumer_plan();
    const auto handoff = owner.handoff_token();
    if (!lease.has_value() || lease->session_generation == 0U || lease->lease_generation == 0U ||
        !owner_plan.has_value() || !handoff.has_value() ||
        !consumer_plans_equal(*owner_plan, plan)) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch;
    }
    if (!plan.player_lease.has_value() || *plan.player_lease != *lease ||
        !plan.timeline_ordinal.has_value() || plan.layout.generation == 0U ||
        plan.frame_generation == 0U || plan.first_buffer == 0U || plan.second_buffer == 0U ||
        plan.buffer_bytes == 0U) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::FrameLeaseMismatch;
    }
    if (!post_publication) {
        if (!context.memory->validate_main_ram_mutation(plan.work_mutation, kMovieWorkAddress,
                                                        kMovieWorkBytes) ||
            !context.memory->validate_main_ram_mutation(plan.buffer_mutation, plan.first_buffer,
                                                        kBufferRangeBytes) ||
            !context.memory->validate_main_ram_mutation(handoff->mutation, kHandoffAddress,
                                                        kHandoffBytes)) {
            return Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMutationMismatch;
        }
    } else if (token != nullptr) {
        // Post-publication tokens carry mutation tokens captured after the guest wrote.
        if (!context.memory->validate_main_ram_mutation(token->work_mutation, kMovieWorkAddress,
                                                        kMovieWorkBytes) ||
            !context.memory->validate_main_ram_mutation(token->buffer_mutation, plan.first_buffer,
                                                        kBufferRangeBytes) ||
            !token->handoff.has_value() ||
            !context.memory->validate_main_ram_mutation(token->handoff->mutation, kHandoffAddress,
                                                        kHandoffBytes)) {
            return Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMutationMismatch;
        }
    }
    const auto ledger = context.ledger->snapshot();
    if (ledger.state != Slus21050FemainFrameLedgerState::Active || !ledger.proof_valid ||
        ledger.overflowed) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerInactive;
    }
    if (ledger.ledger_identity == 0U || ledger.session_generation != player.session_generation()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSessionMismatch;
    }
    if (post_publication) {
        const auto published = owner.published_reservation();
        const std::size_t slot =
            published.has_value() ? static_cast<std::size_t>(published->slot) : 2U;
        const auto owner_ledger_identity = owner.published_ledger_identity();
        const auto owner_ledger_session = owner.published_ledger_session_generation();
        if (!published.has_value() || !owner_ledger_identity.has_value() ||
            !owner_ledger_session.has_value() || ledger.ledger_identity != *owner_ledger_identity ||
            ledger.session_generation != *owner_ledger_session || slot >= ledger.slots.size() ||
            ledger.slots[slot] != Slus21050FemainFrameLedgerSlotState::Published) {
            return Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSnapshotMismatch;
        }
    } else if (ledger.next_media_ordinal != plan.media_ordinal ||
               plan.media_ordinal != ledger.next_publication_ordinal ||
               plan.media_ordinal != ledger.next_graphics_ordinal ||
               plan.media_ordinal != ledger.next_consumption_ordinal ||
               ledger.next_frame_generation == 0U ||
               ledger.next_frame_generation == std::numeric_limits<std::uint64_t>::max()) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSnapshotMismatch;
    }
    if (token != nullptr && !(token->ledger_before == ledger)) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSnapshotMismatch;
    }
    if (token != nullptr &&
        marker_failure(token->source_marker, token->source_range, *context.memory) !=
            Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerInvalid;
    }
    return Slus21050FemainFrameGraphicsSourceProbeFailure::None;
}

} // namespace

std::string_view to_string(const Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainFrameGraphicsSourceProbeFailure::None:
        return "none";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::Disabled:
        return "disabled";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::AuthenticationMismatch:
        return "authentication-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::SourceIdentityInvalid:
        return "source-identity-invalid";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::SourceIdentityMismatch:
        return "source-identity-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::InvalidEmulationThreadIdentity:
        return "invalid-emulation-thread-identity";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing:
        return "context-participant-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMissing:
        return "guest-memory-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerInactive:
        return "player-inactive";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerNotPrepared:
        return "owner-not-prepared";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerGuestCommitted:
        return "owner-guest-committed";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerPlayerMismatch:
        return "owner-player-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::OwnerPlayerSessionMismatch:
        return "owner-player-session-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PreparedFrameMissing:
        return "prepared-frame-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerIdentityMismatch:
        return "player-identity-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerSessionMismatch:
        return "player-session-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::FrameLeaseMismatch:
        return "frame-lease-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch:
        return "frame-plan-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMutationMismatch:
        return "guest-memory-mutation-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::SourceRangeInvalid:
        return "source-range-invalid";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerInvalid:
        return "source-marker-invalid";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerMismatch:
        return "source-marker-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerMissing:
        return "marker-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerCancelled:
        return "marker-cancelled";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered:
        return "marker-reordered";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerInactive:
        return "ledger-inactive";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerIdentityMismatch:
        return "ledger-identity-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSessionMismatch:
        return "ledger-session-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSnapshotMismatch:
        return "ledger-snapshot-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionMissing:
        return "raw-submission-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::RawPathMismatch:
        return "raw-path-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::RawQuadwordMismatch:
        return "raw-quadword-mismatch";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionReordered:
        return "raw-submission-reordered";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionMissing:
        return "packet-completion-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionReordered:
        return "packet-completion-reordered";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::PrimaryCommandStreamMissing:
        return "primary-command-stream-missing";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::RawReset:
        return "raw-reset";
    case Slus21050FemainFrameGraphicsSourceProbeFailure::Replay:
        return "replay";
    }
    return "unknown";
}

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::string_view kTokenDomain = "SLUS21050-FEMAIN-GRAPHICS-SOURCE-TOKEN-v1";
constexpr std::string_view kEvidenceDomain = "SLUS21050-FEMAIN-GRAPHICS-SOURCE-EVIDENCE-v2";

void mix_u8(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void mix_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    for (std::uint32_t shift = 0U; shift != 32U; shift += 8U) {
        mix_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void mix_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::uint32_t shift = 0U; shift != 64U; shift += 8U) {
        mix_u8(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void mix_bool(std::uint64_t& hash, const bool value) noexcept {
    mix_u8(hash, value ? 1U : 0U);
}

void mix_string(std::uint64_t& hash, const std::string_view string) noexcept {
    mix_u64(hash, string.size());
    for (const char character : string) {
        mix_u8(hash, static_cast<std::uint8_t>(character));
    }
}

void mix_boundary(std::uint64_t& hash, const Slus21050FemainFrameGuestBoundary& boundary) noexcept {
    mix_u64(hash, boundary.guest_vblank);
    mix_u64(hash, boundary.audio_sample);
}

void mix_optional_u64(std::uint64_t& hash, const std::optional<std::uint64_t>& value) noexcept {
    mix_bool(hash, value.has_value());
    if (value.has_value()) {
        mix_u64(hash, *value);
    }
}

void mix_optional_lease(std::uint64_t& hash,
                        const std::optional<Slus21050MovieFrameLease>& lease) noexcept {
    mix_bool(hash, lease.has_value());
    if (lease.has_value()) {
        mix_u64(hash, lease->session_generation);
        mix_u64(hash, lease->lease_generation);
    }
}

void mix_source(std::uint64_t& hash, const Slus21050FemainLiveSourceIdentity& source) noexcept {
    mix_string(hash, source.revision);
    mix_string(hash, source.function);
    mix_u32(hash, source.function_address);
    mix_u32(hash, source.function_bytes);
    mix_u64(hash, source.function_fingerprint);
    mix_u64(hash, source.executable_fingerprint);
}

void mix_layout(std::uint64_t& hash, const Slus21050FemainMovieLayoutSnapshot& layout) noexcept {
    for (const auto& slot : layout.slots) {
        mix_u32(hash, slot.buffer);
        mix_u32(hash, slot.texture_columns);
        mix_u32(hash, slot.texture_rows);
        mix_u32(hash, slot.macroblock_columns);
        mix_u32(hash, slot.macroblock_rows);
        mix_u32(hash, slot.gs_base);
    }
    mix_u32(hash, layout.work_witness_30);
    mix_u32(hash, layout.work_witness_34);
    mix_u64(hash, layout.generation);
}

void mix_handoff(std::uint64_t& hash,
                 const std::optional<Slus21050FemainHandoffToken>& handoff) noexcept {
    mix_bool(hash, handoff.has_value());
    if (!handoff.has_value()) {
        return;
    }
    const Slus21050FemainHandoffToken& token = *handoff;
    mix_u64(hash, token.frame_generation);
    mix_u64(hash, token.layout_generation);
    for (const auto queue_word : token.queue_table) {
        mix_u32(hash, queue_word);
    }
    mix_u32(hash, token.pending_end);
    mix_u32(hash, token.read_before);
    mix_u32(hash, token.commit_before);
    mix_u8(hash, token.busy_before);
    mix_u8(hash, token.buffer_index_before);
    mix_optional_lease(hash, token.player_lease);
    mix_optional_u64(hash, token.timeline_ordinal);
    mix_u64(hash, token.source_sequence);
    mix_u32(hash, token.source_checksum);
    mix_u64(hash, token.payload_digest);
    mix_bool(hash, token.mutation == guest::EeMainRamMutationToken{});
}

void mix_ledger(std::uint64_t& hash, const Slus21050FemainFrameLedgerSnapshot& ledger) noexcept {
    mix_u8(hash, static_cast<std::uint8_t>(ledger.state));
    mix_u64(hash, ledger.session_generation);
    mix_u64(hash, ledger.next_frame_generation);
    mix_u64(hash, ledger.next_media_ordinal);
    mix_u64(hash, ledger.next_publication_ordinal);
    mix_u64(hash, ledger.next_graphics_ordinal);
    mix_u64(hash, ledger.next_consumption_ordinal);
    mix_u64(hash, ledger.logical_event_count);
    mix_u64(hash, ledger.frame_record_count);
    mix_u64(hash, ledger.in_flight_count);
    mix_u64(hash, ledger.published_frames);
    mix_u64(hash, ledger.graphics_completed_frames);
    mix_u64(hash, ledger.consumed_frames);
    mix_u64(hash, ledger.repeated_frames);
    mix_u64(hash, ledger.dropped_frames);
    mix_u64(hash, ledger.cancelled_frames);
    mix_bool(hash, ledger.overflowed);
    mix_bool(hash, ledger.proof_valid);
    mix_u64(hash, ledger.digest);
    for (const auto slot : ledger.slots) {
        mix_u8(hash, static_cast<std::uint8_t>(slot));
    }
    mix_u64(hash, ledger.total_logical_event_count);
    mix_u64(hash, ledger.epoch_generation);
    mix_u64(hash, ledger.epoch_first_media_ordinal);
    mix_u64(hash, ledger.epoch_start_digest);
    mix_u64(hash, ledger.ledger_identity);
}

void mix_reservation(std::uint64_t& hash, const Slus21050FemainFrameReservation& reservation) noexcept {
    mix_u64(hash, reservation.ledger_identity);
    mix_u64(hash, reservation.session_generation);
    mix_u64(hash, reservation.frame_generation);
    mix_u8(hash, reservation.slot);
    mix_u64(hash, reservation.frame.media_ordinal);
    mix_u64(hash, reservation.frame.source_sequence);
    mix_u32(hash, reservation.frame.source_checksum);
    mix_boundary(hash, reservation.frame.produced_at);
}

void mix_optional_reservation(std::uint64_t& hash,
                              const std::optional<Slus21050FemainFrameReservation>& reservation) noexcept {
    mix_bool(hash, reservation.has_value());
    if (reservation.has_value()) {
        mix_reservation(hash, *reservation);
    }
}

void mix_marker(std::uint64_t& hash,
                const guest::Vif1DmacPreSnapshotResolvedMarker& marker) noexcept {
    mix_u64(hash, marker.token);
    mix_u64(hash, marker.generation);
    mix_u32(hash, marker.candidate_source);
    mix_u32(hash, marker.request.source);
    mix_u32(hash, marker.request.chain_root);
    mix_u32(hash, marker.request.source_tag_location);
    mix_u32(hash, marker.request.source_tag_ordinal);
    mix_u32(hash, marker.request.source_payload_word_ordinal);
    for (const auto word : marker.request.expected_quadword) {
        mix_u32(hash, word);
    }
}

[[nodiscard]] bool markers_equal(const guest::Vif1DmacPreSnapshotResolvedMarker& left,
                                 const guest::Vif1DmacPreSnapshotResolvedMarker& right) noexcept {
    return left.token == right.token && left.generation == right.generation &&
           left.candidate_source == right.candidate_source &&
           left.request.source == right.request.source &&
           left.request.chain_root == right.request.chain_root &&
           left.request.source_tag_location == right.request.source_tag_location &&
           left.request.source_tag_ordinal == right.request.source_tag_ordinal &&
           left.request.source_payload_word_ordinal == right.request.source_payload_word_ordinal &&
           left.request.expected_quadword == right.request.expected_quadword;
}

} // namespace

std::uint64_t authenticate_slus_21050_femain_frame_graphics_source_token(
    const Slus21050FemainFrameGraphicsSourceToken& token) noexcept {
    std::uint64_t hash = kFnvOffset;
    mix_string(hash, kTokenDomain);
    mix_source(hash, token.source);
    mix_u64(hash, token.emulation_thread_identity);
    mix_u64(hash, token.owner_identity);
    mix_u64(hash, token.player_identity);
    mix_u64(hash, token.player_session_generation);
    mix_u64(hash, token.frame_lease.session_generation);
    mix_u64(hash, token.frame_lease.lease_generation);
    mix_u64(hash, token.frame_generation);
    mix_u64(hash, token.media_ordinal);
    mix_u64(hash, token.source_sequence);
    mix_u32(hash, token.source_checksum);
    mix_u64(hash, token.payload_digest);
    mix_u64(hash, token.media_session_generation);
    mix_u64(hash, token.media_clock_nanoseconds);
    mix_u64(hash, token.media_clock_generation);
    mix_layout(hash, token.layout);
    mix_handoff(hash, token.handoff);
    mix_ledger(hash, token.ledger_before);
    mix_optional_reservation(hash, token.published_reservation);
    mix_u32(hash, token.source_range.buffer);
    mix_u64(hash, token.source_range.span_bytes);
    mix_marker(hash, token.source_marker);
    mix_bool(hash, token.work_mutation == guest::EeMainRamMutationToken{});
    mix_bool(hash, token.buffer_mutation == guest::EeMainRamMutationToken{});
    mix_optional_u64(hash, token.timeline_ordinal);
    return hash;
}

// RECOVERY: not yet identical under tools/gimple.py; structure approximated from GIMPLE.
bool Slus21050FemainFrameGraphicsSourceToken::operator==(
    const Slus21050FemainFrameGraphicsSourceToken& other) const noexcept {
    return source == other.source && emulation_thread_identity == other.emulation_thread_identity &&
           owner_identity == other.owner_identity && player_identity == other.player_identity &&
           player_session_generation == other.player_session_generation &&
           frame_lease == other.frame_lease && frame_generation == other.frame_generation &&
           media_ordinal == other.media_ordinal && source_sequence == other.source_sequence &&
           source_checksum == other.source_checksum && payload_digest == other.payload_digest &&
           media_session_generation == other.media_session_generation &&
           media_clock_nanoseconds == other.media_clock_nanoseconds &&
           media_clock_generation == other.media_clock_generation && layout == other.layout &&
           handoff == other.handoff && ledger_before == other.ledger_before &&
           published_reservation == other.published_reservation &&
           source_range == other.source_range && markers_equal(source_marker, other.source_marker) &&
           work_mutation == other.work_mutation && buffer_mutation == other.buffer_mutation &&
           timeline_ordinal == other.timeline_ordinal &&
           authentication_tag == other.authentication_tag;
}

// RECOVERY: not yet identical under tools/gimple.py; structure approximated from GIMPLE.
std::uint64_t authenticate_slus_21050_femain_frame_graphics_source_evidence(
    const Slus21050FemainFrameGraphicsSourceEvidence& evidence) noexcept {
    std::uint64_t hash = kFnvOffset;
    mix_string(hash, kEvidenceDomain);
    mix_u64(hash, authenticate_slus_21050_femain_frame_graphics_source_token(evidence.token));
    mix_u64(hash, evidence.diagnostic_observation_epoch);
    mix_u64(hash, evidence.source_event_ordinal);
    mix_u64(hash, evidence.raw_submission_event_ordinal);
    mix_u64(hash, evidence.packet_completion_event_ordinal);
    const auto hash_diagnostic_event =
        [&hash](const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept {
            mix_u64(hash, event.domain_identity);
            mix_u64(hash, event.observation_epoch);
            mix_u64(hash, event.ordinal);
            mix_u8(hash, static_cast<std::uint8_t>(event.kind));
        };
    hash_diagnostic_event(evidence.diagnostic_marker_event);
    hash_diagnostic_event(evidence.diagnostic_raw_submission_event);
    hash_diagnostic_event(evidence.diagnostic_packet_completion_event);
    mix_bool(hash, evidence.resolver_proof.has_value());
    if (evidence.resolver_proof.has_value()) {
        const Slus21050FemainLiveResolverSourceProof& proof = *evidence.resolver_proof;
        const Slus21050FemainLiveSourceCandidate& candidate = proof.candidate;
        mix_u32(hash, candidate.source);
        mix_u32(hash, candidate.chain_root);
        mix_u32(hash, candidate.source_tag_location);
        mix_u32(hash, candidate.source_tag_ordinal);
        mix_u32(hash, candidate.terminal_tag_location);
        mix_u32(hash, candidate.terminal_tag_ordinal);
        for (const auto word : candidate.source_tag_words) {
            mix_u32(hash, word);
        }
        for (const auto word : candidate.terminal_tag_words) {
            mix_u32(hash, word);
        }
        mix_u32(hash, candidate.source_payload_word_ordinal);
        mix_u32(hash, candidate.source_qwc);
        for (const auto word : candidate.expected_quadword) {
            mix_u32(hash, word);
        }
        mix_u64(hash, candidate.publication_serial);
        mix_u64(hash, candidate.player_session_generation);
        mix_u64(hash, candidate.frame_generation);
        mix_u64(hash, candidate.authentication_tag);
        mix_u32(hash, proof.publication_queue.queue_read);
        mix_u32(hash, proof.publication_queue.queue_end);
        mix_u8(hash, proof.publication_queue.transfers_active);
        mix_u8(hash, proof.publication_queue.buffer_index);
        mix_u32(hash, proof.publication_queue.queue_base);
        mix_u32(hash, proof.matched_source_qwc);
        mix_u64(hash, proof.matched_span_digest);
    }
    return hash;
}

Slus21050FemainFrameGraphicsSourceCaptureResult
capture_slus_21050_femain_frame_graphics_source_token(
    const Slus21050FemainFrameGraphicsSourceContext& context,
    const guest::Vif1DmacPreSnapshotResolvedMarker& source_marker,
    const guest::GuestAddress source_buffer, const std::size_t source_span_bytes) noexcept {
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = context_failure(context, nullptr);
        failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return {.failure = failure};
    }
    const Slus21050FemainMovieConsumerPlan& plan = *context.consumer_plan;
    const Slus21050FemainFrameGraphicsSourceRange range{
        .buffer = source_buffer,
        .span_bytes = source_span_bytes,
    };
    if (source_span_bytes == 0U || source_span_bytes > plan.buffer_bytes ||
        (source_buffer != plan.first_buffer && source_buffer != plan.second_buffer) ||
        !context.memory->is_main_ram_range(source_buffer, source_span_bytes)) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::SourceRangeInvalid, std::nullopt};
    }
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = marker_failure(source_marker, range, *context.memory);
        failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return {.failure = Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerInvalid};
    }
    auto handoff = context.transaction_owner->handoff_token();
    const auto lease = context.transaction_owner->lease();
    const auto ledger = context.ledger->snapshot();
    const auto published_reservation = context.transaction_owner->published_reservation();
    const bool post_publication = context.transaction_owner->publication_pending();
    guest::EeMainRamMutationToken work_mutation = plan.work_mutation;
    guest::EeMainRamMutationToken buffer_mutation = plan.buffer_mutation;
    if (post_publication) {
        // The prepared plan's tokens predate the guest write; capture fresh ones.
        if (!published_reservation.has_value() || !handoff.has_value()) {
            return {Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch, std::nullopt};
        }
        const auto work_capture =
            context.memory->capture_main_ram_mutation(kMovieWorkAddress, kMovieWorkBytes);
        const auto buffer_capture =
            context.memory->capture_main_ram_mutation(plan.first_buffer, kBufferRangeBytes);
        const auto handoff_capture =
            context.memory->capture_main_ram_mutation(kHandoffAddress, kHandoffBytes);
        if (work_capture.status != guest::EeMainRamMutationCaptureStatus::Captured ||
            buffer_capture.status != guest::EeMainRamMutationCaptureStatus::Captured ||
            handoff_capture.status != guest::EeMainRamMutationCaptureStatus::Captured) {
            return {Slus21050FemainFrameGraphicsSourceProbeFailure::GuestMemoryMutationMismatch, std::nullopt};
        }
        work_mutation = work_capture.token;
        buffer_mutation = buffer_capture.token;
        handoff->mutation = handoff_capture.token;
    }
    Slus21050FemainFrameGraphicsSourceToken token{
        .source = kSlus21050FemainLiveExpectedSource,
        .emulation_thread_identity = context.emulation_thread_identity,
        .owner_identity = context.transaction_owner->identity(),
        .player_identity = context.player->identity(),
        .player_session_generation = context.player->session_generation(),
        .frame_lease = *lease,
        .frame_generation = plan.frame_generation,
        .media_ordinal = plan.media_ordinal,
        .source_sequence = plan.source_sequence,
        .source_checksum = plan.source_checksum,
        .payload_digest = plan.payload_digest,
        .media_session_generation = context.player->session_generation(),
        .media_clock_nanoseconds = context.media_clock_nanoseconds,
        .media_clock_generation = context.media_clock_generation,
        .layout = plan.layout,
        .handoff = handoff,
        .ledger_before = ledger,
        .published_reservation = post_publication
                                     ? published_reservation
                                     : std::optional<Slus21050FemainFrameReservation>{},
        .source_range = range,
        .source_marker = source_marker,
        .work_mutation = work_mutation,
        .buffer_mutation = buffer_mutation,
        .timeline_ordinal = plan.timeline_ordinal,
    };
    token.authentication_tag = authenticate_slus_21050_femain_frame_graphics_source_token(token);
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = context_failure(context, &token);
        failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return {.failure = failure};
    }
    return {Slus21050FemainFrameGraphicsSourceProbeFailure::None, token};
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::result() const noexcept {
    return {failure_, evidence_};
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::fail(const Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept {
    if (failure_ == Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        failure_ = failure;
    }
    evidence_.reset();
    phase_ = Phase::Failed;
    return result();
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::arm(const Slus21050FemainFrameGraphicsSourceToken& token,
                       const Slus21050FemainFrameGraphicsSourceContext& context,
                       const std::uint64_t diagnostic_observation_epoch,
                       const std::uint64_t diagnostic_event_domain_identity) noexcept {
    if (!enabled_) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Disabled, std::nullopt};
    }
    if (phase_ != Phase::Idle) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Replay, std::nullopt};
    }
    if (token.authentication_tag == 0U ||
        token.authentication_tag != authenticate_slus_21050_femain_frame_graphics_source_token(token)) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::AuthenticationMismatch);
    }
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = context_failure(context, &token);
        failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(failure);
    }
    if (const Slus21050FemainFrameGraphicsSourceProbeFailure failure = source_failure(token.source); failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(failure);
    }
    if (token.source != context.source) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::SourceIdentityMismatch);
    }
    token_ = token;
    context_ = context;
    context_bound_ = true;
    diagnostic_observation_epoch_ = diagnostic_observation_epoch;
    diagnostic_event_domain_identity_ = diagnostic_event_domain_identity;
    if (diagnostic_event_domain_identity != 0U && diagnostic_observation_epoch == 0U) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing);
    }
    if (token.emulation_thread_identity != context.emulation_thread_identity ||
        token.media_clock_nanoseconds != context.media_clock_nanoseconds ||
        token.media_clock_generation != context.media_clock_generation) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing);
    }
    if (token.player_identity != context.player->identity()) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerIdentityMismatch);
    }
    const auto lease = context.transaction_owner->lease();
    if (token.player_session_generation != context.player->session_generation() ||
        token.media_session_generation != token.player_session_generation || !lease.has_value()) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PlayerSessionMismatch);
    }
    if (*lease != token.frame_lease) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::FrameLeaseMismatch);
    }
    if (!plan_matches_token(*context.consumer_plan, token)) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch);
    }
    const auto handoff = context.transaction_owner->handoff_token();
    bool handoff_matches = false;
    if (handoff.has_value() && token.handoff.has_value()) {
        if (token.published_reservation.has_value()) {
            // Post-publication mutation tokens were recaptured after the guest wrote.
            Slus21050FemainHandoffToken expected = *handoff;
            expected.mutation = token.handoff->mutation;
            handoff_matches = expected == *token.handoff;
        } else {
            handoff_matches = *handoff == *token.handoff;
        }
    }
    if (!handoff_matches) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch);
    }
    if (marker_failure(token.source_marker, token.source_range, *context.memory) !=
        Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerInvalid);
    }
    const auto ledger = context.ledger->snapshot();
    if (token.ledger_before.ledger_identity != ledger.ledger_identity) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerIdentityMismatch);
    }
    if (token.ledger_before.session_generation != ledger.session_generation) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSessionMismatch);
    }
    if (!(token.ledger_before == ledger)) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::LedgerSnapshotMismatch);
    }
    event_ordinal_ = 0U;
    source_event_ordinal_ = 0U;
    raw_submission_event_ordinal_ = 0U;
    packet_completion_event_ordinal_ = 0U;
    last_diagnostic_event_ordinal_ = 0U;
    diagnostic_marker_event_ = {};
    diagnostic_raw_submission_event_ = {};
    diagnostic_packet_completion_event_ = {};
    evidence_.reset();
    failure_ = Slus21050FemainFrameGraphicsSourceProbeFailure::None;
    phase_ = Phase::Armed;
    return {Slus21050FemainFrameGraphicsSourceProbeFailure::None, std::nullopt};
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_vif1_path2_source_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    if (diagnostic_event_domain_identity_ != 0U) {
        // Shared diagnostics must use the identified callbacks.
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered);
    }
    return observe_marker_reached_impl(marker);
}

bool Slus21050FemainFrameGraphicsSourceProbe::accept_diagnostic_event(const Slus21050FemainSourceDiagnosticEventIdentity& event,
                                    const Slus21050FemainSourceDiagnosticEventKind expected_kind,
                                    const Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept {
    if (diagnostic_event_domain_identity_ == 0U || !event.valid() ||
        event.domain_identity != diagnostic_event_domain_identity_ ||
        event.observation_epoch != diagnostic_observation_epoch_ || event.kind != expected_kind ||
        event.ordinal <= last_diagnostic_event_ordinal_) {
        static_cast<void>(fail(failure));
        return false;
    }
    last_diagnostic_event_ordinal_ = event.ordinal;
    return true;
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_diagnostic_marker_reached(
    const guest::Vif1Path2SourceMarkerReached& marker,
    const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept {
    if (!accept_diagnostic_event(event, Slus21050FemainSourceDiagnosticEventKind::MarkerReached,
                                 Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered)) {
        return result();
    }
    const Slus21050FemainFrameGraphicsSourceProbeResult observed = observe_marker_reached_impl(marker);
    if (observed.failure == Slus21050FemainFrameGraphicsSourceProbeFailure::None && phase_ == Phase::SourceReached) {
        diagnostic_marker_event_ = event;
    }
    return observed;
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_marker_reached_impl(
    const guest::Vif1Path2SourceMarkerReached& marker) noexcept {
    if (!enabled_) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Disabled, std::nullopt};
    }
    if (phase_ == Phase::Completed) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::Replay);
    }
    if (phase_ != Phase::Armed) {
        if (phase_ == Phase::Failed) {
            return result();
        }
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered);
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(context_failure);
    }
    const guest::Vif1DmacPreSnapshotResolvedMarker& expected = token_.source_marker;
    if (marker.generation != expected.generation || marker.source != expected.request.source ||
        marker.chain_root != expected.request.chain_root ||
        marker.source_tag_location != expected.request.source_tag_location ||
        marker.source_tag_ordinal != expected.request.source_tag_ordinal ||
        marker.source_payload_word_ordinal != expected.request.source_payload_word_ordinal ||
        marker.quadword != expected.request.expected_quadword) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerMismatch);
    }
    if (event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered);
    }
    ++event_ordinal_;
    source_event_ordinal_ = event_ordinal_;
    phase_ = Phase::SourceReached;
    return result();
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_vif1_path2_source_marker_cancelled(
    const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept {
    if (!enabled_) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Disabled, std::nullopt};
    }
    if (phase_ == Phase::Completed) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::Replay);
    }
    if (phase_ != Phase::Armed) {
        if (phase_ == Phase::Failed) {
            return result();
        }
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerReordered);
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(context_failure);
    }
    const guest::Vif1DmacPreSnapshotResolvedMarker& expected = token_.source_marker;
    if (marker.generation != expected.generation || marker.source != expected.request.source ||
        marker.chain_root != expected.request.chain_root ||
        marker.source_tag_location != expected.request.source_tag_location ||
        marker.source_tag_ordinal != expected.request.source_tag_ordinal ||
        marker.source_payload_word_ordinal != expected.request.source_payload_word_ordinal) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::SourceMarkerMismatch);
    }
    return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerCancelled);
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::close() noexcept {
    if (!enabled_) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Disabled, std::nullopt};
    }
    if (phase_ == Phase::Completed) {
        return {Slus21050FemainFrameGraphicsSourceProbeFailure::Replay, std::nullopt};
    }
    if (phase_ == Phase::Failed) {
        return result();
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return fail(context_failure);
    }
    if (phase_ == Phase::Armed) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::MarkerMissing);
    }
    if (phase_ == Phase::SourceReached) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionMissing);
    }
    if (phase_ == Phase::SourceSubmitted) {
        return fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionMissing);
    }
    return result();
}

std::optional<Slus21050FemainFrameGraphicsSourceEvidence> Slus21050FemainFrameGraphicsSourceProbe::take_evidence() noexcept {
    if (phase_ != Phase::Completed || !evidence_.has_value()) {
        return std::nullopt;
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        static_cast<void>(fail(context_failure));
        return std::nullopt;
    }
    auto result = evidence_;
    evidence_.reset();
    return result;
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_diagnostic_raw_quadword(
    const guest::GifPath path, const guest::GsQuadword& quadword,
    const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept {
    if (phase_ != Phase::SourceReached) {
        observe_raw_quadword_impl(path, quadword);
        return result();
    }
    if (!accept_diagnostic_event(event, Slus21050FemainSourceDiagnosticEventKind::RawSubmission,
                                 Slus21050FemainFrameGraphicsSourceProbeFailure::RawSubmissionReordered)) {
        return result();
    }
    observe_raw_quadword_impl(path, quadword);
    if (failure_ == Slus21050FemainFrameGraphicsSourceProbeFailure::None && phase_ == Phase::SourceSubmitted) {
        diagnostic_raw_submission_event_ = event;
    }
    return result();
}

void Slus21050FemainFrameGraphicsSourceProbe::on_gs_gif_packet_eop(const guest::GifPath path,
                                 const bool primary_command_stream_available) noexcept {
    if (diagnostic_event_domain_identity_ != 0U) {
        if (phase_ == Phase::SourceReached || phase_ == Phase::SourceSubmitted) {
            static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionReordered));
        }
        return;
    }
    observe_packet_eop_impl(path, primary_command_stream_available);
}

Slus21050FemainFrameGraphicsSourceProbeResult Slus21050FemainFrameGraphicsSourceProbe::observe_diagnostic_packet_eop(
    const guest::GifPath path, const bool primary_command_stream_available,
    const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept {
    if (phase_ != Phase::SourceReached && phase_ != Phase::SourceSubmitted) {
        observe_packet_eop_impl(path, primary_command_stream_available);
        return result();
    }
    if (!accept_diagnostic_event(event, Slus21050FemainSourceDiagnosticEventKind::PacketCompletion,
                                 Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionReordered)) {
        return result();
    }
    diagnostic_packet_completion_event_ = event;
    observe_packet_eop_impl(path, primary_command_stream_available);
    return result();
}

// RECOVERY: not yet identical under tools/gimple.py; structure approximated from GIMPLE.
void Slus21050FemainFrameGraphicsSourceProbe::observe_packet_eop_impl(const guest::GifPath path,
                                    const bool primary_command_stream_available) noexcept {
    if (!enabled_ || phase_ == Phase::Idle || phase_ == Phase::Armed || phase_ == Phase::Failed ||
        phase_ == Phase::Completed || path != guest::GifPath::Path2) {
        return;
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure context_failure = current_context_failure();
    if (context_failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        static_cast<void>(fail(context_failure));
        return;
    }
    if (phase_ == Phase::SourceReached) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionReordered));
        return;
    }
    if (!primary_command_stream_available) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PrimaryCommandStreamMissing));
        return;
    }
    if (event_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::PacketCompletionReordered));
        return;
    }
    ++event_ordinal_;
    packet_completion_event_ordinal_ = event_ordinal_;
    Slus21050FemainFrameGraphicsSourceEvidence evidence{};
    evidence.token = token_;
    evidence.resolver_proof = {};
    evidence.diagnostic_observation_epoch = diagnostic_observation_epoch_;
    evidence.source_event_ordinal = source_event_ordinal_;
    evidence.raw_submission_event_ordinal = raw_submission_event_ordinal_;
    evidence.packet_completion_event_ordinal = packet_completion_event_ordinal_;
    evidence.diagnostic_marker_event = diagnostic_marker_event_;
    evidence.diagnostic_raw_submission_event = diagnostic_raw_submission_event_;
    evidence.diagnostic_packet_completion_event = diagnostic_packet_completion_event_;
    evidence.authentication_tag = authenticate_slus_21050_femain_frame_graphics_source_evidence(evidence);
    evidence_ = evidence;
    phase_ = Phase::Completed;
}

void Slus21050FemainFrameGraphicsSourceProbe::on_gs_gif_reset() noexcept {
    if (!enabled_ || phase_ == Phase::Idle || phase_ == Phase::Failed || phase_ == Phase::Completed) {
        return;
    }
    static_cast<void>(fail(Slus21050FemainFrameGraphicsSourceProbeFailure::RawReset));
}

// RECOVERY: not yet identical under tools/gimple.py; structure approximated from GIMPLE.
Slus21050FemainFrameGraphicsSourceProbeFailure Slus21050FemainFrameGraphicsSourceProbe::current_context_failure() const noexcept {
    if (!context_bound_) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing;
    }
    const Slus21050FemainFrameGraphicsSourceProbeFailure failure = context_failure(context_, &token_);
    if (failure != Slus21050FemainFrameGraphicsSourceProbeFailure::None) {
        return failure;
    }
    if (context_.consumer_plan == nullptr || !plan_matches_token(*context_.consumer_plan, token_)) {
        return Slus21050FemainFrameGraphicsSourceProbeFailure::FramePlanMismatch;
    }
    return Slus21050FemainFrameGraphicsSourceProbeFailure::None;
}

} // namespace takedown::runtime
