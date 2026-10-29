#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/runtime/slus_21050_femain_live_admission.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace takedown::runtime {

// This narrow diagnostic seam reuses the already-reviewed, read-only FEMain
// participant context. It never calls live admission and ignores the
// context's caller-supplied graphics-boundaries pointer. The only graphics
// edges proved here are the exact marked PATH2 quadword reaching GsCapture's
// raw GIF input boundary and the authoritative EeMemory GIF arbiter later
// closing that PATH2 packet at EOP with primary-command delivery configured.
using Slus21050FemainFrameGraphicsSourceContext = Slus21050FemainLiveAdmissionContext;

enum class Slus21050FemainFrameGraphicsSourceProbeFailure : std::uint8_t {
    None,
    Disabled,
    AuthenticationMismatch,
    SourceIdentityInvalid,
    SourceIdentityMismatch,
    InvalidEmulationThreadIdentity,
    ContextParticipantMissing,
    GuestMemoryMissing,
    PlayerInactive,
    OwnerNotPrepared,
    OwnerGuestCommitted,
    OwnerPlayerMismatch,
    OwnerPlayerSessionMismatch,
    PreparedFrameMissing,
    PlayerIdentityMismatch,
    PlayerSessionMismatch,
    FrameLeaseMismatch,
    FramePlanMismatch,
    GuestMemoryMutationMismatch,
    SourceRangeInvalid,
    SourceMarkerInvalid,
    SourceMarkerMismatch,
    MarkerMissing,
    MarkerCancelled,
    MarkerReordered,
    LedgerInactive,
    LedgerIdentityMismatch,
    LedgerSessionMismatch,
    LedgerSnapshotMismatch,
    RawSubmissionMissing,
    RawPathMismatch,
    RawQuadwordMismatch,
    RawSubmissionReordered,
    PacketCompletionMissing,
    PacketCompletionReordered,
    PrimaryCommandStreamMissing,
    RawReset,
    Replay,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept;

// A contiguous source span in one of the two reviewed FEMain guest buffers.
// The probe retains no bytes and never hashes or copies this range.
struct Slus21050FemainFrameGraphicsSourceRange {
    guest::GuestAddress buffer{};
    std::size_t span_bytes{};

    bool operator==(const Slus21050FemainFrameGraphicsSourceRange&) const = default;
};

// Immutable value evidence for one admitted FEMain frame. A pre-publication
// token binds the prepared plan's mutation tokens; a post-publication token
// additionally binds the exact GuestPublished reservation and Published
// ledger slot, and carries fresh mutation tokens captured after publication.
// The marker came from EeMemory's existing singleton VIF1/PATH2 marker
// observer; this probe only receives an exact forwarded callback and never
// registers another observer. Mutation tokens are retained as identity and
// are never consumed.
struct Slus21050FemainFrameGraphicsSourceToken {
    Slus21050FemainLiveSourceIdentity source{};
    std::uint64_t emulation_thread_identity{};
    // Stable identity of the transaction owner which supplied the retained
    // publication. This closes the source-to-consume owner join; ledger and
    // player identities alone are not an owner capability.
    std::uint64_t owner_identity{};
    std::uint64_t player_identity{};
    std::uint64_t player_session_generation{};
    Slus21050MovieFrameLease frame_lease{};
    std::uint64_t frame_generation{};
    std::uint64_t media_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    std::uint64_t media_session_generation{};
    std::uint64_t media_clock_nanoseconds{};
    std::uint64_t media_clock_generation{};
    Slus21050FemainMovieLayoutSnapshot layout{};
    std::optional<Slus21050FemainHandoffToken> handoff{};
    Slus21050FemainFrameLedgerSnapshot ledger_before{};
    // Set only when this token was captured after guest publication. It binds
    // the source observation to the exact owner reservation and Published
    // ledger slot; Prepared-frame tokens intentionally leave it disengaged.
    std::optional<Slus21050FemainFrameReservation> published_reservation{};
    Slus21050FemainFrameGraphicsSourceRange source_range{};
    guest::Vif1DmacPreSnapshotResolvedMarker source_marker{};
    guest::EeMainRamMutationToken work_mutation{};
    guest::EeMainRamMutationToken buffer_mutation{};
    std::optional<std::uint64_t> timeline_ordinal{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainFrameGraphicsSourceToken& other) const noexcept;
};

struct Slus21050FemainFrameGraphicsSourceCaptureResult {
    Slus21050FemainFrameGraphicsSourceProbeFailure failure{
        Slus21050FemainFrameGraphicsSourceProbeFailure::ContextParticipantMissing};
    std::optional<Slus21050FemainFrameGraphicsSourceToken> token{};

