#pragma once

#include "takedown/guest/ipu_mpeg2_decoder.hpp"
#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_femain_guest_boundary.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::runtime {

class Slus21050FemainLiveAdmissionDiagnostic;

inline constexpr guest::GuestAddress kSlus21050MovieWork = 0x0051a870U;
inline constexpr std::size_t kSlus21050MovieDecodeChunkBytes = 64U * 1024U;
// Leave one event in the 64-event FEMain proof window for the due frame which
// follows a complete drop batch.
inline constexpr std::size_t kSlus21050MovieDroppedFrameJournalCapacity = 63U;

enum class Slus21050MoviePlayerStatus : std::uint8_t {
    Empty,
    Decoding,
    FrameStaged,
    WaitingForPresentationTime,
    LoopRestarted,
    EndOfStream,
    MovieOpenFailed,
    DecoderUnavailable,
    MovieReadFailed,
    SourceFrameRateMismatch,
    ConcealedFrame,
    UnsupportedFrameGeometry,
    InvalidFramePlanes,
    GuestLayoutMismatch,
    GuestBufferMismatch,
    GuestRangeInvalid,
    PreparedFrameOutstanding,
    DroppedFrameBatchOutstanding,
    DroppedFrameBatchInvalid,
    DroppedFrameJournalFull,
};

[[nodiscard]] const char* to_string(Slus21050MoviePlayerStatus status) noexcept;

struct Slus21050MovieGeometry {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t texture_columns{};
    std::uint32_t texture_rows{};
    std::uint32_t macroblock_columns{};
    std::uint32_t macroblock_rows{};
    std::size_t column_bytes{};
    std::size_t buffer_bytes{};
    std::size_t final_texture_padding_bytes{};
};

struct Slus21050MovieFrameLease {
    std::uint64_t session_generation{};
    std::uint64_t lease_generation{};

    bool operator==(const Slus21050MovieFrameLease&) const = default;
};

struct Slus21050MovieFrameCompletionReceipt {
    Slus21050MovieFrameLease lease{};
    // Binds an external success acknowledgement to the exact converted
    // payload that was checked out under this lease.
    std::uint64_t payload_digest{};
    std::uint32_t staged_gs_base{};

    bool operator==(const Slus21050MovieFrameCompletionReceipt&) const = default;
};

struct Slus21050MovieDroppedFrameIdentity {
    std::uint64_t session_generation{};
    std::uint64_t timeline_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    // Present only when the timed prepare call sampled the emulation-thread
    // VBlank/audio cursors at the exact catch-up boundary.  Legacy/untracked
    // callers may omit it, but proof admission must reject that absence.
    std::optional<Slus21050FemainFrameGuestBoundary> dropped_at{};

    bool operator==(const Slus21050MovieDroppedFrameIdentity&) const = default;
};

enum class Slus21050MovieDroppedFrameBatchStatus : std::uint8_t {
    Ready,
    Empty,
    Disabled,
    StaleBatch,
    InvalidAcknowledgement,
    InvalidFrameReceipt,
    BatchNotAuthorized,
    AuthorizedFrameOutstanding,
    GenerationExhausted,
};

// Immutable, instance-bound view of the exact pictures discarded by timed
// catch-up. Copies may be inspected, but acknowledging one invalidates every
// copy. The fixed array adds no allocation to the movie decode path.
class Slus21050MovieDroppedFrameBatch {
  public:
    constexpr Slus21050MovieDroppedFrameBatch() noexcept = default;

    [[nodiscard]] std::span<const Slus21050MovieDroppedFrameIdentity> frames() const noexcept {
        return {frames_.data(), count_};
    }
  private:
    friend class Slus21050MoviePlayer;
    const void* owner_{};
    std::uint64_t player_identity_{};
    std::uint64_t session_generation_{};
    std::uint64_t journal_revision_{};
    std::size_t count_{};
    std::array<Slus21050MovieDroppedFrameIdentity,
               kSlus21050MovieDroppedFrameJournalCapacity>
        frames_{};
};

struct Slus21050MovieDroppedFrameBatchResult {
    Slus21050MovieDroppedFrameBatchStatus status{
        Slus21050MovieDroppedFrameBatchStatus::Empty};
    std::optional<Slus21050MovieDroppedFrameBatch> batch{};

    [[nodiscard]] bool available() const noexcept {
        return status == Slus21050MovieDroppedFrameBatchStatus::Ready && batch.has_value();
    }
};

