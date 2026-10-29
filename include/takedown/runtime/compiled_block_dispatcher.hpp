#pragma once

#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace takedown::runtime {

// Maximum guest work a compiled block may perform before returning control to
// the scheduler. A zero in either field is valid: the dispatcher reports
// BudgetExhausted without invoking generated code.
struct CompiledBlockBudget {
    std::uint32_t maximum_retired_instructions{};
    std::uint32_t maximum_ee_cycles{};

    bool operator==(const CompiledBlockBudget&) const = default;
};

enum class CompiledBlockExit : std::uint8_t {
    Safepoint,
    BudgetExhausted,
    GuestSystemCall,
    Fault,
};

// Fault information remains made entirely of guest-domain values so a host
// runner can use the same reporting path for interpreted and compiled code.
// Vu0MicroExecutionFault requires vu0_micro; every other fault status forbids
// it, and a completed VU0 run is not a fault.
struct CompiledBlockFault {
    guest::EeStepStatus status{guest::EeStepStatus::FetchFault};
    guest::DecodedR5900Instruction instruction{};
    std::optional<guest::Vu0MicroRunResult> vu0_micro{};
};

// Exact dynamic cost of the path that just ran, not a static upper bound.
// Instruction and cycle counts are paired (both zero or both nonzero), and the
// current EE timing model requires ee_cycles >= retired_instructions.
struct CompiledBlockExecutionResult {
    CompiledBlockExit exit{CompiledBlockExit::BudgetExhausted};
    std::uint32_t retired_instructions{};
    std::uint32_t ee_cycles{};
    std::optional<CompiledBlockFault> fault{};
};

using CompiledBlockImplementation = std::function<CompiledBlockExecutionResult(
    guest::R5900State&, guest::EeMemory&, const CompiledBlockBudget&)>;

// Optional per-PC fence used by instruction-granular observers. Returning
// true means that exact guest instruction must remain on the scalar path. The
// dispatcher checks the entry PC and every prospective fused successor before
// invoking generated code, so the observer boundary cannot be crossed by a
// straight-line bundle.
using CompiledBlockScalarBarrier = bool (*)(guest::GuestAddress) noexcept;

// Optional instruction-boundary checkpoint for asynchronous scheduler work.
// The dispatcher invokes it after each successfully retired instruction in a
// fusible run, passing cumulative accounting for the current dispatch. Return
// false to stop before the next instruction. This lets a runner advance timed
// devices at the same boundaries as the scalar loop without paying for a full
// dispatcher/scheduler round trip between register-only instructions.
using CompiledBlockProgressCallback = bool (*)(void* context, std::uint32_t retired_instructions,
                                               std::uint32_t ee_cycles);

struct CompiledBlockProgressCheckpoint {
    CompiledBlockProgressCallback callback{};
    void* context{};

    [[nodiscard]] explicit operator bool() const noexcept { return callback != nullptr; }
};

// Boundary services supplied to an explicitly registered multi-instruction
// span. A span owns the instruction effects inside its body, but it must still
// consult these services at the same boundaries as scalar handler chaining:
// scalar_barrier before every continuation and progress_checkpoint after every
// retired instruction with cumulative accounting. The initial register-only
// span contract may validate each later source word immediately before its
// effect. Any future span capable of storing to executable memory must instead
// preflight every source word it can execute before its first mutation, so a
// self-modifying store cannot invalidate a later instruction after the span
// has already committed an architectural prefix.
struct CompiledBlockExecutionServices {
    CompiledBlockScalarBarrier scalar_barrier{};
    CompiledBlockProgressCheckpoint progress_checkpoint{};
};

// A generated span reports the same exclusive terminal reason that the
// existing one-instruction chaining loop would have observed. This keeps
// opt-in fusion telemetry comparable while avoiding a dispatcher lookup merely
// to rediscover a boundary the generated body already checked.
enum class CompiledBlockSpanStopReason : std::uint8_t {
    FusedBudget,
    ProgressCheckpoint,
    ScalarBarrier,
    MissingSuccessor,
    NonfusibleSuccessor,
    SuccessorLiveWordMismatch,
    PostInstructionDelaySlot,
    ExecutionBoundary,
};

