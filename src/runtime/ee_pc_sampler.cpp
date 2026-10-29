#include "takedown/runtime/ee_pc_sampler.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace takedown::runtime {
namespace {

// Saturating add: counters that overflow stick at the maximum instead of wrapping.
void add_saturating(std::uint64_t& total, std::uint64_t value) noexcept {
    total = value <= std::numeric_limits<std::uint64_t>::max() - total
                ? total + value
                : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] std::string hex_address(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

void verify_complete_write(std::ofstream& output, const std::filesystem::path& path) {
    output.flush();
    if (!output) {
        throw std::runtime_error("unable to write complete EE PC profile artifact: " +
                                 path.string());
    }
}

[[nodiscard]] std::ofstream create_artifact(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("refusing to overwrite EE PC profile artifact: " +
                                 path.string());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to create EE PC profile artifact: " + path.string());
    }
    return output;
}

} // namespace

std::string_view to_string(const EePcSampleKind kind) noexcept {
    switch (kind) {
    case EePcSampleKind::Scalar:
        return "scalar";
    case EePcSampleKind::CompiledBlock:
        return "compiled";
    case EePcSampleKind::NativeFunction:
        return "native";
    case EePcSampleKind::Hle:
        return "hle";
    case EePcSampleKind::Count:
        break;
    }
    return "unknown";
}

EePcSampler::EePcSampler(const std::uint64_t interval, HostNanosecondsClock host_clock)
    : interval_(interval),
      interval_mask_(interval - 1U),
      host_clock_(host_clock != nullptr ? host_clock : &EePcSampler::steady_host_nanoseconds) {
    if (interval != 0U && (interval & interval_mask_) == 0U) {
        statistics_.interval = interval;
    } else {
        throw std::invalid_argument("EE PC sample interval must be a nonzero power of two");
    }
}

std::uint64_t EePcSampler::steady_host_nanoseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void EePcSampler::start_host_timing() {
    if (host_timing_started_ || host_timing_stopped_) {
        throw std::logic_error("EE PC host timing may be started only once");
    }
    host_timing_started_ = true;
    host_timing_running_ = true;
    host_timestamp_nanoseconds_ = host_clock_();
}

std::uint64_t EePcSampler::host_elapsed_since_last_timestamp() noexcept {
    const auto now = host_clock_();
    if (host_timestamp_nanoseconds_ > now) {
        ++statistics_.host_clock_regressions;
        host_timestamp_nanoseconds_ = now;
        return 0U;
    }
    const auto elapsed = now - host_timestamp_nanoseconds_;
    host_timestamp_nanoseconds_ = now;
    return elapsed;
}

void EePcSampler::pause_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_ || !host_timing_running_) {
        return;
    }
    add_saturating(pending_host_nanoseconds_, host_elapsed_since_last_timestamp());
    host_timing_running_ = false;
    ++statistics_.host_timing_pauses;
}

void EePcSampler::resume_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_ || host_timing_running_) {
        return;
    }
    const auto now = host_clock_();
    if (now >= host_timestamp_nanoseconds_) {
        add_saturating(statistics_.host_timing_excluded_nanoseconds,
                       now - host_timestamp_nanoseconds_);
    } else {
        ++statistics_.host_clock_regressions;
    }
    host_timestamp_nanoseconds_ = now;
    host_timing_running_ = true;
}

void EePcSampler::stop_host_timing() {
    if (!host_timing_started_ || host_timing_stopped_) {
        return;
    }
    if (host_timing_running_) {
        add_saturating(pending_host_nanoseconds_, host_elapsed_since_last_timestamp());
    } else {
        const auto now = host_clock_();
        if (now >= host_timestamp_nanoseconds_) {
            add_saturating(statistics_.host_timing_excluded_nanoseconds,
                           now - host_timestamp_nanoseconds_);
        } else {
            ++statistics_.host_clock_regressions;
        }
        host_timestamp_nanoseconds_ = now;
    }
    statistics_.host_timing_tail_nanoseconds = pending_host_nanoseconds_;
    pending_host_nanoseconds_ = 0U;
    host_timing_running_ = false;
    host_timing_stopped_ = true;
}

void EePcSampler::attribute_host_window(const std::uint64_t key) noexcept {
    if (!host_timing_running_) {
        return;
    }
    add_saturating(pending_host_nanoseconds_, host_elapsed_since_last_timestamp());
    SampleTotals& totals = samples_[key];
    add_saturating(totals.host_nanoseconds, pending_host_nanoseconds_);
    add_saturating(statistics_.host_nanoseconds, pending_host_nanoseconds_);
    pending_host_nanoseconds_ = 0U;
    ++totals.host_windows;
    ++statistics_.host_timing_windows;
}

std::uint64_t EePcSampler::sample_key(const guest::GuestAddress pc,
                                    const EePcSampleKind kind) noexcept {
    return (static_cast<std::uint64_t>(kind) << 32U) | static_cast<std::uint64_t>(pc);
}

