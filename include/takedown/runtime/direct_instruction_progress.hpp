#pragma once

#include "takedown/guest/ee_kernel.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/compiled_scheduler_poll.hpp"
#include "takedown/runtime/ee_graphics_service_diagnostics.hpp"
#include "takedown/runtime/ee_host_phase_profiler.hpp"
#include "takedown/runtime/graphics_timing.hpp"

#include <cstdint>

namespace takedown::runtime {

// Shared post-instruction device checkpoint for resumable direct execution.
// Graphics producers advance immediately because their completion can affect
// the next instruction. A reached SIF deadline only requests a return to the
// outer scheduler, which owns its ordered completion effects.
struct GraphicsDmacInstructionProgress {
    using BoundaryDrain = bool (*)(void*) noexcept;

    guest::EeMemory* memory{};
    guest::EeKernelHle* kernel{};
    guest::EeKernelHle* diagnostic_kernel{};
    EeHostPhaseProfiler* profiler{};
    EeGraphicsServiceDiagnostics* diagnostics{};
    EeGraphicsCallbackRoute callback_route{EeGraphicsCallbackRoute::Unknown};
    std::uint64_t dispatch_start_cycle{};
    std::uint64_t* vif1_serviced_cycle{};
    std::uint64_t* gif_serviced_cycle{};
    void* boundary_drain_context{};
    BoundaryDrain boundary_drain{};
};

template <bool DrainBoundary>
inline bool service_graphics_after_direct_instruction_impl(
    void* opaque, const std::uint32_t, const std::uint32_t cumulative_ee_cycles) {
    auto& progress = *static_cast<GraphicsDmacInstructionProgress*>(opaque);
    const auto boundary_cycle = progress.dispatch_start_cycle + cumulative_ee_cycles;
    bool boundary_clean = true;
    const auto drain_boundary = [&progress]() noexcept {
        if constexpr (DrainBoundary) {
            if (progress.boundary_drain != nullptr) {
                return progress.boundary_drain(progress.boundary_drain_context);
            }
        }
        return true;
    };
    const auto advance_vif1 = [&progress, &boundary_clean](const std::uint64_t cycles) {
        if constexpr (DrainBoundary) {
            if (progress.boundary_drain != nullptr) {
                boundary_clean = progress.boundary_drain(progress.boundary_drain_context) &&
                                 boundary_clean;
            }
        }
        if (progress.profiler != nullptr) [[unlikely]] {
            auto phase = progress.profiler->begin_phase(EeHostPhase::Vif1DmacVu1Inclusive);
            progress.memory->advance_vif1_dmac(cycles);
            if constexpr (DrainBoundary) {
                if (progress.boundary_drain != nullptr) {
                    boundary_clean =
                        progress.boundary_drain(progress.boundary_drain_context) &&
                        boundary_clean;
                }
            }
            return;
        }
        progress.memory->advance_vif1_dmac(cycles);
        if constexpr (DrainBoundary) {
            if (progress.boundary_drain != nullptr) {
                boundary_clean = progress.boundary_drain(progress.boundary_drain_context) &&
                                 boundary_clean;
            }
        }
    };
    const auto advance_gif = [&progress, &boundary_clean](const std::uint64_t cycles) {
        if (progress.profiler != nullptr) [[unlikely]] {
            auto phase = progress.profiler->begin_phase(EeHostPhase::GifDmacStreamInclusive);
            progress.memory->advance_gif_dmac(cycles);
            if constexpr (DrainBoundary) {
                if (progress.boundary_drain != nullptr) {
                    boundary_clean =
                        progress.boundary_drain(progress.boundary_drain_context) &&
                        boundary_clean;
                }
            }
            return;
        }
        progress.memory->advance_gif_dmac(cycles);
        if constexpr (DrainBoundary) {
            if (progress.boundary_drain != nullptr) {
                boundary_clean = progress.boundary_drain(progress.boundary_drain_context) &&
                                 boundary_clean;
            }
        }
    };
    boundary_clean = drain_boundary() && boundary_clean;
    if (!boundary_clean) {
        return false;
    }
    if constexpr (DrainBoundary) {
        if (boundary_cycle > *progress.vif1_serviced_cycle) {
            advance_vif1(boundary_cycle - *progress.vif1_serviced_cycle);
            *progress.vif1_serviced_cycle = boundary_cycle;
            if (!boundary_clean) {
                return false;
            }
        }
        if (boundary_cycle > *progress.gif_serviced_cycle) {
            advance_gif(boundary_cycle - *progress.gif_serviced_cycle);
            *progress.gif_serviced_cycle = boundary_cycle;
            if (!boundary_clean) {
                return false;
            }
        }
    } else {
        service_graphics_dmacs_until(
            boundary_cycle, *progress.vif1_serviced_cycle,
            *progress.gif_serviced_cycle, advance_vif1, advance_gif);
    }
    boundary_clean = drain_boundary() && boundary_clean;

    const bool sif_completion_reached =
        progress.kernel != nullptr &&
        sif_dma_completion_reached(*progress.kernel, boundary_cycle);

    // Only identify the boundary here. Completion delivery, including its RAM
    // and SPU2 effects, belongs to the outer scheduler after graphics, input,
    // SPU2, and same-cycle VBlank work have retained their established order.
    // This also prevents a final instruction at measurement C from exposing a
    // SIF event which the ordinary one-instruction path would leave unserviced.
    return boundary_clean && progress.memory->queued_dmac_completion_count() == 0U &&
           progress.memory->pending_intc_causes() == 0U && !sif_completion_reached;
}

inline bool service_graphics_after_direct_instruction(
    void* opaque, const std::uint32_t instruction_index,
    const std::uint32_t cumulative_ee_cycles) {
    return service_graphics_after_direct_instruction_impl<false>(
        opaque, instruction_index, cumulative_ee_cycles);
}

inline bool service_graphics_after_direct_instruction_with_boundary_drain(
    void* opaque, const std::uint32_t instruction_index,
    const std::uint32_t cumulative_ee_cycles) {
    return service_graphics_after_direct_instruction_impl<true>(
        opaque, instruction_index, cumulative_ee_cycles);
}

// The scoped exact-window profiler selects this observer wrapper explicitly.
// Ordinary execution continues to call the function above without diagnostic
// cursor reads, callback timing, or result classification.
template <bool DrainBoundary>
inline bool service_profiled_graphics_after_direct_instruction_impl(
    void* opaque, const std::uint32_t instruction_index,
    const std::uint32_t cumulative_ee_cycles) {
    auto& progress = *static_cast<GraphicsDmacInstructionProgress*>(opaque);
    auto* const diagnostics = progress.diagnostics;
    [[maybe_unused]] auto callback_phase = progress.profiler == nullptr
                                               ? EeHostPhaseProfiler::Scope{}
                                               : progress.profiler->begin_phase(
                                                     EeHostPhase::GraphicsServiceCallback);
    std::uint64_t vif1_before = 0U;
    std::uint64_t gif_before = 0U;
    if (diagnostics != nullptr) {
        diagnostics->begin_callback(progress.callback_route);
        vif1_before = *progress.vif1_serviced_cycle;
        gif_before = *progress.gif_serviced_cycle;
    }
    const bool clean = service_graphics_after_direct_instruction_impl<DrainBoundary>(
        opaque, instruction_index, cumulative_ee_cycles);
    if (diagnostics == nullptr) {
        return clean;
    }

    const auto vif1_after = *progress.vif1_serviced_cycle;
    const auto gif_after = *progress.gif_serviced_cycle;
    const bool vif1_advanced = vif1_after > vif1_before;
    const bool gif_advanced = gif_after > gif_before;
    diagnostics->record_callback_advance(
        vif1_advanced, vif1_advanced ? vif1_after - vif1_before : 0U,
        gif_advanced, gif_advanced ? gif_after - gif_before : 0U);

    const auto boundary_cycle = progress.dispatch_start_cycle + cumulative_ee_cycles;
    const bool queued_dmac = progress.memory->queued_dmac_completion_count() != 0U;
    const bool intc = progress.memory->pending_intc_causes() != 0U;
    const bool sif_boundary = progress.diagnostic_kernel != nullptr &&
                              sif_dma_completion_reached(
                                  *progress.diagnostic_kernel, boundary_cycle);
    diagnostics->record_callback_result(clean, queued_dmac, intc, sif_boundary);
    diagnostics->end_callback();
    return clean;
}


inline bool service_profiled_graphics_after_direct_instruction(
    void* opaque, const std::uint32_t instruction_index,
    const std::uint32_t cumulative_ee_cycles) {
    return service_profiled_graphics_after_direct_instruction_impl<false>(
        opaque, instruction_index, cumulative_ee_cycles);
}

inline bool service_profiled_graphics_after_direct_instruction_with_boundary_drain(
    void* opaque, const std::uint32_t instruction_index,
    const std::uint32_t cumulative_ee_cycles) {
    return service_profiled_graphics_after_direct_instruction_impl<true>(
        opaque, instruction_index, cumulative_ee_cycles);
}

} // namespace takedown::runtime
