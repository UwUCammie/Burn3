#include "takedown/runtime/compiled_block_dispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace takedown::runtime {
namespace {

// TAKEDOWN_CFLOW=1 turns on the control-flow taps in dispatch(); they only print.
[[nodiscard]] bool cflow_enabled(const char level) {
    const char* value = std::getenv("TAKEDOWN_CFLOW");
    return value != nullptr && *value == level;
}

// RECOVERY: in the original GIMPLE the non-fault-status throw block sits between the first
// VU0 consistency test and its throw; only that block order differs here (also inlined into
// dispatch_one).
// Checks the accounting a generated block returned against the budget it was
// given and against the CompiledBlockExecutionResult contract.
void validate_execution_result(const CompiledBlockExecutionResult& execution,
                               const CompiledBlockBudget& budget) {
    if (execution.retired_instructions > budget.maximum_retired_instructions ||
        execution.ee_cycles > budget.maximum_ee_cycles) {
        throw std::logic_error("compiled guest block exceeded its supplied execution budget");
    }

    const bool retired_zero = execution.retired_instructions == 0U;
    const bool cycles_zero = execution.ee_cycles == 0U;
    if (retired_zero != cycles_zero) {
        throw std::logic_error("compiled guest block returned unpaired execution cost");
    }
    if (execution.retired_instructions > execution.ee_cycles) {
        throw std::logic_error("compiled guest block retired more instructions than EE cycles");
    }

    const bool is_fault = execution.exit == CompiledBlockExit::Fault;
    if (is_fault != execution.fault.has_value()) {
        throw std::logic_error("compiled guest block returned inconsistent fault information");
    }
    if (is_fault) {
        const auto& fault = *execution.fault;
        switch (fault.status) {
        case guest::EeStepStatus::UnsupportedInstruction:
        case guest::EeStepStatus::UnsupportedOperandCase:
        case guest::EeStepStatus::ArithmeticOverflow:
        case guest::EeStepStatus::MemoryAccessFault:
        case guest::EeStepStatus::FetchFault:
        case guest::EeStepStatus::Vu0MicroExecutionFault: {
            const bool is_vu0_fault =
                fault.status == guest::EeStepStatus::Vu0MicroExecutionFault;
            if (fault.vu0_micro.has_value() != is_vu0_fault) {
                throw std::logic_error(
                    "compiled guest block returned inconsistent VU0 fault details");
            }
            if (fault.vu0_micro.has_value() &&
                fault.vu0_micro->status == guest::Vu0MicroRunStatus::Completed) {
                throw std::logic_error(
                    "compiled guest block reported a completed VU0 run as a fault");
            }
            break;
        }
        default:
            throw std::logic_error("compiled guest block used a non-fault EE status for a fault");
        }
    }

    if (execution.exit == CompiledBlockExit::Safepoint && execution.retired_instructions == 0U) {
        throw std::logic_error("compiled guest block reached a safepoint without retiring time");
    }
    if (execution.exit == CompiledBlockExit::BudgetExhausted &&
        execution.retired_instructions == 0U) {
        throw std::logic_error("compiled guest block exhausted a positive budget without progress");
    }
}

} // namespace

CompiledBlockDispatcher::CompiledBlockDispatcher(const CompiledBlockDispatcher& other)
    : entries_(other.entries_),
      fusion_statistics_enabled_(other.fusion_statistics_enabled_),
      fusion_statistics_(other.fusion_statistics_),
      load_fusion_enabled_(other.load_fusion_enabled_) {}

CompiledBlockDispatcher& CompiledBlockDispatcher::operator=(const CompiledBlockDispatcher& other) {
    if (this != &other) {
        lookup_cache_ = {};
        entries_ = other.entries_;
        fusion_statistics_enabled_ = other.fusion_statistics_enabled_;
        fusion_statistics_ = other.fusion_statistics_;
        load_fusion_enabled_ = other.load_fusion_enabled_;
    }
    return *this;
}

