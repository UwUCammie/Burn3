#pragma once

#include "takedown/runtime/native_function_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// Snapshot of the runner boundaries that an atomic whole-function translation
// must not cross. A blocked translation is not an error: the scalar reference
// path executes that invocation and naturally visits every scheduler boundary.
struct NativeFunctionPreflightState {
    std::size_t committed_instructions{};
    std::size_t instruction_budget{};
    std::uint64_t ee_cycles{};
    std::uint64_t next_vblank_cycle{};
    std::optional<std::uint64_t> next_alarm_cycle{};
    std::size_t queued_dmac_completions{};
    // Active graphics transfers also advance between scalar instructions, but
    // are distinct from the memory-side completion countdown queue.
    bool gif_dma_in_flight{};
    bool vif1_dma_in_flight{};
    std::size_t pending_dmac_callbacks{};
    std::size_t pending_sif_callbacks{};
    bool sif_callback_poll_can_progress{};
    // SIF DMA batches materialize guest-memory effects at their own deadlines.
    // They remain a barrier even while a command callback is not runnable.
    bool sif_dma_completion_pending{};
    // The earliest of those deadlines, when known: a scheduler-transparent
    // leaf that ends before it cannot skip the completion.
    std::optional<std::uint64_t> next_sif_dma_completion_cycle{};
    // A higher-priority ready guest thread preempts immediately; an
    // equal-priority peer may rotate at the kernel's time-slice boundary.
    bool competing_ready_thread{};
    std::uint32_t pending_intc_causes{};
    bool executing_delay_slot{};
};

enum class NativeFunctionPreflightStatus : std::uint8_t {
    Ready,
    DelaySlot,
    InstructionBudget,
    VblankDeadline,
    AlarmDeadline,
    DmacWork,
    SifWork,
    IntcWork,
    ThreadWork,
};

// Diagnostic sources may overlap. Keep the existing aggregate DmacWork status
// while distinguishing actual transfer/countdown work from latched callbacks.
enum NativeFunctionDmaWorkSource : std::uint8_t {
    NativeDmaNoWork = 0U,
    NativeDmaCompletionQueue = 1U << 0U,
    NativeDmaGifTransfer = 1U << 1U,
    NativeDmaVif1Transfer = 1U << 2U,
    NativeDmaSifTransfer = 1U << 3U,
    NativeDmaLatchedCallback = 1U << 4U,
};

[[nodiscard]] std::uint8_t native_function_dma_work_sources(
    const NativeFunctionTraits& traits, const NativeFunctionPreflightState& state) noexcept;

[[nodiscard]] NativeFunctionPreflightStatus
check_native_function_preflight(const NativeFunctionLimits& limits,
                                const NativeFunctionTraits& traits,
                                const NativeFunctionPreflightState& state) noexcept;

// A dynamic resolver may report a smaller exact cost than its registration's
// conservative ceiling. After a ceiling-only budget/deadline rejection, this
// gate permits that read-only probe only when a one-instruction preflight still
// finds no current scheduler barrier. The runner must preflight the resolved
// cost again before dispatching; this function never authorizes execution.
[[nodiscard]] bool native_function_preflight_allows_exact_cost_probe(
    NativeFunctionPreflightStatus initial_status, const NativeFunctionTraits& traits,
    const NativeFunctionPreflightState& state) noexcept;

[[nodiscard]] std::string_view
native_function_preflight_status_name(NativeFunctionPreflightStatus status) noexcept;

} // namespace takedown::runtime
