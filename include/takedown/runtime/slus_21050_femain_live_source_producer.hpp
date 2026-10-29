#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/gs_capture.hpp"
#include "takedown/runtime/slus_21050_femain_graphics_source_probe.hpp"
#include "takedown/runtime/slus_21050_femain_live_source_candidate_resolver.hpp"
#include "takedown/runtime/slus_21050_femain_live_terminal_authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {

// The producer is the small live tee which sits between the existing singleton
// VIF1/PATH2 marker slot, the raw GIF slot, and the source probe.  It owns no
// guest memory and does not own the observers it forwards to.  Its callback
// methods are deliberately limited to copying callback values and forwarding
// them; the two operations which can inspect the bound participants
// (capture/arm and authority completion) are explicit caller-side drains.
class Slus21050FemainLiveSourceProducer final : public guest::Vif1Path2SourceMarkerObserver,
                                                public guest::GsRawGifObserver {
  public:
    enum class Phase : std::uint8_t {
        Disabled,
        Idle,
        PublicationBound,
        MarkerResolved,
        Armed,
        SourceReached,
        CompletionPending,
        SourceCompleted,
        Failed,
    };

    enum class Failure : std::uint8_t {
        None,
        Disabled,
        PublicationAlreadyBound,
        PublicationRejected,
        PublicationMismatch,
        ExecutionIdentityInvalid,
        FrameIdentityInvalid,
        MediaClockInvalid,
        SourceCandidateInvalid,
        SourceCandidateAmbiguous,
        CandidateCapacityExhausted,
        CandidateResolutionMissing,
        CandidateResolutionAmbiguous,
        CandidateResolutionRejected,
        ResolvedMarkerInvalid,
        MarkerMissing,
        MarkerMismatch,
        MarkerCancelled,
        SourceSpanAllocationFailure,
        SourceSpanPathMismatch,
        SourceSpanQuadwordMismatch,
        SourceSpanIncomplete,
        SourceSpanOverflow,
        SourceSpanMarkerNotPacketStart,
        RawReset,
        PacketCompletionMissing,
        PacketCompletionAmbiguous,
        PacketCompletionRejected,
        AuthorityRejected,
        Replay,
    };

    explicit Slus21050FemainLiveSourceProducer(bool enabled = false) noexcept;
    ~Slus21050FemainLiveSourceProducer() override = default;

    Slus21050FemainLiveSourceProducer(const Slus21050FemainLiveSourceProducer&) = delete;
    Slus21050FemainLiveSourceProducer& operator=(const Slus21050FemainLiveSourceProducer&) = delete;
    Slus21050FemainLiveSourceProducer(Slus21050FemainLiveSourceProducer&&) = delete;
    Slus21050FemainLiveSourceProducer& operator=(Slus21050FemainLiveSourceProducer&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] bool bound() const noexcept {
        return phase_ != Phase::Disabled && phase_ != Phase::Idle && phase_ != Phase::Failed;
    }
    [[nodiscard]] bool source_completion_pending() const noexcept { return completion_pending_; }
    // True only after the callback side has frozen a complete Path2 packet
    // that the serialized drain can validate without observing more traffic.
    // This is stricter than source_completion_pending(): an incomplete or
    // primary-unavailable EOP remains fail-closed rather than entering the
    // short post-EOP parking window.
    [[nodiscard]] bool source_completion_ready_for_drain() const noexcept {
        if (!completion_pending_ || phase_ != Phase::CompletionPending ||
            pending_completion_path_ != guest::GifPath::Path2 ||
            !pending_completion_primary_available_ || !staged_marker_.has_value()) {
            return false;
        }
        if (resolver_candidate_.has_value()) {
            return marker_at_packet_start_ &&
                   source_span_quadword_count_ == expected_source_span_.size();
        }
        return staged_raw_quadword_.has_value();
    }
    [[nodiscard]] bool source_completion_delivered() const noexcept {
        return source_completion_delivered_;
    }
    [[nodiscard]] std::uint64_t candidate_token() const noexcept { return candidate_token_; }
    [[nodiscard]] guest::GuestAddress source_candidate() const noexcept {
        return source_candidate_;
    }
    [[nodiscard]] const guest::GsQuadword& expected_quadword() const noexcept {
        return expected_quadword_;
    }
    [[nodiscard]] const Slus21050FemainFrameGraphicsSourceProbe& source_probe() const noexcept {
        return probe_;
    }
    [[nodiscard]] Slus21050FemainFrameGraphicsSourceProbe& source_probe() noexcept {
        return probe_;
    }
    [[nodiscard]] std::size_t staged_raw_quadword_count() const noexcept {
        if (resolver_candidate_.has_value()) {
            return source_span_quadword_count_;
        }
        return staged_raw_quadword_.has_value() ? 1U : 0U;
    }
    [[nodiscard]] bool staged_marker_reached() const noexcept { return staged_marker_.has_value(); }
    // A terminal bridge may retain the complete packet only when the marker
    // callback arrived before its first raw PATH2 quadword.
    [[nodiscard]] bool staged_marker_at_packet_start() const noexcept {
        return staged_marker_.has_value() && marker_at_packet_start_;
    }
    [[nodiscard]] std::optional<guest::Vif1DmacPreSnapshotResolvedMarker>
    resolved_marker() const noexcept {
        return resolved_marker_;
    }

    // Bind one exact GuestPublished packet and arm no callbacks yet.  The
    // authority, owner, ledger, player, and memory references are all borrowed
    // and must remain alive until the producer is drained or fails.  The
    // source candidate must be a 16-byte aligned word range contained by
    // exactly one of packet.plan's two published buffers.
    [[nodiscard]] bool bind_publication(Slus21050FemainLiveTerminalAuthority& authority,
                                        Slus21050FemainMovieTransactionOwner& owner,
                                        Slus21050FemainFrameTransactionLedger& ledger,
                                        Slus21050MoviePlayer& player, guest::EeMemory& memory,
                                        const Slus21050FemainMoviePublicationPacket& packet,
                                        const Slus21050FemainSerializedExecutionIdentity& execution,
                                        std::uint64_t frame_id,
                                        std::uint64_t media_clock_nanoseconds,
                                        std::uint64_t media_clock_generation,
                                        guest::GuestAddress source_candidate) noexcept;

    // Resolver handoff overload. The candidate is value-only evidence from
    // the authenticated queue-chain walk; this method rechecks its
    // authentication, publication/frame identity, and full REF payload span
    // before entering the existing participant binding path. It does not
    // mint source or terminal authority.
    [[nodiscard]] bool bind_publication(
        Slus21050FemainLiveTerminalAuthority& authority,
        Slus21050FemainMovieTransactionOwner& owner,
        Slus21050FemainFrameTransactionLedger& ledger,
        Slus21050MoviePlayer& player, guest::EeMemory& memory,
        const Slus21050FemainMoviePublicationPacket& packet,
        const Slus21050FemainSerializedExecutionIdentity& execution,
        std::uint64_t frame_id, std::uint64_t media_clock_nanoseconds,
        std::uint64_t media_clock_generation,
        const Slus21050FemainLiveSourceCandidate& candidate) noexcept;

    // The caller must invoke this after the VIF1 pre-snapshot callback has
    // returned and before timed VIF1 progress.  It captures fresh
    // post-publication mutation evidence and arms the owned source probe.
    [[nodiscard]] bool drain_resolved_marker() noexcept;

    // EOP callbacks only make a pending value.  This caller-side drain feeds
    // that saved boundary into the probe, takes its immutable evidence, and
    // invokes the exact authority once.  It never calls a primary or terminal
    // producer and cannot be retried after the handoff has been attempted.
    [[nodiscard]] bool drain_source_completion() noexcept;

    // Existing singleton owners remain in front of this tee.  Forwarding is
    // synchronous and non-owning; setters are intended for quiescent callback
    // boundaries.
    void set_marker_forward_observer(guest::Vif1Path2SourceMarkerObserver* observer) noexcept;
    void set_raw_forward_observer(guest::GsRawGifObserver* observer) noexcept;

    // guest::Vif1Path2SourceMarkerObserver
    [[nodiscard]] bool has_vif1_dmac_pre_snapshot_candidates() const noexcept override;
    [[nodiscard]] std::size_t on_vif1_dmac_pre_snapshot_candidates(
        const guest::Vif1DmacPreSnapshotObservation& observation,
        std::span<guest::Vif1DmacPreSnapshotCandidate> candidates) noexcept override;
    void on_vif1_dmac_pre_snapshot_markers_resolved(
        std::span<const guest::Vif1DmacPreSnapshotResolvedMarker> markers,
        std::span<guest::Vif1DmacPreSnapshotMarkerDecision> decisions) noexcept override;
    void on_vif1_path2_source_marker_memory_cleared() noexcept override;
    void on_vif1_path2_source_marker_reached(
        const guest::Vif1Path2SourceMarkerReached& marker) noexcept override;
    void on_vif1_path2_source_marker_cancelled(
        const guest::Vif1Path2SourceMarkerCancelled& marker) noexcept override;

    // guest::GsRawGifObserver
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override { return enabled_; }
    void on_gs_gif_quadword(guest::GifPath path, const guest::GsQuadword& quadword) override;
    void on_gs_gif_packet_eop(guest::GifPath path,
                              bool primary_command_stream_available) noexcept override;
    void on_gs_gif_reset() noexcept override;

  private:
    [[nodiscard]] bool fail(Failure failure) noexcept;
    [[nodiscard]] bool
    marker_matches(const guest::Vif1Path2SourceMarkerReached& marker) const noexcept;
    [[nodiscard]] bool resolver_marker_matches(
        const guest::Vif1DmacPreSnapshotResolvedMarker& marker) const noexcept;
    [[nodiscard]] bool
    marker_matches(const guest::Vif1Path2SourceMarkerCancelled& marker) const noexcept;
    [[nodiscard]] static bool
    valid_execution(const Slus21050FemainSerializedExecutionIdentity& execution) noexcept;
    [[nodiscard]] static guest::GsQuadword read_quadword(const guest::EeMemory& memory,
                                                         guest::GuestAddress address) noexcept;
    [[nodiscard]] static std::uint64_t allocate_candidate_token() noexcept;
    [[nodiscard]] bool forwarded_source_aliases_candidate() const noexcept;
    [[nodiscard]] bool copy_resolver_source_span(
        const Slus21050FemainLiveSourceCandidate& candidate) noexcept;

    struct StagedRawQuadword {
        guest::GifPath path{guest::GifPath::Path1};
        guest::GsQuadword quadword{};
    };

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    guest::Vif1Path2SourceMarkerObserver* marker_forward_{};
    guest::GsRawGifObserver* raw_forward_{};

    Slus21050FemainLiveTerminalAuthority* authority_{};
    Slus21050FemainMovieTransactionOwner* owner_{};
    Slus21050FemainFrameTransactionLedger* ledger_{};
    Slus21050MoviePlayer* player_{};
    guest::EeMemory* memory_{};
    Slus21050FemainMoviePublicationPacket packet_{};
    Slus21050FemainSerializedExecutionIdentity execution_{};
    std::uint64_t frame_id_{};
    std::uint64_t media_clock_nanoseconds_{};
    std::uint64_t media_clock_generation_{};
    Slus21050FemainMovieConsumerPlan plan_{};
    guest::GuestAddress source_buffer_{};
    std::size_t source_span_bytes_{};
    guest::GuestAddress source_candidate_{};
    guest::GsQuadword expected_quadword_{};
    std::uint64_t candidate_token_{};
    // Preserve the resolver value through the callback handoff. The resolved
    // marker omits source_qwc, so drain_resolved_marker() re-runs the bounded
    // resolver and compares this complete candidate before arming the probe.
    std::optional<Slus21050FemainLiveSourceCandidate> resolver_candidate_{};
    // Resolver candidates bind the complete REF payload, not only the first
    // marked word. The copy is made by the serialized caller after re-resolve;
    // callbacks only compare against this immutable span.
    std::vector<guest::GsQuadword> expected_source_span_{};
    std::size_t source_span_quadword_count_{};

    bool candidate_offered_{};
    bool candidate_merge_ambiguous_{};
    std::array<guest::GuestAddress, guest::kVif1DmacPreSnapshotCandidateCapacity>
        forwarded_candidate_sources_{};
    std::size_t forwarded_candidate_count_{};
    std::optional<StagedRawQuadword> staged_raw_quadword_{};
    std::optional<guest::Vif1Path2SourceMarkerReached> staged_marker_{};
    bool raw_packet_open_before_marker_{};
    bool marker_at_packet_start_{true};
    bool completion_pending_{};
    guest::GifPath pending_completion_path_{guest::GifPath::Path1};
    bool pending_completion_primary_available_{};
    bool source_completion_delivered_{};
    std::optional<guest::Vif1DmacPreSnapshotResolvedMarker> resolved_marker_{};
    Slus21050FemainFrameGraphicsSourceProbe probe_{};
};

[[nodiscard]] std::string_view to_string(Slus21050FemainLiveSourceProducer::Phase phase) noexcept;
[[nodiscard]] std::string_view
to_string(Slus21050FemainLiveSourceProducer::Failure failure) noexcept;

} // namespace takedown::runtime
