#pragma once

#include "takedown/guest/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::guest {

class EeMemory;
class R5900State;

} // namespace takedown::guest

namespace takedown::runtime {

// Live architectural state supplied to one native translation. A handler
// represents a complete guest routine, not one EE instruction: it owns every
// architectural effect of that routine, including selecting the continuation
// PC. The dispatcher does not synthesize call/return state around the handler.
struct NativeFunctionContext {
    guest::R5900State& cpu;
    guest::EeMemory& memory;
    guest::GuestAddress entry_address{};
};

// Read-only view used to decide whether one invocation is inside the proven
// domain of a promoted whole-function translation. Resolvers must not change
// architectural state: a rejected invocation immediately continues through
// the resumable compiled-block path.
struct NativeFunctionPrepareContext {
    const guest::R5900State& cpu;
    const guest::EeMemory& memory;
    guest::GuestAddress entry_address{};
};

// Whole-function translations report the guest work they retired so the
// scheduler advances by console time rather than by one host dispatch. The
// values are dynamic because a translated function may contain branches.
struct NativeFunctionExecution {
    std::uint32_t retired_instructions{};
    std::uint32_t ee_cycles{};

    bool operator==(const NativeFunctionExecution&) const = default;
};

using NativeFunction = std::function<NativeFunctionExecution(NativeFunctionContext&)>;

// Conservative upper bounds used before entering an atomic whole-function
// translation. The runner falls back when a scheduler or step-budget boundary
// may occur within this window.
struct NativeFunctionLimits {
    std::uint32_t maximum_retired_instructions{};
    std::uint32_t maximum_ee_cycles{};

    bool operator==(const NativeFunctionLimits&) const = default;
};

// A resolver returns the exact cost for an eligible invocation, or nullopt
// when its arguments/live code/memory mapping are outside the reviewed
// profile. The registration's static limits remain an absolute ceiling.
using NativeFunctionLimitResolver =
    std::function<std::optional<NativeFunctionLimits>(const NativeFunctionPrepareContext&)>;

// Properties proven by analysis/review rather than inferred at dispatch time.
// A scheduler-transparent function cannot change COP0 interrupt eligibility,
// kernel state, interrupt masks/status, or MMIO that makes a previously inert
// callback dispatchable. The conservative default is false.
struct NativeFunctionTraits {
    bool scheduler_state_transparent{};
    // A game-owned PC call replacing semantics rather than accelerating an
    // equivalent guest function. It retires exactly one scheduler boundary,
    // performs no device access and never skips a scalar device/kernel poll.
    // Original-code fallback is not equivalent for an admitted host call.
    bool single_step_host_call{};
    // A leaf that neither reads nor writes device state, DMA control or
    // anything an interrupt handler shares with it (engine replacements of
    // pure game logic). It may run while DMAC completions wait for their
    // dispatch delay; the runner then counts the delay down by the
    // instructions the call stands for, so the completion's interrupt is
    // taken right after the call instead of somewhere inside it.
    bool defers_dmac_completions{};

    bool operator==(const NativeFunctionTraits&) const = default;
};

struct NativeFunctionMetadata {
    guest::GuestAddress address{};
    std::string name;
    NativeFunctionLimits limits{};
    NativeFunctionTraits traits{};
    // True only for the resolver-backed registration overload. The runner
    // uses this to avoid a pointless exact-cost probe for fixed-cost entries.
    bool has_dynamic_cost_resolver{};

    bool operator==(const NativeFunctionMetadata&) const = default;
};

enum class NativeFunctionRegistrationStatus : std::uint8_t {
    Registered,
    DuplicateAddress,
    UnalignedAddress,
    EmptyName,
    EmptyImplementation,
    EmptyLimitResolver,
    InvalidExecutionLimits,
};

struct NativeFunctionRegistrationResult {
    NativeFunctionRegistrationStatus status{NativeFunctionRegistrationStatus::Registered};
    // Points to the new registration for Registered and to the existing one
    // for DuplicateAddress. It is null for all validation failures. The
    // pointer remains valid until the dispatcher is destroyed.
    const NativeFunctionMetadata* function{};

    [[nodiscard]] bool registered() const noexcept {
        return status == NativeFunctionRegistrationStatus::Registered;
    }
};

enum class NativeFunctionDispatchStatus : std::uint8_t {
    Handled,
    Fallback,
};

enum class NativeFunctionPrepareStatus : std::uint8_t {
    Prepared,
    Fallback,
};

struct NativeFunctionPreparation {
    NativeFunctionPrepareStatus status{NativeFunctionPrepareStatus::Fallback};
    // Non-null for both a prepared invocation and a registered invocation
    // rejected by its resolver. It is null only when the address is absent.
    const NativeFunctionMetadata* function{};
    NativeFunctionLimits limits{};
    bool exact_execution_cost{};