    [[nodiscard]] bool captured() const noexcept {
        return failure == Slus21050FemainFrameGraphicsSourceProbeFailure::None &&
               token.has_value();
    }
};

// A value-only identity issued by the bounded terminal diagnostic for the
// three source-side callbacks. The domain and observation epoch keep a
// marker/raw/EOP sequence from one diagnostic collection from being joined to
// another collection; ordinal ordering is checked by the probe before it
// accepts the event. Zero fields are reserved for the legacy, unshared probe
// callbacks below.
enum class Slus21050FemainSourceDiagnosticEventKind : std::uint8_t {
    MarkerReached,
    RawSubmission,
    PacketCompletion,
};

struct Slus21050FemainSourceDiagnosticEventIdentity {
    std::uint64_t domain_identity{};
    std::uint64_t observation_epoch{};
    std::uint64_t ordinal{};
    Slus21050FemainSourceDiagnosticEventKind kind{
        Slus21050FemainSourceDiagnosticEventKind::MarkerReached};

    [[nodiscard]] bool valid() const noexcept {
        if (domain_identity == 0U || observation_epoch == 0U || ordinal == 0U) {
            return false;
        }
        switch (kind) {
        case Slus21050FemainSourceDiagnosticEventKind::MarkerReached:
        case Slus21050FemainSourceDiagnosticEventKind::RawSubmission:
        case Slus21050FemainSourceDiagnosticEventKind::PacketCompletion:
            return true;
        }
        return false;
    }

    bool operator==(const Slus21050FemainSourceDiagnosticEventIdentity&) const = default;
};

// This proves the marked source word reached the raw PATH2 input and that its
// path-local GIF arbiter subsequently reported EOP while primary-command
// delivery was configured, without a reset. It does not prove semantic GS
// acceptance, terminal-frame identity, renderer FIFO execution, guest consume,
// scanout, or presentation; those remain explicit integration gates.
struct Slus21050FemainFrameGraphicsSourceEvidence {
    Slus21050FemainFrameGraphicsSourceToken token{};
    // Populated only by the resolver-backed live producer after its complete
    // REF payload span has matched. Legacy diagnostic evidence leaves this
    // disengaged and remains valid under the existing transport proof.
    std::optional<Slus21050FemainLiveResolverSourceProof> resolver_proof{};
    // Nonzero only when the probe was armed inside a diagnostic A-to-C
    // observation. It prevents completed evidence from an older observation
    // being rebound into a newer provenance window.
    std::uint64_t diagnostic_observation_epoch{};
    std::uint64_t source_event_ordinal{};
    std::uint64_t raw_submission_event_ordinal{};
    std::uint64_t packet_completion_event_ordinal{};
    // Shared diagnostic identities are populated only when arm() receives a
    // nonzero event-domain identity and the explicit diagnostic callbacks are
    // used. The three values must share the domain/epoch and be strictly
    // ordered MarkerReached < RawSubmission < PacketCompletion.
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_marker_event{};
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_raw_submission_event{};
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_packet_completion_event{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainFrameGraphicsSourceEvidence&) const = default;
};

struct Slus21050FemainFrameGraphicsSourceProbeResult {
    Slus21050FemainFrameGraphicsSourceProbeFailure failure{
        Slus21050FemainFrameGraphicsSourceProbeFailure::None};
    std::optional<Slus21050FemainFrameGraphicsSourceEvidence> evidence{};

