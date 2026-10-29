#include "takedown/runtime/native_function_dispatcher.hpp"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace takedown::runtime {

NativeFunctionDispatcher::NativeFunctionDispatcher(const NativeFunctionDispatcher& other)
    : entries_(other.entries_) {}

NativeFunctionDispatcher&
NativeFunctionDispatcher::operator=(const NativeFunctionDispatcher& other) {
    if (this != &other) {
        // Cached entry pointers refer to the old map nodes.
        lookup_cache_ = {};
        entries_ = other.entries_;
    }
    return *this;
}

NativeFunctionDispatcher::NativeFunctionDispatcher(NativeFunctionDispatcher&& other)
    : entries_(std::move(other.entries_)) {
    other.lookup_cache_ = {};
}

NativeFunctionDispatcher& NativeFunctionDispatcher::operator=(NativeFunctionDispatcher&& other) {
    if (this != &other) {
        lookup_cache_ = {};
        entries_ = std::move(other.entries_);
        other.lookup_cache_ = {};
    }
    return *this;
}

void NativeFunctionDispatcher::invalidate_lookup_cache_slot(const guest::GuestAddress address) noexcept {
    const auto instruction_address = address >> 2U;
    const auto cache_index =
        (instruction_address ^ (instruction_address >> 12U) ^ (instruction_address >> 24U)) &
        (kLookupCacheSize - 1U);
    lookup_cache_[cache_index] = {};
}

NativeFunctionRegistrationResult
NativeFunctionDispatcher::register_function(const guest::GuestAddress address, std::string name,
                                            const NativeFunctionLimits limits,
                                            const NativeFunctionTraits traits,
                                            NativeFunction implementation) {
    return register_function_impl(address, std::move(name), limits, traits, {}, false,
                                  std::move(implementation));
}

NativeFunctionRegistrationResult
NativeFunctionDispatcher::register_function(const guest::GuestAddress address, std::string name,
                                            const NativeFunctionLimits limits,
                                            const NativeFunctionTraits traits,
                                            NativeFunctionLimitResolver limit_resolver,
                                            NativeFunction implementation) {
    return register_function_impl(address, std::move(name), limits, traits,
                                  std::move(limit_resolver), true, std::move(implementation));
}

NativeFunctionRegistrationResult NativeFunctionDispatcher::register_function_impl(
    const guest::GuestAddress address, std::string name, const NativeFunctionLimits limits,
    const NativeFunctionTraits traits, NativeFunctionLimitResolver limit_resolver,
    const bool require_limit_resolver, NativeFunction implementation) {
    if ((address & 3U) != 0U) {
        return {.status = NativeFunctionRegistrationStatus::UnalignedAddress};
    }
    const auto existing = entries_.lower_bound(address);
    if (existing != entries_.end() && existing->first == address) {
        return {.status = NativeFunctionRegistrationStatus::DuplicateAddress,
                .function = &existing->second.metadata};
    }
    if (name.empty()) {
        return {.status = NativeFunctionRegistrationStatus::EmptyName};
    }
    if (!implementation) {
        return {.status = NativeFunctionRegistrationStatus::EmptyImplementation};
    }
    if (require_limit_resolver && !limit_resolver) {
        return {.status = NativeFunctionRegistrationStatus::EmptyLimitResolver};
    }
    if (limits.maximum_retired_instructions == 0U || limits.maximum_ee_cycles == 0U ||
        (traits.single_step_host_call &&
         (!traits.scheduler_state_transparent || limits.maximum_retired_instructions != 1U ||
          limits.maximum_ee_cycles != 1U))) {
        return {.status = NativeFunctionRegistrationStatus::InvalidExecutionLimits};
    }

    const auto position = entries_.emplace_hint(
        existing, address,
        Entry{
            .metadata =
                {
                    .address = address,
                    .name = std::move(name),
                    .limits = limits,
                    .traits = traits,
                    .has_dynamic_cost_resolver = require_limit_resolver,
                },
            .limit_resolver = std::move(limit_resolver),
            .implementation = std::move(implementation),
        });
    invalidate_lookup_cache_slot(address);
    return {.status = NativeFunctionRegistrationStatus::Registered,
            .function = &position->second.metadata};
}