    [[nodiscard]] bool prepared() const noexcept {
        return status == NativeFunctionPrepareStatus::Prepared;
    }
};

struct NativeFunctionDispatchResult {
    NativeFunctionDispatchStatus status{NativeFunctionDispatchStatus::Fallback};
    // Non-null only when a native implementation handled the address.
    const NativeFunctionMetadata* function{};
    NativeFunctionExecution execution{};

    [[nodiscard]] bool handled() const noexcept {
        return status == NativeFunctionDispatchStatus::Handled;
    }
};

// Address-indexed table of translated guest routines. Configure the table
// before guest execution; concurrent registration and dispatch are not
// supported. std::map deliberately gives inventory output stable address
// ordering and keeps metadata pointers valid when new functions are added.
class NativeFunctionDispatcher {
  public:
    NativeFunctionDispatcher() = default;
    NativeFunctionDispatcher(const NativeFunctionDispatcher& other);
    NativeFunctionDispatcher& operator=(const NativeFunctionDispatcher& other);
    NativeFunctionDispatcher(NativeFunctionDispatcher&& other);
    NativeFunctionDispatcher& operator=(NativeFunctionDispatcher&& other);

    [[nodiscard]] NativeFunctionRegistrationResult
    register_function(guest::GuestAddress address, std::string name,
                      NativeFunctionLimits limits, NativeFunctionTraits traits,
                      NativeFunction implementation);

    // Dynamic-profile overload. `limits` is an absolute safety ceiling;
    // `limit_resolver` supplies the exact cost for each accepted invocation.
    [[nodiscard]] NativeFunctionRegistrationResult
    register_function(guest::GuestAddress address, std::string name,
                      NativeFunctionLimits limits, NativeFunctionTraits traits,
                      NativeFunctionLimitResolver limit_resolver,
                      NativeFunction implementation);

    [[nodiscard]] const NativeFunctionMetadata*
    lookup(const guest::GuestAddress address) const noexcept {
        const auto* entry = find_entry(address);
        return entry == nullptr ? nullptr : &entry->metadata;
    }

    // Resolve one invocation without changing CPU or memory. An unknown
    // address and a registered-but-ineligible profile both return Fallback;
    // the latter retains metadata so native-only diagnostics can name it.
    [[nodiscard]] NativeFunctionPreparation
    prepare(guest::GuestAddress address, const guest::R5900State& cpu,
            const guest::EeMemory& memory) const;

    // An absent address returns Fallback without touching CPU or memory. A
    // registered handler's exceptions and architectural changes are passed
    // through unchanged; there is intentionally no speculative rollback. A
    // handler must report at least one retired instruction and EE cycle.
    [[nodiscard]] NativeFunctionDispatchResult
    dispatch(guest::GuestAddress address, guest::R5900State& cpu,
             guest::EeMemory& memory) const;

    // Executes an invocation admitted by prepare(). A resolver-backed entry is
    // resolved again against live state before its handler runs; eligibility
    // and exact cost must still match the preparation. This is a scheduler
    // guard rather than a full identity snapshot of every input byte. Every
    // function remains bounded by its registration's absolute ceiling.
    [[nodiscard]] NativeFunctionDispatchResult
    dispatch(const NativeFunctionPreparation& preparation,
             guest::R5900State& cpu, guest::EeMemory& memory) const;

    [[nodiscard]] std::vector<NativeFunctionMetadata> registered_functions() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    struct Entry {
        NativeFunctionMetadata metadata;
        NativeFunctionLimitResolver limit_resolver;
        NativeFunction implementation;
    };

    // Match the resumable-block lookup working set. Native entries are sparse,
    // but the runner probes them at every EE instruction boundary, including
    // across many code pages with the same low address bits.
    static constexpr std::size_t kLookupCacheSize = 16'384U;
    static_assert((kLookupCacheSize & (kLookupCacheSize - 1U)) == 0U);
    struct LookupCacheSlot {
        guest::GuestAddress address{};
        const Entry* entry{};
        bool valid{};
    };

    [[nodiscard]] const Entry* find_entry(const guest::GuestAddress address) const noexcept {
        const auto instruction_address = address >> 2U;
        const auto cache_index =
            (instruction_address ^ (instruction_address >> 12U) ^
             (instruction_address >> 24U)) &
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

    [[nodiscard]] NativeFunctionRegistrationResult
    register_function_impl(guest::GuestAddress address, std::string name,
                           NativeFunctionLimits limits, NativeFunctionTraits traits,
                           NativeFunctionLimitResolver limit_resolver,
                           bool require_limit_resolver,
                           NativeFunction implementation);

    std::map<guest::GuestAddress, Entry> entries_;
    mutable std::array<LookupCacheSlot, kLookupCacheSize> lookup_cache_{};
};

// True when TAKEDOWN_DISABLE_NATIVE_FUNCTIONS (a comma-separated list of
// registration names) names `name`: that routine then runs as the original
// game code. For same-binary A/B measurements and bisecting differences.
[[nodiscard]] bool native_function_disabled_by_environment(std::string_view name);

} // namespace takedown::runtime
