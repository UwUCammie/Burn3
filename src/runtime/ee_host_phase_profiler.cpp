#include "takedown/runtime/ee_host_phase_profiler.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace takedown::runtime {
namespace {

// Saturating sum used by every host and sampled accumulator in the profiler.
std::uint64_t saturating_add(const std::uint64_t left, const std::uint64_t right) noexcept {
    return ~left >= right ? left + right : 18446744073709551615ULL;
}

// Sampled-mean estimate: calls times the mean sample, saturated on overflow.
std::uint64_t estimate_phase_nanoseconds(const std::uint64_t calls, const std::uint64_t timed,
                                         const std::uint64_t sampled) noexcept {
    if (timed == 0) { return 0; }
    const std::uint64_t quotient = sampled / timed;
    const std::uint64_t remainder = sampled % timed;
    std::uint64_t base = 0;
    if (!(timed > sampled || calls == 0)) {
        std::uint64_t product = 0;
        base = __builtin_mul_overflow(calls, quotient, &product) ? 18446744073709551615ULL : product;
    }
    std::uint64_t scaled_remainder = 0;
    if (!(calls == 0 || remainder == 0)) {
        std::uint64_t product = 0;
        scaled_remainder = __builtin_mul_overflow(calls, remainder, &product) ? 18446744073709551615ULL : product;
    }
    const std::uint64_t fraction = scaled_remainder / timed;
    return fraction <= ~base ? base + fraction : 18446744073709551615ULL;
}

void verify_complete_write(std::ofstream& output, const std::filesystem::path& path) {
    output.flush();
    if ((output.rdstate() & (std::ios::badbit | std::ios::failbit)) != 0) {
        throw std::runtime_error("unable to write complete EE host phase profile artifact: " + path.string());
    }
}

std::ofstream create_artifact(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite EE host phase profile artifact: " + path.string());
    }
    std::ofstream output;
    output.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output.is_open()) {
        output.setstate(std::ios::failbit);
    } else {
        output.clear();
    }
    if ((output.rdstate() & (std::ios::badbit | std::ios::failbit)) != 0) {
        throw std::runtime_error("unable to create EE host phase profile artifact: " + path.string());
    }
    return output;
}

} // namespace

std::string_view to_string(const EeHostPhase phase) noexcept {
    switch (phase) {
    case EeHostPhase::Vif1DmacVu1Inclusive:
        return "vif1-dmac-vu1-inclusive";
    case EeHostPhase::GifDmacStreamInclusive:
        return "gif-dmac-stream-inclusive";
    case EeHostPhase::GraphicsServiceCallback:
        return "graphics-service-callback";
    case EeHostPhase::HostPresentation:
        return "host-presentation";
    case EeHostPhase::Scheduler:
        return "scheduler";
    case EeHostPhase::NativeStage:
        return "native-stage";
    case EeHostPhase::CompiledStage:
        return "compiled-stage";
    case EeHostPhase::ScalarStep:
        return "scalar-step";
    case EeHostPhase::KernelSyscall:
        return "kernel-syscall";
    case EeHostPhase::Count:
        break;
    }
    return "unknown";
}

std::string_view ee_host_phase_scope(const EeHostPhase phase) noexcept {
    switch (phase) {
    case EeHostPhase::Vif1DmacVu1Inclusive:
        return "advance-vif1-dmac-including-vu1-and-xgkick";
    case EeHostPhase::GifDmacStreamInclusive:
        return "advance-gif-dmac-including-synchronous-gs-observers";
    case EeHostPhase::GraphicsServiceCallback:
        return "one-direct-instruction-graphics-service-checkpoint";
    case EeHostPhase::HostPresentation:
        return "event-pump-overlay-upload-and-window-present";
    case EeHostPhase::Scheduler:
        return "kernel-callback-thread-and-idle-event-dispatch";
    case EeHostPhase::NativeStage:
        return "native-lookup-preflight-prepare-and-dispatch";
    case EeHostPhase::CompiledStage:
        return "compiled-budget-lookup-and-dispatch";
    case EeHostPhase::ScalarStep:
        return "one-reference-interpreter-step";
    case EeHostPhase::KernelSyscall:
        return "kernel-system-call-handler";
    case EeHostPhase::Count:
        break;
    }
    return "unknown";
}

EeHostPhaseProfiler::Scope::Scope(EeHostPhaseProfiler* profiler, EeHostPhase phase,
                                  std::uint64_t started_nanoseconds) noexcept
    : profiler_(profiler), phase_(phase), started_nanoseconds_(started_nanoseconds) {}

EeHostPhaseProfiler::Scope::Scope(Scope&& other) noexcept
    : profiler_(other.profiler_), phase_(other.phase_), started_nanoseconds_(other.started_nanoseconds_) {
    other.profiler_ = nullptr;
}

