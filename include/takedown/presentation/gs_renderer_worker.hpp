#pragma once

#include "takedown/guest/gs_live_renderer.hpp"
#include "takedown/presentation/gs_scanout.hpp"
#include "takedown/presentation/gs_texture_residency.hpp"
#include "takedown/presentation/native_ui_rectangle_batch.hpp"
#include "takedown/runtime/slus_21050_femain_primary_stream_digest.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

class TextRasterResidencyStore;

} // namespace takedown::runtime::slus_21050

namespace takedown::presentation {

struct NativeUiTextDraw;
struct NativeUiTextDrawRequest;
class NativeUiTextDrawSink;

enum class GsRendererWorkerCommandKind : std::uint8_t {
    RegisterWrite,
    ImageTransfer,
    GifTagBegin,
    GsStateReset,
    Reset,
    AcquireTextureResidency,
    ReleaseTextureResidency,
    NativeUiTextDraw,
    Synchronize,
    FrameCompletion,
};

struct GsRendererWorkerQueueStatistics {
    std::uint64_t submitted_commands{};
    std::uint64_t completed_commands{};
    std::uint64_t producer_waits{};
    std::size_t queued_commands{};
    std::size_t queued_image_bytes{};
    std::size_t queued_native_ui_bytes{};
    std::size_t peak_queued_commands{};
    std::size_t peak_queued_image_bytes{};
    std::size_t peak_queued_native_ui_bytes{};
};

struct GsRendererWorkerConfig {
    // The limits bound commands waiting in the FIFO. The command currently
    // executing on the worker is not included. A single payload larger than its
    // byte-class limit is admitted only when no other payload of that class is
    // queued, so a legal command cannot deadlock a deliberately small limit.
    std::size_t maximum_queued_commands{16U * 1024U};
    std::size_t maximum_queued_image_bytes{64U * 1024U * 1024U};
    // Exact scanouts are never dropped. This separate bound limits immutable
    // 4 MiB local-memory snapshots waiting behind the scanout currently being
    // converted. A full queue backpressures only the GS worker; the command
    // producer remains decoupled until the primary GS FIFO also fills.
    std::size_t maximum_queued_scanouts{2U};
    // Host-only low-latency requests use a separate bounded mailbox. They are
    // observations, not FIFO commands, and are serviced fairly between normal
    // commands so an abusive presenter cannot starve GS execution.
    std::size_t maximum_queued_present_requests{2U};

    // Each queued native draw is charged for its owning value and nested geometry
    // allocation capacities. Externally supplied images are also charged; an
    // authenticated resident draw instead remains covered by the registry budget
    // until its later FIFO release. One oversized draw is admitted only when no
    // other native-draw payload is queued.
    std::size_t maximum_queued_native_ui_bytes{64U * 1024U * 1024U};

    // Immutable texture images are owned by the ordered worker's registry.
    // These limits are independent of transient FIFO IMAGE-byte accounting.
    GsTextureResidencyRegistryConfig texture_residency{};

    // Optional renderer/mod consumer for owned, renderer-neutral UI draws. The
    // callback runs on the GS worker at the draw's exact primary-FIFO position.
    // It must not submit to or wait on this same bounded worker. A sink that
    // retains draws asynchronously must apply its own resource bound.
    std::shared_ptr<NativeUiTextDrawSink> native_ui_text_draw_sink{};
    // Mutually exclusive with the text sink. Compilation occurs on the worker,
    // after resident-state guards. Output limits bound transient materialization
    // separately from queued input bytes; consumers budget retained output.
    std::shared_ptr<NativeUiRectangleBatchSink> native_ui_rectangle_batch_sink{};
    NativeUiRectangleCompileLimits native_ui_rectangle_compile_limits{};

    // Optional worker-thread instrumentation. It is called immediately before
    // a queued command is executed. Besides profiling, this seam lets tests
    // prove that an exception on the worker is delivered to the next producer
    // or synchronization boundary instead of terminating the process.
    std::function<void(GsRendererWorkerCommandKind, std::uint64_t)> dispatch_hook{};