// A prepared picture owns its converted strip-major payload and the exact
// identity used by the media timeline and decoder.  It is deliberately
// independent of EeMemory: callers can preflight a guest consumer, move the
// frame into that consumer, and return the same frame with
// retain_pending_frame() if admission fails.
class Slus21050MoviePreparedFrame {
  public:
    Slus21050MoviePreparedFrame(Slus21050MovieFrameLease lease,
                                std::optional<std::uint64_t> timeline_frame,
                                std::uint64_t source_sequence,
                                std::uint32_t source_checksum,
                                Slus21050MovieGeometry geometry,
                                std::vector<std::uint8_t>&& strip_major_rgba) noexcept;
    Slus21050MoviePreparedFrame(Slus21050MoviePreparedFrame&&) noexcept = default;
    Slus21050MoviePreparedFrame& operator=(Slus21050MoviePreparedFrame&&) noexcept = default;
    Slus21050MoviePreparedFrame(const Slus21050MoviePreparedFrame&) = delete;
    Slus21050MoviePreparedFrame& operator=(const Slus21050MoviePreparedFrame&) = delete;
    ~Slus21050MoviePreparedFrame() = default;

    [[nodiscard]] const Slus21050MovieFrameLease& lease() const noexcept { return lease_; }
    [[nodiscard]] std::optional<std::uint64_t> timeline_frame() const noexcept {
        return timeline_frame_;
    }
    [[nodiscard]] std::uint64_t source_sequence() const noexcept { return source_sequence_; }
    [[nodiscard]] std::uint32_t source_checksum() const noexcept { return source_checksum_; }
    [[nodiscard]] const Slus21050MovieGeometry& geometry() const noexcept { return geometry_; }
    [[nodiscard]] std::uint64_t payload_digest() const noexcept { return payload_digest_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return strip_major_rgba_;
    }

    [[nodiscard]] Slus21050MovieFrameCompletionReceipt
    completion_receipt(const std::uint32_t staged_gs_base) const noexcept {
        return {.lease = lease_,
                .payload_digest = payload_digest(),
                .staged_gs_base = staged_gs_base};
    }

  private:
    friend class Slus21050MoviePlayer;

    Slus21050MovieFrameLease lease_{};
    std::optional<std::uint64_t> timeline_frame_{};
    std::uint64_t source_sequence_{};
    std::uint32_t source_checksum_{};
    Slus21050MovieGeometry geometry_{};
    std::vector<std::uint8_t> strip_major_rgba_{};
    std::uint64_t payload_digest_{};
};

struct Slus21050MoviePlayerStatistics {
    std::uint64_t open_attempts{};
    std::uint64_t input_chunks{};
    std::uint64_t input_bytes{};
    std::uint64_t flushes{};
    std::uint64_t frames_received{};
    std::uint64_t concealed_frames{};
    std::uint64_t unsupported_geometry_frames{};
    std::uint64_t invalid_plane_frames{};
    std::uint64_t frames_converted{};
    std::uint64_t timed_steps{};
    std::uint64_t presentation_frames_due{};
    std::uint64_t presentation_frame_repeats{};
    std::uint64_t late_frames_dropped{};
    std::uint64_t unknown_timing_frames{};
    std::uint64_t source_rate_changes{};
    std::uint64_t stage_attempts{};
    std::uint64_t stage_failures{};
    std::uint64_t frames_staged{};
    std::uint64_t staged_buffers{};
    std::uint64_t staged_bytes{};
    std::uint64_t loop_restarts{};
    std::uint64_t zero_frame_loop_terminations{};
    std::uint64_t end_of_streams{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint32_t source_frame_rate_numerator{};
    std::uint32_t source_frame_rate_denominator{};
    std::uint64_t timeline_elapsed_nanoseconds{};
    std::uint64_t timeline_target_frame{};
    std::optional<std::uint64_t> last_staged_timeline_frame{};
    std::optional<std::uint64_t> media_duration_nanoseconds{};
    std::uint32_t staged_gs_base{};
    Slus21050MoviePlayerStatus last_status{Slus21050MoviePlayerStatus::Empty};
};

// Host-input edge latch for a foreground retail movie session. A session is
// deliberately not armed until its decoder has accepted the stream and Start
// has been observed released. This prevents the button which entered a flow
// from immediately skipping its first movie. Menu background movies are
// permanently exempt from the skip action.
class Slus21050MovieStartSkipLatch {
  public:
    void begin(const bool background_exempt) noexcept {
        background_exempt_ = background_exempt;
        armed_ = false;
        requested_ = false;
    }

