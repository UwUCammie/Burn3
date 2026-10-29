#pragma once

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <cstdint>

namespace takedown::runtime {

// Reasons which keep the graphics-deadline pilot on the established
// per-instruction callback path. The planner combines their bit values
// because a diagnostic run can have more than one independent observer active
// at once; recording every active reason makes the opt-in result auditable.
enum class JoinPumpGraphicsDeadlineFallback : std::uint32_t {
    Deadline,
    ExistingBudget,
    ProfileTiming,
    GraphicsDiagnostics,
    EePcSampling,
    SubframeInput,
    Count = 6U,
};

[[nodiscard]] constexpr std::uint32_t join_pump_graphics_deadline_fallback_bit(
    const JoinPumpGraphicsDeadlineFallback fallback) noexcept {
    return 1U << static_cast<std::uint32_t>(fallback);
}

[[nodiscard]] constexpr bool has_join_pump_graphics_deadline_fallback(
    const std::uint32_t mask,
    const JoinPumpGraphicsDeadlineFallback fallback) noexcept {
    return (mask & join_pump_graphics_deadline_fallback_bit(fallback)) != 0U;
}

struct JoinPumpGraphicsDeadlinePlanInput {
    bool enabled{};
    guest::EeGraphicsServiceDeadline deadline{};
    // These are the two independent capacities already selected by the
    // runner. A JoinPump instruction costs one EE cycle, so the planner clips
    // both dimensions before applying the graphics deadline.
    std::uint64_t existing_instruction_capacity{};
    std::uint64_t existing_cycle_capacity{};
    std::uint64_t maximum_region_capacity{};
    bool profile_timing_active{};
    bool graphics_diagnostics_active{};
    bool ee_pc_sampling_active{};
    bool subframe_input_active{};
};

struct JoinPumpGraphicsDeadlinePlan {
    bool admitted{};
    std::uint32_t instruction_capacity{};
    std::uint32_t cycle_capacity{};
    std::uint32_t fallback_mask{};
};

// Selects the callback-less join-pump slice. This is deliberately a pure
// helper: it does not query EeMemory or mutate runner state, so the exact
// InternalDeadline > 1 admission and capacity clipping remain directly
// testable without a retail ELF.
[[nodiscard]] constexpr JoinPumpGraphicsDeadlinePlan plan_join_pump_graphics_deadline(
    const JoinPumpGraphicsDeadlinePlanInput& input) noexcept {
    if (!input.enabled) {
        return {};
    }

    if (!input.deadline.has_internal_deadline() || input.deadline.cycles_until_event <= 1U) {
        return {.fallback_mask = join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::Deadline)};
    }

    const auto existing_span_capacity = std::min(
        {input.existing_instruction_capacity, input.existing_cycle_capacity,
         input.maximum_region_capacity});
    if (existing_span_capacity <= 1U) {
        return {.fallback_mask = join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::ExistingBudget)};
    }

    std::uint32_t fallback_mask{};
    if (input.profile_timing_active) {
        fallback_mask |= join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::ProfileTiming);
    }
    if (input.graphics_diagnostics_active) {
        fallback_mask |= join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::GraphicsDiagnostics);
    }
    if (input.ee_pc_sampling_active) {
        fallback_mask |= join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::EePcSampling);
    }
    if (input.subframe_input_active) {
        fallback_mask |= join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::SubframeInput);
    }
    if (fallback_mask != 0U) {
        return {.fallback_mask = fallback_mask};
    }

    const auto cycle_capacity = std::min(
        {input.existing_instruction_capacity, input.existing_cycle_capacity,
         input.deadline.cycles_until_event, input.maximum_region_capacity});
    if (cycle_capacity <= 1U) {
        // This can only occur when a future change supplies inconsistent
        // capacities, but keeping the check here makes the helper fail closed
        // if the caller's existing budget is changed later.
        return {.fallback_mask = join_pump_graphics_deadline_fallback_bit(
            JoinPumpGraphicsDeadlineFallback::ExistingBudget)};
    }

    const auto instruction_capacity = std::min<std::uint64_t>(
        input.existing_instruction_capacity, input.maximum_region_capacity);
    return {
        .admitted = true,
        .instruction_capacity = static_cast<std::uint32_t>(instruction_capacity),
        .cycle_capacity = static_cast<std::uint32_t>(cycle_capacity),
        .fallback_mask = 0U,
    };
}

} // namespace takedown::runtime
