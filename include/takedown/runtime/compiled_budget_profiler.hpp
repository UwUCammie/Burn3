#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <tuple>

namespace takedown::runtime {

// Independent reasons the runner would cap a compiled dispatch at one
// instruction. Several may coexist; removing one need not permit fusion.
enum CompiledBudgetLimiter : std::uint32_t {
    StepLimit = 1U << 0U,
    VblankLimit = 1U << 1U,
    AlarmLimit = 1U << 2U,
    RetailTraceLimit = 1U << 3U,
    DelaySlotLimit = 1U << 4U,
    NativeEntryLimit = 1U << 5U,
    DmacCompletionLimit = 1U << 6U,
    DmacCallbackLimit = 1U << 7U,
    SifCallbackLimit = 1U << 8U,
    SifPollLimit = 1U << 9U,
    IntcLimit = 1U << 10U,
    SuccessorEntryLimit = 1U << 11U,
    SifDmaCompletionLimit = 1U << 12U,
    ThreadWorkLimit = 1U << 13U,
    UiSessionBoundaryLimit = 1U << 14U,
};

enum class CompiledLoadGuardState : std::uint8_t {
    Absent,
    Disabled,
    Denied,
    Passed,
};

struct CompiledBudgetSample {
    std::uint32_t pc{};
    std::uint32_t budget{};
    std::uint32_t limiters{};
    std::uint32_t pending_intc_causes{};
    CompiledLoadGuardState load_guard{};
};

// Samples dispatch entries, not retired instructions or host time. Exact
// totals distinguish a one-instruction runner budget from a dispatcher that
// declines to fuse. Call record_sample only when observe_budget returns true.
// The prime interval reduces simple loop-period aliasing, but samples remain
// approximate. Bounded rows prevent diagnostic memory growth on new routes.
class CompiledBudgetProfiler {
  public:
    explicit CompiledBudgetProfiler(std::uint64_t interval = 4093U,
                                    std::size_t maximum_rows = 4096U);
    [[nodiscard]] bool observe_budget(std::uint64_t budget) noexcept;
    void record_sample(const CompiledBudgetSample& sample);
    void write_summary(std::ostream& output) const;
    void write_samples(std::ostream& output) const;

  private:
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t, CompiledLoadGuardState>;
    std::uint64_t interval_{};
    std::uint64_t until_sample_{1U};
    std::size_t maximum_rows_{};
    std::uint64_t dispatches_{};
    std::uint64_t one_instruction_budgets_{};
    std::uint64_t samples_{};
    std::uint64_t overflow_samples_{};
    std::map<Key, std::uint64_t> rows_;
};

} // namespace takedown::runtime