NativeFunctionPreparation NativeFunctionDispatcher::prepare(const guest::GuestAddress address,
                                                            const guest::R5900State& cpu,
                                                            const guest::EeMemory& memory) const {
    const auto* entry = find_entry(address);
    if (entry == nullptr) {
        return {NativeFunctionPrepareStatus::Fallback, nullptr, {}, false};
    }
    if (!entry->limit_resolver) {
        return {NativeFunctionPrepareStatus::Prepared, &entry->metadata, entry->metadata.limits,
                false};
    }
    const NativeFunctionPrepareContext context{cpu, memory, address};
    const auto resolved = entry->limit_resolver(context);
    if (!resolved) {
        return {NativeFunctionPrepareStatus::Fallback, &entry->metadata, {}, false};
    }
    if (resolved->maximum_retired_instructions == 0U || resolved->maximum_ee_cycles == 0U) {
        throw std::logic_error("native guest function resolver reported zero execution cost");
    }
    if (resolved->maximum_retired_instructions > entry->metadata.limits.maximum_retired_instructions ||
        resolved->maximum_ee_cycles > entry->metadata.limits.maximum_ee_cycles) {
        throw std::logic_error(
            "native guest function resolver exceeded its absolute execution ceiling");
    }
    return {NativeFunctionPrepareStatus::Prepared, &entry->metadata, *resolved, true};
}

NativeFunctionDispatchResult NativeFunctionDispatcher::dispatch(const guest::GuestAddress address,
                                                                guest::R5900State& cpu,
                                                                guest::EeMemory& memory) const {
    return dispatch(prepare(address, cpu, memory), cpu, memory);
}

NativeFunctionDispatchResult
NativeFunctionDispatcher::dispatch(const NativeFunctionPreparation& preparation,
                                   guest::R5900State& cpu, guest::EeMemory& memory) const {
    if (!preparation.prepared()) {
        return {};
    }
    if (preparation.function == nullptr) {
        throw std::logic_error("prepared native guest function has no metadata");
    }
    const auto* entry = find_entry(preparation.function->address);
    if (entry == nullptr || preparation.function != &entry->metadata) {
        throw std::logic_error("native guest function preparation belongs to another dispatcher");
    }
    if (preparation.exact_execution_cost != static_cast<bool>(entry->limit_resolver)) {
        throw std::logic_error("native guest function preparation has the wrong cost contract");
    }
    if (preparation.limits.maximum_retired_instructions == 0U ||
        preparation.limits.maximum_ee_cycles == 0U) {
        throw std::logic_error("prepared native guest function has zero execution limits");
    }
    if (preparation.limits.maximum_retired_instructions >
            entry->metadata.limits.maximum_retired_instructions ||
        preparation.limits.maximum_ee_cycles > entry->metadata.limits.maximum_ee_cycles) {
        throw std::logic_error(
            "prepared native guest function exceeds its absolute execution ceiling");
    }
    if (entry->limit_resolver) {
        const NativeFunctionPrepareContext prepare_context{cpu, memory, entry->metadata.address};
        const auto live_limits = entry->limit_resolver(prepare_context);
        if (!live_limits || !(*live_limits == preparation.limits)) {
            throw std::logic_error(
                "native guest function eligibility or exact cost changed after preparation");
        }
    }

    NativeFunctionContext context{cpu, memory, entry->metadata.address};
    const auto execution = entry->implementation(context);
    if (execution.retired_instructions == 0U || execution.ee_cycles == 0U) {
        throw std::logic_error("native guest function reported zero execution cost");
    }
    if (execution.retired_instructions > entry->metadata.limits.maximum_retired_instructions ||
        execution.ee_cycles > entry->metadata.limits.maximum_ee_cycles) {
        throw std::logic_error("native guest function exceeded its declared execution limits");
    }
    if (preparation.exact_execution_cost) {
        if (execution.retired_instructions != preparation.limits.maximum_retired_instructions ||
            execution.ee_cycles != preparation.limits.maximum_ee_cycles) {
            throw std::logic_error(
                "native guest function did not match its prepared exact execution cost");
        }
    } else if (execution.retired_instructions > preparation.limits.maximum_retired_instructions ||
               execution.ee_cycles > preparation.limits.maximum_ee_cycles) {
        throw std::logic_error("native guest function exceeded its prepared execution limits");
    }
    return {NativeFunctionDispatchStatus::Handled, &entry->metadata, execution};
}

std::vector<NativeFunctionMetadata> NativeFunctionDispatcher::registered_functions() const {
    std::vector<NativeFunctionMetadata> functions;
    functions.reserve(entries_.size());
    for (const auto& [address, entry] : entries_) {
        (void)address;
        functions.push_back(entry.metadata);
    }
    return functions;
}

std::size_t NativeFunctionDispatcher::size() const noexcept { return entries_.size(); }

bool NativeFunctionDispatcher::empty() const noexcept { return entries_.empty(); }

bool native_function_disabled_by_environment(const std::string_view name) {
    const char* list = std::getenv("TAKEDOWN_DISABLE_NATIVE_FUNCTIONS");
    if (list == nullptr) {
        return false;
    }
    std::string_view rest{list};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        if (rest.substr(0, comma) == name) {
            return true;
        }
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1U);
    }
    return false;
}

} // namespace takedown::runtime