void EeHostPhaseProfiler::Scope::finish_sampled() noexcept {
    EeHostPhaseProfiler* const profiler = profiler_;
    const std::uint64_t now = profiler->host_clock_();
    if (started_nanoseconds_ > now) {
        ++profiler->host_clock_regressions_;
    } else {
        const std::uint64_t raw_elapsed = now - started_nanoseconds_;
        const std::uint64_t overhead = profiler->sample_clock_pair_overhead_nanoseconds_;
        const std::uint64_t elapsed = raw_elapsed > overhead ? raw_elapsed - overhead : 0;
        auto& totals = profiler->phases_[static_cast<std::size_t>(phase_)];
        totals.sampled_nanoseconds = saturating_add(totals.sampled_nanoseconds, elapsed);
        totals.maximum_sampled_nanoseconds = std::max(elapsed, totals.maximum_sampled_nanoseconds);
    }
    profiler_ = nullptr;
}

void EeHostPhaseProfiler::finish_phase(const EeHostPhase phase, const std::uint64_t started_nanoseconds) noexcept {
    const std::uint64_t now = host_clock_();
    if (now < started_nanoseconds) {
        ++host_clock_regressions_;
        return;
    }
    const std::uint64_t raw_elapsed = now - started_nanoseconds;
    const std::uint64_t overhead = sample_clock_pair_overhead_nanoseconds_;
    const std::uint64_t elapsed = overhead < raw_elapsed ? raw_elapsed - overhead : 0;
    PhaseTotals& totals = phases_[static_cast<std::size_t>(phase)];
    totals.sampled_nanoseconds = saturating_add(totals.sampled_nanoseconds, elapsed);
    totals.maximum_sampled_nanoseconds = std::max(elapsed, totals.maximum_sampled_nanoseconds);
}

EeHostPhaseProfiler::EeHostPhaseProfiler(const std::uint64_t sample_interval, const HostNanosecondsClock host_clock)
    : sample_interval_(sample_interval), sample_interval_mask_(sample_interval - 1U) {
    host_clock_ = host_clock != nullptr ? host_clock : steady_host_nanoseconds;
    host_timestamp_nanoseconds_ = 0;
    host_nanoseconds_ = 0;
    host_timing_pauses_ = 0;
    host_timing_excluded_nanoseconds_ = 0;
    host_clock_regressions_ = 0;
    sample_clock_pair_overhead_nanoseconds_ = 0;
    host_timing_started_ = false;
    host_timing_running_ = false;
    host_timing_stopped_ = false;
    if (sample_interval == 0 || (sample_interval_mask_ & sample_interval) != 0) {
        throw std::invalid_argument("EE host phase sample interval must be a nonzero power of two");
    }
    for (std::size_t index = 0; index < phases_.size(); ++index) {
        std::uint64_t value = (static_cast<std::uint64_t>(index) + 1U) * 11400714819323198485ULL;
        value ^= value >> 30U;
        value *= 13787848793156543929ULL;
        value ^= value >> 27U;
        value *= 10723151780598845931ULL;
        value ^= value >> 31U;
        phases_[index].next_sample_ordinal = sample_interval_mask_ & value;
    }
    sample_clock_pair_overhead_nanoseconds_ = 18446744073709551615ULL;
    for (std::size_t sample = 0; sample != 64U; ++sample) {
        const std::uint64_t begin = host_clock_();
        const std::uint64_t end = host_clock_();
        if (begin <= end) {
            sample_clock_pair_overhead_nanoseconds_ = std::min(end - begin, sample_clock_pair_overhead_nanoseconds_);
        }
    }
    if (sample_clock_pair_overhead_nanoseconds_ == 18446744073709551615ULL) {
        sample_clock_pair_overhead_nanoseconds_ = 0;
    }
}

std::uint64_t EeHostPhaseProfiler::steady_host_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void EeHostPhaseProfiler::start_host_timing() {
    if (host_timing_started_ || host_timing_stopped_) {
        throw std::logic_error("EE host phase timing may be started only once");
    }
    host_timing_started_ = true;
    host_timing_running_ = true;
    host_timestamp_nanoseconds_ = host_clock_();
}

std::uint64_t EeHostPhaseProfiler::host_elapsed_since_last_timestamp() noexcept {
    const std::uint64_t now = host_clock_();
    std::uint64_t elapsed = 0;
    if (host_timestamp_nanoseconds_ > now) {
        ++host_clock_regressions_;
    } else {
        elapsed = now - host_timestamp_nanoseconds_;
    }
    host_timestamp_nanoseconds_ = now;
    return elapsed;
}