    // Optional instrumentation on the dedicated scanout thread, immediately
    // before it converts an immutable local-memory snapshot. Keeping this
    // separate from dispatch_hook lets tests and profilers stall scanout
    // without stalling later GS commands or statistics-only fences.
    std::function<void(std::uint64_t)> scanout_dispatch_hook{};
};

struct GsRendererWorkerSnapshot {
    // These are values, never references into the live worker-owned renderer.
    guest::GsLiveRendererStatistics statistics{};
    GsRendererWorkerQueueStatistics queue_statistics{};
    std::uint64_t completed_serial{};

    // When synchronize() receives a plan, both the exact plan and its render
    // result are returned from the same ordered fence. An invalid plan still
    // produces a GsScanoutFrame carrying the corresponding failure status.
    std::optional<GsScanoutPlan> scanout_plan{};
    std::optional<GsScanoutFrame> scanout_frame{};

    // A successful scanout returns the same immutable 4 MiB local-memory
    // image that the scanout thread consumed. This is moved out of the
    // conversion job, never copied a second time, so diagnostic exports can
    // retain the exact VRAM corresponding to scanout_frame.
    std::optional<guest::GsLocalMemory> local_memory{};
};

struct GsRendererWorkerFrameCompletionResult {
    std::uint64_t frame_id{};
    std::uint64_t completed_serial{};
};

using GsRendererWorkerPrimaryBatchCommandEvidence =
    runtime::Slus21050FemainPrimaryStreamDigestEntry;

class GsRendererWorkerPrimaryBatchEvidence;

enum class GsTextureResidencyAcquireStatus : std::uint8_t {
    Acquired,
    MissingTex0,
    Tex0Mismatch,
    UnsupportedTexture,
    FeedbackCacheConflict,
    RegistryRejected,
};

struct GsTextureResidencyPin {
    GsTextureResidencyToken token{};
    std::shared_ptr<const guest::GsTextureImageSnapshot> image{};
};

struct GsTextureResidencyAcquireResult {
    GsTextureResidencyAcquireStatus status{GsTextureResidencyAcquireStatus::MissingTex0};
    std::optional<GsTextureResidencyPin> pin{};
    std::uint64_t completed_serial{};

    [[nodiscard]] bool acquired() const noexcept {
        return status == GsTextureResidencyAcquireStatus::Acquired && pin.has_value() &&
               pin->token.valid() && pin->image != nullptr;
    }
};

enum class GsTextureResidencyReleaseStatus : std::uint8_t {
    Released,
    UnknownToken,
};

struct GsTextureResidencyReleaseResult {
    GsTextureResidencyReleaseStatus status{GsTextureResidencyReleaseStatus::UnknownToken};
    GsTextureResidencyToken token{};
    std::uint64_t completed_serial{};

    [[nodiscard]] bool released() const noexcept {
        return status == GsTextureResidencyReleaseStatus::Released;
    }
};

enum class NativeUiTextDrawStatus : std::uint8_t {
    Delivered,
    MalformedDraw,
    UnsupportedMipState,
    IncompleteAmbientState,
    AmbientStateMismatch,
    MissingResidency,
    MissingTex0,
    Tex0Mismatch,
    UnsupportedTexture,
    FeedbackCacheConflict,
    StaleResidency,
    MissingSink,
    IncompleteEffectiveState,
    UnsupportedEffectiveState,
    MissingConversion,
    UnsupportedRectangleBatch,
};

struct NativeUiTextDrawResult {
    NativeUiTextDrawStatus status{NativeUiTextDrawStatus::MalformedDraw};
    // Echoes NativeUiTextDraw::raster_generation.value without exposing the
    // game-specific token type through this generic worker header.
    std::uint64_t raster_generation{};
    std::uint64_t completed_serial{};
    // True only when the resident image was admitted from a complete
    // per-block unchanged proof and no full texel/index comparison ran.
    bool texture_content_scan_skipped{};
    // True when currentness narrowed later global writes through the texture's
    // conservative physical-block footprint at this FIFO position.
    bool texture_mutation_block_scan_performed{};

    [[nodiscard]] bool delivered() const noexcept {
        return status == NativeUiTextDrawStatus::Delivered;
    }
};

// Host-side ordered GS executor. GsCapture remains the single producer and
// GIF decoder; this observer takes ownership of every decoded command and
// replays it on one dedicated thread. GsLiveRenderer itself stays deliberately
// single-threaded. Queue order is callback order -- command sequence fields are
// diagnostic metadata and are never used for sorting.
class GsRendererWorker final : public guest::GsCommandObserver {
  public:
    class PrimaryBatchSubmission;

