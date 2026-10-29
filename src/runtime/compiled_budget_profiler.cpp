#include "takedown/runtime/compiled_budget_profiler.hpp"

#include <array>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <stdexcept>
#include <string_view>

namespace takedown::runtime {

namespace {

constexpr std::array<std::string_view, 4> guard_names{
    "absent", "disabled", "denied", "passed"};

// Indexed by the bit position of CompiledBudgetLimiter.
constexpr std::array<std::string_view, 15> limiter_names{
    "step",
    "vblank",
    "alarm",
    "retail_trace",
    "delay_slot",
    "native_entry",
    "dmac_completion",
    "dmac_callback",
    "sif_callback",
    "sif_poll",
    "intc",
    "successor_entry",
    "sif_dma_completion",
    "thread_work",
    "ui_session_boundary"};

} // namespace

CompiledBudgetProfiler::CompiledBudgetProfiler(const std::uint64_t interval,
                                               const std::size_t maximum_rows)
    : interval_(interval), maximum_rows_(maximum_rows) {
    if ((interval == 0U) | (maximum_rows == 0U)) {
        throw std::invalid_argument(
            "compiled budget sampling requires nonzero bounds");
    }
}

bool CompiledBudgetProfiler::observe_budget(const std::uint64_t budget) noexcept {
    ++dispatches_;
    one_instruction_budgets_ += static_cast<std::uint64_t>(budget == 1U);
    if (--until_sample_ != 0U) {
        return false;
    }
    until_sample_ = interval_;
    return true;
}

void CompiledBudgetProfiler::record_sample(const CompiledBudgetSample& sample) {
    const Key key{sample.pc, sample.budget, sample.limiters,
                  sample.pending_intc_causes, sample.load_guard};
    ++samples_;
    if (const auto found = rows_.find(key); found != rows_.end()) {
        ++found->second;
    } else if (rows_.size() < maximum_rows_) {
        rows_.emplace(key, 1U);
    } else {
        ++overflow_samples_;
    }
}

void CompiledBudgetProfiler::write_summary(std::ostream& output) const {
    output << "metric\tcount\n"
           << "sample_interval_dispatches\t" << interval_ << '\n'
           << "dispatch_attempts\t" << dispatches_ << '\n'
           << "one_instruction_budgets\t" << one_instruction_budgets_ << '\n'
           << "samples\t" << samples_ << '\n'
           << "rows\t" << rows_.size() << '\n'
           << "overflow_samples\t" << overflow_samples_ << '\n';
}

void CompiledBudgetProfiler::write_samples(std::ostream& output) const {
    output << "pc\tbudget\tlimiter_mask\tlimiters\tpending_intc_causes\t"
              "load_guard\tsamples\n";
    const auto flags = output.flags();
    const auto fill = output.fill();
    for (const auto& [key, count] : rows_) {
        const auto& [pc, budget, limiters, pending, guard] = key;
        output << "0x" << std::hex << std::setw(8) << std::setfill('0') << pc
               << '\t' << std::dec << budget << "\t0x" << std::hex << limiters
               << '\t';
        bool separator = false;
        for (std::size_t bit = 0U; bit != limiter_names.size(); ++bit) {
            if (((limiters >> bit) & 1U) != 0U) {
                output << (separator ? "," : "") << limiter_names[bit];
                separator = true;
            }
        }
        if (!separator) {
            output << "none";
        }
        output << "\t0x" << std::hex << pending << '\t'
               << guard_names.at(static_cast<std::size_t>(guard)) << '\t'
               << std::dec << count << '\n';
    }
    output.flags(flags);
    output.fill(fill);
}

} // namespace takedown::runtime