void EeHostPhaseProfiler::pause_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_ || !host_timing_running_) { return; }
    const std::uint64_t now = host_clock_();
    std::uint64_t elapsed = 0;
    if (now < host_timestamp_nanoseconds_) {
        ++host_clock_regressions_;
    } else {
        elapsed = now - host_timestamp_nanoseconds_;
    }
    host_timestamp_nanoseconds_ = now;
    host_nanoseconds_ = saturating_add(host_nanoseconds_, elapsed);
    host_timing_running_ = false;
    ++host_timing_pauses_;
}

void EeHostPhaseProfiler::resume_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_ || host_timing_running_) { return; }
    const std::uint64_t now = host_clock_();
    if (now >= host_timestamp_nanoseconds_) {
        const std::uint64_t gap = now - host_timestamp_nanoseconds_;
        host_timing_excluded_nanoseconds_ = saturating_add(host_timing_excluded_nanoseconds_, gap);
    } else {
        ++host_clock_regressions_;
    }
    host_timestamp_nanoseconds_ = now;
    host_timing_running_ = true;
}

void EeHostPhaseProfiler::stop_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_) { return; }
    const std::uint64_t now = host_clock_();
    if (host_timing_running_) {
        std::uint64_t elapsed = 0;
        if (now < host_timestamp_nanoseconds_) {
            ++host_clock_regressions_;
        } else {
            elapsed = now - host_timestamp_nanoseconds_;
        }
        host_timestamp_nanoseconds_ = now;
        host_nanoseconds_ = saturating_add(host_nanoseconds_, elapsed);
    } else {
        if (now >= host_timestamp_nanoseconds_) {
            const std::uint64_t gap = now - host_timestamp_nanoseconds_;
            host_timing_excluded_nanoseconds_ = saturating_add(host_timing_excluded_nanoseconds_, gap);
        } else {
            ++host_clock_regressions_;
        }
        host_timestamp_nanoseconds_ = now;
    }
    host_timing_running_ = false;
    host_timing_stopped_ = true;
}

EeHostPhaseProfiler::Scope EeHostPhaseProfiler::begin_phase(const EeHostPhase phase) {
    const auto index = static_cast<std::size_t>(phase);
    if (index >= phases_.size()) {
        throw std::invalid_argument("cannot time the EE host phase sentinel");
    }
    if (!host_timing_started_ || host_timing_stopped_) {
        throw std::logic_error("EE host phase timing must be active before beginning a phase");
    }
    if (!host_timing_running_) {
        return Scope{};
    }
    PhaseTotals& totals = phases_[index];
    const std::uint64_t call = totals.calls;
    totals.calls = call + 1U;
    if (call != totals.next_sample_ordinal) {
        return Scope{};
    }
    const std::uint64_t block = call / sample_interval_;
    std::uint64_t next_ordinal = 18446744073709551615ULL;
    if (18446744073709551615ULL / sample_interval_ > block) {
        const std::uint64_t next_block = block + 1U;
        std::uint64_t value = ((static_cast<std::uint64_t>(index) + 1U) * 11400714819323198485ULL) ^ next_block;
        value ^= value >> 30U;
        value *= 13787848793156543929ULL;
        value ^= value >> 27U;
        value *= 10723151780598845931ULL;
        value ^= value >> 31U;
        const std::uint64_t next_offset = sample_interval_mask_ & value;
        next_ordinal = sample_interval_ * next_block + next_offset;
    }
    totals.next_sample_ordinal = next_ordinal;
    ++totals.timed_calls;
    return Scope(this, phase, host_clock_());
}

EeHostPhaseProfilerStatistics EeHostPhaseProfiler::statistics() const noexcept {
    EeHostPhaseProfilerStatistics result{};
    result.sample_interval = sample_interval_;
    result.host_nanoseconds = host_nanoseconds_;
    result.host_timing_pauses = host_timing_pauses_;
    result.host_timing_excluded_nanoseconds = host_timing_excluded_nanoseconds_;
    result.host_clock_regressions = host_clock_regressions_;
    result.sample_clock_pair_overhead_nanoseconds = sample_clock_pair_overhead_nanoseconds_;
    for (const PhaseTotals& totals : phases_) {
        result.estimated_profiled_nanoseconds = saturating_add(
            result.estimated_profiled_nanoseconds,
            estimate_phase_nanoseconds(totals.calls, totals.timed_calls, totals.sampled_nanoseconds));
    }
    if (result.host_nanoseconds > result.estimated_profiled_nanoseconds) {
        result.estimated_unattributed_nanoseconds = result.host_nanoseconds - result.estimated_profiled_nanoseconds;
    } else {
        result.estimated_overattributed_nanoseconds = result.estimated_profiled_nanoseconds - result.host_nanoseconds;
    }
    return result;
}

