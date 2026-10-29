#include "takedown/runtime/graphics_async.hpp"

#include <chrono>
#include <immintrin.h>
#include <utility>

namespace takedown::runtime {
namespace {

// Set for the lifetime of GraphicsAsyncWorker::run() on its own thread.
thread_local bool g_on_worker_thread = false;

[[nodiscard]] std::uint64_t monotonic_nanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

const char* graphics_async_fence_name(const GraphicsAsyncFence reason) noexcept {
    switch (reason) {
    case GraphicsAsyncFence::Latency:
        return "latency";
    case GraphicsAsyncFence::BusReadGifVif:
        return "bus-read-gif-vif";
    case GraphicsAsyncFence::BusReadFifo:
        return "bus-read-fifo";
    case GraphicsAsyncFence::BusReadDmac:
        return "bus-read-dmac-channel";
    case GraphicsAsyncFence::BusWriteGifVif:
        return "bus-write-gif-vif";
    case GraphicsAsyncFence::BusWriteFifo:
        return "bus-write-fifo";
    case GraphicsAsyncFence::BusWriteDmac:
        return "bus-write-dmac";
    case GraphicsAsyncFence::BusWriteGsPriv:
        return "bus-write-gs-priv";
    case GraphicsAsyncFence::Vu1Memory:
        return "vu1-memory";
    case GraphicsAsyncFence::CompletionTake:
        return "completion-take";
    case GraphicsAsyncFence::Accessor:
        return "accessor";
    case GraphicsAsyncFence::DmacStatusWrite:
        return "dmac-status-write";
    case GraphicsAsyncFence::SchedulerIdle:
        return "scheduler-idle";
    case GraphicsAsyncFence::SchedulerPoll:
        return "scheduler-poll";
    case GraphicsAsyncFence::Reconfigure:
        return "reconfigure";
    case GraphicsAsyncFence::Teardown:
        return "teardown";
    case GraphicsAsyncFence::Explicit:
        return "explicit";
    case GraphicsAsyncFence::Count:
        break;
    }
    return "unknown";
}

const char* graphics_async_decline_name(const GraphicsAsyncDecline reason) noexcept {
    switch (reason) {
    case GraphicsAsyncDecline::NotConfigured:
        return "not-configured";
    case GraphicsAsyncDecline::Observer:
        return "observer";
    case GraphicsAsyncDecline::ActiveMarkers:
        return "active-markers";
    case GraphicsAsyncDecline::Diagnostics:
        return "diagnostics";
    case GraphicsAsyncDecline::VuWorker:
        return "vu-worker";
    case GraphicsAsyncDecline::SmallWork:
        return "small-work";
    case GraphicsAsyncDecline::Count:
        break;
    }
    return "unknown";
}

bool on_graphics_async_worker_thread() noexcept {
    return g_on_worker_thread;
}

GraphicsAsyncWorker::GraphicsAsyncWorker() : thread_([this] { run(); }) {}

GraphicsAsyncWorker::~GraphicsAsyncWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    work_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GraphicsAsyncWorker::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
        pending_.fetch_add(1U, std::memory_order_release);
    }
    work_cv_.notify_one();
}

// RECOVERY: the original GIMPLE keeps the "no earlier error" test as an unnamed bool
// PHI; this equivalent form folds it differently (control flow otherwise the same).
void GraphicsAsyncWorker::run() noexcept {
    g_on_worker_thread = true;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto started = monotonic_nanoseconds();
        // Once a task has failed, later tasks are skipped but still retired so
        // waiters are released.
        bool skip = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            skip = error_ != nullptr;
        }
        if (!skip) {
            try {
                task();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!error_) {
                    error_ = std::current_exception();
                }
            }
        }
        busy_ns_.fetch_add(monotonic_nanoseconds() - started, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.fetch_sub(1U, std::memory_order_release);
        }
        idle_cv_.notify_all();
    }
}

std::exception_ptr GraphicsAsyncWorker::wait_idle_nothrow(std::uint64_t& waited) noexcept {
    if (!idle()) {
        const auto started = monotonic_nanoseconds();
        // Short jobs finish while we spin; only then pay for a sleep.
        for (int spin = 0; spin < 2048 && !idle(); ++spin) {
            _mm_pause();
        }
        if (!idle()) {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_cv_.wait(lock, [this] { return idle(); });
        }
        waited += monotonic_nanoseconds() - started;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(error_, nullptr);
}

void GraphicsAsyncWorker::wait_idle(std::uint64_t& waited) {
    std::exception_ptr error = wait_idle_nothrow(waited);
    if (error) {
        std::rethrow_exception(error);
    }
}

} // namespace takedown::runtime
