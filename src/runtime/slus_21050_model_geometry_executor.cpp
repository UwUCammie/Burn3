#include "takedown/runtime/slus_21050_model_geometry_executor.hpp"

#include <xmmintrin.h>

#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace takedown::runtime::slus_21050 {
namespace {

// A thread's complete FP state: the x87/SSE environment plus MXCSR.
class FloatingPointEnvironment {
  public:
    void capture() {
        if (std::fegetenv(&standard_) != 0) {
            throw std::runtime_error("cannot capture model geometry floating-point environment");
        }
        mxcsr_ = _mm_getcsr();
    }
    void install() const {
        if (std::fesetenv(&standard_) != 0) {
            throw std::runtime_error("cannot install model geometry floating-point environment");
        }
        _mm_setcsr(mxcsr_);
    }
    [[gnu::noinline]] void restore() const {
        if (std::fesetenv(&standard_) != 0) {
            std::terminate();
        }
        _mm_setcsr(mxcsr_);
    }

  private:
    std::fenv_t standard_{};
    unsigned mxcsr_{};
};

// Installs the caller's environment for one job and restores the worker's own on exit.
class FloatingPointJobScope {
  public:
    explicit FloatingPointJobScope(const FloatingPointEnvironment& caller) {
        saved_.capture();
        caller.install();
    }
    ~FloatingPointJobScope() { saved_.restore(); }
    FloatingPointJobScope(const FloatingPointJobScope&) = delete;
    FloatingPointJobScope& operator=(const FloatingPointJobScope&) = delete;

  private:
    FloatingPointEnvironment saved_{};
};

} // namespace

struct ModelGeometryExecutor::Impl {
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable available;
    std::condition_variable complete;
    std::vector<std::exception_ptr> failures;
    FloatingPointEnvironment environment;
    ModelGeometryExecutor::Job callback{};
    void* context{};
    std::size_t count{};
    std::uint64_t generation{};
    std::size_t pending{};
    bool stopping{};
    std::atomic_flag running{};
    std::atomic<std::size_t> next{};
    std::atomic<std::uint64_t> batches{};
    std::atomic<std::uint64_t> jobs{};
    std::atomic<std::uint64_t> worker_jobs{};
    std::atomic<std::uint64_t> caller_jobs{};
    std::atomic<std::uint64_t> failed_batches{};

    explicit Impl(const unsigned worker_threads) {
        workers.reserve(worker_threads);
        try {
            for (unsigned index = 0U; index != worker_threads; ++index) {
                workers.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> guard(mutex);
                stopping = true;
            }
            available.notify_all();
            for (auto& worker : workers) {
                worker.join();
            }
            throw;
        }
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex);
        std::uint64_t observed = 0U;
        while (true) {
            while (!stopping && generation == observed) {
                available.wait(lock);
            }
            if (stopping) {
                return;
            }
            observed = generation;
            lock.unlock();
            std::size_t consumed = 0U;
            while (true) {
                const std::size_t ordinal = next.fetch_add(1U, std::memory_order_relaxed);
                if (ordinal >= count) {
                    break;
                }
                try {
                    FloatingPointJobScope scope(environment);
                    callback(context, ordinal);
                } catch (...) {
                    failures[ordinal] = std::current_exception();
                }
                ++consumed;
            }
            worker_jobs.fetch_add(consumed, std::memory_order_relaxed);
            lock.lock();
            pending -= consumed;
            if (pending == 0U) {
                complete.notify_one();
            }
        }
    }
};

ModelGeometryExecutor::ModelGeometryExecutor(const unsigned worker_threads) {
    if (worker_threads > max_worker_threads) {
        throw std::invalid_argument("too many model geometry worker threads");
    }
    impl_ = std::make_unique<Impl>(worker_threads);
}

ModelGeometryExecutor::~ModelGeometryExecutor() {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->available.notify_all();
    for (auto& worker : impl_->workers) {
        worker.join();
    }
}

void ModelGeometryExecutor::run(const std::size_t jobs, void* context, const Job callback) {
    if (jobs > max_jobs || (jobs != 0U && callback == nullptr)) {
        throw std::invalid_argument("invalid model geometry job extent");
    }
    if (impl_->running.test_and_set(std::memory_order_acquire)) {
        throw std::logic_error("model geometry executor is already running");
    }
    if (jobs == 0U) {
        impl_->running.clear(std::memory_order_release);
        return;
    }

    FloatingPointEnvironment caller;
    caller.capture();
    impl_->failures.assign(jobs, nullptr);
    impl_->batches.fetch_add(1U, std::memory_order_relaxed);
    impl_->jobs.fetch_add(jobs, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->context = context;
        impl_->callback = callback;
        impl_->count = jobs;
        impl_->environment = caller;
        impl_->next.store(0U, std::memory_order_relaxed);
        impl_->pending = jobs;
        ++impl_->generation;
    }
    impl_->available.notify_all();

    std::size_t consumed = 0U;
    while (true) {
        const std::size_t ordinal = impl_->next.fetch_add(1U, std::memory_order_relaxed);
        if (ordinal >= impl_->count) {
            break;
        }
        try {
            FloatingPointJobScope scope(impl_->environment);
            impl_->callback(impl_->context, ordinal);
        } catch (...) {
            impl_->failures[ordinal] = std::current_exception();
        }
        ++consumed;
    }
    impl_->caller_jobs.fetch_add(consumed, std::memory_order_relaxed);

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->pending -= consumed;
        while (impl_->pending != 0U) {
            impl_->complete.wait(lock);
        }
        impl_->context = nullptr;
        impl_->callback = nullptr;
        impl_->count = 0U;
    }
    impl_->running.clear(std::memory_order_release);

    for (const auto& failure : impl_->failures) {
        if (failure) {
            impl_->failed_batches.fetch_add(1U, std::memory_order_relaxed);
            std::rethrow_exception(failure);
        }
    }
}

ModelGeometryExecutorStatistics ModelGeometryExecutor::statistics() const noexcept {
    ModelGeometryExecutorStatistics result{};
    result.worker_threads = static_cast<unsigned>(impl_->workers.size());
    result.batches = impl_->batches.load(std::memory_order_relaxed);
    result.jobs = impl_->jobs.load(std::memory_order_relaxed);
    result.worker_jobs = impl_->worker_jobs.load(std::memory_order_relaxed);
    result.caller_jobs = impl_->caller_jobs.load(std::memory_order_relaxed);
    result.failed_batches = impl_->failed_batches.load(std::memory_order_relaxed);
    return result;
}

} // namespace takedown::runtime::slus_21050
