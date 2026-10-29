#pragma once

#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace takedown::runtime {

// Host execution path which retired the sampled guest instruction. Atomic
// native functions and resumable compiled blocks are attributed to their
// dispatch entry because they intentionally do not expose an instruction trace.
enum class EePcSampleKind : std::uint8_t {
    Scalar,
    CompiledBlock,
    NativeFunction,
    Hle,
    Count,
};

[[nodiscard]] std::string_view to_string(EePcSampleKind kind) noexcept;

struct EePcSample {
    guest::GuestAddress pc{};
    EePcSampleKind kind{EePcSampleKind::Scalar};
    std::uint64_t samples{};
    // Approximate wall time in complete intervals ending at this sampled PC.
    // This is intentionally not the execution time of the instruction itself.
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_windows{};

    bool operator==(const EePcSample&) const = default;
};

struct EePcRangeSample {
    guest::GuestAddress begin{};
    std::uint64_t end{};
    std::array<std::uint64_t, static_cast<std::size_t>(EePcSampleKind::Count)> by_kind{};
    std::uint64_t samples{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_windows{};

    bool operator==(const EePcRangeSample&) const = default;
};

struct EePcSamplerStatistics {
    std::uint64_t interval{};
    std::uint64_t retired_instructions{};
    std::uint64_t spans_observed{};
    std::uint64_t samples{};
    std::uint64_t ordinal_discontinuities{};
    std::uint64_t host_timing_windows{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_timing_pauses{};
    std::uint64_t host_timing_excluded_nanoseconds{};
    std::uint64_t host_timing_tail_nanoseconds{};
    std::uint64_t host_clock_regressions{};

    bool operator==(const EePcSamplerStatistics&) const = default;
};

// Deterministic instruction-ordinal sampler. observe_span() performs no hash
// lookup until a power-of-two sample boundary falls inside the retired span;
// a long native span is counted in bulk with one table update. Normal runs do
// not construct this object or call it.
class EePcSampler {
  public:
    using HostNanosecondsClock = std::uint64_t (*)() noexcept;

    static constexpr std::uint64_t kDefaultInterval = 4096U;
    static constexpr std::uint32_t kRangeShift = 12U;
    static constexpr std::uint64_t kRangeBytes = 1ULL << kRangeShift;

    explicit EePcSampler(std::uint64_t interval = kDefaultInterval,
                         HostNanosecondsClock host_clock = nullptr);

    // Host timing is opt-in even when instruction sampling is enabled. One
    // steady-clock read occurs only when observe_span() crosses an existing
    // sample boundary. The elapsed window is attributed to its ending PC, so
    // the result is a low-overhead ranking guide rather than a per-instruction
    // duration. Pause/resume excludes explicit debugger/capture waits without
    // discarding the active partial interval.
    void start_host_timing();
    void pause_host_timing();
    void resume_host_timing();
    void stop_host_timing();

    void observe_span(std::uint64_t first_instruction,
                      guest::GuestAddress dispatch_pc,
                      std::uint64_t retired_instructions,
                      EePcSampleKind kind);

    [[nodiscard]] EePcSamplerStatistics statistics() const noexcept;
    [[nodiscard]] std::vector<EePcSample> ranked_pc_samples() const;
    [[nodiscard]] std::vector<EePcRangeSample> ranked_range_samples() const;
    [[nodiscard]] std::vector<EePcSample> ranked_host_pc_samples() const;
    [[nodiscard]] std::vector<EePcRangeSample> ranked_host_range_samples() const;

  private:
    [[nodiscard]] static std::uint64_t sample_key(guest::GuestAddress pc,
                                                   EePcSampleKind kind) noexcept;

    struct SampleTotals {
        std::uint64_t samples{};
        std::uint64_t host_nanoseconds{};
        std::uint64_t host_windows{};
    };

    [[nodiscard]] static std::uint64_t steady_host_nanoseconds() noexcept;
    [[nodiscard]] std::uint64_t host_elapsed_since_last_timestamp() noexcept;
    void attribute_host_window(std::uint64_t key) noexcept;

    std::uint64_t interval_{};
    std::uint64_t interval_mask_{};
    EePcSamplerStatistics statistics_{};
    std::unordered_map<std::uint64_t, SampleTotals> samples_;
    HostNanosecondsClock host_clock_{};
    std::uint64_t host_timestamp_nanoseconds_{};
    std::uint64_t pending_host_nanoseconds_{};
    bool host_timing_started_{};
    bool host_timing_running_{};
    bool host_timing_stopped_{};
};

// Refuses nonempty destinations so a long run cannot mix artifacts from
// different executable revisions or sampling intervals.
void prepare_empty_ee_pc_profile_directory(const std::filesystem::path& directory);
void write_ee_pc_profile_artifacts(const std::filesystem::path& directory,
                                   const EePcSampler& sampler);

} // namespace takedown::runtime