    [[nodiscard]] bool complete() const noexcept {
        return failure == Slus21050FemainFrameGraphicsSourceProbeFailure::None &&
               evidence.has_value();
    }
};

// Captures the exact marker and participant snapshot for either an admitted
// Prepared frame or a GuestPublished frame. In the latter case the owner
// reservation and Published ledger slot are required, and mutation tokens are
// captured fresh from guest memory after publication rather than copied from
// the pre-publication plan.
[[nodiscard]] Slus21050FemainFrameGraphicsSourceCaptureResult
capture_slus_21050_femain_frame_graphics_source_token(
    const Slus21050FemainFrameGraphicsSourceContext& context,
    const guest::Vif1DmacPreSnapshotResolvedMarker& source_marker,
    guest::GuestAddress source_buffer, std::size_t source_span_bytes) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_frame_graphics_source_token(
    const Slus21050FemainFrameGraphicsSourceToken& token) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_frame_graphics_source_evidence(
    const Slus21050FemainFrameGraphicsSourceEvidence& evidence) noexcept;

// Passive, default-off diagnostic raw GIF observer. It is intentionally not
// a Vif1Path2SourceMarkerObserver: the application already has one singleton
// owner. A trusted serialized owner forwards the exact reached/cancelled
// marker synchronously while this object is installed as a diagnostic raw GIF
// tap. GsCapture retains a non-owning observer pointer, so that owner must
// detach or replace the tap before destroying this object. It must also retire
// pending evidence on any player, owner, ledger, memory, or runtime-session
// lifecycle transition. No method writes guest memory or mutates those
// participants, and a recreated probe is not independent replay protection.
class Slus21050FemainFrameGraphicsSourceProbe final : public guest::GsRawGifObserver {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Idle,
        Armed,
        SourceReached,
        SourceSubmitted,
        Completed,
        Failed,
    };

    explicit Slus21050FemainFrameGraphicsSourceProbe(bool enabled = false) noexcept
        : enabled_(enabled), phase_(enabled ? Phase::Idle : Phase::Disabled) {}