struct CompiledBlockSpanExecutionResult {
    CompiledBlockExecutionResult execution{};
    CompiledBlockSpanStopReason stop_reason{CompiledBlockSpanStopReason::ExecutionBoundary};
};

using CompiledBlockSpanImplementation = CompiledBlockSpanExecutionResult (*)(
    guest::R5900State&, guest::EeMemory&, const CompiledBlockBudget&,
    const CompiledBlockExecutionServices&);

// An O(N) generated span body can be shared by every interior entry in one
// contiguous run. maximum_instructions is the number remaining from this
// particular registered entry, not the total size of the shared body. Entries
// with fewer than two members deliberately omit the span and retain the
// ordinary one-instruction handler.
struct CompiledBlockSpan {
    CompiledBlockSpanImplementation implementation{};
    std::uint32_t maximum_instructions{};

    [[nodiscard]] explicit operator bool() const noexcept { return implementation != nullptr; }
};

// Traits are deliberately opt-in. A straight-line-fusible generated block is
// proven to be one non-faulting, one-cycle instruction with no memory,
// control-flow, privileged, or host-visible side effect. CPU-internal FPU and
// VU0 register/flag effects are eligible when the handler publishes all of
// them before its instruction boundary. The dispatcher may join adjacent
// blocks carrying this trait while retaining every scheduler and code-version
// boundary supplied by the caller.
struct CompiledBlockTraits {
    bool straight_line_fusible{};
    // Separate conditional tier: pure/noexcept proof, evaluated on the live
    // pre-instruction state. True promises a one-cycle, sequential, non-faulting
    // ordinary-RAM read with all CPU effects published at its boundary.
    // Never attach a generated span to this tier. Null preserves old modules.
    bool (*load_fusion_guard)(const guest::R5900State&,
                              const guest::EeMemory&) noexcept{};

    bool operator==(const CompiledBlockTraits&) const = default;
};

// Opt-in diagnostics for the boundary that currently joins adjacent generated
// one-instruction blocks. Each compiled dispatch which reaches that boundary
// records one terminal reason. The two fused-work totals are orthogonal: they
// describe how much useful joining happened before that terminal reason.
//
// Statistics are disabled by default because dispatch is an extremely hot
// single-emulation-thread path. When disabled, terminal accounting is one
// strongly predicted branch and every counter remains unchanged. Observation,
// reset, and enable/disable must happen on the same emulation thread as
// dispatch.
struct CompiledBlockFusionStatistics {
    std::uint64_t outer_not_fusible_stops{};
    std::uint64_t entered_delay_slot_stops{};
    std::uint64_t first_result_not_safepoint_or_one_to_one_stops{};
    std::uint64_t progress_checkpoint_stops{};
    std::uint64_t zero_capacity_budget_stops{};
    std::uint64_t one_instruction_budget_stops{};
    std::uint64_t fused_budget_stops{};
    // Includes a barrier at the outer entry and one encountered while looking
    // for a fused successor.
    std::uint64_t scalar_barrier_stops{};
    std::uint64_t outer_live_word_mismatch_stops{};
    std::uint64_t missing_successor_stops{};
    std::uint64_t nonfusible_successor_stops{};
    std::uint64_t successor_live_word_mismatch_stops{};
    // A correctly annotated straight-line instruction should not create a
    // pending delay slot. Keep this distinct from entry in an existing slot.
    std::uint64_t post_instruction_delay_slot_stops{};
    // Incremented immediately before the existing logic_error for a fusible
    // handler which fails to advance to its exact sequential PC.
    std::uint64_t trait_invariant_violations{};

    std::uint64_t fused_dispatches{};
    std::uint64_t fused_successor_instructions{};

