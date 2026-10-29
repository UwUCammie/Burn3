#pragma once

#include "takedown/presentation/gs_renderer_worker.hpp"
#include "takedown/presentation/native_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_text_raster_guest.hpp"
#include "takedown/runtime/slus_21050_ui_text_raster.hpp"

#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

// Strong, immutable ownership record for one admitted text-raster generation.
// TextRasterResidencyStore does not expose the generic worker token and uses it
// only with its bound worker. Prepared draws retain this record until their
// callback is accepted. The worker command then relies on primary-FIFO order:
// it resolves the registry image before a later explicit release can remove it.
struct TextRasterResidencyPin {
    const TextRasterResidencyToken token;
    const TextRasterBinding logical;
    const ResidentBaseRasterResourceGuestPlan guest_plan;
    const std::shared_ptr<const guest::GsTextureImageSnapshot> image;
    const std::uint64_t acquired_serial{};
};

enum class TextRasterResidencyBeginStatus : std::uint8_t {
    Enqueued,
    StaleGuestPlan,
};

[[nodiscard]] const char* to_string(TextRasterResidencyBeginStatus status) noexcept;

enum class TextRasterResidencyAcquireStatus : std::uint8_t {
    Acquired,
    StaleGuestPlan,
    WorkerMissingTex0,
    WorkerTex0Mismatch,
    WorkerUnsupportedTexture,
    WorkerFeedbackCacheConflict,
    WorkerRegistryRejected,
    MalformedWorkerResult,
    TextTokenExhausted,
    WrongStoreOrConsumed,
};

[[nodiscard]] const char* to_string(TextRasterResidencyAcquireStatus status) noexcept;

enum class TextRasterResidencyReleaseStatus : std::uint8_t {
    Released,
    ZeroToken,
    UnknownOrWrongStore,
    GenericTokenMissing,
};

[[nodiscard]] const char* to_string(TextRasterResidencyReleaseStatus status) noexcept;

class TextRasterResidencyStore;

// Only TextRasterResidencyStore can construct a pending acquisition, which
// preserves the exact plan, Context1 request, and worker provenance without
// trusting caller-supplied completion metadata. The type is single-use and
// move-only. Destroying an unfinished request waits for its result and performs
// an ordered best-effort release if the worker acquired a generic pin.
//
// Every pending request must be finished or destroyed before its originating
// GsRendererWorker. An abandoned request may outlive its store, but finish() may
// only be called while the originating store is alive. A shared opaque identity
// prevents a different store later constructed at the same address from
// accepting that request.
class TextRasterResidencyPendingAcquire final {
  public:
    ~TextRasterResidencyPendingAcquire() noexcept;

    TextRasterResidencyPendingAcquire(const TextRasterResidencyPendingAcquire&) = delete;
    TextRasterResidencyPendingAcquire& operator=(const TextRasterResidencyPendingAcquire&) = delete;
    TextRasterResidencyPendingAcquire(TextRasterResidencyPendingAcquire&& other) noexcept;
    TextRasterResidencyPendingAcquire&
    operator=(TextRasterResidencyPendingAcquire&& other) noexcept;

  private:
    friend class TextRasterResidencyStore;

    TextRasterResidencyPendingAcquire(
        std::shared_ptr<const void> owner_identity, presentation::GsRendererWorker* worker,
        ResidentBaseRasterResourceGuestPlan guest_plan,
        std::future<presentation::GsTextureResidencyAcquireResult> completion) noexcept;

    void abandon_noexcept() noexcept;

    std::shared_ptr<const void> owner_identity_{};
    presentation::GsRendererWorker* worker_{};
    ResidentBaseRasterResourceGuestPlan guest_plan_{};
    std::future<presentation::GsTextureResidencyAcquireResult> completion_{};
};

struct TextRasterResidencyBeginResult {
    TextRasterResidencyBeginStatus status{TextRasterResidencyBeginStatus::StaleGuestPlan};
    std::optional<TextRasterResidencyPendingAcquire> pending{};

