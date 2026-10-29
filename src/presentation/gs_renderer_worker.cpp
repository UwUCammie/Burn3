#include "takedown/presentation/gs_renderer_worker.hpp"

#include "takedown/platform/host_wait_stats.hpp"
#include "takedown/presentation/native_ui_rectangle_batch.hpp"
#include "takedown/presentation/native_ui_text_draw.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace takedown::presentation {

namespace {

struct GifTagBeginCommand {
    guest::GifPath path{guest::GifPath::Path1};
};

struct GsStateResetCommand {};

struct ResetCommand {};

struct AcquireTextureResidencyCommand {
    guest::GsDrawingContext context{};
    std::uint64_t expected_tex0{};
    std::shared_ptr<std::promise<GsTextureResidencyAcquireResult>> promise{};
};

struct ReleaseTextureResidencyCommand {
    GsTextureResidencyToken token{};
    std::shared_ptr<std::promise<GsTextureResidencyReleaseResult>> promise{};
};

struct NativeUiTextDrawCommand {
    // Public submissions carry a complete worker-owned draw. Prepared
    // submissions name a registry token that resolves at this FIFO position.
    std::shared_ptr<const NativeUiTextDraw> draw{};
    std::optional<GsTextureResidencyToken> residency{};
    std::shared_ptr<NativeUiTextDrawRequest> request{};
    std::uint64_t expected_tex0{};
    std::shared_ptr<std::promise<NativeUiTextDrawResult>> promise{};
};

struct SynchronizeCommand {
    std::optional<GsScanoutPlan> scanout_plan{};
    std::shared_ptr<std::promise<GsRendererWorkerSnapshot>> promise{};
};

struct FrameCompletionCommand {
    std::uint64_t frame_id{};
    std::shared_ptr<std::promise<GsRendererWorkerFrameCompletionResult>> promise{};
};

// The alternative index equals GsRendererWorkerCommandKind for every command.
using CommandPayload =
    std::variant<guest::GsRegisterWrite, guest::GsImageTransfer, GifTagBeginCommand,
                 GsStateResetCommand, ResetCommand, AcquireTextureResidencyCommand,
                 ReleaseTextureResidencyCommand, NativeUiTextDrawCommand, SynchronizeCommand,
                 FrameCompletionCommand>;

struct QueuedCommand {
    std::uint64_t serial{};
    std::uint64_t image_bytes{};
    std::uint64_t native_ui_bytes{};
    CommandPayload payload{};
};

struct PresentSnapshotRequest {
    GsScanoutPlan scanout_plan{};
    std::shared_ptr<std::promise<GsRendererWorkerSnapshot>> promise{};
};

struct ScanoutJob {
    GsRendererWorkerSnapshot snapshot{};
    GsScanoutPlan plan{};
    guest::GsLocalMemory memory{};
    std::shared_ptr<std::promise<GsRendererWorkerSnapshot>> promise{};
};

template <typename T>
void deliver_exception(const std::shared_ptr<std::promise<T>>& promise,
                       const std::exception_ptr& error) noexcept {
    if (!promise) {
        return;
    }
    try {
        promise->set_exception(error);
    } catch (const std::future_error&) {
        // The promise was already satisfied; its original result stays authoritative.
    }
}

void deliver_command_exception(CommandPayload& payload, const std::exception_ptr& error) noexcept {
    if (auto* command = std::get_if<AcquireTextureResidencyCommand>(&payload)) {
        deliver_exception(command->promise, error);
    } else if (auto* command = std::get_if<ReleaseTextureResidencyCommand>(&payload)) {
        deliver_exception(command->promise, error);
    } else if (auto* command = std::get_if<NativeUiTextDrawCommand>(&payload)) {
        deliver_exception(command->promise, error);
    } else if (auto* command = std::get_if<SynchronizeCommand>(&payload)) {
        deliver_exception(command->promise, error);
    } else if (auto* command = std::get_if<FrameCompletionCommand>(&payload)) {
        deliver_exception(command->promise, error);
    }
}

[[nodiscard]] bool byte_class_fits(const std::size_t queued, const std::size_t requested,
                                   const std::size_t limit) noexcept {
    if (requested == 0U) {
        return true;
    }
    // An oversized payload is admitted only as the sole payload of its class.
    if (requested > limit) {
        return queued == 0U;
    }
    return queued <= limit && requested <= limit - queued;
}

[[nodiscard]] std::uint64_t next_worker_instance_id() {
    static std::atomic<std::uint64_t> counter{0U};
    const std::uint64_t value = counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (value == 0U) {
        throw std::runtime_error("GS renderer worker instance identity exhausted");
    }
    return value;
}

// Owns one dedicated thread that converts immutable local-memory snapshots.
// The GS worker keeps executing later commands while a conversion is pending.
class ScanoutWorker {
  public:
    ScanoutWorker(const std::uint64_t maximum_queued_scanouts,
                  std::function<void(std::uint64_t)> dispatch_hook)
        : maximum_queued_scanouts_(maximum_queued_scanouts),
          dispatch_hook_(std::move(dispatch_hook)) {
        if (maximum_queued_scanouts_ == 0U) {
            throw std::invalid_argument("GS scanout worker queue must hold at least one snapshot");
        }
        worker_ = std::thread([this] { run(); });
    }