    Slus21050FemainFrameGraphicsSourceProbe(
        const Slus21050FemainFrameGraphicsSourceProbe&) = delete;
    Slus21050FemainFrameGraphicsSourceProbe& operator=(
        const Slus21050FemainFrameGraphicsSourceProbe&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint64_t event_ordinal() const noexcept { return event_ordinal_; }
    [[nodiscard]] std::uint64_t source_event_ordinal() const noexcept {
        return source_event_ordinal_;
    }
    [[nodiscard]] std::uint64_t raw_submission_event_ordinal() const noexcept {
        return raw_submission_event_ordinal_;
    }
    [[nodiscard]] std::uint64_t packet_completion_event_ordinal() const noexcept {
        return packet_completion_event_ordinal_;
    }
    [[nodiscard]] std::uint64_t diagnostic_observation_epoch() const noexcept {
        return diagnostic_observation_epoch_;
    }
    [[nodiscard]] std::uint64_t diagnostic_event_domain_identity() const noexcept {
        return diagnostic_event_domain_identity_;
    }
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeFailure failure() const noexcept {
        return failure_;
    }
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult result() const noexcept;

    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult arm(
        const Slus21050FemainFrameGraphicsSourceToken& token,
        const Slus21050FemainFrameGraphicsSourceContext& context,
        std::uint64_t diagnostic_observation_epoch = 0U,
        std::uint64_t diagnostic_event_domain_identity = 0U) noexcept;

    // Called by the existing singleton marker owner; these methods never
    // register this object with EeMemory.
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_vif1_path2_source_marker_reached(
        const guest::Vif1Path2SourceMarkerReached& marker) noexcept;
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_vif1_path2_source_marker_cancelled(
        const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept;

    // Shared diagnostic callbacks. The caller supplies identities only for
    // this probe's exact marker, source quadword, and packet EOP. They are
    // validated against the armed diagnostic domain/epoch and a single
    // strictly increasing sequence before the existing state transitions are
    // performed. These methods never register an EeMemory observer.
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_diagnostic_marker_reached(
        const guest::Vif1Path2SourceMarkerReached& marker,
        const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept;
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_diagnostic_raw_quadword(
        guest::GifPath path, const guest::GsQuadword& quadword,
        const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept;
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_diagnostic_packet_eop(
        guest::GifPath path, bool primary_command_stream_available,
        const Slus21050FemainSourceDiagnosticEventIdentity& event) noexcept;

    // Explicitly close an incomplete observation. It reports a missing marker,
    // raw submission, or packet completion rather than allowing a partial
    // result to be mistaken for evidence. An ineligible EOP fails at delivery.
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult close() noexcept;

    // Completed evidence can be handed to a diagnostic sink once. The live
    // owner/player/ledger/memory context is revalidated immediately before
    // handoff; a failed, stale, idle, or already-taken probe returns none.
    [[nodiscard]] std::optional<Slus21050FemainFrameGraphicsSourceEvidence>
    take_evidence() noexcept;

    // GsRawGifObserver. EeMemory emits the marker immediately before this
    // exact word reaches GsCapture. Evidence remains incomplete until the raw
    // observer receives EeMemory's authoritative later PATH2 EOP boundary.
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override {
        return true;
    }
    void on_gs_gif_quadword(guest::GifPath path,
                            const guest::GsQuadword& quadword) override;
    void on_gs_gif_packet_eop(guest::GifPath path,
                              bool primary_command_stream_available) noexcept override;
    void on_gs_gif_reset() noexcept override;

  private:
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult fail(
        Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept;
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeFailure
    current_context_failure() const noexcept;
    [[nodiscard]] bool accept_diagnostic_event(
        const Slus21050FemainSourceDiagnosticEventIdentity& event,
        Slus21050FemainSourceDiagnosticEventKind expected_kind,
        Slus21050FemainFrameGraphicsSourceProbeFailure failure) noexcept;
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbeResult
    observe_marker_reached_impl(
        const guest::Vif1Path2SourceMarkerReached& marker) noexcept;
    void observe_raw_quadword_impl(guest::GifPath path,
                                   const guest::GsQuadword& quadword) noexcept;
    void observe_packet_eop_impl(guest::GifPath path,
                                 bool primary_command_stream_available) noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Slus21050FemainFrameGraphicsSourceProbeFailure failure_{
        Slus21050FemainFrameGraphicsSourceProbeFailure::None};
    Slus21050FemainFrameGraphicsSourceToken token_{};
    Slus21050FemainFrameGraphicsSourceContext context_{};
    bool context_bound_{};
    std::uint64_t diagnostic_observation_epoch_{};
    std::uint64_t diagnostic_event_domain_identity_{};
    std::uint64_t last_diagnostic_event_ordinal_{};
    std::uint64_t event_ordinal_{};
    std::uint64_t source_event_ordinal_{};
    std::uint64_t raw_submission_event_ordinal_{};
    std::uint64_t packet_completion_event_ordinal_{};
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_marker_event_{};
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_raw_submission_event_{};
    Slus21050FemainSourceDiagnosticEventIdentity diagnostic_packet_completion_event_{};
    std::optional<Slus21050FemainFrameGraphicsSourceEvidence> evidence_{};
};

static_assert(!std::is_base_of_v<guest::Vif1Path2SourceMarkerObserver,
                                 Slus21050FemainFrameGraphicsSourceProbe>);

} // namespace takedown::runtime