CompiledBlockDispatcher::CompiledBlockDispatcher(CompiledBlockDispatcher&& other)
    : entries_(std::move(other.entries_)),
      fusion_statistics_enabled_(std::exchange(other.fusion_statistics_enabled_, false)),
      fusion_statistics_(std::exchange(other.fusion_statistics_, {})),
      load_fusion_enabled_(std::exchange(other.load_fusion_enabled_, false)) {
    other.lookup_cache_ = {};
}

CompiledBlockDispatcher& CompiledBlockDispatcher::operator=(CompiledBlockDispatcher&& other) {
    if (this != &other) {
        lookup_cache_ = {};
        entries_ = std::move(other.entries_);
        fusion_statistics_enabled_ = std::exchange(other.fusion_statistics_enabled_, false);
        fusion_statistics_ = std::exchange(other.fusion_statistics_, {});
        load_fusion_enabled_ = std::exchange(other.load_fusion_enabled_, false);
        other.lookup_cache_ = {};
    }
    return *this;
}

void CompiledBlockDispatcher::invalidate_lookup_cache_slot(const guest::GuestAddress address) noexcept {
    const auto instruction_address = address >> 2U;
    const auto cache_index =
        (instruction_address ^ (instruction_address >> 12U) ^ (instruction_address >> 24U)) &
        (kLookupCacheSize - 1U);
    lookup_cache_[cache_index] = {};
}

CompiledBlockRegistrationResult CompiledBlockDispatcher::register_block(
    const guest::GuestAddress address, std::string name, CompiledBlockImplementation implementation,
    const std::optional<std::uint32_t> expected_retail_word, const CompiledBlockTraits traits,
    const CompiledBlockSpan span) {
    if ((address & 3U) != 0U) {
        return {.status = CompiledBlockRegistrationStatus::UnalignedAddress};
    }
    const auto existing = entries_.lower_bound(address);
    if (existing != entries_.end() && existing->first == address) {
        return {.status = CompiledBlockRegistrationStatus::DuplicateAddress,
                .block = &existing->second.metadata};
    }
    if (name.empty()) {
        return {.status = CompiledBlockRegistrationStatus::EmptyName};
    }
    if (!implementation) {
        return {.status = CompiledBlockRegistrationStatus::EmptyImplementation};
    }
    // A span must cover at least two instructions, belong to a straight-line
    // fusible entry, and stay inside the 32-bit guest address space.
    if (!span && span.maximum_instructions == 0U) {
        // No span requested.
    } else {
        const auto maximum_offset =
            span.maximum_instructions > 1U
                ? static_cast<std::uint64_t>(span.maximum_instructions - 1U) * 4U
                : std::numeric_limits<std::uint64_t>::max();
        if (!span || span.maximum_instructions <= 1U || !traits.straight_line_fusible ||
            std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint64_t>(address) <
                maximum_offset) {
            return {.status = CompiledBlockRegistrationStatus::InvalidSpan};
        }
    }

    DirectImplementation direct_implementation = nullptr;
    if (const auto* target = implementation.target<DirectImplementation>()) {
        direct_implementation = *target;
    }
    const auto inserted = entries_.emplace_hint(
        existing, address,
        Entry{
            .metadata = {.address = address,
                         .name = std::move(name),
                         .expected_retail_word = expected_retail_word,
                         .traits = traits},
            .implementation = std::move(implementation),
            .direct_implementation = direct_implementation,
            .span = span,
        });
    invalidate_lookup_cache_slot(address);
    return {.status = CompiledBlockRegistrationStatus::Registered,
            .block = &inserted->second.metadata};
}

CompiledBlockDispatchResult CompiledBlockDispatcher::dispatch(
    const guest::GuestAddress address, guest::R5900State& cpu, guest::EeMemory& memory,
    const CompiledBlockBudget& budget, const CompiledBlockScalarBarrier scalar_barrier,
    const CompiledBlockProgressCheckpoint progress_checkpoint) const {
    return dispatch(prepare(address), cpu, memory, budget, scalar_barrier, progress_checkpoint);
}

