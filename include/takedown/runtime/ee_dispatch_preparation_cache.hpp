#pragma once

#include "takedown/runtime/compiled_block_dispatcher.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace takedown::runtime {

// Immutable candidates for one EE instruction boundary. Native eligibility is
// intentionally not resolved here: the runner must retain native precedence,
// apply focused-trace masking, and perform its scheduler gate before a dynamic
// resolver observes live state. A ceiling-only step/deadline refusal may use
// the separately guarded one-instruction probe, then re-preflights the exact
// resolved cost. Likewise, CompiledBlockPreparation retains only the
// registered entry; dispatch() still checks the live PC, code word, scalar
// barriers, and budget.
struct EeDispatchPreparation {
    const NativeFunctionMetadata* native_candidate{};
    CompiledBlockPreparation compiled_preparation{};
};

// Combines the two immutable registration-table probes made at every EE
// boundary. After a warmed hit, prepare() performs one direct-mapped tag probe
// and returns both candidates. A miss delegates to the dispatchers' existing
// lookup caches, preserving their exact address/absence behavior.
//
// Sources are non-owning and must outlive the cache. Registration must be
// complete before lookups begin; concurrent registration/lookup is unsupported
// just as it is for the underlying dispatchers. If sequential registration is
// intentionally resumed, call refresh_after_registration() before the next
// lookup. Moving or assigning either source object requires constructing or
// rebinding a cache, because retained compiled preparations identify their
// owning dispatcher and address-stable entries.
// Copying or moving this cache itself is safe: copies keep the same stable
// sources, while a moved-from cache becomes an empty interpreter configuration.
class EeDispatchPreparationCache {
  public:
    EeDispatchPreparationCache() = default;

    EeDispatchPreparationCache(const NativeFunctionDispatcher* native_dispatcher,
                               const CompiledBlockDispatcher* compiled_dispatcher)
        : native_dispatcher_(native_dispatcher), compiled_dispatcher_(compiled_dispatcher) {
        refresh_after_registration();
    }

    EeDispatchPreparationCache(const NativeFunctionDispatcher& native_dispatcher,
                               const CompiledBlockDispatcher& compiled_dispatcher)
        : EeDispatchPreparationCache(&native_dispatcher, &compiled_dispatcher) {}

    EeDispatchPreparationCache(const EeDispatchPreparationCache& other)
        : native_dispatcher_(other.native_dispatcher_),
          compiled_dispatcher_(other.compiled_dispatcher_),
          native_coverage_available_(other.native_coverage_available_),
          compiled_coverage_available_(other.compiled_coverage_available_) {
        if (other.slots_ != nullptr) {
            slots_ = std::make_unique<SlotArray>(*other.slots_);
        }
    }

    EeDispatchPreparationCache& operator=(const EeDispatchPreparationCache& other) {
        if (this != &other) {
            auto copy = EeDispatchPreparationCache{other};
            swap(copy);
        }
        return *this;
    }

    EeDispatchPreparationCache(EeDispatchPreparationCache&& other) noexcept
        : native_dispatcher_(std::exchange(other.native_dispatcher_, nullptr)),
          compiled_dispatcher_(std::exchange(other.compiled_dispatcher_, nullptr)),
          native_coverage_available_(std::exchange(other.native_coverage_available_, false)),
          compiled_coverage_available_(std::exchange(other.compiled_coverage_available_, false)),
          slots_(std::move(other.slots_)) {}

    EeDispatchPreparationCache& operator=(EeDispatchPreparationCache&& other) noexcept {
        if (this != &other) {
            native_dispatcher_ = std::exchange(other.native_dispatcher_, nullptr);
            compiled_dispatcher_ = std::exchange(other.compiled_dispatcher_, nullptr);
            native_coverage_available_ = std::exchange(other.native_coverage_available_, false);
            compiled_coverage_available_ = std::exchange(other.compiled_coverage_available_, false);
            slots_ = std::move(other.slots_);
        }
        return *this;
    }

