#include "takedown/runtime/slus_21050_text_raster_residency.hpp"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace takedown::runtime::slus_21050 {

namespace {

// Process-wide text-raster tokens are never reused; zero is the invalid token.
std::atomic<std::uint64_t> next_text_residency_token{1U};

[[nodiscard]] std::optional<TextRasterResidencyToken> allocate_text_residency_token() noexcept {
    auto value = next_text_residency_token.load(std::memory_order_relaxed);
    while (value != 0U) {
        const auto next = value == std::numeric_limits<std::uint64_t>::max() ? 0U : value + 1U;
        if (next_text_residency_token.compare_exchange_weak(value, next, std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
            return TextRasterResidencyToken{value};
        }
    }
    return std::nullopt;
}

// Ordered cleanup of an acquired generic pin; any rejection is a logic error.
void release_generic_or_throw(presentation::GsRendererWorker& worker,
                              const presentation::GsTextureResidencyToken token) {
    if (!token.valid()) {
        throw std::logic_error("acquired GS texture residency returned an invalid generic token");
    }
    const presentation::GsTextureResidencyReleaseResult released =
        worker.request_texture_residency_release(token).get();
    if (!released.released() || released.token != token) {
        throw std::logic_error("ordered GS texture residency cleanup rejected its owned token");
    }
}

// Owns the generic pin until the text-raster entry takes it over. Destruction
// without an explicit release or dismissal is a best-effort ordered release.
class GenericResidencyCleanup final {
  public:
    GenericResidencyCleanup(presentation::GsRendererWorker& worker,
                            const presentation::GsTextureResidencyToken token) noexcept
        : worker_(worker), token_(token), active_(token.valid()) {}

    ~GenericResidencyCleanup() {
        if (!active_) {
            return;
        }
        try {
            release_generic_or_throw(worker_, token_);
        } catch (...) {
        }
    }

    GenericResidencyCleanup(const GenericResidencyCleanup&) = delete;
    GenericResidencyCleanup& operator=(const GenericResidencyCleanup&) = delete;

    void release() {
        if (!active_) {
            return;
        }
        release_generic_or_throw(worker_, token_);
        active_ = false;
    }

    void dismiss() noexcept { active_ = false; }

  private:
    presentation::GsRendererWorker& worker_;
    presentation::GsTextureResidencyToken token_;
    bool active_;
};

[[nodiscard]] TextRasterResidencyAcquireStatus
map_worker_acquire_status(const presentation::GsTextureResidencyAcquireStatus status) noexcept {
    switch (status) {
    case presentation::GsTextureResidencyAcquireStatus::MissingTex0:
        return TextRasterResidencyAcquireStatus::WorkerMissingTex0;
    case presentation::GsTextureResidencyAcquireStatus::Tex0Mismatch:
        return TextRasterResidencyAcquireStatus::WorkerTex0Mismatch;
    case presentation::GsTextureResidencyAcquireStatus::UnsupportedTexture:
        return TextRasterResidencyAcquireStatus::WorkerUnsupportedTexture;
    case presentation::GsTextureResidencyAcquireStatus::FeedbackCacheConflict:
        return TextRasterResidencyAcquireStatus::WorkerFeedbackCacheConflict;
    case presentation::GsTextureResidencyAcquireStatus::RegistryRejected:
        return TextRasterResidencyAcquireStatus::WorkerRegistryRejected;
    case presentation::GsTextureResidencyAcquireStatus::Acquired:
        break;
    }
    return TextRasterResidencyAcquireStatus::MalformedWorkerResult;
}

// Renderer-profile gate shared by prepare and the resident transaction: base
// level only, nearest/linear MMAG/MMIN bits clear, TFX modulate, the fixed UI
// TEST/COLCLAMP words, and alpha blending on.
[[nodiscard]] NativeUiTextDrawPrepareStatus
validate_native_ui_text_draw_profile(const UiTextDrawBatchSubmission& draw,
                                     const std::uint64_t tex0_bits) noexcept {
    const auto& sampling = draw.ui.state.texture_sampling;
    if (sampling.maximum_mip_level != 0U || (sampling.raw & 0x1CU) != 0U) {
        return NativeUiTextDrawPrepareStatus::UnsupportedMipState;
    }
    if ((tex0_bits & 0x1800000000ULL) != 0U) {
        return NativeUiTextDrawPrepareStatus::UnsupportedTextureFunction;
    }
    if (draw.ui.state.test_raw != 0x41001U) {
        return NativeUiTextDrawPrepareStatus::UnsupportedTestState;
    }
    if (draw.ui.state.colclamp_raw != kUiDrawColclampRaw) {
        return NativeUiTextDrawPrepareStatus::UnsupportedColorClampState;
    }
    if (!draw.primitive.alpha_blending_enabled) {
        return NativeUiTextDrawPrepareStatus::AlphaBlendingDisabled;
    }
    if (draw.batch.raster.raster.value == 0U) {
        return NativeUiTextDrawPrepareStatus::MalformedNativeDraw;
    }
    if (!is_canonical_ui_draw_state_snapshot(draw.ui.state)) {
        return NativeUiTextDrawPrepareStatus::MalformedNativeDraw;
    }
    return NativeUiTextDrawPrepareStatus::Prepared;
}

[[nodiscard]] NativeUiTextDrawPrepareResult
rejected_native_ui_text_draw(const NativeUiTextDrawPrepareStatus status,
                             const std::size_t index) noexcept {
    return {.status = status, .rejected_submission_index = index};
}

} // namespace

const char* to_string(const TextRasterResidencyBeginStatus status) noexcept {
    switch (status) {
    case TextRasterResidencyBeginStatus::Enqueued:
        return "enqueued";
    case TextRasterResidencyBeginStatus::StaleGuestPlan:
        return "stale guest plan";
    }
    return "unknown text-raster residency begin status";
}

const char* to_string(const TextRasterResidencyAcquireStatus status) noexcept {
    switch (status) {
    case TextRasterResidencyAcquireStatus::Acquired:
        return "acquired";
    case TextRasterResidencyAcquireStatus::StaleGuestPlan:
        return "stale guest plan";
    case TextRasterResidencyAcquireStatus::WorkerMissingTex0:
        return "worker TEX0 is missing";
    case TextRasterResidencyAcquireStatus::WorkerTex0Mismatch:
        return "worker TEX0 does not match the guest plan";
    case TextRasterResidencyAcquireStatus::WorkerUnsupportedTexture:
        return "worker texture snapshot is unsupported";
    case TextRasterResidencyAcquireStatus::WorkerFeedbackCacheConflict:
        return "worker texture snapshot conflicts with the active framebuffer-feedback cache";
    case TextRasterResidencyAcquireStatus::WorkerRegistryRejected:
        return "worker texture registry rejected the snapshot";
    case TextRasterResidencyAcquireStatus::MalformedWorkerResult:
        return "worker returned a malformed texture residency result";
    case TextRasterResidencyAcquireStatus::TextTokenExhausted:
        return "process-wide text-raster token space is exhausted";
    case TextRasterResidencyAcquireStatus::WrongStoreOrConsumed:
        return "pending acquisition belongs to another store or was already consumed";
    }
    return "unknown text-raster residency acquire status";
}

const char* to_string(const TextRasterResidencyReleaseStatus status) noexcept {
    switch (status) {
    case TextRasterResidencyReleaseStatus::Released:
        return "released";
    case TextRasterResidencyReleaseStatus::ZeroToken:
        return "zero text-raster token";
    case TextRasterResidencyReleaseStatus::UnknownOrWrongStore:
        return "unknown, already released, or wrong-store text-raster token";
    case TextRasterResidencyReleaseStatus::GenericTokenMissing:
        return "owned generic GS texture token is missing";
    }
    return "unknown text-raster residency release status";
}

const char* to_string(const NativeUiTextDrawPrepareStatus status) noexcept {
    switch (status) {
    case NativeUiTextDrawPrepareStatus::Prepared:
        return "prepared";
    case NativeUiTextDrawPrepareStatus::InvalidAssociation:
        return "invalid draw/raster association";
    case NativeUiTextDrawPrepareStatus::UnknownOrWrongStoreResidency:
        return "unknown or wrong-store text-raster residency";
    case NativeUiTextDrawPrepareStatus::MalformedResidencyPin:
        return "malformed text-raster residency pin";
    case NativeUiTextDrawPrepareStatus::UnsupportedMipState:
        return "unsupported native UI mip state";
    case NativeUiTextDrawPrepareStatus::UnsupportedTextureFunction:
        return "unsupported native UI texture function";
    case NativeUiTextDrawPrepareStatus::UnsupportedTestState:
        return "unsupported native UI TEST state";
    case NativeUiTextDrawPrepareStatus::UnsupportedColorClampState:
        return "unsupported native UI COLCLAMP state";
    case NativeUiTextDrawPrepareStatus::AlphaBlendingDisabled:
        return "native UI alpha blending is disabled";
    case NativeUiTextDrawPrepareStatus::MalformedNativeDraw:
        return "malformed native UI draw";
    }
    return "unknown native UI draw prepare status";
}

const char* to_string(const PreparedNativeUiTextDrawEnqueueStatus status) noexcept {
    switch (status) {
    case PreparedNativeUiTextDrawEnqueueStatus::Enqueued:
        return "enqueued";
    case PreparedNativeUiTextDrawEnqueueStatus::WrongStoreOrConsumed:
        return "prepared draw belongs to another store or was consumed";
    case PreparedNativeUiTextDrawEnqueueStatus::CallbackMismatch:
        return "prepared draw does not match the callback boundary";
    case PreparedNativeUiTextDrawEnqueueStatus::ResidencyReleased:
        return "prepared draw residency was released";
    case PreparedNativeUiTextDrawEnqueueStatus::CallbackEntryRejected:
        return "guest callback entry could not be captured";
    case PreparedNativeUiTextDrawEnqueueStatus::CallbackPayloadMismatch:
        return "prepared draw differs from current callback values";
    case PreparedNativeUiTextDrawEnqueueStatus::TargetSourceRejected:
        return "current UI render-target source could not be captured";
    case PreparedNativeUiTextDrawEnqueueStatus::TargetSourceMismatch:
        return "prepared draw differs from the current UI render target";
    case PreparedNativeUiTextDrawEnqueueStatus::StaleGuestRaster:
        return "resident guest raster plan changed";
    }
    return "unknown prepared native UI draw enqueue status";
}

const char* to_string(const ResidentNativeUiTextDrawPhase phase) noexcept {
    switch (phase) {
    case ResidentNativeUiTextDrawPhase::Preflight:
        return "preflight";
    case ResidentNativeUiTextDrawPhase::Acquire:
        return "acquire";
    case ResidentNativeUiTextDrawPhase::Prepare:
        return "prepare";
    case ResidentNativeUiTextDrawPhase::Enqueue:
        return "enqueue";
    case ResidentNativeUiTextDrawPhase::Draw:
        return "draw";
    case ResidentNativeUiTextDrawPhase::Release:
        return "release";
    case ResidentNativeUiTextDrawPhase::Complete:
        return "complete";
    }
    return "unknown resident native UI text draw phase";
}

const char* to_string(const ResidentNativeUiTextDrawStatus status) noexcept {
    switch (status) {
    case ResidentNativeUiTextDrawStatus::DeliveredAndReleased:
        return "delivered and released";
    case ResidentNativeUiTextDrawStatus::StaleGuestPlan:
        return "stale guest plan";
    case ResidentNativeUiTextDrawStatus::AcquireRejected:
        return "acquire rejected";
    case ResidentNativeUiTextDrawStatus::PrepareRejected:
        return "prepare rejected";
    case ResidentNativeUiTextDrawStatus::EnqueueRejected:
        return "enqueue rejected";
    case ResidentNativeUiTextDrawStatus::DrawRejected:
        return "draw rejected";
    case ResidentNativeUiTextDrawStatus::ReleaseRejected:
        return "release rejected";
    }
    return "unknown resident native UI text draw status";
}

PreparedUiTextDrawSubmission::PreparedUiTextDrawSubmission(
    std::shared_ptr<const void> owner_identity, std::shared_ptr<const TextRasterResidencyPin> pin,
    const presentation::GsTextureResidencyToken generic_token,
    std::shared_ptr<presentation::NativeUiTextDrawRequest> request) noexcept
    : owner_identity_(std::move(owner_identity)), pin_(std::move(pin)),
      generic_token_(generic_token), request_(std::move(request)),
      key_(request_->draw.ui.key), bucket_index_(request_->draw.ui.bucket_index),
      node_index_(request_->draw.ui.node_index), logical_(request_->draw.batch.raster),
      raster_generation_(request_->raster_generation) {}

TextRasterResidencyPendingAcquire::TextRasterResidencyPendingAcquire(
    std::shared_ptr<const void> owner_identity, presentation::GsRendererWorker* const worker,
    ResidentBaseRasterResourceGuestPlan guest_plan,
    std::future<presentation::GsTextureResidencyAcquireResult> completion) noexcept
    : owner_identity_(std::move(owner_identity)), worker_(worker), guest_plan_(guest_plan),
      completion_(std::move(completion)) {}

TextRasterResidencyPendingAcquire::~TextRasterResidencyPendingAcquire() noexcept {
    abandon_noexcept();
}

TextRasterResidencyPendingAcquire::TextRasterResidencyPendingAcquire(
    TextRasterResidencyPendingAcquire&& other) noexcept
    : owner_identity_(std::move(other.owner_identity_)),
      worker_(std::exchange(other.worker_, nullptr)), guest_plan_(other.guest_plan_),
      completion_(std::move(other.completion_)) {}

TextRasterResidencyPendingAcquire&
TextRasterResidencyPendingAcquire::operator=(TextRasterResidencyPendingAcquire&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    abandon_noexcept();
    owner_identity_ = std::move(other.owner_identity_);
    worker_ = std::exchange(other.worker_, nullptr);
    guest_plan_ = other.guest_plan_;
    completion_ = std::move(other.completion_);
    return *this;
}

void TextRasterResidencyPendingAcquire::abandon_noexcept() noexcept {
    if (worker_ == nullptr || !completion_.valid()) {
        owner_identity_.reset();
        worker_ = nullptr;
        return;
    }
    try {
        auto result = completion_.get();
        if (result.pin.has_value() && result.pin->token.valid()) {
            (void)worker_->request_texture_residency_release(result.pin->token).get();
        }
    } catch (...) {
    }
    owner_identity_.reset();
    worker_ = nullptr;
}

TextRasterResidencyStore::TextRasterResidencyStore(presentation::GsRendererWorker& worker)
    : worker_(worker), identity_(std::make_shared<std::byte>()) {}

TextRasterResidencyStore::~TextRasterResidencyStore() noexcept {
    while (!entries_.empty()) {
        auto entry = entries_.extract(entries_.begin());
        try {
            (void)worker_.request_texture_residency_release(entry.mapped().generic_token).get();
        } catch (...) {
        }
    }
}

TextRasterResidencyBeginResult
TextRasterResidencyStore::begin(const guest::EeMemory& memory,
                                const ResidentBaseRasterResourceGuestPlan& plan) {
    const auto current = plan_resident_base_raster_resource_from_guest(memory, plan.logical);
    if (!current.has_value() || *current != plan) {
        return {.status = TextRasterResidencyBeginStatus::StaleGuestPlan};
    }
    auto completion =
        worker_.request_texture_residency(guest::GsDrawingContext::Context1, plan.tex0_bits);
    return {.status = TextRasterResidencyBeginStatus::Enqueued,
            .pending = TextRasterResidencyPendingAcquire{identity_, &worker_, plan,
                                                         std::move(completion)}};
}

TextRasterResidencyAcquireResult
TextRasterResidencyStore::finish(const guest::EeMemory& memory,
                                 TextRasterResidencyPendingAcquire pending) {
    if (pending.owner_identity_ != identity_ || pending.worker_ != &worker_ ||
        !pending.completion_.valid()) {
        return {.status = TextRasterResidencyAcquireStatus::WrongStoreOrConsumed};
    }
    auto worker_result = pending.completion_.get();
    pending.owner_identity_.reset();
    pending.worker_ = nullptr;

    GenericResidencyCleanup generic_cleanup{worker_, worker_result.pin.has_value()
                                                         ? worker_result.pin->token
                                                         : presentation::GsTextureResidencyToken{}};

    const auto current =
        plan_resident_base_raster_resource_from_guest(memory, pending.guest_plan_.logical);
    if (!current.has_value() || *current != pending.guest_plan_) {
        generic_cleanup.release();
        return {.status = TextRasterResidencyAcquireStatus::StaleGuestPlan,
                .completed_serial = worker_result.completed_serial};
    }
    if (!worker_result.acquired()) {
        if (worker_result.pin.has_value()) {
            // A pin without a clean acquisition is a malformed worker result.
            generic_cleanup.release();
            return {.status = TextRasterResidencyAcquireStatus::MalformedWorkerResult,
                    .completed_serial = worker_result.completed_serial};
        }
        return {.status = map_worker_acquire_status(worker_result.status),
                .completed_serial = worker_result.completed_serial};
    }

    presentation::GsTextureResidencyPin generic_pin = std::move(*worker_result.pin);

    // Reserve the map slot before minting a token so allocation failure cannot burn one.
    auto [placeholder, inserted] =
        entries_.emplace(0U, Entry{.generic_token = generic_pin.token});
    if (!inserted) {
        generic_cleanup.release();
        throw std::logic_error("concurrent text-raster residency admission is unsupported");
    }

    const std::optional<TextRasterResidencyToken> token = allocate_text_residency_token();
    if (!token.has_value()) {
        entries_.erase(placeholder);
        generic_cleanup.release();
        return {.status = TextRasterResidencyAcquireStatus::TextTokenExhausted,
                .completed_serial = worker_result.completed_serial};
    }

    std::shared_ptr<const TextRasterResidencyPin> public_pin;
    try {
        public_pin = std::make_shared<const TextRasterResidencyPin>(TextRasterResidencyPin{
            .token = *token,
            .logical = pending.guest_plan_.logical,
            .guest_plan = pending.guest_plan_,
            .image = generic_pin.image,
            .acquired_serial = worker_result.completed_serial,
        });
        placeholder->second.pin = public_pin;
    } catch (...) {
        entries_.erase(placeholder);
        throw;
    }

    auto node = entries_.extract(placeholder);
    node.key() = token->value;
    const auto final_entry = entries_.insert(std::move(node));
    if (!final_entry.inserted) {
        generic_cleanup.release();
        throw std::logic_error("process-wide text-raster token was issued more than once");
    }
    generic_cleanup.dismiss();
    return {.status = TextRasterResidencyAcquireStatus::Acquired,
            .pin = std::move(public_pin),
            .completed_serial = worker_result.completed_serial};
}

std::shared_ptr<const TextRasterResidencyPin>
TextRasterResidencyStore::find(const TextRasterResidencyToken token) const noexcept {
    if (token.value == 0U) {
        return {};
    }
    const auto it = entries_.find(token.value);
    if (it == entries_.end()) {
        return {};
    }
    return it->second.pin;
}

TextRasterResidencyReleaseResult
TextRasterResidencyStore::release(const TextRasterResidencyToken token) {
    if (token.value == 0U) {
        return {.status = TextRasterResidencyReleaseStatus::ZeroToken, .token = token};
    }
    const auto it = entries_.find(token.value);
    if (it == entries_.end()) {
        return {.status = TextRasterResidencyReleaseStatus::UnknownOrWrongStore, .token = token};
    }
    const auto generic_token = it->second.generic_token;
    const auto generic_result = worker_.request_texture_residency_release(generic_token).get();
    if (!generic_result.released() || generic_result.token != generic_token) {
        return {.status = TextRasterResidencyReleaseStatus::GenericTokenMissing,
                .token = token,
                .completed_serial = generic_result.completed_serial};
    }
    entries_.erase(it);
    return {.status = TextRasterResidencyReleaseStatus::Released,
            .token = token,
            .completed_serial = generic_result.completed_serial};
}

NativeUiTextDrawPrepareStatus
TextRasterResidencyStore::validate_native_ui_text_draw(const UiTextDrawBatchSubmission& draw,
                                                       const UiTextRasterResolution& raster) const {
    if (raster.residency.value == 0U || raster.key != draw.ui.key ||
        raster.bucket_index != draw.ui.bucket_index || raster.node_index != draw.ui.node_index ||
        raster.logical != draw.batch.raster) {
        return NativeUiTextDrawPrepareStatus::InvalidAssociation;
    }
    const auto it = entries_.find(raster.residency.value);
    if (it == entries_.end()) {
        return NativeUiTextDrawPrepareStatus::UnknownOrWrongStoreResidency;
    }
    const auto& pin = it->second.pin;
    if (pin == nullptr || !it->second.generic_token.valid() ||
        pin->token != raster.residency || pin->logical != raster.logical ||
        pin->guest_plan.logical != raster.logical || pin->image == nullptr ||
        pin->acquired_serial == 0U ||
        !presentation::validated_gs_texture_image_payload_bytes(*pin->image).has_value()) {
        return NativeUiTextDrawPrepareStatus::MalformedResidencyPin;
    }
    return validate_native_ui_text_draw_profile(draw, pin->guest_plan.tex0_bits);
}

TextRasterResidencyStore::SingleNativeUiTextDrawPrepareResult
TextRasterResidencyStore::prepare_native_ui_text_draw(UiTextDrawBatchSubmission draw,
                                                      const UiTextRasterResolution& raster) const {
    if (const auto status = validate_native_ui_text_draw(draw, raster);
        status != NativeUiTextDrawPrepareStatus::Prepared) {
        return {.status = status};
    }
    const auto entry = entries_.find(raster.residency.value);
    presentation::NativeUiTextDrawRequest request{.draw = std::move(draw),
                                                  .raster_generation = raster.residency};
    if (!presentation::native_ui_text_draw_request_retained_bytes(request).has_value()) {
        return {.status = NativeUiTextDrawPrepareStatus::MalformedNativeDraw};
    }
    auto owned_request =
        std::make_shared<presentation::NativeUiTextDrawRequest>(std::move(request));
    return {.status = NativeUiTextDrawPrepareStatus::Prepared,
            .submission = PreparedUiTextDrawSubmission{identity_, entry->second.pin,
                                                       entry->second.generic_token,
                                                       std::move(owned_request)}};
}

NativeUiTextDrawPrepareResult TextRasterResidencyStore::prepare_native_ui_text_draws(
    const ResolvedUiTextDrawBatchesCandidate& candidate) const {
    const auto associated_ui = associate_ui_text_draw_submissions(
        candidate.ui_plan.state, candidate.ui_plan.text_nodes);
    if (!associated_ui.has_value() || associated_ui->size() != candidate.submissions.size()) {
        return {.status = NativeUiTextDrawPrepareStatus::InvalidAssociation,
                .rejected_submission_index = 0U};
    }
    for (std::size_t index = 0U; index < candidate.submissions.size(); ++index) {
        const auto& submission = candidate.submissions[index];
        const auto& planned = candidate.ui_plan.state.submissions[index];
        if ((*associated_ui)[index] != submission.draw.ui ||
            planned.key != submission.draw.ui.key ||
            planned.bucket_index != submission.draw.ui.bucket_index ||
            planned.node_index != submission.draw.ui.node_index ||
            planned.state != submission.draw.ui.state) {
            return rejected_native_ui_text_draw(NativeUiTextDrawPrepareStatus::InvalidAssociation,
                                                index);
        }
        if (const auto status = validate_native_ui_text_draw(submission.draw, submission.raster);
            status != NativeUiTextDrawPrepareStatus::Prepared) {
            return rejected_native_ui_text_draw(status, index);
        }
    }

    PreparedUiTextDrawBatchesCandidate prepared{.ui_plan = candidate.ui_plan};
    prepared.submissions.reserve(candidate.submissions.size());
    for (std::size_t index = 0U; index < candidate.submissions.size(); ++index) {
        const auto& submission = candidate.submissions[index];
        auto single = prepare_native_ui_text_draw(submission.draw, submission.raster);
        if (single.status != NativeUiTextDrawPrepareStatus::Prepared ||
            !single.submission.has_value()) {
            return rejected_native_ui_text_draw(single.status, index);
        }
        prepared.submissions.push_back(std::move(*single.submission));
    }
    return {.status = NativeUiTextDrawPrepareStatus::Prepared, .candidate = std::move(prepared)};
}

std::optional<PreparedNativeUiTextDrawEnqueueStatus>
TextRasterResidencyStore::validate_prepared_native_ui_text_draw(
    const PreparedUiTextDrawSubmission& submission, const UiSubmissionKey expected_key,
    const std::size_t expected_bucket_index, const std::size_t expected_node_index) const {
    if (submission.owner_identity_ != identity_) {
        return PreparedNativeUiTextDrawEnqueueStatus::WrongStoreOrConsumed;
    }
    if (submission.key_ != expected_key || submission.bucket_index_ != expected_bucket_index ||
        submission.node_index_ != expected_node_index) {
        return PreparedNativeUiTextDrawEnqueueStatus::CallbackMismatch;
    }
    const auto it = entries_.find(submission.raster_generation_.value);
    if (it == entries_.end() || it->second.pin != submission.pin_ ||
        it->second.generic_token != submission.generic_token_) {
        return PreparedNativeUiTextDrawEnqueueStatus::ResidencyReleased;
    }
    return std::nullopt;
}

PreparedNativeUiTextDrawEnqueueResult
TextRasterResidencyStore::enqueue_native_ui_text_draw_from_guest(
    PreparedUiTextDrawSubmission& submission, const UiSubmissionKey expected_key,
    const std::size_t expected_bucket_index, const std::size_t expected_node_index,
    const guest::R5900State& cpu, const guest::EeMemory& memory) {
    const auto reject = [&](const PreparedNativeUiTextDrawEnqueueStatus status) {
        return PreparedNativeUiTextDrawEnqueueResult{
            .status = status, .raster_generation = submission.raster_generation_};
    };
    if (const auto rejected = validate_prepared_native_ui_text_draw(
            submission, expected_key, expected_bucket_index, expected_node_index);
        rejected.has_value()) {
        return reject(*rejected);
    }
    const auto current = capture_ui_text_callback_values_from_guest(cpu, memory);
    if (!current.has_value()) {
        return reject(PreparedNativeUiTextDrawEnqueueStatus::CallbackEntryRejected);
    }
    const UiTextDrawBatchSubmission& prepared = submission.request_->draw;
    if (static_cast<std::uint64_t>(current->entry.descriptor) != prepared.ui.key.value ||
        current->render != prepared.ui.render || current->effect != prepared.ui.effect ||
        current->batch != prepared.batch || current->primitive != prepared.primitive) {
        return reject(PreparedNativeUiTextDrawEnqueueStatus::CallbackPayloadMismatch);
    }
    const auto active_target = capture_active_ui_render_target_from_guest(memory);
    if (!active_target.has_value()) {
        return reject(PreparedNativeUiTextDrawEnqueueStatus::TargetSourceRejected);
    }
    if (active_target->view != prepared.ui.state.target) {
        return reject(PreparedNativeUiTextDrawEnqueueStatus::TargetSourceMismatch);
    }
    const auto raster = plan_resident_base_raster_resource_from_guest(memory, submission.logical_);
    if (!raster.has_value() || *raster != submission.pin_->guest_plan) {
        return reject(PreparedNativeUiTextDrawEnqueueStatus::StaleGuestRaster);
    }
    return enqueue_native_ui_text_draw(submission, expected_key, expected_bucket_index,
                                       expected_node_index, current->conversion);
}

PreparedNativeUiTextDrawEnqueueResult TextRasterResidencyStore::enqueue_native_ui_text_draw(
    PreparedUiTextDrawSubmission& submission, const UiSubmissionKey expected_key,
    const std::size_t expected_bucket_index, const std::size_t expected_node_index,
    const std::optional<presentation::NativeUiTextConversionInput> conversion) {
    const TextRasterResidencyToken raster_generation = submission.raster_generation_;
    if (const auto rejected = validate_prepared_native_ui_text_draw(
            submission, expected_key, expected_bucket_index, expected_node_index);
        rejected.has_value()) {
        return {.status = *rejected, .raster_generation = raster_generation};
    }
    const auto entry = entries_.find(raster_generation.value);
    // The request is shared with the worker only after admission; restore the previous
    // conversion if the worker rejects the command so the prepared draw stays retryable.
    const auto previous_conversion = submission.request_->conversion;
    submission.request_->conversion = conversion;
    auto completion = [&]() {
        try {
            return worker_.request_prepared_native_ui_text_draw(
                entry->second.generic_token, entry->second.pin->guest_plan.tex0_bits,
                submission.request_);
        } catch (...) {
            submission.request_->conversion = previous_conversion;
            throw;
        }
    }();
    submission.owner_identity_.reset();
    submission.pin_.reset();
    submission.generic_token_ = {};
    submission.request_.reset();
    return {.status = PreparedNativeUiTextDrawEnqueueStatus::Enqueued,
            .raster_generation = raster_generation,
            .completion = std::move(completion)};
}

void TextRasterResidencyStore::cleanup_transaction_residency_noexcept(
    const TextRasterResidencyToken token) noexcept {
    const auto entry = entries_.find(token.value);
    if (entry == entries_.end()) {
        return;
    }
    try {
        const auto generic_token = entry->second.generic_token;
        (void)worker_.request_texture_residency_release(generic_token).get();
        entries_.erase(token.value);
    } catch (...) {
        if (worker_.failed()) {
            entries_.erase(token.value);
        }
    }
}

ResidentNativeUiTextDrawResult TextRasterResidencyStore::submit_resident_native_ui_text_draw(
    const guest::EeMemory& memory, const ResidentBaseRasterResourceGuestPlan& raster_plan,
    UiTextDrawBatchSubmission draw) {
    ResidentNativeUiTextDrawResult result{};

    const auto current = plan_resident_base_raster_resource_from_guest(memory, raster_plan.logical);
    if (!current.has_value() || *current != raster_plan) {
        result.status = ResidentNativeUiTextDrawStatus::StaleGuestPlan;
        return result;
    }
    if (draw.batch.raster != raster_plan.logical) {
        result.prepare_status = NativeUiTextDrawPrepareStatus::InvalidAssociation;
        return result;
    }
    if (const auto profile = validate_native_ui_text_draw_profile(draw, raster_plan.tex0_bits);
        profile != NativeUiTextDrawPrepareStatus::Prepared) {
        result.prepare_status = profile;
        return result;
    }

    auto begun = begin(memory, raster_plan);
    result.begin_status = begun.status;
    if (!begun.enqueued()) {
        result.status = ResidentNativeUiTextDrawStatus::StaleGuestPlan;
        return result;
    }

    auto acquired = finish(memory, std::move(*begun.pending));
    result.acquire_status = acquired.status;
    result.acquire_serial = acquired.completed_serial;
    if (!acquired.acquired()) {
        result.phase = ResidentNativeUiTextDrawPhase::Acquire;
        result.status = acquired.status == TextRasterResidencyAcquireStatus::StaleGuestPlan
                            ? ResidentNativeUiTextDrawStatus::StaleGuestPlan
                            : ResidentNativeUiTextDrawStatus::AcquireRejected;
        return result;
    }
    result.raster_generation = acquired.pin->token;

    try {
        const UiTextRasterResolution raster{.key = draw.ui.key,
                                            .bucket_index = draw.ui.bucket_index,
                                            .node_index = draw.ui.node_index,
                                            .logical = raster_plan.logical,
                                            .residency = result.raster_generation};
        auto prepared = prepare_native_ui_text_draw(std::move(draw), raster);
        result.prepare_status = prepared.status;
        if (prepared.status != NativeUiTextDrawPrepareStatus::Prepared ||
            !prepared.submission.has_value()) {
            result.phase = ResidentNativeUiTextDrawPhase::Prepare;
            const auto released = release(result.raster_generation);
            result.release_status = released.status;
            result.release_serial = released.completed_serial;
            if (!released.released()) {
                result.phase = ResidentNativeUiTextDrawPhase::Release;
                result.status = ResidentNativeUiTextDrawStatus::ReleaseRejected;
                cleanup_transaction_residency_noexcept(result.raster_generation);
            }
            return result;
        }

        auto enqueued = enqueue_native_ui_text_draw(
            *prepared.submission, prepared.submission->key(), prepared.submission->bucket_index(),
            prepared.submission->node_index());
        result.enqueue_status = enqueued.status;
        if (!enqueued.enqueued()) {
            result.phase = ResidentNativeUiTextDrawPhase::Enqueue;
            result.status = ResidentNativeUiTextDrawStatus::EnqueueRejected;
            const auto released = release(result.raster_generation);
            result.release_status = released.status;
            result.release_serial = released.completed_serial;
            if (!released.released()) {
                result.phase = ResidentNativeUiTextDrawPhase::Release;
                result.status = ResidentNativeUiTextDrawStatus::ReleaseRejected;
                cleanup_transaction_residency_noexcept(result.raster_generation);
            }
            return result;
        }

        // The draw is already in the primary FIFO, so releasing now orders it after the draw.
        auto draw_completion = std::move(enqueued.completion);
        const auto released = release(result.raster_generation);
        result.release_status = released.status;
        result.release_serial = released.completed_serial;
        const auto drawn = draw_completion.get();
        result.draw_status = drawn.status;
        result.draw_serial = drawn.completed_serial;
        if (!released.released()) {
            result.phase = ResidentNativeUiTextDrawPhase::Release;
            result.status = ResidentNativeUiTextDrawStatus::ReleaseRejected;
            cleanup_transaction_residency_noexcept(result.raster_generation);
        } else if (drawn.status != presentation::NativeUiTextDrawStatus::Delivered) {
            result.phase = ResidentNativeUiTextDrawPhase::Draw;
            result.status = ResidentNativeUiTextDrawStatus::DrawRejected;
        } else {
            result.phase = ResidentNativeUiTextDrawPhase::Complete;
            result.status = ResidentNativeUiTextDrawStatus::DeliveredAndReleased;
        }
        return result;
    } catch (...) {
        cleanup_transaction_residency_noexcept(result.raster_generation);
        throw;
    }
}

std::size_t TextRasterResidencyStore::size() const noexcept { return entries_.size(); }

} // namespace takedown::runtime::slus_21050