CompiledBlockDispatchResult CompiledBlockDispatcher::dispatch(
    const CompiledBlockPreparation& preparation, guest::R5900State& cpu, guest::EeMemory& memory,
    const CompiledBlockBudget& budget, const CompiledBlockScalarBarrier scalar_barrier,
    const CompiledBlockProgressCheckpoint progress_checkpoint) const {
    if (!preparation.prepared()) {
        return {};
    }
    if (preparation.owner_ != this || preparation.entry_ == nullptr) {
        throw std::logic_error("compiled block preparation belongs to another dispatcher");
    }
    const auto* const entry = static_cast<const Entry*>(preparation.entry_);
    if (&entry->metadata != preparation.block_) {
        throw std::logic_error("compiled block preparation has inconsistent metadata");
    }
    const auto address = entry->metadata.address;
    if (address != cpu.pc()) {
        throw std::invalid_argument("compiled block dispatch address does not match the live EE PC");
    }

    if (address == 0x2ac610U) {
        static const bool tap = cflow_enabled('1');
        if (tap) {
            const auto loader = cpu.read_gpr_u32(4U);
            const auto a1 = cpu.read_gpr_u32(5U);
            std::fprintf(stderr, "[CFLOW-UP] loader=0x%08x a1=0x%08x\n", loader, a1);
            if (loader != 0U) {
                for (std::uint32_t offset = 0x38U; offset < 0xb0U; offset += 4U) {
                    if (memory.contains(loader + offset, 4U)) {
                        std::fprintf(stderr, "[CFLOW-UP] +%03x=0x%08x\n", offset,
                                     memory.read_u32_le(loader + offset));
                    }
                }
            }
        }
    } else if (address == 0x2abd90U) {
        static const bool tap = cflow_enabled('1');
        if (tap) {
            std::fprintf(stderr, "[CFLOW-TEARDOWN] a0=0x%08x a1=0x%08x\n", cpu.read_gpr_u32(4U),
                         cpu.read_gpr_u32(5U));
        }
    }

    if (scalar_barrier != nullptr && scalar_barrier(address)) {
        record_fusion_stop(FusionStopReason::ScalarBarrier);
        return {.status = CompiledBlockDispatchStatus::ScalarBarrier, .block = &entry->metadata};
    }
    if (entry->metadata.expected_retail_word.has_value() &&
        (!memory.contains(address, 4U) ||
         memory.read_u32_le(address) != *entry->metadata.expected_retail_word)) {
        record_fusion_stop(FusionStopReason::OuterLiveWordMismatch);
        return {.status = CompiledBlockDispatchStatus::CodeMismatch, .block = &entry->metadata};
    }
    if (budget.maximum_retired_instructions == 0U || budget.maximum_ee_cycles == 0U) {
        record_fusion_stop(FusionStopReason::ZeroCapacityBudget);
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = {}};
    }

    const bool entered_delay_slot = cpu.is_executing_delay_slot();
    const bool sequential_slot_entry = cpu.next_pc() == cpu.pc() + 4U;
    const CompiledBlockBudget span_budget{
        .maximum_retired_instructions =
            std::min(budget.maximum_retired_instructions, entry->span.maximum_instructions),
        .maximum_ee_cycles = std::min(budget.maximum_ee_cycles, entry->span.maximum_instructions),
    };
    if (entry->span &&
        (!entered_delay_slot || (delay_slot_dispatch_enabled_ && sequential_slot_entry)) &&
        span_budget.maximum_retired_instructions > 1U && span_budget.maximum_ee_cycles > 1U) {
        const auto span_result = entry->span.implementation(
            cpu, memory, span_budget,
            CompiledBlockExecutionServices{.scalar_barrier = scalar_barrier,
                                           .progress_checkpoint = progress_checkpoint});
        const auto& span_execution = span_result.execution;
        validate_execution_result(span_execution, span_budget);
        if (span_execution.retired_instructions > entry->span.maximum_instructions) {
            record_fusion_stop(FusionStopReason::TraitInvariantViolation);
            throw std::logic_error(
                "compiled guest span exceeded its registered instruction capacity");
        }
        const auto sequential_offset =
            static_cast<std::uint64_t>(span_execution.retired_instructions) * 4U;
        const auto address_capacity = 0xffffffffULL - static_cast<std::uint64_t>(address);
        if (sequential_offset > address_capacity ||
            cpu.pc() != address + static_cast<std::uint32_t>(sequential_offset)) {
            record_fusion_stop(FusionStopReason::TraitInvariantViolation);
            throw std::logic_error(
                "compiled guest span did not stop at its exact committed-prefix PC");
        }
        const auto fused_successors = span_execution.retired_instructions -
                                      static_cast<std::uint32_t>(span_execution.retired_instructions != 0U);
        switch (span_result.stop_reason) {
        case CompiledBlockSpanStopReason::FusedBudget:
            record_fusion_stop(FusionStopReason::FusedBudget, fused_successors);
            break;
        case CompiledBlockSpanStopReason::ProgressCheckpoint:
            record_fusion_stop(FusionStopReason::ProgressCheckpoint, fused_successors);
            break;
        case CompiledBlockSpanStopReason::ScalarBarrier:
            record_fusion_stop(FusionStopReason::ScalarBarrier, fused_successors);
            break;
        case CompiledBlockSpanStopReason::MissingSuccessor:
            record_fusion_stop(FusionStopReason::MissingSuccessor, fused_successors);
            break;
        case CompiledBlockSpanStopReason::NonfusibleSuccessor:
            record_fusion_stop(FusionStopReason::NonfusibleSuccessor, fused_successors);
            break;
        case CompiledBlockSpanStopReason::SuccessorLiveWordMismatch:
            record_fusion_stop(FusionStopReason::SuccessorLiveWordMismatch, fused_successors);
            break;
        case CompiledBlockSpanStopReason::PostInstructionDelaySlot:
            record_fusion_stop(FusionStopReason::PostInstructionDelaySlot, fused_successors);
            break;
        case CompiledBlockSpanStopReason::ExecutionBoundary:
            record_fusion_stop(FusionStopReason::FirstResultNotSafepointOrOneToOne,
                               fused_successors);
            break;
        }
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = span_execution};
    }

    const bool fusible = can_fuse(entry->metadata.traits, cpu, memory);
    const CompiledBlockExecutionResult first_execution =
        entry->direct_implementation != nullptr
            ? entry->direct_implementation(cpu, memory, budget)
            : entry->implementation(cpu, memory, budget);
    validate_execution_result(first_execution, budget);
    CompiledBlockExecutionResult execution = first_execution;

    if (!fusible) {
        record_fusion_stop(FusionStopReason::OuterNotFusible);
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = execution};
    }
    if (entered_delay_slot && !(delay_slot_dispatch_enabled_ && sequential_slot_entry)) {
        record_fusion_stop(FusionStopReason::EnteredDelaySlot);
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = execution};
    }
    if (execution.exit != CompiledBlockExit::Safepoint || execution.retired_instructions != 1U ||
        execution.ee_cycles != 1U) {
        record_fusion_stop(FusionStopReason::FirstResultNotSafepointOrOneToOne);
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = execution};
    }
    if (address > std::numeric_limits<guest::GuestAddress>::max() - 4U || cpu.pc() != address + 4U) {
        record_fusion_stop(FusionStopReason::TraitInvariantViolation);
        throw std::logic_error("straight-line-fusible block violated its registered trait");
    }
    if (progress_checkpoint &&
        !progress_checkpoint.callback(progress_checkpoint.context, execution.retired_instructions,
                                      execution.ee_cycles)) {
        record_fusion_stop(FusionStopReason::ProgressCheckpoint);
        entry->reached = true;
        return {.status = CompiledBlockDispatchStatus::Executed,
                .block = &entry->metadata,
                .execution = execution};
    }

    std::uint32_t fused_successors = 0U;
    for (;;) {
        if (execution.retired_instructions >= budget.maximum_retired_instructions ||
            execution.ee_cycles >= budget.maximum_ee_cycles) {
            record_fusion_stop(fused_successors == 0U ? FusionStopReason::OneInstructionBudget
                                                      : FusionStopReason::FusedBudget,
                               fused_successors);
            break;
        }
        if (cpu.is_executing_delay_slot()) {
            record_fusion_stop(FusionStopReason::PostInstructionDelaySlot, fused_successors);
            break;
        }
        if (scalar_barrier != nullptr && scalar_barrier(cpu.pc())) {
            record_fusion_stop(FusionStopReason::ScalarBarrier, fused_successors);
            break;
        }
        const Entry* next = find_entry(cpu.pc());
        if (next == nullptr) {
            record_fusion_stop(FusionStopReason::MissingSuccessor, fused_successors);
            break;
        }
        if (!can_fuse(next->metadata.traits, cpu, memory)) {
            record_fusion_stop(FusionStopReason::NonfusibleSuccessor, fused_successors);
            break;
        }
        const auto expected_word = next->metadata.expected_retail_word;
        if (expected_word.has_value() &&
            (!memory.contains(cpu.pc(), 4U) || memory.read_u32_le(cpu.pc()) != *expected_word)) {
            record_fusion_stop(FusionStopReason::SuccessorLiveWordMismatch, fused_successors);
            break;
        }

        const CompiledBlockBudget remaining{
            .maximum_retired_instructions =
                budget.maximum_retired_instructions - execution.retired_instructions,
            .maximum_ee_cycles = budget.maximum_ee_cycles - execution.ee_cycles,
        };
        const auto next_pc_before = cpu.pc();
        const CompiledBlockExecutionResult next_execution =
            next->direct_implementation != nullptr
                ? next->direct_implementation(cpu, memory, remaining)
                : next->implementation(cpu, memory, remaining);
        validate_execution_result(next_execution, remaining);
        if (next_execution.exit != CompiledBlockExit::Safepoint ||
            next_execution.retired_instructions != 1U || next_execution.ee_cycles != 1U ||
            next_pc_before > std::numeric_limits<guest::GuestAddress>::max() - 4U ||
            cpu.pc() != next_pc_before + 4U) {
            record_fusion_stop(FusionStopReason::TraitInvariantViolation, fused_successors);
            throw std::logic_error("straight-line-fusible block violated its registered trait");
        }
        ++fused_successors;
        execution.retired_instructions += 1U;
        execution.ee_cycles += 1U;
        if (progress_checkpoint &&
            !progress_checkpoint.callback(progress_checkpoint.context,
                                          execution.retired_instructions, execution.ee_cycles)) {
            record_fusion_stop(FusionStopReason::ProgressCheckpoint, fused_successors);
            break;
        }
    }

    entry->reached = true;
    return {.status = CompiledBlockDispatchStatus::Executed,
            .block = &entry->metadata,
            .execution = execution};
}

