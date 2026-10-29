#pragma once

#include "takedown/guest/ee_kernel.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/compiled_block_dispatcher.hpp"

namespace takedown::runtime {

// A latched DMAC cause is inert across work which cannot alter COP0 status,
// handler/channel registration, or the interrupted CPU context. Device
// countdowns, SIF deadlines, thread scheduling, and the work's own effect class
// remain separate admission barriers at the caller.
[[nodiscard]] inline bool pending_dmac_callback_is_inert(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const guest::EeKernelHle& kernel) noexcept {
    return !cpu.is_executing_delay_slot() &&
           kernel.pending_dmac_callback_count() != 0U &&
           memory.queued_dmac_completion_count() == 0U &&
           !kernel.has_competing_ready_thread() &&
           !kernel.pending_dmac_callback_can_dispatch(cpu);
}

// Admission for the default-off compiled-budget experiment. Other runner
// gates remain independent. Ordinary RAM reads cannot change callback
// eligibility; stores/COP0/syscalls/control flow never join such a batch.
[[nodiscard]] inline bool can_elide_inert_dmac_poll(
    const guest::R5900State& cpu, const guest::EeMemory& memory,
    const guest::EeKernelHle& kernel, const CompiledBlockTraits& traits,
    const bool load_fusion_enabled) noexcept {
    if (!pending_dmac_callback_is_inert(cpu, memory, kernel) ||
        kernel.sif_dma_completion_pending()) {
        return false;
    }
    return traits.straight_line_fusible ||
           (load_fusion_enabled && traits.load_fusion_guard != nullptr &&
            traits.load_fusion_guard(cpu, memory));
}

} // namespace takedown::runtime