    [[nodiscard]] bool enqueued() const noexcept {
        return status == TextRasterResidencyBeginStatus::Enqueued && pending.has_value();
    }
};

struct TextRasterResidencyAcquireResult {
    TextRasterResidencyAcquireStatus status{TextRasterResidencyAcquireStatus::StaleGuestPlan};
    std::shared_ptr<const TextRasterResidencyPin> pin{};
    std::uint64_t completed_serial{};

    [[nodiscard]] bool acquired() const noexcept {
        return status == TextRasterResidencyAcquireStatus::Acquired && pin != nullptr &&
               pin->token.value != 0U && pin->image != nullptr;
    }
};

struct TextRasterResidencyReleaseResult {
    TextRasterResidencyReleaseStatus status{TextRasterResidencyReleaseStatus::UnknownOrWrongStore};
    TextRasterResidencyToken token{};
    std::uint64_t completed_serial{};

    [[nodiscard]] bool released() const noexcept {
        return status == TextRasterResidencyReleaseStatus::Released;
    }
};

enum class NativeUiTextDrawPrepareStatus : std::uint8_t {
    Prepared,
    InvalidAssociation,
    UnknownOrWrongStoreResidency,
    MalformedResidencyPin,
    UnsupportedMipState,
    UnsupportedTextureFunction,
    UnsupportedTestState,
    UnsupportedColorClampState,
    AlphaBlendingDisabled,
    MalformedNativeDraw,
};

[[nodiscard]] const char* to_string(NativeUiTextDrawPrepareStatus status) noexcept;

// One store-authenticated callback value. Construction copies the typed draw
// geometry, retains the exact immutable public pin, and preserves the private
// generic worker token. Only the owning store can consume it. It never releases
// residency automatically because ordered duplicate callbacks may share a pin.
class PreparedUiTextDrawSubmission final {
  public:
    PreparedUiTextDrawSubmission(const PreparedUiTextDrawSubmission&) = delete;
    PreparedUiTextDrawSubmission& operator=(const PreparedUiTextDrawSubmission&) = delete;
    PreparedUiTextDrawSubmission(PreparedUiTextDrawSubmission&&) noexcept = default;
    PreparedUiTextDrawSubmission& operator=(PreparedUiTextDrawSubmission&&) noexcept = default;

    [[nodiscard]] UiSubmissionKey key() const noexcept { return key_; }
    [[nodiscard]] std::size_t bucket_index() const noexcept { return bucket_index_; }
    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_; }
    [[nodiscard]] const TextRasterBinding& logical() const noexcept { return logical_; }
    [[nodiscard]] TextRasterResidencyToken raster_generation() const noexcept {
        return raster_generation_;
    }
    [[nodiscard]] bool consumed() const noexcept { return owner_identity_ == nullptr; }

  private:
    friend class TextRasterResidencyStore;

    PreparedUiTextDrawSubmission(
        std::shared_ptr<const void> owner_identity,
        std::shared_ptr<const TextRasterResidencyPin> pin,
        presentation::GsTextureResidencyToken generic_token,
        std::shared_ptr<presentation::NativeUiTextDrawRequest> request) noexcept;

    std::shared_ptr<const void> owner_identity_{};
    std::shared_ptr<const TextRasterResidencyPin> pin_{};
    presentation::GsTextureResidencyToken generic_token_{};
    std::shared_ptr<presentation::NativeUiTextDrawRequest> request_{};
    UiSubmissionKey key_{};
    std::size_t bucket_index_{};
    std::size_t node_index_{};
    TextRasterBinding logical_{};
    TextRasterResidencyToken raster_generation_{};
};

struct PreparedUiTextDrawBatchesCandidate {
    SubmitUiDrawBucketsGuestPlan ui_plan;
    std::vector<PreparedUiTextDrawSubmission> submissions;
};

