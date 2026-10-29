#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace takedown::runtime {

// Coarse host-side phases around the EE runner's durable execution seams.
// VIF1 service deliberately includes any VU1 execution and XGKICK work it
// performs synchronously. GIF service includes synchronous GS observers (and
// therefore live-stream encoding/writes when enabled).
enum class EeHostPhase : std::uint8_t {
    Vif1DmacVu1Inclusive,
    GifDmacStreamInclusive,
    GraphicsServiceCallback,
    HostPresentation,
    Scheduler,
    NativeStage,
    CompiledStage,
    ScalarStep,
    KernelSyscall,
    Count,
};

[[nodiscard]] std::string_view to_string(EeHostPhase phase) noexcept;
[[nodiscard]] std::string_view ee_host_phase_scope(EeHostPhase phase) noexcept;

struct EeHostPhaseSample {
    EeHostPhase phase{EeHostPhase::Vif1DmacVu1Inclusive};
    std::uint64_t calls{};
    std::uint64_t timed_calls{};
    std::uint64_t sampled_nanoseconds{};
    std::uint64_t estimated_nanoseconds{};
    std::uint64_t maximum_sampled_nanoseconds{};

    bool operator==(const EeHostPhaseSample&) const = default;
};

struct EeHostPhaseProfilerStatistics {
    std::uint64_t sample_interval{};
    std::uint64_t host_nanoseconds{};
    std::uint64_t host_timing_pauses{};
    std::uint64_t host_timing_excluded_nanoseconds{};
    std::uint64_t host_clock_regressions{};
    std::uint64_t sample_clock_pair_overhead_nanoseconds{};
    std::uint64_t estimated_profiled_nanoseconds{};
    std::uint64_t estimated_unattributed_nanoseconds{};
    std::uint64_t estimated_overattributed_nanoseconds{};

    bool operator==(const EeHostPhaseProfilerStatistics&) const = default;
};

// Explicitly opt-in, deterministic sampler for host phase costs. A stable
// integer mixer selects one pseudo-random call from every complete
// interval-sized block, de-correlating samples from periodic guest loops. The
// measured mean is scaled by the exact call count, avoiding a clock read on
// every guest instruction while retaining stable, comparable output. Normal
// runs do not construct this object or call begin_phase().
class EeHostPhaseProfiler {
  public:
    using HostNanosecondsClock = std::uint64_t (*)() noexcept;

    static constexpr std::uint64_t kDefaultSampleInterval = 1024U;

    class Scope {
      public:
        Scope() = default;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&&) = delete;
        ~Scope() { finish(); }

        // Keep the disabled path visible at each hot call site. Even an LTO
        // build may otherwise outline these millions of empty scope calls.
        void finish() noexcept {
            if (profiler_ != nullptr) [[unlikely]] finish_sampled();
        }

      private:
        friend class EeHostPhaseProfiler;
        void finish_sampled() noexcept;
        Scope(EeHostPhaseProfiler* profiler, EeHostPhase phase,
              std::uint64_t started_nanoseconds) noexcept;

        EeHostPhaseProfiler* profiler_{};
        EeHostPhase phase_{EeHostPhase::Vif1DmacVu1Inclusive};
        std::uint64_t started_nanoseconds_{};
    };

    explicit EeHostPhaseProfiler(std::uint64_t sample_interval = kDefaultSampleInterval,
                                 HostNanosecondsClock host_clock = nullptr);

    void start_host_timing();
    void pause_host_timing();
    void resume_host_timing();
    void stop_host_timing();

    // A paused profiler ignores host/debugger work entirely. Call counts and
    // samples therefore cover the same guest-execution domain as host time.
    [[nodiscard]] Scope begin_phase(EeHostPhase phase);

    [[nodiscard]] EeHostPhaseProfilerStatistics statistics() const noexcept;
    [[nodiscard]] std::vector<EeHostPhaseSample> ranked_phase_samples() const;

  private:
    struct PhaseTotals {
        std::uint64_t calls{};
        std::uint64_t timed_calls{};
        std::uint64_t sampled_nanoseconds{};
        std::uint64_t maximum_sampled_nanoseconds{};
        std::uint64_t next_sample_ordinal{};
    };

    [[nodiscard]] static std::uint64_t steady_host_nanoseconds() noexcept;
    [[nodiscard]] std::uint64_t host_elapsed_since_last_timestamp() noexcept;
    void finish_phase(EeHostPhase phase, std::uint64_t started_nanoseconds) noexcept;

    std::uint64_t sample_interval_{};
    std::uint64_t sample_interval_mask_{};
    std::array<PhaseTotals, static_cast<std::size_t>(EeHostPhase::Count)> phases_{};
    HostNanosecondsClock host_clock_{};
    std::uint64_t host_timestamp_nanoseconds_{};
    std::uint64_t host_nanoseconds_{};
    std::uint64_t host_timing_pauses_{};
    std::uint64_t host_timing_excluded_nanoseconds_{};
    std::uint64_t host_clock_regressions_{};
    std::uint64_t sample_clock_pair_overhead_nanoseconds_{};
    bool host_timing_started_{};
    bool host_timing_running_{};
    bool host_timing_stopped_{};
};

// Uses distinct artifact names so one explicitly shared empty directory can
// contain both EE-PC and host-phase profiles from the same run.
void prepare_empty_ee_host_phase_profile_directory(const std::filesystem::path& directory);
void write_ee_host_phase_profile_artifacts(const std::filesystem::path& directory,
                                           const EeHostPhaseProfiler& profiler);

} // namespace takedown::runtime