void EePcSampler::observe_span(const std::uint64_t first_instruction,
                               const guest::GuestAddress dispatch_pc,
                               const std::uint64_t retired_instructions,
                               const EePcSampleKind kind) {
    if (retired_instructions == 0U) {
        return;
    }
    if (kind >= EePcSampleKind::Count) {
        throw std::invalid_argument("cannot record the EE PC sample-kind sentinel");
    }
    ++statistics_.spans_observed;
    if (statistics_.retired_instructions != first_instruction) {
        ++statistics_.ordinal_discontinuities;
    }
    const std::uint64_t end =
        retired_instructions <= std::numeric_limits<std::uint64_t>::max() - first_instruction
            ? first_instruction + retired_instructions
            : std::numeric_limits<std::uint64_t>::max();
    statistics_.retired_instructions = std::max(statistics_.retired_instructions, end);

    // Samples land on the first instruction ordinal at or after each boundary.
    if (first_instruction > ~interval_mask_) {
        return;
    }
    const auto first_sample = (first_instruction + interval_mask_) & ~interval_mask_;
    if (end <= first_sample) {
        return;
    }
    const auto sample_count = (end - first_sample - 1U) / interval_ + 1U;
    const auto key = sample_key(dispatch_pc, kind);
    samples_[key].samples += sample_count;
    statistics_.samples += sample_count;
    attribute_host_window(key);
}

EePcSamplerStatistics EePcSampler::statistics() const noexcept {
    return statistics_;
}

std::vector<EePcSample> EePcSampler::ranked_pc_samples() const {
    std::vector<EePcSample> ranked;
    ranked.reserve(samples_.size());
    for (const auto& [key, totals] : samples_) {
        ranked.push_back({
            .pc = static_cast<guest::GuestAddress>(key),
            .kind = static_cast<EePcSampleKind>(key >> 32U),
            .samples = totals.samples,
            .host_nanoseconds = totals.host_nanoseconds,
            .host_windows = totals.host_windows,
        });
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.samples != right.samples) {
            return left.samples > right.samples;
        }
        if (left.pc != right.pc) {
            return left.pc < right.pc;
        }
        return left.kind < right.kind;
    });
    return ranked;
}

std::vector<EePcSample> EePcSampler::ranked_host_pc_samples() const {
    auto ranked = ranked_pc_samples();
    std::erase_if(ranked, [](const EePcSample& sample) { return sample.host_windows == 0U; });
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.host_nanoseconds != right.host_nanoseconds) {
            return left.host_nanoseconds > right.host_nanoseconds;
        }
        if (left.host_windows != right.host_windows) {
            return left.host_windows > right.host_windows;
        }
        if (left.pc != right.pc) {
            return left.pc < right.pc;
        }
        return left.kind < right.kind;
    });
    return ranked;
}

std::vector<EePcRangeSample> EePcSampler::ranked_range_samples() const {
    std::unordered_map<guest::GuestAddress,
                       std::array<std::uint64_t, static_cast<std::size_t>(EePcSampleKind::Count)>>
        ranges;
    for (const auto& sample : ranked_pc_samples()) {
        const auto begin = static_cast<guest::GuestAddress>(sample.pc & ~(kRangeBytes - 1U));
        ranges[begin][static_cast<std::size_t>(sample.kind)] += sample.samples;
    }

    std::vector<EePcRangeSample> ranked;
    ranked.reserve(ranges.size());
    for (const auto& [begin, by_kind] : ranges) {
        EePcRangeSample sample{
            .begin = begin,
            .end = static_cast<std::uint64_t>(begin) + kRangeBytes,
            .by_kind = by_kind,
        };
        for (const auto count : by_kind) {
            sample.samples += count;
        }
        ranked.push_back(sample);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.samples != right.samples) {
            return left.samples > right.samples;
        }
        return left.begin < right.begin;
    });
    return ranked;
}

std::vector<EePcRangeSample> EePcSampler::ranked_host_range_samples() const {
    std::unordered_map<guest::GuestAddress, EePcRangeSample> ranges;
    for (const auto& sample : ranked_host_pc_samples()) {
        const auto begin = static_cast<guest::GuestAddress>(sample.pc & ~(kRangeBytes - 1U));
        auto& range = ranges[begin];
        range.begin = begin;
        range.end = static_cast<std::uint64_t>(begin) + kRangeBytes;
        range.by_kind[static_cast<std::size_t>(sample.kind)] += sample.samples;
        range.samples += sample.samples;
        range.host_nanoseconds += sample.host_nanoseconds;
        range.host_windows += sample.host_windows;
    }

    std::vector<EePcRangeSample> ranked;
    ranked.reserve(ranges.size());
    for (const auto& [begin, range] : ranges) {
        ranked.push_back(range);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.host_nanoseconds != right.host_nanoseconds) {
            return left.host_nanoseconds > right.host_nanoseconds;
        }
        return left.begin < right.begin;
    });
    return ranked;
}

void prepare_empty_ee_pc_profile_directory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        throw std::invalid_argument("--ee-pc-profile requires a non-empty directory path");
    }
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error || !std::filesystem::is_directory(directory, error)) {
            throw std::invalid_argument(
                "EE PC profile path exists and is not an inspectable directory: " +
                directory.string());
        }
        if (!std::filesystem::is_empty(directory, error) || error) {
            throw std::invalid_argument("EE PC profile directory must be empty: " +
                                        directory.string());
        }
        return;
    }
    if (!std::filesystem::create_directories(directory, error) || error) {
        throw std::runtime_error("unable to create EE PC profile directory: " + error.message());
    }
}