struct NativeUiTextDrawPrepareResult {
    NativeUiTextDrawPrepareStatus status{NativeUiTextDrawPrepareStatus::InvalidAssociation};
    std::optional<PreparedUiTextDrawBatchesCandidate> candidate{};
    std::optional<std::size_t> rejected_submission_index{};

    [[nodiscard]] bool prepared() const noexcept {
        return status == NativeUiTextDrawPrepareStatus::Prepared && candidate.has_value();
    }
};

enum class PreparedNativeUiTextDrawEnqueueStatus : std::uint8_t {
    Enqueued,
    WrongStoreOrConsumed,
    CallbackMismatch,
    ResidencyReleased,
    CallbackEntryRejected,
    CallbackPayloadMismatch,
    TargetSourceRejected,
    TargetSourceMismatch,
    StaleGuestRaster,
};

[[nodiscard]] const char* to_string(PreparedNativeUiTextDrawEnqueueStatus status) noexcept;

struct PreparedNativeUiTextDrawEnqueueResult {
    PreparedNativeUiTextDrawEnqueueStatus status{
        PreparedNativeUiTextDrawEnqueueStatus::WrongStoreOrConsumed};
    TextRasterResidencyToken raster_generation{};
    std::future<presentation::NativeUiTextDrawResult> completion{};

    [[nodiscard]] bool enqueued() const noexcept {
        return status == PreparedNativeUiTextDrawEnqueueStatus::Enqueued && completion.valid();
    }
};

// The terminal phase of one synchronous producer-boundary submission. Complete
// means the draw reached the native sink and its raster generation was released.
// For every other value, phase identifies the operation whose normal rejection
// selected the result; later cleanup may still be described by release_status.
enum class ResidentNativeUiTextDrawPhase : std::uint8_t {
    Preflight,
    Acquire,
    Prepare,
    Enqueue,
    Draw,
    Release,
    Complete,
};

[[nodiscard]] const char* to_string(ResidentNativeUiTextDrawPhase phase) noexcept;

enum class ResidentNativeUiTextDrawStatus : std::uint8_t {
    DeliveredAndReleased,
    StaleGuestPlan,
    AcquireRejected,
    PrepareRejected,
    EnqueueRejected,
    DrawRejected,
    ReleaseRejected,
};

[[nodiscard]] const char* to_string(ResidentNativeUiTextDrawStatus status) noexcept;

// One complete outcome with the typed status from every phase that ran. The
// three serials are exact positions in the worker's primary FIFO; a successful
// result always orders acquire < draw < release. Fatal worker or sink failures
// remain exceptions and are not converted into ordinary statuses.
struct ResidentNativeUiTextDrawResult {
    ResidentNativeUiTextDrawPhase phase{ResidentNativeUiTextDrawPhase::Preflight};
    ResidentNativeUiTextDrawStatus status{ResidentNativeUiTextDrawStatus::PrepareRejected};
    std::optional<TextRasterResidencyBeginStatus> begin_status{};
    std::optional<TextRasterResidencyAcquireStatus> acquire_status{};
    std::optional<NativeUiTextDrawPrepareStatus> prepare_status{};
    std::optional<PreparedNativeUiTextDrawEnqueueStatus> enqueue_status{};
    std::optional<presentation::NativeUiTextDrawStatus> draw_status{};
    std::optional<TextRasterResidencyReleaseStatus> release_status{};
    TextRasterResidencyToken raster_generation{};
    std::uint64_t acquire_serial{};
    std::uint64_t draw_serial{};
    std::uint64_t release_serial{};

    [[nodiscard]] bool delivered_and_released() const noexcept {
        return phase == ResidentNativeUiTextDrawPhase::Complete &&
               status == ResidentNativeUiTextDrawStatus::DeliveredAndReleased &&
               draw_status == presentation::NativeUiTextDrawStatus::Delivered &&
               release_status == TextRasterResidencyReleaseStatus::Released;
    }
};