    bool operator==(const CompiledBlockFusionStatistics&) const = default;
};

struct CompiledBlockMetadata {
    guest::GuestAddress address{};
    std::string name;
    // Generated retail translations carry the exact source word they were
    // compiled from. Reviewed/manual handlers leave this empty and are not
    // invalidated by guest-memory code changes.
    std::optional<std::uint32_t> expected_retail_word{};
    CompiledBlockTraits traits{};

    bool operator==(const CompiledBlockMetadata&) const = default;
};

enum class CompiledBlockRegistrationStatus : std::uint8_t {
    Registered,
    DuplicateAddress,
    UnalignedAddress,
    EmptyName,
    EmptyImplementation,
    InvalidSpan,
};

struct CompiledBlockRegistrationResult {
    CompiledBlockRegistrationStatus status{CompiledBlockRegistrationStatus::Registered};
    // Points to the new metadata for Registered and the original metadata for
    // DuplicateAddress. It is null for every validation failure and remains
    // stable until the dispatcher is destroyed.
    const CompiledBlockMetadata* block{};

    [[nodiscard]] bool registered() const noexcept {
        return status == CompiledBlockRegistrationStatus::Registered;
    }
};

enum class CompiledBlockDispatchStatus : std::uint8_t {
    Executed,
    CoverageGap,
    CodeMismatch,
    ScalarBarrier,
};

struct CompiledBlockDispatchResult {
    CompiledBlockDispatchStatus status{CompiledBlockDispatchStatus::CoverageGap};
    // block is null only for CoverageGap. Execution remains default for
    // CoverageGap, CodeMismatch, and ScalarBarrier. A generated block can
    // validly execute with zero cost at a guest boundary, so status rather
    // than cost identifies successful compiled coverage.
    const CompiledBlockMetadata* block{};
    CompiledBlockExecutionResult execution{};

    [[nodiscard]] bool covered() const noexcept {
        return status == CompiledBlockDispatchStatus::Executed;
    }
};

class CompiledBlockDispatcher;
class EeDispatchPreparationCache;

// Opaque result of one address-table lookup. The runner needs to establish
// coverage before it computes scheduler budgets; retaining that exact entry
// avoids repeating the lookup immediately before dispatch. It does not retain
// CPU, memory, code-version, observer, or budget state, all of which are still
// checked by dispatch().
class CompiledBlockPreparation {
  public:
    CompiledBlockPreparation() = default;

    [[nodiscard]] bool prepared() const noexcept { return block_ != nullptr; }
    [[nodiscard]] const CompiledBlockMetadata* block() const noexcept { return block_; }

  private:
    friend class CompiledBlockDispatcher;
    friend class EeDispatchPreparationCache;

    const CompiledBlockDispatcher* owner_{};
    const void* entry_{};
    const CompiledBlockMetadata* block_{};
};

// Address-indexed table of resumable generated blocks. Configure it before
// guest execution; registration concurrent with lookup/dispatch is unsupported.
// std::map gives deterministic inventories and stable metadata addresses.
class CompiledBlockDispatcher {
  public:
    CompiledBlockDispatcher() = default;
    CompiledBlockDispatcher(const CompiledBlockDispatcher& other);
    CompiledBlockDispatcher& operator=(const CompiledBlockDispatcher& other);
    CompiledBlockDispatcher(CompiledBlockDispatcher&& other);
    CompiledBlockDispatcher& operator=(CompiledBlockDispatcher&& other);

    [[nodiscard]] CompiledBlockRegistrationResult
    register_block(guest::GuestAddress address, std::string name,
                   CompiledBlockImplementation implementation,
                   std::optional<std::uint32_t> expected_retail_word = std::nullopt,
                   CompiledBlockTraits traits = {}, CompiledBlockSpan span = {});

    [[nodiscard]] const CompiledBlockMetadata*
    lookup(const guest::GuestAddress address) const noexcept {
        const auto* entry = find_entry(address);
        return entry == nullptr ? nullptr : &entry->metadata;
    }