    explicit GsRendererWorker(GsRendererWorkerConfig config = {});
    ~GsRendererWorker() override;

    GsRendererWorker(const GsRendererWorker&) = delete;
    GsRendererWorker& operator=(const GsRendererWorker&) = delete;
    GsRendererWorker(GsRendererWorker&&) = delete;
    GsRendererWorker& operator=(GsRendererWorker&&) = delete;

    void on_gs_gif_tag_begin(guest::GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const guest::GsRegisterWrite& write) override;
    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override;
    void on_gs_image_transfer(guest::GsImageTransfer&& transfer) override;

    // At a quiescent GIF boundary, replaces this worker as capture's primary
    // observer with a synchronous forwarding bridge and installs the same
    // bridge in the terminal raw slot. Every decoded callback is inserted at
    // the worker's normal bounded FIFO admission point. Capture's authoritative
    // EOP callback seals evidence only for expected_path while the complete
    // primary command stream remained available. The serialized owner should
    // detach before destroying either endpoint. Lifetime tokens prevent a
    // destroyed worker or capture from being dereferenced/restored if cleanup
    // order is violated.
    [[nodiscard]] std::unique_ptr<PrimaryBatchSubmission>
    begin_primary_batch(guest::GsCapture& capture, guest::GifPath expected_path);

    // A process-local identity that changes whenever a worker instance is
    // replaced. It is used only to reject stale evidence after replacement.
    [[nodiscard]] std::uint64_t worker_instance_id() const noexcept;

    // Enqueues a renderer reset after all previously submitted commands. Later
    // commands observe the reset in FIFO order. Call synchronize() when the
    // caller also needs to wait until the reset has completed.
    void reset();

    // Inserts a strict FIFO fence and waits for its result. With a scanout
    // plan, the GS worker copies local memory at that exact prefix and a
    // dedicated scanout worker converts the immutable copy. Later GS commands
    // may execute while this caller waits; the returned frame and statistics
    // still describe only the requested prefix.
    [[nodiscard]] GsRendererWorkerSnapshot
    synchronize(std::optional<GsScanoutPlan> scanout_plan = std::nullopt);

    // Enqueues the same exact-prefix snapshot boundary without waiting for it.
    // Modeled VBlank presentation, diagnostic exports, and guest-visible
    // fences use this path so the scanout plan and local-memory image describe
    // one FIFO boundary. Queue backpressure may still briefly block submission
    // at the configured hard limit; commands are never lost.
    [[nodiscard]] std::future<GsRendererWorkerSnapshot>
    request_snapshot(std::optional<GsScanoutPlan> scanout_plan = std::nullopt);

    // Inserts a strict primary-FIFO frame completion marker. The worker only
    // echoes the caller's nonzero frame identity and this command's FIFO
    // serial; it does not inspect scanout or guest/FEMain state. The caller
    // owns identity uniqueness and invalidation across worker/runtime-session
    // replacement.
    // A zero frame identity is rejected synchronously and does not poison the
    // worker.
    [[nodiscard]] std::future<GsRendererWorkerFrameCompletionResult>
    request_frame_completion(std::uint64_t frame_id);

    // Requests a host-only diagnostic view of the latest coherent renderer prefix. Unlike
    // request_snapshot(), this does not append a fence behind the complete GS
    // FIFO: the worker services it between two ordered commands. It therefore
    // cannot be used for guest-visible FINISH/readback semantics or exact
    // exports or normal VBlank presentation: its plan may have been sampled at
    // a different guest boundary than the selected command prefix. The mailbox
    // is bounded to two requests by default.
    [[nodiscard]] std::future<GsRendererWorkerSnapshot>
    request_present_snapshot(GsScanoutPlan scanout_plan);

    // Inserts an exact FIFO request for the current base-level texture image.
    // The command succeeds only when the selected context has a known TEX0
    // exactly equal to expected_tex0. The returned image is immutable and can
    // outlive a later release; its token remains valid in this worker registry
    // until an ordered release command completes. The caller must enqueue this
    // only after the matching TEX0/CLD stream position. The worker rejects a
    // snapshot whose complete base-level source cannot be proven disjoint from
    // pages retained by an active framebuffer-feedback cache epoch.
    [[nodiscard]] std::future<GsTextureResidencyAcquireResult>
    request_texture_residency(guest::GsDrawingContext context, std::uint64_t expected_tex0);