// Game-facing owner for the bounded selector-zero resident profile. The store
// is intentionally single-threaded and is bound permanently to one ordered GS
// worker. The worker must outlive the store, because explicit release, abandoned
// pending cleanup, and best-effort store destruction enqueue worker commands.
// Every store operation and every store or pending destructor must run off the
// worker dispatch thread because completion and cleanup may wait for that thread.
// Direct submissions to the bound worker must share this same sole producer;
// concurrent external producers are outside the ordering contract.
//
// begin() is valid only at a producer boundary after the matching decoded
// TEX0/CLD traffic has entered that worker's primary FIFO. It does not establish
// that temporal placement itself. Checked worker acquisition proves bounded
// base-level feedback-cache authority; this store does not make eager UI
// planning safe for dispatch. Guest execution, writes to the planned guest
// evidence, and later GS submissions must remain quiesced until finish()
// returns: the two exact replans reject visible endpoint drift but cannot
// observe a transient A-B-A mutation. Prepared draws retain their immutable pin
// through worker admission. Every intended use must enter the primary FIFO
// before release(token), so each command resolves its private generic token
// before that later ordered release.
class TextRasterResidencyStore final {
  public:
    explicit TextRasterResidencyStore(presentation::GsRendererWorker& worker);
    ~TextRasterResidencyStore() noexcept;

    TextRasterResidencyStore(const TextRasterResidencyStore&) = delete;
    TextRasterResidencyStore& operator=(const TextRasterResidencyStore&) = delete;
    TextRasterResidencyStore(TextRasterResidencyStore&&) = delete;
    TextRasterResidencyStore& operator=(TextRasterResidencyStore&&) = delete;

    // Exact-replans before enqueueing a fixed Context1 request for plan.tex0_bits.
    // Worker submission failures remain exceptional and propagate to the caller.
    [[nodiscard]] TextRasterResidencyBeginResult
    begin(const guest::EeMemory& memory, const ResidentBaseRasterResourceGuestPlan& plan);

    // Waits for the worker completion, exact-replans a second time, and only then
    // mints and stores a fresh process-wide text token. The caller must preserve
    // the quiescent interval described above. A successful generic pin is
    // synchronously released in FIFO order if the post-check or text-token
    // admission fails. Passing a pending request consumes it exactly once.
    [[nodiscard]] TextRasterResidencyAcquireResult
    finish(const guest::EeMemory& memory, TextRasterResidencyPendingAcquire pending);

    [[nodiscard]] std::shared_ptr<const TextRasterResidencyPin>
    find(TextRasterResidencyToken token) const noexcept;

    // Synchronously enqueues and waits for release on the bound worker. Zero,
    // unknown, already-released, and another store's process-wide tokens reject
    // without enqueueing a generic release. Worker failures remain exceptional.
    [[nodiscard]] TextRasterResidencyReleaseResult release(TextRasterResidencyToken token);

    // Recomputes and exact-matches every UI callback association, then
    // authenticates its raster residency before publishing any output. Typed
    // batch geometry remains trusted input from the upstream guest planner.
    // This copies geometry and retains pins but has no worker/FIFO effects. The
    // current begin/finish quiescence contract still applies.
    [[nodiscard]] NativeUiTextDrawPrepareResult
    prepare_native_ui_text_draws(const ResolvedUiTextDrawBatchesCandidate& candidate) const;

    // Enqueues exactly one prepared draw at the caller-provided retail callback
    // boundary. Normal identity/callback/release rejection does not consume the
    // entry. Successful worker admission consumes it; synchronous exceptions
    // leave it retryable. Release remains an explicit later FIFO action after
    // the final callback using that raster generation has been enqueued.
    [[nodiscard]] PreparedNativeUiTextDrawEnqueueResult enqueue_native_ui_text_draw(
        PreparedUiTextDrawSubmission& submission, UiSubmissionKey expected_key,
        std::size_t expected_bucket_index, std::size_t expected_node_index,
        std::optional<presentation::NativeUiTextConversionInput> conversion = std::nullopt);

