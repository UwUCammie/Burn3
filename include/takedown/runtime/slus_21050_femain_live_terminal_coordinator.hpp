#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/runtime/slus_21050_femain_live_terminal_authority.hpp"
#include "takedown/runtime/slus_21050_femain_primary_stream_digest.hpp"
#include "takedown/runtime/slus_21050_femain_renderer_retirement_witness.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace takedown::runtime {

class Slus21050FemainLiveSourceProducer;
struct Slus21050FemainLiveSourceCandidate;

// A one-shot, default-off bridge for the real GsCapture terminal callback
// slots.  The bridge is deliberately passive while callbacks are running:
// callbacks only copy raw/decoded values into bounded state.  The serialized
// caller must invoke commit() after the source producer has completed its own
// caller-side drain and the shared authority is SourceBound.
class Slus21050FemainLiveTerminalCoordinator final
    : public guest::GsCommandObserver,
      public guest::GsRawGifObserver {
  public:
    static constexpr std::size_t kMaximumCommands = 65536U;
    static constexpr std::size_t kMaximumRawQuadwords = 65536U;
    static constexpr std::size_t kMaximumImageBytes = 16U * 1024U * 1024U;

    enum class Phase : std::uint8_t {
        Disabled,
        Collecting,
        EopReady,
        Committed,
        Consumed,
        Failed,
    };

    enum class Failure : std::uint8_t {
        None,
        Disabled,
        RawPathMismatch,
        SourceQuadwordMismatch,
        SourceMarkerNotPacketStart,
        CommandBeforeRaw,
        CommandPathMismatch,
        EopMissing,
        EopWrongPath,
        PrimaryStreamUnavailable,
        EmptyBatch,
        RawReset,
        GsReset,
        Overflow,
        AllocationFailure,
        LateCallback,
        Replay,
        SourceNotBound,
        SourceEvidenceMismatch,
        WitnessAuthenticationMismatch,
        PacketIdentityMismatch,
        ExecutionIdentityMismatch,
        FrameIdentityMismatch,
        CountMismatch,
        DigestMismatch,
        ImageOverflow,
        PrimaryEvidenceUnavailable,
        AuthorityRejected,
        SerializedTokenRejected,
    };

    using CommandKind = Slus21050FemainPrimaryStreamCommandKind;

    struct RawQuadwordEvidence {
        guest::GifPath path{guest::GifPath::Path1};
        guest::GsQuadword quadword{};

        bool operator==(const RawQuadwordEvidence&) const = default;
    };

    // Every field is owned by the coordinator.  image_transfer.pixels is
    // copied in both const-reference and rvalue callback forms so a primary
    // observer can never lose IMAGE bytes to this terminal tap.
    using CommandEvidence = Slus21050FemainPrimaryStreamCommand;

    // A value view of the one closed terminal packet.  The command span and
    // first raw word remain owned by this coordinator; callers may retain the
    // view only while this coordinator is alive and before its one-shot
    // lifecycle is destroyed.  No callback can create this value: it is
    // exposed only after the serialized caller has observed EOP.
    struct CompleteBatch {
      public:
        [[nodiscard]] std::span<const CommandEvidence> commands() const noexcept {
            return commands_;
        }
        [[nodiscard]] const std::optional<RawQuadwordEvidence>& first_raw_quadword() const noexcept {
            return first_raw_quadword_;
        }
        [[nodiscard]] guest::GifPath packet_path() const noexcept { return packet_path_; }
        [[nodiscard]] std::size_t raw_quadword_count() const noexcept {
            return raw_quadword_count_;
        }
        // Resolver-backed arms retain the complete bounded raw span outside
        // the callback and digest it with the same source-span function used
        // by the source producer. Legacy diagnostic arms leave this zero.
        [[nodiscard]] std::uint64_t raw_stream_digest() const noexcept {
            return raw_stream_digest_;
        }
        [[nodiscard]] bool eop_received() const noexcept { return eop_received_; }
        [[nodiscard]] bool primary_command_stream_available() const noexcept {
            return primary_command_stream_available_;
        }
        [[nodiscard]] std::uint64_t primary_first_ordinal() const noexcept {
            return primary_first_ordinal_;
        }
        [[nodiscard]] std::uint64_t primary_stream_digest() const noexcept {
            return primary_stream_digest_;
        }

        [[nodiscard]] bool complete() const noexcept {
            return !commands_.empty() && first_raw_quadword_.has_value() &&
                   packet_path_ == guest::GifPath::Path2 && raw_quadword_count_ != 0U &&
                   eop_received_ && primary_command_stream_available_ &&
                   primary_first_ordinal_ != 0U && primary_stream_digest_ != 0U;
        }

      private:
        friend class Slus21050FemainLiveTerminalCoordinator;
        std::span<const CommandEvidence> commands_{};
        std::optional<RawQuadwordEvidence> first_raw_quadword_{};
        guest::GifPath packet_path_{guest::GifPath::Path1};
        std::size_t raw_quadword_count_{};
        std::uint64_t raw_stream_digest_{};
        bool eop_received_{};
        bool primary_command_stream_available_{};
        std::uint64_t primary_first_ordinal_{};
        std::uint64_t primary_stream_digest_{};
    };

    explicit Slus21050FemainLiveTerminalCoordinator(bool enabled = false) noexcept;
    ~Slus21050FemainLiveTerminalCoordinator() override = default;

    Slus21050FemainLiveTerminalCoordinator(
        const Slus21050FemainLiveTerminalCoordinator&) = delete;
    Slus21050FemainLiveTerminalCoordinator& operator=(
        const Slus21050FemainLiveTerminalCoordinator&) = delete;
    Slus21050FemainLiveTerminalCoordinator(
        Slus21050FemainLiveTerminalCoordinator&&) = delete;
    Slus21050FemainLiveTerminalCoordinator& operator=(
        Slus21050FemainLiveTerminalCoordinator&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Failure failure() const noexcept { return failure_; }
    [[nodiscard]] bool complete() const noexcept {
        return phase_ == Phase::Committed || phase_ == Phase::Consumed;
    }
    [[nodiscard]] std::size_t command_count() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t raw_quadword_count() const noexcept { return raw_count_; }
    [[nodiscard]] bool eop_received() const noexcept { return eop_received_; }
    [[nodiscard]] bool primary_command_stream_available() const noexcept {
        return primary_command_stream_available_;
    }
    [[nodiscard]] std::uint64_t primary_stream_digest() const noexcept;
    [[nodiscard]] std::uint64_t
    primary_stream_digest(std::uint64_t primary_first_ordinal) const noexcept;
    [[nodiscard]] std::span<const CommandEvidence> commands() const noexcept { return commands_; }
    [[nodiscard]] std::optional<RawQuadwordEvidence> first_raw_quadword() const noexcept {
        return first_raw_quadword_;
    }
    // Return the complete callback-owned batch after EOP has closed the
    // one-shot collection. EopReady accepts a caller-supplied diagnostic
    // ordinal; once commit succeeds, Committed and Consumed accept only the
    // exact primary ordinal authenticated by that commit. The digest is
    // computed by this coordinator over its own retained command values. The
    // frozen batch remains available read-only in Committed and Consumed;
    // callbacks are ignored in those terminal phases. An incomplete, failed,
    // or reset coordinator still returns no batch.
    [[nodiscard]] std::optional<CompleteBatch>
    complete_batch(std::uint64_t primary_first_ordinal) const noexcept;
    [[nodiscard]] std::optional<Slus21050FemainSerializedSourceTerminalToken>
    serialized_token() const noexcept {
        return serialized_token_;
    }

    // Install both dedicated terminal slots.  Attachment is a caller-side,
    // quiescent operation; no slot is installed by the constructor.
    void attach(guest::GsCapture& capture) noexcept;
    void attach(guest::GsCapture& capture,
                Slus21050FemainLiveSourceProducer& source_producer) noexcept;
    void detach(guest::GsCapture& capture) noexcept;
    void detach(guest::GsCapture& capture,
                Slus21050FemainLiveSourceProducer& source_producer) noexcept;

    // The source producer exposes the exact marked quadword after publication.
    // Arming is caller-side and causes raw retention to begin at that first
    // quadword; decoded commands continue to be retained while only the
    // first raw edge and checked raw count are needed for packet identity.
    [[nodiscard]] bool arm_for_source(const guest::GsQuadword& expected_quadword) noexcept;
    // Resolver handoff overload. The candidate remains value-only evidence;
    // arm_for_source copies only its authenticated source quadword into the
    // bounded callback gate and does not issue authority events.
    [[nodiscard]] bool arm_for_source(
        const Slus21050FemainLiveSourceCandidate& candidate) noexcept;

    // The staged source producer may forward its raw tap here instead of
    // installing this object in GsCapture's terminal raw slot.  The observer
    // is borrowed and is never called from commit().
    void set_raw_forward_observer(guest::GsRawGifObserver* observer) noexcept;

    // Commit is the only method allowed to call the live authority.  It must
    // run after the source producer's caller-side drain has moved that exact
    // authority to SourceBound.  All witness, packet, count, digest, frame,
    // and execution checks happen before the first primary event is issued.
    [[nodiscard]] bool commit(
        Slus21050FemainLiveTerminalAuthority& authority,
        const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept;

    // The authority token is moved into this coordinator only after terminal
    // sealing succeeds.  It can be taken once; the authority itself is never
    // queried from a callback.
    [[nodiscard]] std::optional<Slus21050FemainSerializedSourceTerminalToken>
    take_serialized_token() noexcept;

    // guest::GsRawGifObserver
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override {
        return enabled_;
    }
    void on_gs_gif_quadword(guest::GifPath path,
                            const guest::GsQuadword& quadword) override;
    void on_gs_gif_packet_eop(guest::GifPath path,
                              bool primary_command_stream_available) noexcept override;
    void on_gs_gif_reset() noexcept override;

    // guest::GsCommandObserver
    void on_gs_gif_tag_begin(guest::GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const guest::GsRegisterWrite& write) override;
    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override;
    void on_gs_image_transfer(guest::GsImageTransfer&& transfer) override;

  private:
    [[nodiscard]] bool fail(Failure failure) noexcept;
    [[nodiscard]] bool accepting_callback() const noexcept;
    [[nodiscard]] bool accepting_command(guest::GifPath path) noexcept;
    [[nodiscard]] bool append_command(CommandEvidence command) noexcept;
    [[nodiscard]] bool packet_identity_matches(
        const Slus21050FemainSerializedSourceBoundary& source) const noexcept;
    [[nodiscard]] bool witness_matches(
        const Slus21050FemainSerializedSourceBoundary& source,
        const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) const noexcept;

    bool enabled_{};
    Phase phase_{Phase::Disabled};
    Failure failure_{Failure::None};
    guest::GsRawGifObserver* raw_forward_{};
    Slus21050FemainLiveSourceProducer* source_gate_{};
    std::optional<RawQuadwordEvidence> first_raw_quadword_{};
    std::size_t raw_count_{};
    std::vector<guest::GsQuadword> resolver_raw_quadwords_{};
    std::size_t resolver_expected_raw_count_{};
    std::size_t image_bytes_{};
    guest::GifPath packet_path_{guest::GifPath::Path1};
    bool packet_path_bound_{};
    bool prefix_raw_seen_{};
    bool source_raw_started_{};
    bool source_quadword_armed_{};
    guest::GsQuadword expected_source_quadword_{};
    bool eop_received_{};
    bool primary_command_stream_available_{};
    std::vector<CommandEvidence> commands_{};
    // Set only after the authority has accepted the terminal witness and
    // yielded the serialized token. This seals the primary range used by all
    // post-commit CompleteBatch views.
    std::uint64_t committed_primary_first_ordinal_{};
    std::optional<Slus21050FemainSerializedSourceTerminalToken> serialized_token_{};
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainLiveTerminalCoordinator::Phase phase) noexcept;
[[nodiscard]] std::string_view to_string(
    Slus21050FemainLiveTerminalCoordinator::Failure failure) noexcept;

} // namespace takedown::runtime