    ~ScanoutWorker() {
        {
            std::lock_guard lock{mutex_};
            shutting_down_ = true;
        }
        work_available_.notify_all();
        queue_space_available_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ScanoutWorker(const ScanoutWorker&) = delete;
    ScanoutWorker& operator=(const ScanoutWorker&) = delete;
    ScanoutWorker(ScanoutWorker&&) = delete;
    ScanoutWorker& operator=(ScanoutWorker&&) = delete;

    void enqueue(ScanoutJob job) {
        std::unique_lock lock{mutex_};
        queue_space_available_.wait(lock, [this] {
            return shutting_down_ || queue_.size() < maximum_queued_scanouts_;
        });
        if (shutting_down_) {
            throw std::logic_error("GS scanout worker is shutting down");
        }
        queue_.push_back(std::move(job));
        work_available_.notify_one();
    }

  private:
    void run() {
        for (;;) {
            ScanoutJob job;
            {
                std::unique_lock lock{mutex_};
                work_available_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            queue_space_available_.notify_one();
            try {
                if (dispatch_hook_) {
                    dispatch_hook_(job.snapshot.completed_serial);
                }
                job.snapshot.scanout_frame = render_gs_scanout(job.memory, job.plan);
                job.snapshot.local_memory = std::move(job.memory);
                job.promise->set_value(std::move(job.snapshot));
            } catch (...) {
                deliver_exception(job.promise, std::current_exception());
            }
        }
    }

    std::uint64_t maximum_queued_scanouts_{};
    std::function<void(std::uint64_t)> dispatch_hook_{};
    std::mutex mutex_{};
    std::condition_variable work_available_{};
    std::condition_variable queue_space_available_{};
    std::deque<ScanoutJob> queue_{};
    bool shutting_down_{};
    std::thread worker_{};
};

} // namespace

class GsRendererWorker::Impl {
  public:
    explicit Impl(GsRendererWorkerConfig config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] PrimaryBatchAdmission enqueue(CommandPayload payload,
                                                std::size_t image_bytes,
                                                std::size_t native_ui_bytes, bool primary);
    void enqueue_present(PresentSnapshotRequest request);
    [[nodiscard]] std::future<NativeUiTextDrawResult>
    request_native_ui_text_draw(const NativeUiTextDraw& draw);
    [[nodiscard]] GsRendererWorkerQueueStatistics queue_statistics() const;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::uint64_t worker_instance_id() const noexcept {
        return worker_instance_id_;
    }

  private:
    void run();
    void execute(QueuedCommand& command);
    void execute(PresentSnapshotRequest& request);
    void execute_acquire(AcquireTextureResidencyCommand& command, std::uint64_t serial);
    void execute_release(ReleaseTextureResidencyCommand& command, std::uint64_t serial);
    void execute_native_ui_text_draw(NativeUiTextDrawCommand& command, std::uint64_t serial);
    void execute_synchronize(SynchronizeCommand& command, std::uint64_t serial);
    void execute_frame_completion(FrameCompletionCommand& command, std::uint64_t serial);
    [[nodiscard]] GsRendererWorkerSnapshot capture_snapshot(std::uint64_t serial,
                                                            std::optional<GsScanoutPlan> plan);
    void publish_snapshot(GsRendererWorkerSnapshot snapshot,
                          const std::shared_ptr<std::promise<GsRendererWorkerSnapshot>>& promise);
    void poison(std::exception_ptr error, QueuedCommand* command,
                PresentSnapshotRequest* request) noexcept;
    [[nodiscard]] bool can_admit_locked(std::size_t image_bytes,
                                        std::size_t native_ui_bytes) const noexcept;

