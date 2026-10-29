#include "takedown/runtime/native_function_preflight.hpp"

namespace takedown::runtime {

namespace {

bool fits_before_vblank(std::uint64_t now, std::uint64_t deadline, std::uint32_t cost) {
    return deadline >= now && cost <= deadline - now;
}

bool fits_before_alarm(std::uint64_t now, const std::optional<std::uint64_t>& deadline,
                       std::uint32_t cost) {
    return !deadline || (now <= *deadline && cost <= *deadline - now);
}

} // namespace

std::uint8_t native_function_dma_work_sources(const NativeFunctionTraits& traits,
                                              const NativeFunctionPreflightState& state) noexcept {
    return static_cast<std::uint8_t>(
        (state.queued_dmac_completions != 0U && !traits.defers_dmac_completions
             ? NativeDmaCompletionQueue
             : NativeDmaNoWork) |
        (!traits.scheduler_state_transparent && state.gif_dma_in_flight ? NativeDmaGifTransfer : NativeDmaNoWork) |
        (!traits.scheduler_state_transparent && state.vif1_dma_in_flight ? NativeDmaVif1Transfer : NativeDmaNoWork) |
        (state.sif_dma_completion_pending ? NativeDmaSifTransfer : NativeDmaNoWork) |
        (!traits.scheduler_state_transparent && state.pending_dmac_callbacks != 0U ? NativeDmaLatchedCallback
                                                           : NativeDmaNoWork));
}

NativeFunctionPreflightStatus
check_native_function_preflight(const NativeFunctionLimits& limits,
                                const NativeFunctionTraits& traits,
                                const NativeFunctionPreflightState& state) noexcept {
    if (state.executing_delay_slot) {
        return NativeFunctionPreflightStatus::DelaySlot;
    }
    if (state.committed_instructions > state.instruction_budget ||
        limits.maximum_retired_instructions >
            state.instruction_budget - state.committed_instructions) {
        return NativeFunctionPreflightStatus::InstructionBudget;
    }
    if (!fits_before_vblank(state.ee_cycles, state.next_vblank_cycle,
                            limits.maximum_ee_cycles)) {
        return NativeFunctionPreflightStatus::VblankDeadline;
    }
    if (!fits_before_alarm(state.ee_cycles, state.next_alarm_cycle, limits.maximum_ee_cycles)) {
        return NativeFunctionPreflightStatus::AlarmDeadline;
    }
    if (traits.single_step_host_call && traits.scheduler_state_transparent &&
        limits.maximum_retired_instructions == 1U && limits.maximum_ee_cycles == 1U) {
        return NativeFunctionPreflightStatus::Ready;
    }
    const bool sif_dma_completes_after_call =
        traits.scheduler_state_transparent && state.next_sif_dma_completion_cycle &&
        state.ee_cycles <= *state.next_sif_dma_completion_cycle &&
        limits.maximum_ee_cycles <= *state.next_sif_dma_completion_cycle - state.ee_cycles;
    if ((state.queued_dmac_completions != 0U && !traits.defers_dmac_completions) ||
        (state.sif_dma_completion_pending && !sif_dma_completes_after_call)) {
        return NativeFunctionPreflightStatus::DmacWork;
    }
    if (!traits.scheduler_state_transparent &&
        (state.gif_dma_in_flight || state.vif1_dma_in_flight)) {
        return NativeFunctionPreflightStatus::DmacWork;
    }
    if (state.competing_ready_thread) {
        return NativeFunctionPreflightStatus::ThreadWork;
    }
    if (state.sif_callback_poll_can_progress) {
        return NativeFunctionPreflightStatus::SifWork;
    }
    if (!traits.scheduler_state_transparent) {
        if (state.pending_dmac_callbacks != 0U) {
            return NativeFunctionPreflightStatus::DmacWork;
        }
        if (state.pending_sif_callbacks != 0U) {
            return NativeFunctionPreflightStatus::SifWork;
        }
        if (state.pending_intc_causes != 0U) {
            return NativeFunctionPreflightStatus::IntcWork;
        }
    }
    return NativeFunctionPreflightStatus::Ready;
}

bool native_function_preflight_allows_exact_cost_probe(
    const NativeFunctionPreflightStatus initial_status, const NativeFunctionTraits& traits,
    const NativeFunctionPreflightState& state) noexcept {
    switch (initial_status) {
    case NativeFunctionPreflightStatus::Ready:
    case NativeFunctionPreflightStatus::DelaySlot:
    case NativeFunctionPreflightStatus::DmacWork:
    case NativeFunctionPreflightStatus::SifWork:
    case NativeFunctionPreflightStatus::IntcWork:
    case NativeFunctionPreflightStatus::ThreadWork:
        return false;
    default:
        break;
    }
    const NativeFunctionLimits kMinimumNativeExecution{1U, 1U};
    return check_native_function_preflight(kMinimumNativeExecution, traits, state) ==
           NativeFunctionPreflightStatus::Ready;
}

std::string_view
native_function_preflight_status_name(const NativeFunctionPreflightStatus status) noexcept {
    switch (status) {
    case NativeFunctionPreflightStatus::Ready:
        return "ready";
    case NativeFunctionPreflightStatus::DelaySlot:
        return "delay-slot";
    case NativeFunctionPreflightStatus::InstructionBudget:
        return "step-budget";
    case NativeFunctionPreflightStatus::VblankDeadline:
        return "vblank-deadline";
    case NativeFunctionPreflightStatus::AlarmDeadline:
        return "alarm-deadline";
    case NativeFunctionPreflightStatus::DmacWork:
        return "dmac-work";
    case NativeFunctionPreflightStatus::SifWork:
        return "sif-work";
    case NativeFunctionPreflightStatus::IntcWork:
        return "intc-work";
    case NativeFunctionPreflightStatus::ThreadWork:
        return "thread-work";
    }
    return "unknown";
}

} // namespace takedown::runtime