    // Releases registry ownership at an exact FIFO position. Unknown and zero
    // tokens are ordinary results and do not poison later renderer commands.
    [[nodiscard]] std::future<GsTextureResidencyReleaseResult>
    request_texture_residency_release(GsTextureResidencyToken token);

    // Inserts one fully owned renderer-neutral draw at an exact FIFO position.
    // The canonical image is copied into worker-owned immutable storage before
    // enqueue, preventing caller aliases from changing content or byte charge.
    // Malformed envelopes, the currently unsupported mipmapped profile, and a
    // missing sink complete with typed results and leave the worker usable.
    // This public snapshot transport does not authenticate residency or match
    // ambient/effective GS controls; it is not the production migration path.
    // Sink exceptions poison the worker and settle the current and still-queued
    // worker futures with the same exception.
    [[nodiscard]] std::future<NativeUiTextDrawResult>
    request_native_ui_text_draw(const NativeUiTextDraw& draw);

    // An instantaneous, locked copy for diagnostics and bounded-queue tests.
    // It is not a rendering fence; use synchronize() for completed GS state.
    [[nodiscard]] GsRendererWorkerQueueStatistics queue_statistics() const;

  private:
    friend class runtime::slus_21050::TextRasterResidencyStore;

    struct PrimaryBatchAdmission {
        std::uint64_t worker_instance_id{};
        std::uint64_t serial{};
        GsRendererWorkerCommandKind kind{};
        std::uint64_t payload_digest{};
    };

    [[nodiscard]] PrimaryBatchAdmission
    admit_primary(const guest::GsRegisterWrite& write);
    [[nodiscard]] PrimaryBatchAdmission admit_primary(const guest::GifPath path);
    [[nodiscard]] PrimaryBatchAdmission admit_primary_reset();
    [[nodiscard]] PrimaryBatchAdmission admit_primary(const guest::GsImageTransfer& transfer);
    [[nodiscard]] PrimaryBatchAdmission admit_primary(guest::GsImageTransfer&& transfer);

    // Store-only fast path. The request was isolated during authenticated
    // preparation, and the worker resolves the private generic token at command
    // execution so repeated draws share the registry image without copying it.
    // Ambient state and the observed effective-control profile are classified
    // at that FIFO position before either image lookup or sink delivery.
    [[nodiscard]] std::future<NativeUiTextDrawResult>
    request_prepared_native_ui_text_draw(GsTextureResidencyToken residency,
                                         std::uint64_t expected_tex0,
                                         std::shared_ptr<NativeUiTextDrawRequest> request);

    // Store-only distinction for exception cleanup: true means no later
    // command can run, while false leaves a pre-admission allocation failure
    // eligible for destructor cleanup on this still-healthy worker.
    [[nodiscard]] bool failed() const noexcept;

    class Impl;
    std::shared_ptr<const bool> lifetime_token_{std::make_shared<const bool>(true)};
    std::unique_ptr<Impl> impl_;
};

// Immutable, worker-owned evidence for one contiguous accepted primary batch.
// The command span remains valid for the lifetime of this value. Serial range
// and worker identity are worker-issued; compact command entries use the same
// canonical digest schema as the FEMain terminal coordinator.
class GsRendererWorkerPrimaryBatchEvidence final {
  public:
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint64_t worker_instance_id() const noexcept {
        return worker_instance_id_;
    }
    [[nodiscard]] std::uint64_t first_serial() const noexcept { return first_serial_; }
    [[nodiscard]] std::uint64_t last_serial() const noexcept { return last_serial_; }
    [[nodiscard]] std::uint64_t command_count() const noexcept { return command_count_; }
    [[nodiscard]] std::uint64_t
    primary_stream_digest(std::uint64_t primary_first_ordinal) const noexcept {
        return valid_ ? runtime::digest_slus_21050_femain_primary_stream_entries(
                            commands_, primary_first_ordinal)
                      : 0U;
    }
    [[nodiscard]] std::span<const GsRendererWorkerPrimaryBatchCommandEvidence> commands()
        const noexcept {
        return commands_;
    }
    [[nodiscard]] bool belongs_to_worker_instance(const std::uint64_t instance_id) const noexcept {
        return valid_ && instance_id != 0U && instance_id == worker_instance_id_;
    }
    [[nodiscard]] bool belongs_to(const GsRendererWorker& worker) const noexcept;