    void swap(EeDispatchPreparationCache& other) noexcept {
        using std::swap;
        swap(native_dispatcher_, other.native_dispatcher_);
        swap(compiled_dispatcher_, other.compiled_dispatcher_);
        swap(native_coverage_available_, other.native_coverage_available_);
        swap(compiled_coverage_available_, other.compiled_coverage_available_);
        swap(slots_, other.slots_);
    }

    // Rebind to a completed registration inventory and discard every cached
    // hit/miss. Null pointers represent an interpreter-only configuration.
    void reset(const NativeFunctionDispatcher* native_dispatcher,
               const CompiledBlockDispatcher* compiled_dispatcher) {
        native_dispatcher_ = native_dispatcher;
        compiled_dispatcher_ = compiled_dispatcher;
        refresh_after_registration();
    }

    // Must be called after any sequential registration through either source
    // and before lookups resume. Empty sources stay probe-free; storage is
    // allocated only when at least one dispatcher has immutable coverage.
    void refresh_after_registration() {
        native_coverage_available_ = native_dispatcher_ != nullptr && !native_dispatcher_->empty();
        compiled_coverage_available_ =
            compiled_dispatcher_ != nullptr && !compiled_dispatcher_->empty();
        if (!native_coverage_available_ && !compiled_coverage_available_) {
            slots_.reset();
            return;
        }
        if (slots_ == nullptr) {
            slots_ = std::make_unique<SlotArray>();
        } else {
            *slots_ = {};
        }
    }

    [[nodiscard]] EeDispatchPreparation prepare(const guest::GuestAddress address) const noexcept {
        if (slots_ == nullptr) {
            return {};
        }

        auto& slot = (*slots_)[cache_index(address)];
        if (slot.valid && slot.address == address) {
            return materialize(slot);
        }

        const NativeFunctionMetadata* native_candidate{};
        if (native_coverage_available_) {
            native_candidate = native_dispatcher_->lookup(address);
        }
        const CompiledBlockDispatcher::Entry* compiled_entry{};
        if (compiled_coverage_available_) {
            compiled_entry = compiled_dispatcher_->find_entry(address);
        }
        slot = {
            .address = address,
            .native_candidate = native_candidate,
            .compiled_entry = compiled_entry,
            .valid = true,
        };
        return materialize(slot);
    }

    [[nodiscard]] bool empty() const noexcept { return slots_ == nullptr; }

  private:
    static constexpr std::size_t kLookupCacheSize = 16'384U;
    static_assert((kLookupCacheSize & (kLookupCacheSize - 1U)) == 0U);

    struct Slot {
        guest::GuestAddress address{};
        const NativeFunctionMetadata* native_candidate{};
        const CompiledBlockDispatcher::Entry* compiled_entry{};
        bool valid{};
    };
    using SlotArray = std::array<Slot, kLookupCacheSize>;

    [[nodiscard]] static constexpr std::size_t
    cache_index(const guest::GuestAddress address) noexcept {
        const auto instruction_address = address >> 2U;
        return (instruction_address ^ (instruction_address >> 12U) ^ (instruction_address >> 24U)) &
               (kLookupCacheSize - 1U);
    }

    [[nodiscard]] EeDispatchPreparation materialize(const Slot& slot) const noexcept {
        EeDispatchPreparation preparation{
            .native_candidate = slot.native_candidate,
        };
        if (slot.compiled_entry != nullptr) {
            preparation.compiled_preparation.owner_ = compiled_dispatcher_;
            preparation.compiled_preparation.entry_ = slot.compiled_entry;
            preparation.compiled_preparation.block_ = &slot.compiled_entry->metadata;
        }
        return preparation;
    }

    const NativeFunctionDispatcher* native_dispatcher_{};
    const CompiledBlockDispatcher* compiled_dispatcher_{};
    bool native_coverage_available_{};
    bool compiled_coverage_available_{};
    mutable std::unique_ptr<SlotArray> slots_;
};

inline void swap(EeDispatchPreparationCache& left, EeDispatchPreparationCache& right) noexcept {
    left.swap(right);
}

} // namespace takedown::runtime
