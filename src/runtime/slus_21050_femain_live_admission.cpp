#include "takedown/runtime/slus_21050_femain_live_admission.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_femain_frame_transaction_ledger.hpp"
#include "takedown/runtime/slus_21050_movie_player.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

namespace {

[[nodiscard]] bool not_after(const Slus21050FemainFrameGuestBoundary& earlier,
                             const Slus21050FemainFrameGuestBoundary& later) noexcept {
    return earlier.guest_vblank <= later.guest_vblank && earlier.audio_sample <= later.audio_sample;
}

[[nodiscard]] bool graphics_order_valid(
    const Slus21050FemainMovieTransactionBoundaries& boundaries) noexcept {
    return boundaries.vif1_before_gif && not_after(boundaries.produced_at, boundaries.published_at) &&
           not_after(boundaries.published_at, boundaries.graphics_completed_at) &&
           not_after(boundaries.graphics_completed_at, boundaries.consumed_at);
}

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::string_view kAuthenticationDomain = "SLUS21050-FEMAIN-LIVE-ADMISSION-v1";

void mix_u8(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void mix_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    mix_u8(hash, static_cast<std::uint8_t>(value & 0xffU));
    mix_u8(hash, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    mix_u8(hash, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    mix_u8(hash, static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void mix_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    mix_u32(hash, static_cast<std::uint32_t>(value));
    mix_u32(hash, static_cast<std::uint32_t>(value >> 32U));
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

} // namespace

std::string_view to_string(const Slus21050FemainLiveAdmissionFailure failure) noexcept {
    switch (failure) {
    case Slus21050FemainLiveAdmissionFailure::None:
        return "none";
    case Slus21050FemainLiveAdmissionFailure::Disabled:
        return "disabled";
    case Slus21050FemainLiveAdmissionFailure::SourceIdentityInvalid:
        return "source-identity-invalid";
    case Slus21050FemainLiveAdmissionFailure::SourceRevisionMismatch:
        return "source-revision-mismatch";
    case Slus21050FemainLiveAdmissionFailure::SourceFunctionMismatch:
        return "source-function-mismatch";
    case Slus21050FemainLiveAdmissionFailure::SourceFunctionAddressMismatch:
        return "source-function-address-mismatch";
    case Slus21050FemainLiveAdmissionFailure::SourceFunctionExtentMismatch:
        return "source-function-extent-mismatch";
    case Slus21050FemainLiveAdmissionFailure::SourceFunctionFingerprintMismatch:
        return "source-function-fingerprint-mismatch";
    case Slus21050FemainLiveAdmissionFailure::SourceExecutableFingerprintMismatch:
        return "source-executable-fingerprint-mismatch";
    case Slus21050FemainLiveAdmissionFailure::InvalidEmulationThreadIdentity:
        return "invalid-emulation-thread-identity";
    case Slus21050FemainLiveAdmissionFailure::EmulationThreadIdentityMismatch:
        return "emulation-thread-identity-mismatch";
    case Slus21050FemainLiveAdmissionFailure::ContextParticipantMissing:
        return "context-participant-missing";
    case Slus21050FemainLiveAdmissionFailure::GuestMemoryMissing:
        return "guest-memory-missing";
    case Slus21050FemainLiveAdmissionFailure::GuestMemoryMutationMismatch:
        return "guest-memory-mutation-mismatch";
    case Slus21050FemainLiveAdmissionFailure::PlayerInactive:
        return "player-inactive";
    case Slus21050FemainLiveAdmissionFailure::OwnerNotPrepared:
        return "owner-not-prepared";
    case Slus21050FemainLiveAdmissionFailure::OwnerGuestCommitted:
        return "owner-guest-committed";
    case Slus21050FemainLiveAdmissionFailure::OwnerPlayerMismatch:
        return "owner-player-mismatch";
    case Slus21050FemainLiveAdmissionFailure::OwnerPlayerSessionMismatch:
        return "owner-player-session-mismatch";
    case Slus21050FemainLiveAdmissionFailure::PreparedFrameMissing:
        return "prepared-frame-missing";
    case Slus21050FemainLiveAdmissionFailure::PlayerIdentityMismatch:
        return "player-identity-mismatch";
    case Slus21050FemainLiveAdmissionFailure::PlayerSessionMismatch:
        return "player-session-mismatch";
    case Slus21050FemainLiveAdmissionFailure::FrameLeaseMismatch:
        return "frame-lease-mismatch";
    case Slus21050FemainLiveAdmissionFailure::FramePlanMismatch:
        return "frame-plan-mismatch";
    case Slus21050FemainLiveAdmissionFailure::MediaSessionMismatch:
        return "media-session-mismatch";
    case Slus21050FemainLiveAdmissionFailure::MediaClockInvalid:
        return "media-clock-invalid";
    case Slus21050FemainLiveAdmissionFailure::MediaClockMismatch:
        return "media-clock-mismatch";
    case Slus21050FemainLiveAdmissionFailure::LayoutGenerationMismatch:
        return "layout-generation-mismatch";
    case Slus21050FemainLiveAdmissionFailure::LayoutSnapshotMismatch:
        return "layout-snapshot-mismatch";
    case Slus21050FemainLiveAdmissionFailure::LedgerInactive:
        return "ledger-inactive";
    case Slus21050FemainLiveAdmissionFailure::LedgerIdentityMismatch:
        return "ledger-identity-mismatch";
    case Slus21050FemainLiveAdmissionFailure::LedgerSessionMismatch:
        return "ledger-session-mismatch";
    case Slus21050FemainLiveAdmissionFailure::LedgerSnapshotMismatch:
        return "ledger-snapshot-mismatch";
    case Slus21050FemainLiveAdmissionFailure::GraphicsEvidenceMissing:
        return "graphics-evidence-missing";
    case Slus21050FemainLiveAdmissionFailure::GraphicsOrderInvalid:
        return "graphics-order-invalid";
    case Slus21050FemainLiveAdmissionFailure::GraphicsEvidenceMismatch:
        return "graphics-evidence-mismatch";
    case Slus21050FemainLiveAdmissionFailure::AuthenticationMismatch:
        return "authentication-mismatch";
    case Slus21050FemainLiveAdmissionFailure::Replay:
        return "replay";
    }
    return "unknown";
}

std::uint64_t authenticate_slus_21050_femain_live_admission(
    const Slus21050FemainLiveAdmissionSnapshot& snapshot) noexcept {
    std::uint64_t hash = kFnvOffset;
    mix_string(hash, kAuthenticationDomain);
    mix_source(hash, snapshot.source);
    mix_u64(hash, snapshot.emulation_thread_identity);
    mix_u64(hash, snapshot.player_identity);
    mix_u64(hash, snapshot.player_session_generation);
    mix_u64(hash, snapshot.frame_lease.session_generation);
    mix_u64(hash, snapshot.frame_lease.lease_generation);
    mix_u64(hash, snapshot.frame_generation);
    mix_u64(hash, snapshot.media_ordinal);
    mix_u64(hash, snapshot.source_sequence);
    mix_u32(hash, snapshot.source_checksum);
    mix_u64(hash, snapshot.payload_digest);
    mix_u64(hash, snapshot.media_session_generation);
    mix_u64(hash, snapshot.media_clock_nanoseconds);
    mix_u64(hash, snapshot.media_clock_generation);
    mix_layout(hash, snapshot.layout);
    mix_handoff(hash, snapshot.handoff);
    mix_ledger(hash, snapshot.ledger);
    mix_boundary(hash, snapshot.graphics_boundaries.produced_at);
    mix_boundary(hash, snapshot.graphics_boundaries.published_at);
    mix_boundary(hash, snapshot.graphics_boundaries.graphics_completed_at);
    mix_boundary(hash, snapshot.graphics_boundaries.consumed_at);
    mix_bool(hash, snapshot.graphics_boundaries.vif1_before_gif);
    mix_bool(hash, snapshot.graphics_evidence_present);
    mix_u32(hash, snapshot.first_buffer);
    mix_u32(hash, snapshot.second_buffer);
    mix_u64(hash, snapshot.buffer_bytes);
    mix_bool(hash, snapshot.work_mutation == guest::EeMainRamMutationToken{});
    mix_bool(hash, snapshot.buffer_mutation == guest::EeMainRamMutationToken{});
    mix_optional_u64(hash, snapshot.timeline_ordinal);
    return hash;
}

namespace {

constexpr guest::GuestAddress kMovieWorkAddress = 0x0051a870U;
constexpr std::size_t kMovieWorkBytes = 0x38U;
constexpr guest::GuestAddress kHandoffAddress = 0x004e27f8U;
constexpr std::size_t kHandoffBytes = 72U;
constexpr std::size_t kBufferRangeBytes = 0x1b8800U;

[[nodiscard]] bool source_complete(const Slus21050FemainLiveSourceIdentity& source) noexcept {
    return !source.revision.empty() && !source.function.empty() && source.function_address != 0U &&
           source.function_bytes != 0U && source.function_fingerprint != 0U &&
           source.executable_fingerprint != 0U;
}

[[nodiscard]] Slus21050FemainLiveAdmissionFailure
validate_source(const Slus21050FemainLiveSourceIdentity& source) noexcept {
    if (!source_complete(source)) {
        return Slus21050FemainLiveAdmissionFailure::SourceIdentityInvalid;
    }
    if (source.revision != kSlus21050FemainLiveRevision) {
        return Slus21050FemainLiveAdmissionFailure::SourceRevisionMismatch;
    }
    if (source.function != kSlus21050FemainLiveFunction) {
        return Slus21050FemainLiveAdmissionFailure::SourceFunctionMismatch;
    }
    if (source.function_address != kSlus21050FemainLiveFunctionAddress) {
        return Slus21050FemainLiveAdmissionFailure::SourceFunctionAddressMismatch;
    }
    if (source.function_bytes != kSlus21050FemainLiveFunctionBytes) {
        return Slus21050FemainLiveAdmissionFailure::SourceFunctionExtentMismatch;
    }
    if (source.function_fingerprint != kSlus21050FemainLiveFunctionFingerprint) {
        return Slus21050FemainLiveAdmissionFailure::SourceFunctionFingerprintMismatch;
    }
    if (source.executable_fingerprint != kSlus21050FemainLiveExecutableFingerprint) {
        return Slus21050FemainLiveAdmissionFailure::SourceExecutableFingerprintMismatch;
    }
    return Slus21050FemainLiveAdmissionFailure::None;
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

[[nodiscard]] bool plan_matches_snapshot(const Slus21050FemainMovieConsumerPlan& plan,
                                         const Slus21050FemainLiveAdmissionSnapshot& snapshot) noexcept {
    return plan.first_buffer == snapshot.first_buffer &&
           plan.second_buffer == snapshot.second_buffer &&
           plan.buffer_bytes == snapshot.buffer_bytes &&
           plan.frame_generation == snapshot.frame_generation &&
           plan.media_ordinal == snapshot.media_ordinal &&
           plan.source_sequence == snapshot.source_sequence &&
           plan.source_checksum == snapshot.source_checksum &&
           plan.payload_digest == snapshot.payload_digest &&
           plan.work_mutation == snapshot.work_mutation &&
           plan.buffer_mutation == snapshot.buffer_mutation &&
           plan.player_lease == snapshot.frame_lease &&
           plan.timeline_ordinal == snapshot.timeline_ordinal;
}

[[nodiscard]] Slus21050FemainLiveAdmissionFailure
validate_context(const Slus21050FemainLiveAdmissionContext& context) noexcept {
    if (!source_complete(context.source)) {
        return Slus21050FemainLiveAdmissionFailure::SourceIdentityInvalid;
    }
    if (context.emulation_thread_identity == 0U) {
        return Slus21050FemainLiveAdmissionFailure::InvalidEmulationThreadIdentity;
    }
    if (context.media_clock_generation == 0U) {
        return Slus21050FemainLiveAdmissionFailure::MediaClockInvalid;
    }
    if (context.transaction_owner == nullptr || context.player == nullptr ||
        context.consumer_plan == nullptr || context.ledger == nullptr) {
        return Slus21050FemainLiveAdmissionFailure::ContextParticipantMissing;
    }
    if (context.memory == nullptr) {
        return Slus21050FemainLiveAdmissionFailure::GuestMemoryMissing;
    }
    if (!context.player->active()) {
        return Slus21050FemainLiveAdmissionFailure::PlayerInactive;
    }
    if (!context.transaction_owner->occupied()) {
        return Slus21050FemainLiveAdmissionFailure::OwnerNotPrepared;
    }
    if (context.transaction_owner->guest_commit_complete()) {
        return Slus21050FemainLiveAdmissionFailure::OwnerGuestCommitted;
    }
    if (!context.transaction_owner->player_identity_matches() ||
        context.transaction_owner->player_identity() != context.player->identity()) {
        return Slus21050FemainLiveAdmissionFailure::OwnerPlayerMismatch;
    }
    if (context.player->identity() == 0U) {
        return Slus21050FemainLiveAdmissionFailure::PlayerIdentityMismatch;
    }
    if (context.player->session_generation() == 0U) {
        return Slus21050FemainLiveAdmissionFailure::PlayerSessionMismatch;
    }
    const auto owner_session = context.transaction_owner->player_session_generation();
    if (!owner_session.has_value() || *owner_session != context.player->session_generation()) {
        return Slus21050FemainLiveAdmissionFailure::OwnerPlayerSessionMismatch;
    }
    if (!context.player->has_outstanding_prepared_frame()) {
        return Slus21050FemainLiveAdmissionFailure::PreparedFrameMissing;
    }
    const auto lease = context.transaction_owner->lease();
    if (!lease.has_value() || lease->session_generation == 0U || lease->lease_generation == 0U) {
        return Slus21050FemainLiveAdmissionFailure::FrameLeaseMismatch;
    }
    const auto owner_plan = context.transaction_owner->consumer_plan();
    if (!owner_plan.has_value() || !consumer_plans_equal(*owner_plan, *context.consumer_plan)) {
        return Slus21050FemainLiveAdmissionFailure::FramePlanMismatch;
    }
    const auto owner_handoff = context.transaction_owner->handoff_token();
    if (!owner_handoff.has_value()) {
        return Slus21050FemainLiveAdmissionFailure::FramePlanMismatch;
    }
    if (!context.consumer_plan->player_lease.has_value() || *context.consumer_plan->player_lease != *lease ||
        !context.consumer_plan->timeline_ordinal.has_value()) {
        return Slus21050FemainLiveAdmissionFailure::FrameLeaseMismatch;
    }
    if (context.consumer_plan->layout.generation == 0U || context.consumer_plan->frame_generation == 0U || context.consumer_plan->first_buffer == 0U ||
        context.consumer_plan->second_buffer == 0U || context.consumer_plan->buffer_bytes == 0U) {
        return Slus21050FemainLiveAdmissionFailure::FramePlanMismatch;
    }
    if (!context.memory->validate_main_ram_mutation(context.consumer_plan->work_mutation, kMovieWorkAddress,
                                                    kMovieWorkBytes) ||
        !context.memory->validate_main_ram_mutation(context.consumer_plan->buffer_mutation, context.consumer_plan->first_buffer,
                                                    kBufferRangeBytes) ||
        !context.memory->validate_main_ram_mutation(owner_handoff->mutation, kHandoffAddress,
                                                    kHandoffBytes)) {
        return Slus21050FemainLiveAdmissionFailure::GuestMemoryMutationMismatch;
    }
    const auto ledger = context.ledger->snapshot();
    if (ledger.state != Slus21050FemainFrameLedgerState::Active) {
        return Slus21050FemainLiveAdmissionFailure::LedgerInactive;
    }
    if (ledger.ledger_identity == 0U || ledger.session_generation == 0U ||
        ledger.session_generation != context.player->session_generation()) {
        return Slus21050FemainLiveAdmissionFailure::LedgerSessionMismatch;
    }
    if (context.graphics_boundaries == nullptr) {
        return Slus21050FemainLiveAdmissionFailure::GraphicsEvidenceMissing;
    }
    if (!graphics_order_valid(*context.graphics_boundaries)) {
        return Slus21050FemainLiveAdmissionFailure::GraphicsOrderInvalid;
    }
    return Slus21050FemainLiveAdmissionFailure::None;
}

[[nodiscard]] Slus21050FemainLiveAdmissionSnapshot
build_snapshot(const Slus21050FemainLiveAdmissionContext& context) noexcept {
    const auto lease = context.transaction_owner->lease();
    const auto handoff = context.transaction_owner->handoff_token();
    const Slus21050FemainMovieConsumerPlan& plan = *context.consumer_plan;
    Slus21050FemainLiveAdmissionSnapshot snapshot{
        .source = kSlus21050FemainLiveExpectedSource,
        .emulation_thread_identity = context.emulation_thread_identity,
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
        .ledger = context.ledger->snapshot(),
        .graphics_boundaries = *context.graphics_boundaries,
        .graphics_evidence_present = true,
        .first_buffer = plan.first_buffer,
        .second_buffer = plan.second_buffer,
        .buffer_bytes = plan.buffer_bytes,
        .work_mutation = plan.work_mutation,
        .buffer_mutation = plan.buffer_mutation,
        .timeline_ordinal = plan.timeline_ordinal,
    };
    snapshot.authentication_tag = authenticate_slus_21050_femain_live_admission(snapshot);
    return snapshot;
}

} // namespace

std::optional<Slus21050FemainLiveAdmissionSnapshot> capture_slus_21050_femain_live_admission(
    const Slus21050FemainLiveAdmissionContext& context) noexcept {
    if (validate_context(context) != Slus21050FemainLiveAdmissionFailure::None ||
        validate_source(context.source) != Slus21050FemainLiveAdmissionFailure::None) {
        return std::nullopt;
    }
    return build_snapshot(context);
}

Slus21050FemainLiveAdmissionResult Slus21050FemainLiveAdmissionDiagnostic::admit(
    const Slus21050FemainLiveAdmissionSnapshot& snapshot,
    const Slus21050FemainLiveAdmissionContext& current) const noexcept {
    if (!enabled_) {
        return {Slus21050FemainLiveAdmissionFailure::Disabled};
    }
    if (snapshot.authentication_tag != authenticate_slus_21050_femain_live_admission(snapshot)) {
        return {Slus21050FemainLiveAdmissionFailure::AuthenticationMismatch};
    }
    if (const auto failure = validate_source(snapshot.source);
        failure != Slus21050FemainLiveAdmissionFailure::None) {
        return {failure};
    }
    if (const auto failure = validate_context(current);
        failure != Slus21050FemainLiveAdmissionFailure::None) {
        return {failure};
    }
    if (const auto failure = validate_source(current.source);
        failure != Slus21050FemainLiveAdmissionFailure::None) {
        return {failure};
    }
    if (snapshot.source != current.source) {
        return {Slus21050FemainLiveAdmissionFailure::SourceFunctionFingerprintMismatch};
    }
    if (snapshot.emulation_thread_identity != current.emulation_thread_identity) {
        return {Slus21050FemainLiveAdmissionFailure::EmulationThreadIdentityMismatch};
    }
    if (snapshot.player_identity != current.player->identity()) {
        return {Slus21050FemainLiveAdmissionFailure::PlayerIdentityMismatch};
    }
    if (snapshot.player_session_generation != current.player->session_generation()) {
        return {Slus21050FemainLiveAdmissionFailure::PlayerSessionMismatch};
    }
    const auto owner_session = current.transaction_owner->player_session_generation();
    if (!owner_session.has_value() || snapshot.player_session_generation != *owner_session) {
        return {Slus21050FemainLiveAdmissionFailure::OwnerPlayerSessionMismatch};
    }
    if (snapshot.media_session_generation != current.player->session_generation()) {
        return {Slus21050FemainLiveAdmissionFailure::MediaSessionMismatch};
    }
    const auto lease = current.transaction_owner->lease();
    if (!lease.has_value() || snapshot.frame_lease != *lease) {
        return {Slus21050FemainLiveAdmissionFailure::FrameLeaseMismatch};
    }
    if (current.consumer_plan == nullptr ||
        !plan_matches_snapshot(*current.consumer_plan, snapshot)) {
        return {Slus21050FemainLiveAdmissionFailure::FramePlanMismatch};
    }
    const auto current_handoff = current.transaction_owner->handoff_token();
    if (!current_handoff.has_value() || !snapshot.handoff.has_value() ||
        !(*snapshot.handoff == *current_handoff)) {
        return {Slus21050FemainLiveAdmissionFailure::FramePlanMismatch};
    }
    if (snapshot.layout.generation != current.consumer_plan->layout.generation) {
        return {Slus21050FemainLiveAdmissionFailure::LayoutGenerationMismatch};
    }
    if (!(snapshot.layout == current.consumer_plan->layout)) {
        return {Slus21050FemainLiveAdmissionFailure::LayoutSnapshotMismatch};
    }
    if (snapshot.media_clock_nanoseconds != current.media_clock_nanoseconds ||
        snapshot.media_clock_generation != current.media_clock_generation) {
        return {Slus21050FemainLiveAdmissionFailure::MediaClockMismatch};
    }
    const auto ledger = current.ledger->snapshot();
    if (snapshot.ledger.ledger_identity != ledger.ledger_identity) {
        return {Slus21050FemainLiveAdmissionFailure::LedgerIdentityMismatch};
    }
    if (snapshot.ledger.session_generation != ledger.session_generation) {
        return {Slus21050FemainLiveAdmissionFailure::LedgerSessionMismatch};
    }
    if (!(snapshot.ledger == ledger)) {
        return {Slus21050FemainLiveAdmissionFailure::LedgerSnapshotMismatch};
    }
    if (!snapshot.graphics_evidence_present || current.graphics_boundaries == nullptr) {
        return {Slus21050FemainLiveAdmissionFailure::GraphicsEvidenceMissing};
    }
    if (!graphics_order_valid(snapshot.graphics_boundaries) ||
        !graphics_order_valid(*current.graphics_boundaries)) {
        return {Slus21050FemainLiveAdmissionFailure::GraphicsOrderInvalid};
    }
    if (!(snapshot.graphics_boundaries == *current.graphics_boundaries)) {
        return {Slus21050FemainLiveAdmissionFailure::GraphicsEvidenceMismatch};
    }
    if (last_admitted_.has_value() && *last_admitted_ == snapshot) {
        return {Slus21050FemainLiveAdmissionFailure::Replay};
    }
    if (!current.player->claim_femain_live_admission(snapshot.frame_lease,
                                                      snapshot.authentication_tag)) {
        return {Slus21050FemainLiveAdmissionFailure::Replay};
    }
    last_admitted_ = snapshot;
    return {};
}

} // namespace takedown::runtime