    [[nodiscard]] bool observe(const bool decoder_accepted, const bool start_down) noexcept {
        if (background_exempt_ || requested_ || !decoder_accepted) {
            return false;
        }
        if (!start_down) {
            armed_ = true;
            return false;
        }
        if (!armed_) {
            return false;
        }
        requested_ = true;
        return true;
    }

    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] bool requested() const noexcept { return requested_; }

  private:
    bool background_exempt_{};
    bool armed_{};
    bool requested_{};
};

// Incremental host decoder and revision-specific guest staging seam. The
// unpaced step reads no more than kSlus21050MovieDecodeChunkBytes and stages
// at most one picture. The timed seam may drain more input while catching up,
// but still stages at most one current picture. A converted picture remains
// pending when the live retail slots are not yet safe to write.
class Slus21050MoviePlayer {
  public:
    using DecoderFactory =
        std::function<std::unique_ptr<guest::IpuMpeg2Decoder>()>;

    Slus21050MoviePlayer();
    ~Slus21050MoviePlayer();
    Slus21050MoviePlayer(Slus21050MoviePlayer&&) noexcept;
    Slus21050MoviePlayer& operator=(Slus21050MoviePlayer&&) noexcept;
    Slus21050MoviePlayer(const Slus21050MoviePlayer&) = delete;
    Slus21050MoviePlayer& operator=(const Slus21050MoviePlayer&) = delete;

    [[nodiscard]] Slus21050MoviePlayerStatus
    open(const std::filesystem::path& movie_path, bool loop = false);

    // Test/embedding seam. The factory is called again at every loop so the
    // MPEG decoder begins each pass with clean parser and reordering state.
    [[nodiscard]] Slus21050MoviePlayerStatus
    open_with_decoder_factory(const std::filesystem::path& movie_path,
                              DecoderFactory decoder_factory, bool loop = false);

    void close() noexcept;

    // Pure decode/conversion preparation. These methods never inspect or
    // mutate EeMemory. A FrameStaged result means take_pending_frame() owns
    // one converted picture; retain it when an external consumer rejects its
    // preflight so the next prepare call cannot decode a replacement.
    [[nodiscard]] Slus21050MoviePlayerStatus prepare();

    [[nodiscard]] Slus21050MoviePlayerStatus
    prepare_at_media_time(std::uint64_t elapsed_nanoseconds);
    // Evidence-aware form for an emulation-thread caller. Every overdue
    // identity decoded by this call is bound to this exact boundary.
    [[nodiscard]] Slus21050MoviePlayerStatus prepare_at_media_time_with_guest_boundary(
        std::uint64_t elapsed_nanoseconds,
        Slus21050FemainFrameGuestBoundary dropped_at);
    // A current drop-batch ticket may authorize decoding the immediately due
    // frame without first retiring those drops. The player will not append to
    // or replace the authorized batch if the requested media target advances.
    [[nodiscard]] Slus21050MoviePlayerStatus prepare_at_media_time(
        std::uint64_t elapsed_nanoseconds,
        const Slus21050MovieDroppedFrameBatch& dropped_frames);

    // Move the exact pending payload and its timeline/source/geometry identity
    // to an external consumer. The player has no pending picture until the
    // caller either commits that frame or returns it with retain_pending_frame.
    [[nodiscard]] std::optional<Slus21050MoviePreparedFrame> take_pending_frame() noexcept;

    // Restores a frame previously returned by take_pending_frame(). Returns
    // false without changing player state when a picture is already pending or
    // the supplied frame is internally inconsistent.
    [[nodiscard]] bool retain_pending_frame(Slus21050MoviePreparedFrame&& frame) noexcept;

    // A consumer calls this with the checked-out frame's typed lease after
    // successfully committing it. The receipt supplies the validated GS base
    // needed to account the external double-buffer stage exactly once. Until
    // then prepare() is guarded and cannot decode a replacement.
    [[nodiscard]] bool complete_pending_frame(
        const Slus21050MovieFrameCompletionReceipt& receipt) noexcept;
    [[nodiscard]] bool can_complete_pending_frame(
        const Slus21050MovieFrameCompletionReceipt& receipt) const noexcept;

    [[nodiscard]] bool has_outstanding_prepared_frame() const noexcept;