void write_ee_pc_profile_artifacts(const std::filesystem::path& directory,
                               const EePcSampler& sampler) {
    const std::filesystem::path summary_path = directory / "summary.tsv";
    auto summary = create_artifact(summary_path);
    const auto statistics = sampler.statistics();
    summary << "field\tvalue\n"
            << "sample_interval\t" << statistics.interval << '\n'
            << "range_bytes\t" << EePcSampler::kRangeBytes << '\n'
            << "retired_instructions\t" << statistics.retired_instructions << '\n'
            << "spans_observed\t" << statistics.spans_observed << '\n'
            << "samples\t" << statistics.samples << '\n'
            << "ordinal_discontinuities\t" << statistics.ordinal_discontinuities << '\n'
            << "host_timing_windows\t" << statistics.host_timing_windows << '\n'
            << "host_nanoseconds\t" << statistics.host_nanoseconds << '\n'
            << "host_timing_pauses\t" << statistics.host_timing_pauses << '\n'
            << "host_timing_excluded_nanoseconds\t"
            << statistics.host_timing_excluded_nanoseconds << '\n'
            << "host_timing_tail_nanoseconds\t" << statistics.host_timing_tail_nanoseconds
            << '\n'
            << "host_clock_regressions\t" << statistics.host_clock_regressions << '\n';
    verify_complete_write(summary, summary_path);

    const std::filesystem::path pcs_path = directory / "hot-pcs.tsv";
    auto pcs = create_artifact(pcs_path);
    pcs << "rank\tkind\tpc\tsamples\tattribution\n";
    const auto ranked_pcs = sampler.ranked_pc_samples();
    for (std::size_t index = 0; index < ranked_pcs.size(); ++index) {
        const auto& sample = ranked_pcs[index];
        const bool exact =
            sample.kind == EePcSampleKind::Scalar || sample.kind == EePcSampleKind::Hle;
        pcs << index + 1U << '\t' << to_string(sample.kind) << '\t' << hex_address(sample.pc)
            << '\t' << sample.samples << '\t' << (exact ? "exact-pc" : "dispatch-entry")
            << '\n';
    }
    verify_complete_write(pcs, pcs_path);

    const std::filesystem::path ranges_path = directory / "hot-ranges.tsv";
    auto ranges = create_artifact(ranges_path);
    ranges << "rank\tbegin\tend\tsamples\tscalar\tcompiled\tnative\thle\n";
    const auto ranked_ranges = sampler.ranked_range_samples();
    for (std::size_t index = 0; index < ranked_ranges.size(); ++index) {
        const auto& sample = ranked_ranges[index];
        ranges << index + 1U << '\t' << hex_address(sample.begin) << '\t'
               << hex_address(sample.end) << '\t' << sample.samples;
        for (const auto count : sample.by_kind) {
            ranges << '\t' << count;
        }
        ranges << '\n';
    }
    verify_complete_write(ranges, ranges_path);

    const std::filesystem::path host_pcs_path = directory / "hot-host-pcs.tsv";
    auto host_pcs = create_artifact(host_pcs_path);
    host_pcs << "rank\tkind\tpc\thost_nanoseconds\twindows\tsamples\tattribution\n";
    const auto ranked_host_pcs = sampler.ranked_host_pc_samples();
    for (std::size_t index = 0; index < ranked_host_pcs.size(); ++index) {
        const auto& sample = ranked_host_pcs[index];
        host_pcs << index + 1U << '\t' << to_string(sample.kind) << '\t'
                 << hex_address(sample.pc) << '\t' << sample.host_nanoseconds << '\t'
                 << sample.host_windows << '\t' << sample.samples
                 << "\tpreceding-window-ending-at-pc\n";
    }
    verify_complete_write(host_pcs, host_pcs_path);

    const std::filesystem::path host_ranges_path = directory / "hot-host-ranges.tsv";
    auto host_ranges = create_artifact(host_ranges_path);
    host_ranges << "rank\tbegin\tend\thost_nanoseconds\twindows\tsamples\tscalar\tcompiled\t"
                   "native\thle\tattribution\n";
    const auto ranked_host_ranges = sampler.ranked_host_range_samples();
    for (std::size_t index = 0; index < ranked_host_ranges.size(); ++index) {
        const auto& sample = ranked_host_ranges[index];
        host_ranges << index + 1U << '\t' << hex_address(sample.begin) << '\t'
                    << hex_address(sample.end) << '\t' << sample.host_nanoseconds << '\t'
                    << sample.host_windows << '\t' << sample.samples;
        for (const auto count : sample.by_kind) {
            host_ranges << '\t' << count;
        }
        host_ranges << "\tpreceding-window-ending-in-range\n";
    }
    verify_complete_write(host_ranges, host_ranges_path);
}

} // namespace takedown::runtime