    // The per-step compiled-stage probe only needs coverage, not a retained
    // dispatch handle. prepare() reconstructs the (address-stable) handle on
    // the rare hit inside dispatch(); keeping this probe as a plain entry
    // lookup avoids the preparation object churn on every scalar frontier
    // step. Registration is complete before the run loop, so the entry table
    // cannot change between the probe and dispatch.
    [[nodiscard]] CompiledBlockPreparation
    prepare(const guest::GuestAddress address) const noexcept {
        const auto* entry = find_entry(address);
        if (entry == nullptr) {
            return {};
        }
        CompiledBlockPreparation preparation;
        preparation.owner_ = this;
        preparation.entry_ = entry;
        preparation.block_ = &entry->metadata;
        return preparation;
    }

    // CoverageGap, CodeMismatch, ScalarBarrier, and zero-capacity budgets do
    // not invoke any implementation and therefore cannot mutate CPU or
    // memory. Versioned blocks compare the current guest word before budget
    // handling. For registered dispatches, address must equal cpu.pc().
    // Adjacent opt-in straight-line blocks may be fused up to the supplied
    // budget. An explicitly registered shared span receives first preference
    // when at least two instructions fit; otherwise the existing one-handler
    // path remains exact fallback. Every subsequent live word and optional
    // scalar barrier is checked before it executes, and delay-slot entry always
    // remains scalar.
    // When supplied, progress_checkpoint runs after each retired instruction
    // in a candidate fused run and may return false to yield at that exact
    // boundary before another instruction executes.
    // A block works directly on live state; if its returned accounting is
    // malformed, the dispatcher throws without attempting speculative
    // rollback.
    [[nodiscard]] CompiledBlockDispatchResult
    dispatch(guest::GuestAddress address, guest::R5900State& cpu, guest::EeMemory& memory,
             const CompiledBlockBudget& budget, CompiledBlockScalarBarrier scalar_barrier = nullptr,
             CompiledBlockProgressCheckpoint progress_checkpoint = {}) const;

    // Executes a table entry retained by prepare(). A preparation from another
    // dispatcher is rejected. Live code, observer, budget, and fusion checks
    // are identical to the address overload.
    [[nodiscard]] CompiledBlockDispatchResult
    dispatch(const CompiledBlockPreparation& preparation, guest::R5900State& cpu,
             guest::EeMemory& memory, const CompiledBlockBudget& budget,
             CompiledBlockScalarBarrier scalar_barrier = nullptr,
             CompiledBlockProgressCheckpoint progress_checkpoint = {}) const;

    // Scheduler-visible device/callback work clips the runner to exactly one
    // guest instruction. This entry point preserves dispatch({1, 1}) semantics
    // without entering the general straight-line fusion machinery. It still
    // validates the live retail word, observer barrier, implementation result,
    // and every straight_line_fusible trait invariant before returning to the
    // outer scheduler.
    [[nodiscard]] CompiledBlockDispatchResult
    dispatch_one(guest::GuestAddress address, guest::R5900State& cpu, guest::EeMemory& memory,
                 CompiledBlockScalarBarrier scalar_barrier = nullptr) const;

    [[nodiscard]] CompiledBlockDispatchResult
    dispatch_one(const CompiledBlockPreparation& preparation, guest::R5900State& cpu,
                 guest::EeMemory& memory,
                 CompiledBlockScalarBarrier scalar_barrier = nullptr) const;

    [[nodiscard]] std::vector<CompiledBlockMetadata> registered_blocks() const;
    // Number of registered blocks which have been selected as the outer entry
    // of at least one successful Executed dispatch. An internally fused
    // successor is deliberately not counted, matching the runner's historical
    // entry-PC diagnostic. This keeps that diagnostic inside the address table:
    // recording a hot dispatch is one direct boolean update instead of an
    // unordered-map lookup by guest PC. Like the mutable lookup cache, reached
    // accounting is owned by the single emulation thread; concurrent dispatch
    // or observation is unsupported.
    [[nodiscard]] std::size_t reached_entry_count() const noexcept;

