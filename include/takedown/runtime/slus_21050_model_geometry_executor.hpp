#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace takedown::runtime::slus_21050 {

struct ModelGeometryExecutorStatistics {
    unsigned worker_threads{};
    std::uint64_t batches{}, jobs{}, worker_jobs{}, caller_jobs{}, failed_batches{};
};

// Reusable workers for an owned model's independent geometry draws. A run is
// synchronous: the caller also executes jobs, and every job completes before
// return or exception. Inputs must remain immutable until then; each ordinal
// must have its own output storage. No guest, producer or renderer work belongs
// in these callbacks. One owner may call run at a time; recursive runs reject.
class ModelGeometryExecutor {
public:
    static constexpr unsigned max_worker_threads = 8;
    static constexpr std::size_t max_jobs = 65536;
    using Job = void (*)(void*, std::size_t);

    explicit ModelGeometryExecutor(unsigned worker_threads);
    ~ModelGeometryExecutor();
    ModelGeometryExecutor(const ModelGeometryExecutor&) = delete;
    ModelGeometryExecutor& operator=(const ModelGeometryExecutor&) = delete;

    // Capture the calling thread's complete FP environment for this run. Each
    // callback receives it and restores its executing thread's previous state,
    // including on exceptions. All jobs are joined before the lowest-ordinal
    // exception is rethrown; completion order never selects the failure.
    void run(std::size_t jobs, void* context, Job);
    [[nodiscard]] ModelGeometryExecutorStatistics statistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takedown::runtime::slus_21050