  private:
    friend class GsRendererWorker;
    friend class GsRendererWorker::PrimaryBatchSubmission;

    GsRendererWorkerPrimaryBatchEvidence(
        std::uint64_t worker_instance_id,
        std::vector<GsRendererWorkerPrimaryBatchCommandEvidence> commands,
        std::uint64_t first_serial, std::uint64_t last_serial) noexcept
        : worker_instance_id_(worker_instance_id), first_serial_(first_serial),
          last_serial_(last_serial), command_count_(commands.size()), commands_(std::move(commands)),
          valid_(worker_instance_id_ != 0U && !commands_.empty() && first_serial_ != 0U &&
                 last_serial_ >= first_serial_ && command_count_ != 0U) {}

    std::uint64_t worker_instance_id_{};
    std::uint64_t first_serial_{};
    std::uint64_t last_serial_{};
    std::uint64_t command_count_{};
    std::vector<GsRendererWorkerPrimaryBatchCommandEvidence> commands_{};
    bool valid_{};
};

// Synchronous primary/raw observer bridge for one packet-sized collection. It
// remains usable as a forwarding command observer after EOP, but later
// commands are deliberately excluded from the immutable sealed evidence.
class GsRendererWorker::PrimaryBatchSubmission final : public guest::GsCommandObserver,
                                                        public guest::GsRawGifObserver {
  public:
    using Evidence = GsRendererWorkerPrimaryBatchEvidence;

    ~PrimaryBatchSubmission() override;

    PrimaryBatchSubmission(const PrimaryBatchSubmission&) = delete;
    PrimaryBatchSubmission& operator=(const PrimaryBatchSubmission&) = delete;
    PrimaryBatchSubmission(PrimaryBatchSubmission&&) = delete;
    PrimaryBatchSubmission& operator=(PrimaryBatchSubmission&&) = delete;

    void on_gs_gif_tag_begin(guest::GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const guest::GsRegisterWrite& write) override;
    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override;
    void on_gs_image_transfer(guest::GsImageTransfer&& transfer) override;

    // The terminal raw slot supplies the authoritative packet EOP. Raw words
    // are deliberately ignored because command evidence is minted only by the
    // worker's actual FIFO admission point.
    void on_gs_gif_quadword(guest::GifPath, const guest::GsQuadword&) override {}
    [[nodiscard]] bool observes_gs_gif_packet_eop() const noexcept override {
        return true;
    }
    void on_gs_gif_packet_eop(guest::GifPath path,
                              bool primary_command_stream_available) noexcept override;
    void on_gs_gif_reset() noexcept override;

    // Detach succeeds only at a quiescent GIF boundary, clears the terminal
    // raw slot, and restores the worker as primary. State queries are
    // synchronized and safe while the serialized capture owner is idle.
    [[nodiscard]] bool detach() noexcept;
    [[nodiscard]] bool attached() const noexcept;
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::shared_ptr<const Evidence> evidence() const noexcept;
    [[nodiscard]] std::uint64_t worker_instance_id() const noexcept {
        return worker_instance_id_;
    }

  private:
    friend class GsRendererWorker;

    PrimaryBatchSubmission(GsRendererWorker& worker, guest::GsCapture& capture,
                           guest::GifPath expected_path);
    void record_locked(const GsRendererWorker::PrimaryBatchAdmission& admission,
                       guest::GifPath path, std::uint64_t guest_sequence) noexcept;
    void fail_locked() noexcept;
    void seal_locked() noexcept;

    GsRendererWorker* worker_{};
    guest::GsCapture* capture_{};
    std::weak_ptr<const bool> worker_lifetime_{};
    std::weak_ptr<const bool> capture_lifetime_{};
    guest::GifPath expected_path_{guest::GifPath::Path1};
    std::uint64_t worker_instance_id_{};
    std::uint64_t first_serial_{};
    std::uint64_t last_serial_{};
    std::vector<GsRendererWorkerPrimaryBatchCommandEvidence> commands_{};
    std::shared_ptr<const Evidence> evidence_{};
    mutable std::mutex mutex_{};
    bool attached_{};
    bool eop_received_{};
    bool sealed_{};
    bool failed_{};
};

} // namespace takedown::presentation