    // Checked semantic handoff at a caller-supplied callback position. Captures
    // actual PC/A0/A1 values, exact-matches the prepared render/effect/batch,
    // captures the current game-side render-target binding, revalidates the
    // resident guest raster plan, then binds current conversion values and
    // admits one command. Rejection has no FIFO or consumption effect. Caller
    // must serialize guest execution through admission. This low-level method
    // still trusts the supplied bucket/node position; the source-private
    // coordinator wrapper authenticates and spends an occurrence before calling
    // it. The worker authenticates observable GS target/state and texture storage
    // at FIFO execution. This does not install a live hook or replace guest
    // execution.
    [[nodiscard]] PreparedNativeUiTextDrawEnqueueResult enqueue_native_ui_text_draw_from_guest(
        PreparedUiTextDrawSubmission& submission, UiSubmissionKey expected_key,
        std::size_t expected_bucket_index, std::size_t expected_node_index,
        const guest::R5900State& cpu, const guest::EeMemory& memory);

    // Executes one caller-proved, already-planned callback as one synchronous
    // producer-boundary transaction. Guest-plan identity, the draw's logical
    // raster binding, and renderer profile checks run before acquisition. The
    // callback key, indices, effect, state, and geometry remain trusted typed
    // input from the upstream guest planner; this overload does not recompute
    // their complete association. Retained-size validation runs while
    // constructing the request with its real acquired token. begin() and
    // finish() run back-to-back, so a single-threaded guest dispatcher cannot
    // execute or mutate the planned evidence during their quiescent interval.
    // Under the sole producer contract, the draw and its release enter the same
    // primary FIFO consecutively before this function returns.
    //
    // Normal rejection after acquisition always attempts an ordered release.
    // Exceptions also perform best-effort synchronous cleanup and then propagate.
    // A cleanup command that completes lets the store erase the entry. A
    // pre-admission exception on a healthy worker retains ownership so the
    // store destructor can retry. A permanently poisoned worker cannot retry;
    // its private registry remains bounded by worker destruction.
    [[nodiscard]] ResidentNativeUiTextDrawResult
    submit_resident_native_ui_text_draw(const guest::EeMemory& memory,
                                        const ResidentBaseRasterResourceGuestPlan& raster_plan,
                                        UiTextDrawBatchSubmission draw);

    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct Entry {
        std::shared_ptr<const TextRasterResidencyPin> pin;
        presentation::GsTextureResidencyToken generic_token;
    };

    struct SingleNativeUiTextDrawPrepareResult {
        NativeUiTextDrawPrepareStatus status{NativeUiTextDrawPrepareStatus::InvalidAssociation};
        std::optional<PreparedUiTextDrawSubmission> submission{};
    };

    [[nodiscard]] NativeUiTextDrawPrepareStatus
    validate_native_ui_text_draw(const UiTextDrawBatchSubmission& draw,
                                 const UiTextRasterResolution& raster) const;
    [[nodiscard]] SingleNativeUiTextDrawPrepareResult
    prepare_native_ui_text_draw(UiTextDrawBatchSubmission draw,
                                const UiTextRasterResolution& raster) const;
    [[nodiscard]] std::optional<PreparedNativeUiTextDrawEnqueueStatus>
    validate_prepared_native_ui_text_draw(const PreparedUiTextDrawSubmission& submission,
                                          UiSubmissionKey expected_key,
                                          std::size_t expected_bucket_index,
                                          std::size_t expected_node_index) const;
    void cleanup_transaction_residency_noexcept(TextRasterResidencyToken token) noexcept;

    presentation::GsRendererWorker& worker_;
    std::shared_ptr<const void> identity_;
    std::map<std::uint64_t, Entry> entries_{};
};

} // namespace takedown::runtime::slus_21050