    // Enables strict drop tracking for the current open session before any
    // picture is consumed. Default playback remains untracked and unchanged.
    // In tracked mode preparation cannot overtake an unacknowledged drop batch
    // or consume a 64th dropped identity from the decoder.
    [[nodiscard]] bool enable_dropped_frame_tracking() noexcept;
    [[nodiscard]] bool dropped_frame_tracking_enabled() const noexcept;
    [[nodiscard]] std::uint64_t identity() const noexcept;
    [[nodiscard]] std::uint64_t session_generation() const noexcept;

    // Returns a read-only, fixed-capacity journal ticket for every exact media
    // identity dropped since the last acknowledgement. Evidence-aware timed
    // calls embed their same-call guest boundary; legacy calls leave it absent
    // and proof admission rejects that batch. Acknowledging a FIFO prefix
    // invalidates all copies and retains the unacknowledged suffix for retry.
    [[nodiscard]] Slus21050MovieDroppedFrameBatchResult
    dropped_frame_batch() const noexcept;
    [[nodiscard]] Slus21050MovieDroppedFrameBatchStatus validate_dropped_frame_batch(
        const Slus21050MovieDroppedFrameBatch& batch) const noexcept;
    // Read-only final preflight for a FIFO prefix acknowledgement. This
    // includes the exact live-batch binding, authorization exclusion, count
    // bounds, and journal-revision headroom checked by the mutating seam.
    [[nodiscard]] Slus21050MovieDroppedFrameBatchStatus
    validate_dropped_frame_batch_acknowledgement(
        const Slus21050MovieDroppedFrameBatch& batch,
        std::size_t acknowledged_count) const noexcept;
    [[nodiscard]] Slus21050MovieDroppedFrameBatchStatus acknowledge_dropped_frame_batch(
        const Slus21050MovieDroppedFrameBatch& batch,
        std::size_t acknowledged_count) noexcept;

    // Final serialized success seam for a composite guest/ledger transaction.
    // Both capabilities are validated before either the checked-out frame or
    // complete drop batch is retired.
    [[nodiscard]] bool can_complete_pending_frame_and_acknowledge_dropped_frame_batch(
        const Slus21050MovieFrameCompletionReceipt& receipt,
        const Slus21050MovieDroppedFrameBatch& batch) const noexcept;
    [[nodiscard]] Slus21050MovieDroppedFrameBatchStatus
    complete_pending_frame_and_acknowledge_dropped_frame_batch(
        const Slus21050MovieFrameCompletionReceipt& receipt,
        const Slus21050MovieDroppedFrameBatch& batch) noexcept;

    [[nodiscard]] Slus21050MoviePlayerStatus step(guest::EeMemory& memory);

    // Media-clock presentation seam. `elapsed_nanoseconds` is time since the
    // first visible frame of this playback session, excluding explicit host
    // debugger pauses.  A fast producer retains the already-staged picture
    // until its next source frame is due; a slow producer decodes past and
    // drops overdue pictures so finite EOS still occurs at the source-media
    // duration.  This never advances guest cycles or consults host refresh.
    [[nodiscard]] Slus21050MoviePlayerStatus
    step_at_media_time(guest::EeMemory& memory, std::uint64_t elapsed_nanoseconds);
    [[nodiscard]] Slus21050MoviePlayerStatus step_at_media_time_with_guest_boundary(
        guest::EeMemory& memory, std::uint64_t elapsed_nanoseconds,
        Slus21050FemainFrameGuestBoundary dropped_at);

    [[nodiscard]] Slus21050MoviePlayerStatus status() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool looping() const noexcept;
    [[nodiscard]] bool has_pending_frame() const noexcept;
    [[nodiscard]] std::optional<Slus21050MovieGeometry> geometry() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> pending_strip_major_rgba() const noexcept;
    [[nodiscard]] const Slus21050MoviePlayerStatistics& statistics() const noexcept;

  private:
    friend class Slus21050FemainLiveAdmissionDiagnostic;

    [[nodiscard]] Slus21050MoviePlayerStatus prepare_at_media_time_impl(
        std::uint64_t elapsed_nanoseconds,
        const Slus21050MovieDroppedFrameBatch* authorized_dropped_frames,
        std::optional<Slus21050FemainFrameGuestBoundary> dropped_at);
    [[nodiscard]] bool claim_femain_live_admission(
        const Slus21050MovieFrameLease& lease, std::uint64_t authentication_tag) const noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime
