#include "takedown/runtime/slus_21050_engine_functions.hpp"

#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_engine_function_table.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace takedown::runtime {

std::uint64_t original_code_fnv1a64(const guest::EeMemory& memory, const guest::GuestAddress address,
                                    const std::uint32_t bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint32_t index = 0U; index < bytes; ++index) {
        hash ^= memory.read_u8(address + index);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::span<const EngineFunction> slus_21050_engine_functions() {
    static const std::vector<EngineFunction> functions = [] {
        std::vector<EngineFunction> all;
        for (const auto group : slus_21050_engine_function_groups()) {
            all.insert(all.end(), group.begin(), group.end());
        }
        return all;
    }();
    return functions;
}

// RECOVERY: the original initialises the first (limit-less) lambda's closure
// field by field inside the std::function buffer; this source builds the same
// closure but GCC prints its two stores as typed memory writes instead.
EngineFunctionRegistration register_slus_21050_engine_functions(NativeFunctionDispatcher& dispatcher,
                                                                const guest::EeMemory& memory) {
    EngineFunctionRegistration result;
    for (const EngineFunction& function : slus_21050_engine_functions()) {
        if (native_function_disabled_by_environment(function.name)) {
            ++result.disabled;
            continue;
        }
        if (original_code_fnv1a64(memory, function.address, function.code_bytes) != function.code_fnv1a64) {
            ++result.revision_mismatches;
            continue;
        }
        const auto ceiling = function.cost;
        auto implementation = function.implementation;
        const NativeFunctionTraits traits{
            .scheduler_state_transparent = true,
            .single_step_host_call = false,
            .defers_dmac_completions = true,
        };
        auto call_cost = function.call_cost;
        const auto live_cost = std::make_shared<std::uint32_t>(0U);
        const NativeFunctionRegistrationResult registration = call_cost == nullptr
            ? dispatcher.register_function(
                function.address, std::string(function.name), NativeFunctionLimits{ceiling, ceiling}, traits,
                [implementation, ceiling](NativeFunctionContext& context) {
                    NativeFunctionExecution execution = implementation(context);
                    execution.retired_instructions = std::min(execution.retired_instructions, ceiling);
                    execution.ee_cycles = std::min(execution.ee_cycles, ceiling);
                    return execution;
                })
            : dispatcher.register_function(
                function.address, std::string(function.name), NativeFunctionLimits{ceiling, ceiling}, traits,
                [call_cost, ceiling, live_cost](const NativeFunctionPrepareContext& context)
                    -> std::optional<NativeFunctionLimits> {
                    const std::uint32_t cost = std::min(call_cost(context), ceiling);
                    *live_cost = cost;
                    if (cost == 0U) {
                        return std::nullopt;
                    }
                    return NativeFunctionLimits{cost, cost};
                },
                [implementation, live_cost](NativeFunctionContext& context) {
                    const std::uint32_t cost = *live_cost;
                    static_cast<void>(implementation(context));
                    return NativeFunctionExecution{cost, cost};
                });
        if (registration.registered()) {
            ++result.registered;
        }
    }
    return result;
}

} // namespace takedown::runtime