    // Fusion telemetry is deliberately opt-in. Disabling freezes accumulated
    // counters; reset is explicit so callers can take interval snapshots
    // without changing collection state.
    void set_fusion_statistics_enabled(bool enabled) noexcept {
        fusion_statistics_enabled_ = enabled;
    }
    [[nodiscard]] bool fusion_statistics_enabled() const noexcept {
        return fusion_statistics_enabled_;
    }
    void reset_fusion_statistics() noexcept { fusion_statistics_ = {}; }
    [[nodiscard]] CompiledBlockFusionStatistics fusion_statistics() const noexcept {
        return fusion_statistics_;
    }

    // Delay-slot dispatch relaxation (T2, TAKEDOWN_DELAY_SLOT_DISPATCH).
    // Default off: a dispatch entered while the pipeline holds a pending
    // branch delay slot retires exactly one instruction, the historical
    // conservative boundary. When enabled, a slot entry whose resolved target
    // is the sequential successor (a not-taken ordinary branch, where
    // next_pc == pc+4) may retire its registered straight-line span; a taken
    // slot (target != pc+4) keeps the one-instruction boundary because the
    // span contract's sequential-PC invariant cannot hold across the
    // non-sequential target, and a slot-entry delayed control still faults to
    // the scalar interpreter through its generated guard. The pending slot is
    // architecturally resolved before it is observable — every
    // schedule_delayed_target writer commits an evaluated target — so the
    // relaxation cannot observe or manufacture an unresolved slot. The runner
    // sets this once before execution from the environment flag; toggling
    // mid-run is unsupported, like registration.
    void set_load_fusion_enabled(bool enabled) noexcept { load_fusion_enabled_ = enabled; }
    [[nodiscard]] bool load_fusion_enabled() const noexcept { return load_fusion_enabled_; }

    void set_delay_slot_dispatch_enabled(const bool enabled) noexcept {
        delay_slot_dispatch_enabled_ = enabled;
    }
    [[nodiscard]] bool delay_slot_dispatch_enabled() const noexcept {
        return delay_slot_dispatch_enabled_;
    }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    friend class EeDispatchPreparationCache;

    using DirectImplementation = CompiledBlockExecutionResult (*)(guest::R5900State&,
                                                                  guest::EeMemory&,
                                                                  const CompiledBlockBudget&);

    struct Entry {
        CompiledBlockMetadata metadata;
        CompiledBlockImplementation implementation;
        // Generated modules register ordinary static functions. Preserve the
        // general std::function API for reviewed/stateful callables, but let
        // this overwhelmingly common case bypass std::function's manager
        // thunk on every guest instruction.
        DirectImplementation direct_implementation{};
        CompiledBlockSpan span{};
        mutable bool reached{};
    };

    // The generated retail inventory spans thousands of instructions across
    // many 4 KiB code pages. A 256-slot low-PC-only cache made identical page
    // offsets evict one another continuously in the race renderer. Keep the
    // table power-of-two and large enough for the active code working set;
    // lookup folds higher PC bits before masking below.
    static constexpr std::size_t kLookupCacheSize = 16'384U;
    static_assert((kLookupCacheSize & (kLookupCacheSize - 1U)) == 0U);
    struct LookupCacheSlot {
        guest::GuestAddress address{};
        const Entry* entry{};
        bool valid{};
    };

    enum class FusionStopReason : std::uint8_t {
        OuterNotFusible,
        EnteredDelaySlot,
        FirstResultNotSafepointOrOneToOne,
        ProgressCheckpoint,
        ZeroCapacityBudget,
        OneInstructionBudget,
        FusedBudget,
        ScalarBarrier,
        OuterLiveWordMismatch,
        MissingSuccessor,
        NonfusibleSuccessor,
        SuccessorLiveWordMismatch,
        PostInstructionDelaySlot,
        TraitInvariantViolation,
    };