    GsRendererWorkerConfig config_{};
    std::uint64_t worker_instance_id_{};
    guest::GsLiveRenderer renderer_{};
    GsTextureResidencyRegistry texture_residency_{};
    std::unique_ptr<ScanoutWorker> scanout_worker_{};
    std::deque<QueuedCommand> queue_{};
    std::deque<PresentSnapshotRequest> present_requests_{};
    std::size_t maximum_present_requests_{};
    mutable std::mutex mutex_{};
    std::condition_variable work_available_{};
    std::condition_variable queue_space_available_{};
    std::condition_variable present_queue_space_available_{};
    GsRendererWorkerQueueStatistics queue_statistics_{};
    std::uint64_t next_serial_{1U};
    std::uint64_t completed_serial_{};
    std::exception_ptr worker_error_{};
    bool shutting_down_{};
    bool previous_item_was_present_{};
    std::thread worker_{};
};

GsRendererWorker::Impl::Impl(GsRendererWorkerConfig config)
    : config_(std::move(config)), worker_instance_id_(next_worker_instance_id()),
      texture_residency_(config_.texture_residency),
      scanout_worker_(std::make_unique<ScanoutWorker>(config_.maximum_queued_scanouts,
                                                      config_.scanout_dispatch_hook)),
      maximum_present_requests_(config_.maximum_queued_present_requests),
      worker_([this] { run(); }) {
    if (config_.maximum_queued_commands == 0U) {
        throw std::invalid_argument("GS renderer worker queue must hold at least one command");
    }
    if (config_.maximum_queued_image_bytes == 0U) {
        throw std::invalid_argument("GS renderer worker IMAGE byte limit must be greater than zero");
    }
    if (config_.maximum_queued_native_ui_bytes == 0U) {
        throw std::invalid_argument(
            "GS renderer worker native UI byte limit must be greater than zero");
    }
    if (maximum_present_requests_ == 0U) {
        throw std::invalid_argument(
            "GS renderer worker presentation mailbox must hold at least one request");
    }
    if (config_.native_ui_text_draw_sink && config_.native_ui_rectangle_batch_sink) {
        throw std::invalid_argument("native UI delivery requires exactly one sink type");
    }
}

GsRendererWorker::Impl::~Impl() {
    {
        std::lock_guard lock{mutex_};
        shutting_down_ = true;
    }
    work_available_.notify_all();
    queue_space_available_.notify_all();
    present_queue_space_available_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    scanout_worker_.reset();
}

bool GsRendererWorker::Impl::can_admit_locked(const std::size_t image_bytes,
                                              const std::size_t native_ui_bytes) const noexcept {
    if (queue_.size() >= config_.maximum_queued_commands) {
        return false;
    }
    return byte_class_fits(queue_statistics_.queued_image_bytes, image_bytes,
                           config_.maximum_queued_image_bytes) &&
           byte_class_fits(queue_statistics_.queued_native_ui_bytes, native_ui_bytes,
                           config_.maximum_queued_native_ui_bytes);
}

GsRendererWorker::PrimaryBatchAdmission
GsRendererWorker::Impl::enqueue(CommandPayload payload, const std::size_t image_bytes,
                                const std::size_t native_ui_bytes, const bool primary) {
    std::unique_lock lock{mutex_};
    for (;;) {
        if (worker_error_) {
            std::rethrow_exception(worker_error_);
        }
        if (shutting_down_) {
            throw std::logic_error("GS renderer worker is shutting down");
        }
        if (can_admit_locked(image_bytes, native_ui_bytes)) {
            break;
        }
        ++queue_statistics_.producer_waits;
        platform::ScopedHostWait wait{platform::HostWaitSite::GsWorkerQueue};
        queue_space_available_.wait(lock);
    }

    PrimaryBatchAdmission admission{};
    if (next_serial_ == 0U) {
        throw std::overflow_error("GS renderer worker command serial exhausted");
    }
    QueuedCommand command{};
    command.serial = next_serial_++;
    command.image_bytes = image_bytes;
    command.native_ui_bytes = native_ui_bytes;

    if (primary) {
        std::uint64_t digest = 0U;
        if (const auto* write = std::get_if<guest::GsRegisterWrite>(&payload)) {
            digest = runtime::digest_slus_21050_femain_primary_register_write(*write);
        } else if (const auto* transfer = std::get_if<guest::GsImageTransfer>(&payload)) {
            digest = runtime::digest_slus_21050_femain_primary_image_transfer(*transfer);
        } else if (const auto* tag = std::get_if<GifTagBeginCommand>(&payload)) {
            digest = runtime::digest_slus_21050_femain_primary_gif_tag(tag->path);
        }
        admission.worker_instance_id = worker_instance_id_;
        admission.serial = command.serial;
        admission.kind = static_cast<GsRendererWorkerCommandKind>(payload.index());
        admission.payload_digest = digest;
    }
    command.payload = std::move(payload);

    queue_.push_back(std::move(command));
    ++queue_statistics_.submitted_commands;
    queue_statistics_.queued_commands = queue_.size();
    queue_statistics_.queued_image_bytes += image_bytes;
    queue_statistics_.queued_native_ui_bytes += native_ui_bytes;
    queue_statistics_.peak_queued_commands =
        std::max(queue_statistics_.peak_queued_commands, queue_statistics_.queued_commands);
    queue_statistics_.peak_queued_image_bytes =
        std::max(queue_statistics_.peak_queued_image_bytes, queue_statistics_.queued_image_bytes);
    queue_statistics_.peak_queued_native_ui_bytes = std::max(
        queue_statistics_.peak_queued_native_ui_bytes, queue_statistics_.queued_native_ui_bytes);
    work_available_.notify_one();
    return admission;
}

void GsRendererWorker::Impl::enqueue_present(PresentSnapshotRequest request) {
    std::unique_lock lock{mutex_};
    for (;;) {
        if (worker_error_) {
            std::rethrow_exception(worker_error_);
        }
        if (shutting_down_) {
            throw std::logic_error("GS renderer worker is shutting down");
        }
        if (present_requests_.size() < maximum_present_requests_) {
            break;
        }
        platform::ScopedHostWait wait{platform::HostWaitSite::GsWorkerSnapshot};
        present_queue_space_available_.wait(lock);
    }
    present_requests_.push_back(std::move(request));
    work_available_.notify_one();
}

std::future<NativeUiTextDrawResult>
GsRendererWorker::Impl::request_native_ui_text_draw(const NativeUiTextDraw& draw) {
    auto promise = std::make_shared<std::promise<NativeUiTextDrawResult>>();
    auto future = promise->get_future();

    NativeUiTextDrawResult immediate{};
    immediate.raster_generation = draw.raster_generation.value;
    if (!draw.raster_image) {
        immediate.status = NativeUiTextDrawStatus::MalformedDraw;
        promise->set_value(std::move(immediate));
        return future;
    }
    if (!config_.native_ui_text_draw_sink && !config_.native_ui_rectangle_batch_sink) {
        immediate.status = NativeUiTextDrawStatus::MissingSink;
        promise->set_value(std::move(immediate));
        return future;
    }
    const auto retained = native_ui_text_draw_retained_bytes(draw);
    if (!retained) {
        immediate.status = NativeUiTextDrawStatus::MalformedDraw;
        promise->set_value(std::move(immediate));
        return future;
    }

    // The canonical image is copied into worker-owned immutable storage so a
    // caller alias cannot change content or the byte charge after enqueue.
    auto owned = std::make_shared<NativeUiTextDraw>(draw);
    owned->raster_image = std::make_shared<const guest::GsTextureImageSnapshot>(*draw.raster_image);

    NativeUiTextDrawCommand command{};
    command.draw = std::move(owned);
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(enqueue(std::move(payload), 0U, *retained, false));
    return future;
}

GsRendererWorkerQueueStatistics GsRendererWorker::Impl::queue_statistics() const {
    std::lock_guard lock{mutex_};
    return queue_statistics_;
}

bool GsRendererWorker::Impl::failed() const noexcept {
    return worker_error_ != nullptr;
}

void GsRendererWorker::Impl::run() {
    for (;;) {
        QueuedCommand command{};
        PresentSnapshotRequest request{};
        bool is_present = false;
        {
            std::unique_lock lock{mutex_};
            work_available_.wait(lock, [this] {
                return shutting_down_ || !queue_.empty() || !present_requests_.empty();
            });
            if (queue_.empty() && present_requests_.empty()) {
                return;
            }
            // Present requests are serviced fairly between normal FIFO commands.
            is_present = !present_requests_.empty() &&
                         (queue_.empty() || !previous_item_was_present_);
            previous_item_was_present_ = is_present;
            if (is_present) {
                request = std::move(present_requests_.front());
                present_requests_.pop_front();
            } else {
                command = std::move(queue_.front());
                queue_.pop_front();
                queue_statistics_.queued_commands = queue_.size();
                queue_statistics_.queued_image_bytes -= command.image_bytes;
                queue_statistics_.queued_native_ui_bytes -= command.native_ui_bytes;
            }
        }
        if (is_present) {
            present_queue_space_available_.notify_one();
        } else {
            queue_space_available_.notify_one();
        }

        try {
            if (is_present) {
                execute(request);
            } else {
                execute(command);
            }
        } catch (...) {
            poison(std::current_exception(), is_present ? nullptr : &command,
                   is_present ? &request : nullptr);
            return;
        }
    }
}

void GsRendererWorker::Impl::poison(std::exception_ptr error, QueuedCommand* command,
                                    PresentSnapshotRequest* request) noexcept {
    std::deque<QueuedCommand> pending_commands;
    std::deque<PresentSnapshotRequest> pending_presents;
    {
        std::lock_guard lock{mutex_};
        if (!worker_error_) {
            worker_error_ = error;
        }
        pending_commands.swap(queue_);
        pending_presents.swap(present_requests_);
        queue_statistics_.queued_commands = 0U;
        queue_statistics_.queued_image_bytes = 0U;
        queue_statistics_.queued_native_ui_bytes = 0U;
    }
    if (command != nullptr) {
        deliver_command_exception(command->payload, error);
    }
    if (request != nullptr) {
        deliver_exception(request->promise, error);
    }
    for (auto& pending : pending_commands) {
        deliver_command_exception(pending.payload, error);
    }
    for (auto& pending : pending_presents) {
        deliver_exception(pending.promise, error);
    }
    queue_space_available_.notify_all();
    present_queue_space_available_.notify_all();
    work_available_.notify_all();
}

void GsRendererWorker::Impl::execute(QueuedCommand& command) {
    if (config_.dispatch_hook) {
        config_.dispatch_hook(static_cast<GsRendererWorkerCommandKind>(command.payload.index()),
                              command.serial);
    }
    switch (command.payload.index()) {
    case 0:
        renderer_.on_gs_register_write(std::get<guest::GsRegisterWrite>(command.payload));
        break;
    case 1:
        renderer_.on_gs_image_transfer(std::get<guest::GsImageTransfer>(command.payload));
        break;
    case 2:
        renderer_.on_gs_gif_tag_begin(std::get<GifTagBeginCommand>(command.payload).path);
        break;
    case 3:
        renderer_.on_gs_reset();
        break;
    case 4:
        renderer_.reset();
        break;
    case 5:
        execute_acquire(std::get<AcquireTextureResidencyCommand>(command.payload), command.serial);
        break;
    case 6:
        execute_release(std::get<ReleaseTextureResidencyCommand>(command.payload), command.serial);
        break;
    case 7:
        execute_native_ui_text_draw(std::get<NativeUiTextDrawCommand>(command.payload),
                                    command.serial);
        break;
    case 8:
        execute_synchronize(std::get<SynchronizeCommand>(command.payload), command.serial);
        break;
    case 9:
        execute_frame_completion(std::get<FrameCompletionCommand>(command.payload), command.serial);
        break;
    default:
        break;
    }

    std::lock_guard lock{mutex_};
    completed_serial_ = command.serial;
    ++queue_statistics_.completed_commands;
}

void GsRendererWorker::Impl::execute(PresentSnapshotRequest& request) {
    std::uint64_t serial = 0U;
    {
        std::lock_guard lock{mutex_};
        serial = completed_serial_;
    }
    publish_snapshot(capture_snapshot(serial, request.scanout_plan), request.promise);
}

void GsRendererWorker::Impl::execute_acquire(AcquireTextureResidencyCommand& command,
                                             const std::uint64_t serial) {
    GsTextureResidencyAcquireResult result{};
    result.completed_serial = serial;
    const auto tex0 = renderer_.current_tex0(command.context);
    if (!tex0) {
        result.status = GsTextureResidencyAcquireStatus::MissingTex0;
    } else if (*tex0 != command.expected_tex0) {
        result.status = GsTextureResidencyAcquireStatus::Tex0Mismatch;
    } else {
        switch (renderer_.classify_texture_snapshot_source(*tex0)) {
        case guest::GsTextureSnapshotSourceStatus::CurrentLocalMemory: {
            auto capture = renderer_.capture_texture_image_for_residency(*tex0);
            if (!capture) {
                throw std::logic_error(
                    "supported GS texture snapshot failed against authoritative local memory");
            }
            const auto token = texture_residency_.acquire(std::move(*capture));
            if (!token) {
                result.status = GsTextureResidencyAcquireStatus::RegistryRejected;
            } else {
                result.pin = GsTextureResidencyPin{*token, texture_residency_.find(*token)};
                result.status = GsTextureResidencyAcquireStatus::Acquired;
            }
            break;
        }
        case guest::GsTextureSnapshotSourceStatus::UnsupportedTexture:
            result.status = GsTextureResidencyAcquireStatus::UnsupportedTexture;
            break;
        case guest::GsTextureSnapshotSourceStatus::FeedbackCacheConflict:
            result.status = GsTextureResidencyAcquireStatus::FeedbackCacheConflict;
            break;
        }
    }
    command.promise->set_value(std::move(result));
}

void GsRendererWorker::Impl::execute_release(ReleaseTextureResidencyCommand& command,
                                             const std::uint64_t serial) {
    GsTextureResidencyReleaseResult result{};
    result.token = command.token;
    result.completed_serial = serial;
    result.status = texture_residency_.release(command.token)
                        ? GsTextureResidencyReleaseStatus::Released
                        : GsTextureResidencyReleaseStatus::UnknownToken;
    command.promise->set_value(std::move(result));
}

void GsRendererWorker::Impl::execute_native_ui_text_draw(NativeUiTextDrawCommand& command,
                                                         const std::uint64_t serial) {
    NativeUiTextDrawResult result{};
    result.completed_serial = serial;

    std::shared_ptr<const NativeUiTextDraw> draw = command.draw;
    if (!draw) {
        if (!command.residency || !command.request) {
            result.status = NativeUiTextDrawStatus::MalformedDraw;
            command.promise->set_value(std::move(result));
            return;
        }
        auto image = texture_residency_.find(*command.residency);
        if (!image) {
            result.status = NativeUiTextDrawStatus::MissingResidency;
            command.promise->set_value(std::move(result));
            return;
        }
        auto built = std::make_shared<NativeUiTextDraw>();
        static_cast<NativeUiTextDrawRequest&>(*built) = *command.request;
        built->raster_image = std::move(image);
        draw = std::move(built);
    }
    result.raster_generation = draw->raster_generation.value;

    if (config_.native_ui_text_draw_sink) {
        config_.native_ui_text_draw_sink->on_native_ui_text_draw(serial, draw);
        result.status = NativeUiTextDrawStatus::Delivered;
    } else if (config_.native_ui_rectangle_batch_sink) {
        if (!draw->conversion) {
            result.status = NativeUiTextDrawStatus::MissingConversion;
        } else {
            auto batch = compile_native_ui_text_draw(*draw, *draw->conversion,
                                                     config_.native_ui_rectangle_compile_limits);
            if (!batch) {
                result.status = NativeUiTextDrawStatus::UnsupportedRectangleBatch;
            } else {
                config_.native_ui_rectangle_batch_sink->on_native_ui_rectangle_batch(
                    serial, std::make_shared<const NativeUiRectangleBatch>(std::move(*batch)));
                result.status = NativeUiTextDrawStatus::Delivered;
            }
        }
    } else {
        result.status = NativeUiTextDrawStatus::MissingSink;
    }
    command.promise->set_value(std::move(result));
}

void GsRendererWorker::Impl::execute_synchronize(SynchronizeCommand& command,
                                                 const std::uint64_t serial) {
    publish_snapshot(capture_snapshot(serial, command.scanout_plan), command.promise);
}

void GsRendererWorker::Impl::execute_frame_completion(FrameCompletionCommand& command,
                                                      const std::uint64_t serial) {
    GsRendererWorkerFrameCompletionResult result{};
    result.frame_id = command.frame_id;
    result.completed_serial = serial;
    command.promise->set_value(std::move(result));
}

GsRendererWorkerSnapshot GsRendererWorker::Impl::capture_snapshot(
    const std::uint64_t serial, std::optional<GsScanoutPlan> plan) {
    GsRendererWorkerSnapshot snapshot{};
    snapshot.statistics = renderer_.statistics();
    snapshot.queue_statistics = queue_statistics();
    snapshot.completed_serial = serial;
    snapshot.scanout_plan = std::move(plan);
    return snapshot;
}

void GsRendererWorker::Impl::publish_snapshot(
    GsRendererWorkerSnapshot snapshot,
    const std::shared_ptr<std::promise<GsRendererWorkerSnapshot>>& promise) {
    if (!snapshot.scanout_plan) {
        promise->set_value(std::move(snapshot));
        return;
    }
    if (!snapshot.scanout_plan->ready()) {
        // An invalid plan is answered on this thread with its failure status.
        snapshot.scanout_frame = render_gs_scanout(renderer_.local_memory(), *snapshot.scanout_plan);
        promise->set_value(std::move(snapshot));
        return;
    }
    ScanoutJob job{};
    job.plan = *snapshot.scanout_plan;
    job.memory = renderer_.local_memory();
    job.snapshot = std::move(snapshot);
    job.promise = promise;
    scanout_worker_->enqueue(std::move(job));
}

GsRendererWorker::GsRendererWorker(GsRendererWorkerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

GsRendererWorker::~GsRendererWorker() {
    lifetime_token_.reset();
    impl_.reset();
}

void GsRendererWorker::on_gs_gif_tag_begin(guest::GifPath path) {
    CommandPayload payload{GifTagBeginCommand{path}};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
}

void GsRendererWorker::on_gs_reset() {
    CommandPayload payload{GsStateResetCommand{}};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
}

void GsRendererWorker::on_gs_register_write(const guest::GsRegisterWrite& write) {
    CommandPayload payload{write};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
}

void GsRendererWorker::on_gs_image_transfer(const guest::GsImageTransfer& transfer) {
    CommandPayload payload{transfer};
    static_cast<void>(impl_->enqueue(std::move(payload), transfer.pixels.size(), 0U, false));
}

void GsRendererWorker::on_gs_image_transfer(guest::GsImageTransfer&& transfer) {
    const std::size_t bytes = transfer.pixels.size();
    CommandPayload payload{std::move(transfer)};
    static_cast<void>(impl_->enqueue(std::move(payload), bytes, 0U, false));
}

std::unique_ptr<GsRendererWorker::PrimaryBatchSubmission>
GsRendererWorker::begin_primary_batch(guest::GsCapture& capture, guest::GifPath expected_path) {
    return std::unique_ptr<PrimaryBatchSubmission>(
        new PrimaryBatchSubmission(*this, capture, expected_path));
}

std::uint64_t GsRendererWorker::worker_instance_id() const noexcept {
    return impl_ ? impl_->worker_instance_id() : 0U;
}

void GsRendererWorker::reset() {
    CommandPayload payload{ResetCommand{}};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
}

GsRendererWorkerSnapshot GsRendererWorker::synchronize(std::optional<GsScanoutPlan> scanout_plan) {
    auto future = request_snapshot(std::move(scanout_plan));
    platform::ScopedHostWait wait{platform::HostWaitSite::GsWorkerSnapshot};
    return future.get();
}

std::future<GsRendererWorkerSnapshot>
GsRendererWorker::request_snapshot(std::optional<GsScanoutPlan> scanout_plan) {
    auto promise = std::make_shared<std::promise<GsRendererWorkerSnapshot>>();
    auto future = promise->get_future();
    SynchronizeCommand command{};
    command.scanout_plan = std::move(scanout_plan);
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
    return future;
}

std::future<GsRendererWorkerFrameCompletionResult>
GsRendererWorker::request_frame_completion(std::uint64_t frame_id) {
    if (frame_id == 0U) {
        throw std::invalid_argument("GS frame completion requires a nonzero frame ID");
    }
    auto promise = std::make_shared<std::promise<GsRendererWorkerFrameCompletionResult>>();
    auto future = promise->get_future();
    FrameCompletionCommand command{};
    command.frame_id = frame_id;
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
    return future;
}

std::future<GsRendererWorkerSnapshot>
GsRendererWorker::request_present_snapshot(GsScanoutPlan scanout_plan) {
    auto promise = std::make_shared<std::promise<GsRendererWorkerSnapshot>>();
    auto future = promise->get_future();
    PresentSnapshotRequest request{};
    request.scanout_plan = std::move(scanout_plan);
    request.promise = std::move(promise);
    impl_->enqueue_present(std::move(request));
    return future;
}

std::future<GsTextureResidencyAcquireResult>
GsRendererWorker::request_texture_residency(guest::GsDrawingContext context,
                                            std::uint64_t expected_tex0) {
    auto promise = std::make_shared<std::promise<GsTextureResidencyAcquireResult>>();
    auto future = promise->get_future();
    AcquireTextureResidencyCommand command{};
    command.context = context;
    command.expected_tex0 = expected_tex0;
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
    return future;
}

std::future<GsTextureResidencyReleaseResult>
GsRendererWorker::request_texture_residency_release(GsTextureResidencyToken token) {
    auto promise = std::make_shared<std::promise<GsTextureResidencyReleaseResult>>();
    auto future = promise->get_future();
    ReleaseTextureResidencyCommand command{};
    command.token = token;
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
    return future;
}

std::future<NativeUiTextDrawResult>
GsRendererWorker::request_native_ui_text_draw(const NativeUiTextDraw& draw) {
    return impl_->request_native_ui_text_draw(draw);
}

std::future<NativeUiTextDrawResult> GsRendererWorker::request_prepared_native_ui_text_draw(
    GsTextureResidencyToken residency, std::uint64_t expected_tex0,
    std::shared_ptr<NativeUiTextDrawRequest> request) {
    auto promise = std::make_shared<std::promise<NativeUiTextDrawResult>>();
    auto future = promise->get_future();
    NativeUiTextDrawCommand command{};
    command.residency = residency;
    command.expected_tex0 = expected_tex0;
    command.request = std::move(request);
    command.promise = std::move(promise);
    CommandPayload payload{std::move(command)};
    static_cast<void>(impl_->enqueue(std::move(payload), 0U, 0U, false));
    return future;
}

GsRendererWorkerQueueStatistics GsRendererWorker::queue_statistics() const {
    return impl_->queue_statistics();
}

bool GsRendererWorker::failed() const noexcept {
    return impl_->failed();
}

GsRendererWorker::PrimaryBatchAdmission
GsRendererWorker::admit_primary(const guest::GsRegisterWrite& write) {
    CommandPayload payload{write};
    return impl_->enqueue(std::move(payload), 0U, 0U, true);
}

GsRendererWorker::PrimaryBatchAdmission GsRendererWorker::admit_primary(const guest::GifPath path) {
    CommandPayload payload{GifTagBeginCommand{path}};
    return impl_->enqueue(std::move(payload), 0U, 0U, true);
}

GsRendererWorker::PrimaryBatchAdmission GsRendererWorker::admit_primary_reset() {
    CommandPayload payload{ResetCommand{}};
    return impl_->enqueue(std::move(payload), 0U, 0U, true);
}

GsRendererWorker::PrimaryBatchAdmission
GsRendererWorker::admit_primary(const guest::GsImageTransfer& transfer) {
    CommandPayload payload{transfer};
    return impl_->enqueue(std::move(payload), transfer.pixels.size(), 0U, true);
}

GsRendererWorker::PrimaryBatchAdmission GsRendererWorker::admit_primary(guest::GsImageTransfer&& transfer) {
    const std::size_t bytes = transfer.pixels.size();
    CommandPayload payload{std::move(transfer)};
    return impl_->enqueue(std::move(payload), bytes, 0U, true);
}

bool GsRendererWorkerPrimaryBatchEvidence::belongs_to(const GsRendererWorker& worker) const noexcept {
    return belongs_to_worker_instance(worker.worker_instance_id());
}

GsRendererWorker::PrimaryBatchSubmission::PrimaryBatchSubmission(GsRendererWorker& worker,
                                                                 guest::GsCapture& capture,
                                                                 guest::GifPath expected_path)
    : worker_(&worker), capture_(&capture), worker_lifetime_(worker.lifetime_token_),
      capture_lifetime_(capture.lifetime_token()), expected_path_(expected_path),
      worker_instance_id_(worker.worker_instance_id()) {
    capture.set_command_observer(this);
    capture.set_terminal_raw_gif_observer(this);
    attached_ = true;
}

GsRendererWorker::PrimaryBatchSubmission::~PrimaryBatchSubmission() {
    static_cast<void>(detach());
}

void GsRendererWorker::PrimaryBatchSubmission::record_locked(
    const GsRendererWorker::PrimaryBatchAdmission& admission, guest::GifPath path,
    std::uint64_t guest_sequence) noexcept {
    if (sealed_ || failed_) {
        return;
    }
    if (worker_ == nullptr || admission.worker_instance_id == 0U ||
        admission.worker_instance_id != worker_instance_id_ || admission.serial == 0U ||
        admission.payload_digest == 0U || path != expected_path_) {
        failed_ = true;
        return;
    }
    if (commands_.empty()) {
        first_serial_ = admission.serial;
    } else if (last_serial_ == std::numeric_limits<std::uint64_t>::max() || admission.serial != last_serial_ + 1U) {
        failed_ = true;
        return;
    }
    runtime::Slus21050FemainPrimaryStreamCommandKind kind{};
    switch (admission.kind) {
    case GsRendererWorkerCommandKind::RegisterWrite:
        kind = runtime::Slus21050FemainPrimaryStreamCommandKind::RegisterWrite;
        break;
    case GsRendererWorkerCommandKind::ImageTransfer:
        kind = runtime::Slus21050FemainPrimaryStreamCommandKind::ImageTransfer;
        break;
    case GsRendererWorkerCommandKind::GifTagBegin:
        kind = runtime::Slus21050FemainPrimaryStreamCommandKind::GifTagBegin;
        break;
    default:
        failed_ = true;
        return;
    }
    try {
        commands_.push_back(GsRendererWorkerPrimaryBatchCommandEvidence{
            .kind = kind,
            .path = path,
            .guest_sequence = guest_sequence,
            .command_digest = admission.payload_digest,
        });
        last_serial_ = admission.serial;
    } catch (...) {
        failed_ = true;
    }
}

void GsRendererWorker::PrimaryBatchSubmission::fail_locked() noexcept {
    if (!sealed_) {
        failed_ = true;
    }
}

void GsRendererWorker::PrimaryBatchSubmission::seal_locked() noexcept {
    if (sealed_ || failed_) {
        return;
    }
    try {
        auto evidence = std::shared_ptr<const Evidence>(new Evidence(
            worker_instance_id_, std::move(commands_), first_serial_, last_serial_));
        commands_.clear();
        if (!evidence->valid()) {
            failed_ = true;
            return;
        }
        evidence_ = std::move(evidence);
        sealed_ = true;
    } catch (...) {
        failed_ = true;
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_gif_tag_begin(guest::GifPath path) {
    std::lock_guard lock{mutex_};
    if (!attached_ || worker_ == nullptr) {
        return;
    }
    const auto admission = worker_->admit_primary(path);
    if (!sealed_) {
        record_locked(admission, path, 0U);
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_reset() {
    std::lock_guard lock{mutex_};
    if (!attached_ || worker_ == nullptr) {
        return;
    }
    // A reset inside a primary batch has no canonical evidence form, so the
    // record fails and the batch is never sealed.
    const auto admission = worker_->admit_primary_reset();
    if (!sealed_) {
        record_locked(admission, expected_path_, 0U);
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_register_write(
    const guest::GsRegisterWrite& write) {
    std::lock_guard lock{mutex_};
    if (!attached_ || worker_ == nullptr) {
        return;
    }
    const auto admission = worker_->admit_primary(write);
    if (!sealed_) {
        record_locked(admission, write.path, write.sequence);
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_image_transfer(
    const guest::GsImageTransfer& transfer) {
    std::lock_guard lock{mutex_};
    if (!attached_ || worker_ == nullptr) {
        return;
    }
    const auto admission = worker_->admit_primary(transfer);
    if (!sealed_) {
        record_locked(admission, transfer.path, transfer.sequence);
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_image_transfer(guest::GsImageTransfer&& transfer) {
    std::lock_guard lock{mutex_};
    if (!attached_ || worker_ == nullptr) {
        return;
    }
    const guest::GifPath path = transfer.path;
    const std::uint64_t sequence = transfer.sequence;
    const auto admission = worker_->admit_primary(std::move(transfer));
    if (!sealed_) {
        record_locked(admission, path, sequence);
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_gif_packet_eop(
    guest::GifPath path, bool primary_command_stream_available) noexcept {
    std::lock_guard lock{mutex_};
    if (!attached_ || path != expected_path_) {
        return;
    }
    eop_received_ = true;
    if (primary_command_stream_available) {
        seal_locked();
    } else {
        fail_locked();
    }
}

void GsRendererWorker::PrimaryBatchSubmission::on_gs_gif_reset() noexcept {
    std::lock_guard lock{mutex_};
    fail_locked();
}

bool GsRendererWorker::PrimaryBatchSubmission::detach() noexcept {
    std::lock_guard lock{mutex_};
    if (!attached_) {
        return false;
    }
    const auto worker_alive = worker_lifetime_.lock();
    const auto capture_alive = capture_lifetime_.lock();
    if (!worker_alive || !capture_alive) {
        // An endpoint was destroyed first; the stale pointers must not be used.
        attached_ = false;
        fail_locked();
        return false;
    }
    if (!capture_->gif_observation_quiescent()) {
        return false;
    }
    capture_->set_terminal_raw_gif_observer(nullptr);
    capture_->set_command_observer(worker_);
    attached_ = false;
    return true;
}

bool GsRendererWorker::PrimaryBatchSubmission::attached() const noexcept {
    std::lock_guard lock{mutex_};
    return attached_;
}

bool GsRendererWorker::PrimaryBatchSubmission::sealed() const noexcept {
    std::lock_guard lock{mutex_};
    return sealed_;
}

bool GsRendererWorker::PrimaryBatchSubmission::failed() const noexcept {
    std::lock_guard lock{mutex_};
    return failed_;
}

std::shared_ptr<const GsRendererWorker::PrimaryBatchSubmission::Evidence>
GsRendererWorker::PrimaryBatchSubmission::evidence() const noexcept {
    std::lock_guard lock{mutex_};
    return evidence_;
}

} // namespace takedown::presentation
