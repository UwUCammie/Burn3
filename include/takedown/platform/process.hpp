#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace takedown::platform {

// Cumulative host counters for diagnostic boundaries, not guest identity.
// Cycles must not be converted into time; Windows CPU accounting supplies ns.
// Unsupported/failed queries are absent, never reported as zero usage.
struct HostCpuSnapshot {
    std::optional<std::uint64_t> thread_nanoseconds;
    std::optional<std::uint64_t> process_nanoseconds;
    std::optional<std::uint64_t> thread_cycles;
};
[[nodiscard]] HostCpuSnapshot capture_host_cpu_snapshot() noexcept;

// The process inherits this process's standard input, output, error, and
// environment. Arguments are passed directly to execvp; no shell is involved.
struct ProcessResult {
    int exit_code{-1};
    int termination_signal{0};

    [[nodiscard]] bool exited_normally() const noexcept;
    [[nodiscard]] bool succeeded() const noexcept;
};

// Starts arguments[0] with the remaining elements as its argv and waits for it.
// Throws std::system_error if the process could not be started or waited for.
[[nodiscard]] ProcessResult run_process(std::span<const std::string> arguments);

struct CapturedProcessResult {
    ProcessResult process;
    std::string output;
    bool output_truncated{false};
};

// Captures standard output and standard error into one bounded string while
// preserving argv-safe execution. Excess output is drained but not retained.
[[nodiscard]] CapturedProcessResult
run_process_and_capture(std::span<const std::string> arguments,
                        std::size_t maximum_output_bytes = 1024U * 1024U);

[[nodiscard]] std::string describe_process_result(const ProcessResult& result);

} // namespace takedown::platform