    // Inline so the default-disabled path remains just one predictable branch
    // at the terminal fusion boundary. No lookup, map, allocation, atomic, or
    // clock read is introduced into per-instruction execution.
    void record_fusion_stop(const FusionStopReason reason,
                            const std::uint32_t fused_successors = 0U) const noexcept {
        if (!fusion_statistics_enabled_) {
            return;
        }

        if (fused_successors != 0U) {
            ++fusion_statistics_.fused_dispatches;
            fusion_statistics_.fused_successor_instructions += fused_successors;
        }
        switch (reason) {
        case FusionStopReason::OuterNotFusible:
            ++fusion_statistics_.outer_not_fusible_stops;
            break;
        case FusionStopReason::EnteredDelaySlot:
            ++fusion_statistics_.entered_delay_slot_stops;
            break;
        case FusionStopReason::FirstResultNotSafepointOrOneToOne:
            ++fusion_statistics_.first_result_not_safepoint_or_one_to_one_stops;
            break;
        case FusionStopReason::ProgressCheckpoint:
            ++fusion_statistics_.progress_checkpoint_stops;
            break;
        case FusionStopReason::ZeroCapacityBudget:
            ++fusion_statistics_.zero_capacity_budget_stops;
            break;
        case FusionStopReason::OneInstructionBudget:
            ++fusion_statistics_.one_instruction_budget_stops;
            break;
        case FusionStopReason::FusedBudget:
            ++fusion_statistics_.fused_budget_stops;
            break;
        case FusionStopReason::ScalarBarrier:
            ++fusion_statistics_.scalar_barrier_stops;
            break;
        case FusionStopReason::OuterLiveWordMismatch:
            ++fusion_statistics_.outer_live_word_mismatch_stops;
            break;
        case FusionStopReason::MissingSuccessor:
            ++fusion_statistics_.missing_successor_stops;
            break;
        case FusionStopReason::NonfusibleSuccessor:
            ++fusion_statistics_.nonfusible_successor_stops;
            break;
        case FusionStopReason::SuccessorLiveWordMismatch:
            ++fusion_statistics_.successor_live_word_mismatch_stops;
            break;
        case FusionStopReason::PostInstructionDelaySlot:
            ++fusion_statistics_.post_instruction_delay_slot_stops;
            break;
        case FusionStopReason::TraitInvariantViolation:
            ++fusion_statistics_.trait_invariant_violations;
            break;
        }
    }

    [[nodiscard]] const Entry* find_entry(const guest::GuestAddress address) const noexcept {
        const auto instruction_address = address >> 2U;
        const auto cache_index =
            (instruction_address ^ (instruction_address >> 12U) ^ (instruction_address >> 24U)) &
            (kLookupCacheSize - 1U);
        auto& slot = lookup_cache_[cache_index];
        if (slot.valid && slot.address == address) {
            return slot.entry;
        }
        const auto position = entries_.find(address);
        slot = {
            .address = address,
            .entry = position == entries_.end() ? nullptr : &position->second,
            .valid = true,
        };
        return slot.entry;
    }
    void invalidate_lookup_cache_slot(guest::GuestAddress address) noexcept;

    std::map<guest::GuestAddress, Entry> entries_;
    mutable std::array<LookupCacheSlot, kLookupCacheSize> lookup_cache_{};
    bool fusion_statistics_enabled_{};
    mutable CompiledBlockFusionStatistics fusion_statistics_{};
    bool delay_slot_dispatch_enabled_{};
    bool load_fusion_enabled_{};
    [[nodiscard]] bool can_fuse(const CompiledBlockTraits& traits,
                                const guest::R5900State& cpu,
                                const guest::EeMemory& memory) const noexcept {
        return traits.straight_line_fusible ||
               (load_fusion_enabled_ && traits.load_fusion_guard != nullptr &&
                traits.load_fusion_guard(cpu, memory));
    }
};

} // namespace takedown::runtime