CompiledBlockDispatchResult CompiledBlockDispatcher::dispatch_one(
    const guest::GuestAddress address, guest::R5900State& cpu, guest::EeMemory& memory,
    const CompiledBlockScalarBarrier scalar_barrier) const {
    return dispatch_one(prepare(address), cpu, memory, scalar_barrier);
}

CompiledBlockDispatchResult CompiledBlockDispatcher::dispatch_one(
    const CompiledBlockPreparation& preparation, guest::R5900State& cpu, guest::EeMemory& memory,
    const CompiledBlockScalarBarrier scalar_barrier) const {
    if (!preparation.prepared()) {
        return {};
    }
    if (preparation.owner_ != this || preparation.entry_ == nullptr) {
        throw std::logic_error("compiled block preparation belongs to another dispatcher");
    }
    const auto* const entry = static_cast<const Entry*>(preparation.entry_);
    if (&entry->metadata != preparation.block_) {
        throw std::logic_error("compiled block preparation has inconsistent metadata");
    }
    const auto address = entry->metadata.address;
    if (address != cpu.pc()) {
        throw std::invalid_argument("compiled block dispatch address does not match the live EE PC");
    }

    if (address == 0x2ab500U) {
        static const bool tap = cflow_enabled('1');
        if (tap) {
            const auto node = cpu.read_gpr_u32(4U);
            if (node != 0U && memory.contains(node + 0x30U, 4U)) {
                std::fprintf(stderr, "[CFLOW-PUMP] node=0x%08x slot30=0x%08x state104=%u\n", node,
                             memory.read_u32_le(node + 0x30U), memory.read_u32_le(node + 0x68U));
            }
        }
    }

    if (scalar_barrier != nullptr && scalar_barrier(address)) {
        record_fusion_stop(FusionStopReason::ScalarBarrier);
        return {.status = CompiledBlockDispatchStatus::ScalarBarrier, .block = &entry->metadata};
    }
    if (entry->metadata.expected_retail_word.has_value() &&
        (!memory.contains(address, 4U) ||
         memory.read_u32_le(address) != *entry->metadata.expected_retail_word)) {
        record_fusion_stop(FusionStopReason::OuterLiveWordMismatch);
        return {.status = CompiledBlockDispatchStatus::CodeMismatch, .block = &entry->metadata};
    }

    constexpr CompiledBlockBudget kOneInstructionBudget{1U, 1U};
    const bool entered_delay_slot = cpu.is_executing_delay_slot();
    const bool sequential_slot_entry = cpu.next_pc() == cpu.pc() + 4U;
    const bool fusible = can_fuse(entry->metadata.traits, cpu, memory);
    const CompiledBlockExecutionResult execution =
        entry->direct_implementation != nullptr
            ? entry->direct_implementation(cpu, memory, kOneInstructionBudget)
            : entry->implementation(cpu, memory, kOneInstructionBudget);
    validate_execution_result(execution, kOneInstructionBudget);

    if (!fusible) {
        record_fusion_stop(FusionStopReason::OuterNotFusible);
    } else if (entered_delay_slot && !(delay_slot_dispatch_enabled_ && sequential_slot_entry)) {
        record_fusion_stop(FusionStopReason::EnteredDelaySlot);
    } else if (execution.exit != CompiledBlockExit::Safepoint ||
               execution.retired_instructions != 1U || execution.ee_cycles != 1U) {
        record_fusion_stop(FusionStopReason::FirstResultNotSafepointOrOneToOne);
    } else {
        if (address > std::numeric_limits<guest::GuestAddress>::max() - 4U ||
            cpu.pc() != address + 4U) {
            record_fusion_stop(FusionStopReason::TraitInvariantViolation);
            throw std::logic_error("straight-line-fusible block violated its registered trait");
        }
        record_fusion_stop(FusionStopReason::OneInstructionBudget);
    }
    entry->reached = true;
    return {.status = CompiledBlockDispatchStatus::Executed,
            .block = &entry->metadata,
            .execution = execution};
}

std::vector<CompiledBlockMetadata> CompiledBlockDispatcher::registered_blocks() const {
    std::vector<CompiledBlockMetadata> blocks;
    blocks.reserve(entries_.size());
    for (const auto& [address, entry] : entries_) {
        static_cast<void>(address);
        blocks.push_back(entry.metadata);
    }
    return blocks;
}

std::size_t CompiledBlockDispatcher::reached_entry_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [](const auto& entry) { return entry.second.reached; }));
}

std::size_t CompiledBlockDispatcher::size() const noexcept {
    return entries_.size();
}

bool CompiledBlockDispatcher::empty() const noexcept {
    return entries_.empty();
}

} // namespace takedown::runtime