std::vector<EeHostPhaseSample> EeHostPhaseProfiler::ranked_phase_samples() const {
    std::vector<EeHostPhaseSample> ranked;
    ranked.reserve(phases_.size());
    for (std::size_t index = 0; index < phases_.size(); ++index) {
        const PhaseTotals& totals = phases_[index];
        EeHostPhaseSample sample;
        sample.phase = static_cast<EeHostPhase>(index);
        sample.calls = totals.calls;
        sample.timed_calls = totals.timed_calls;
        sample.sampled_nanoseconds = totals.sampled_nanoseconds;
        sample.estimated_nanoseconds =
            estimate_phase_nanoseconds(totals.calls, totals.timed_calls, totals.sampled_nanoseconds);
        sample.maximum_sampled_nanoseconds = totals.maximum_sampled_nanoseconds;
        ranked.push_back(sample);
    }
    std::sort(ranked.begin(), ranked.end(), [](const EeHostPhaseSample& left, const EeHostPhaseSample& right) {
        if (left.estimated_nanoseconds != right.estimated_nanoseconds) {
            return left.estimated_nanoseconds > right.estimated_nanoseconds;
        }
        return left.phase < right.phase;
    });
    return ranked;
}

void prepare_empty_ee_host_phase_profile_directory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument("--ee-host-phase-profile requires a non-empty directory path");
    }
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error || std::filesystem::status(directory, error).type() != std::filesystem::file_type::directory) {
            throw std::invalid_argument("EE host phase profile path exists and is not an inspectable directory: " +
                                        directory.string());
        }
        if (std::filesystem::is_empty(directory, error) && !error) { return; }
        throw std::invalid_argument("EE host phase profile directory must be empty: " + directory.string());
    }
    if (std::filesystem::create_directories(directory, error) && !error) { return; }
    throw std::runtime_error("unable to create EE host phase profile directory: " + error.message());
}

void write_ee_host_phase_profile_artifacts(const std::filesystem::path& directory,
                                           const EeHostPhaseProfiler& profiler) {
    const EeHostPhaseProfilerStatistics statistics = profiler.statistics();
    const std::filesystem::path summary_path = directory / "host-phase-summary.tsv";
    std::ofstream summary = create_artifact(summary_path);
    summary << "field\tvalue\n";
    summary << "sample_interval\t" << statistics.sample_interval << '\n';
    summary << "host_nanoseconds\t" << statistics.host_nanoseconds << '\n';
    summary << "host_timing_pauses\t" << statistics.host_timing_pauses << '\n';
    summary << "host_timing_excluded_nanoseconds\t" << statistics.host_timing_excluded_nanoseconds << '\n';
    summary << "host_clock_regressions\t" << statistics.host_clock_regressions << '\n';
    summary << "sample_clock_pair_overhead_nanoseconds\t" << statistics.sample_clock_pair_overhead_nanoseconds << '\n';
    summary << "estimated_profiled_nanoseconds\t" << statistics.estimated_profiled_nanoseconds << '\n';
    summary << "estimated_unattributed_nanoseconds\t" << statistics.estimated_unattributed_nanoseconds << '\n';
    summary << "estimated_overattributed_nanoseconds\t" << statistics.estimated_overattributed_nanoseconds << '\n';
    summary << "estimator\tsampled-mean-times-exact-call-count\n";
    summary << "sampling_note\testimates-may-exceed-total-host-time-from-sampling-variance\n";
    summary << "nesting\tvif1-includes-vu1-and-xgkick;gif-includes-synchronous-gs-observers\n";
    summary << "vu1_split\trun-with-vu1-profile-for-exact-nested-vu1-execute-time\n";
    verify_complete_write(summary, summary_path);

    const std::filesystem::path phases_path = directory / "host-phases.tsv";
    std::ofstream phases = create_artifact(phases_path);
    phases << "rank\tphase\tcalls\ttimed_calls\tsampled_nanoseconds\testimated_nanoseconds\t"
              "maximum_sampled_nanoseconds\tscope\n";
    const std::vector<EeHostPhaseSample> ranked = profiler.ranked_phase_samples();
    for (std::size_t index = 0; index < ranked.size(); ++index) {
        const EeHostPhaseSample& sample = ranked[index];
        phases << (index + 1U) << '\t' << to_string(sample.phase) << '\t' << sample.calls << '\t'
               << sample.timed_calls << '\t' << sample.sampled_nanoseconds << '\t' << sample.estimated_nanoseconds
               << '\t' << sample.maximum_sampled_nanoseconds << '\t' << ee_host_phase_scope(sample.phase) << '\n';
    }
    verify_complete_write(phases, phases_path);
}

} // namespace takedown::runtime
