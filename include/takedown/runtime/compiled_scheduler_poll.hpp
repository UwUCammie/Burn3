#pragma once

#include "takedown/guest/ee_kernel.hpp"
#include "takedown/guest/ee_memory.hpp"

#include <cstdint>

namespace takedown::runtime {

struct CompiledSchedulerPollState {
    bool sif_dma_completion_pending{};
    bool competing_ready_thread{};
    bool pending_callback_work{};

    [[nodiscard]] bool requires_scalar_poll(const bool relaxed_pending_poll) const noexcept {
        // These two deadlines can change RAM or the running CPU context even
        // without a latched callback. They cannot share the experimental
        // relaxation of already-pending callback polls.
        return sif_dma_completion_pending || competing_ready_thread ||
               (pending_callback_work && !relaxed_pending_poll);
    }
};

// Read-only boundary detection for resumable direct execution. Reaching a SIF
// deadline must return control to the outer scheduler; it must not publish
// kernel time or materialize the completion before the scheduler has processed
// graphics, input, SPU2, and same-cycle VBlank work in its established order.
[[nodiscard]] inline bool sif_dma_completion_reached(
    const guest::EeKernelHle& kernel, const std::uint64_t boundary_cycle) noexcept {
    const auto deadline = kernel.next_sif_dma_completion_deadline();
    return deadline.has_value() && *deadline <= boundary_cycle;
}

// Capture the runner's read-only scheduler decision after its current boundary
// has been serviced. The inert-DMAC proof may suppress only its own callback
// cause; independent timed SIF DMA and thread work must retain scalar polling.
[[nodiscard]] inline CompiledSchedulerPollState inspect_compiled_scheduler_poll(
    const guest::EeMemory& memory, const guest::EeKernelHle& kernel,
    const bool inert_dmac_poll) noexcept {
    return {
        .sif_dma_completion_pending = kernel.sif_dma_completion_pending(),
        .competing_ready_thread = kernel.has_competing_ready_thread(),
        .pending_callback_work =
            memory.queued_dmac_completion_count() != 0U ||
            (kernel.pending_dmac_callback_count() != 0U && !inert_dmac_poll) ||
            kernel.pending_sif_command_callback_count() != 0U ||
            kernel.sif_command_callback_poll_can_progress() ||
            memory.pending_intc_causes() != 0U,
    };
}

} // namespace takedown::runtime
