#pragma once

#include "takedown/guest/types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takedown::runtime {

// Support types for the boot runner's "scalar burst" (src/app/main.cpp).
//
// The runner's outer loop evaluates several thousand lines of hooks, device
// service and scheduler polling before every interpreted guest instruction,
// and almost none of it acts on an ordinary instruction. A burst keeps
// stepping the interpreter in a tight inner loop for as long as a complete
// outer iteration at each skipped boundary would provably have done nothing
// except execute the next instruction. This header holds the two pieces that
// are independent of the runner's local state: the PC filter and the counters.

// Constant-time answer to "can the outer loop do anything PC-dependent at this
// guest PC?". One bit per aligned main-RAM instruction address.
//
// The runner fills it from exactly one registration block. Every address or
// range that the outer loop body, or a helper it hands the CPU state to,
// compares against the PC must be marked there; the CTest consistency check
// tests/test_scalar_burst_hooks.py parses the loop and fails when one is not.
class ScalarBurstHookFilter {
  public:
    // Every hook comparison in the runner is exact-value against a KUSEG
    // main-RAM address, so an aligned PC below this limit, inside installed
    // RAM, is the only kind of PC a burst may execute. KSEG and TLB aliases,
    // scratchpad, VU and MMIO addresses, the kernel's 0xfffffff0 callback
    // sentinel, unaligned PCs and unmapped PCs always read as marked.
    static constexpr guest::GuestAddress kOrdinaryCodeLimit = 0x02000000U;

    explicit ScalarBurstHookFilter(const std::size_t main_ram_size)
        : limit_(static_cast<guest::GuestAddress>(
              std::min<std::size_t>(main_ram_size, kOrdinaryCodeLimit) & ~std::size_t{0x3U})),
          bits_((static_cast<std::size_t>(limit_ >> 2U) + 63U) / 64U, 0U) {}

    // Marks one PC. Addresses outside ordinary code are already marked.
    void mark_pc(const guest::GuestAddress pc) noexcept {
        if (pc < limit_ && (pc & 0x3U) == 0U) {
            const auto word = static_cast<std::size_t>(pc >> 2U);
            bits_[word >> 6U] |= std::uint64_t{1U} << (word & 63U);
        }
    }
    // Marks every instruction address in [first, last].
    void mark_closed_range(const guest::GuestAddress first,
                           const guest::GuestAddress last) noexcept {
        for (auto pc = (static_cast<std::uint64_t>(first) + 3U) & ~std::uint64_t{0x3U};
             pc <= last && pc < limit_; pc += 4U) {
            mark_pc(static_cast<guest::GuestAddress>(pc));
        }
    }
    // Marks every instruction address in [begin, end).
    void mark_half_open_range(const guest::GuestAddress begin,
                              const guest::GuestAddress end) noexcept {
        if (end > begin) {
            mark_closed_range(begin, end - 1U);
        }
    }

    [[nodiscard]] bool marked(const guest::GuestAddress pc) const noexcept {
        if (pc >= limit_ || (pc & 0x3U) != 0U) {
            return true;
        }
        const auto word = static_cast<std::size_t>(pc >> 2U);
        return ((bits_[word >> 6U] >> (word & 63U)) & 1U) != 0U;
    }

    // Exclusive upper bound of the addresses that can read as unmarked.
    [[nodiscard]] guest::GuestAddress ordinary_code_limit() const noexcept { return limit_; }
    [[nodiscard]] std::size_t marked_ordinary_pc_count() const noexcept {
        std::size_t count{};
        for (const auto word : bits_) {
            count += static_cast<std::size_t>(std::popcount(word));
        }
        return count;
    }

  private:
    guest::GuestAddress limit_{};
    std::vector<std::uint64_t> bits_;
};

// Registered native function entries whose only reason to be in the hook filter
// is the native dispatch itself: at such a PC the outer loop does nothing
// PC-dependent but offer the entry to the native dispatcher. A burst may make
// that dispatch itself (after the same scheduler preflight) and go on, instead
// of ending and having the whole outer iteration repeat it. One bit per aligned
// main-RAM instruction address, filled from the hook registration block.
class ScalarBurstNativeEntries {
  public:
    explicit ScalarBurstNativeEntries(const std::size_t main_ram_size)
        : limit_(static_cast<guest::GuestAddress>(
              std::min<std::size_t>(main_ram_size, ScalarBurstHookFilter::kOrdinaryCodeLimit) &
              ~std::size_t{0x3U})),
          bits_((static_cast<std::size_t>(limit_ >> 2U) + 63U) / 64U, 0U) {}

    void add(const guest::GuestAddress pc) noexcept {
        if (pc < limit_ && (pc & 0x3U) == 0U) {
            const auto word = static_cast<std::size_t>(pc >> 2U);
            bits_[word >> 6U] |= std::uint64_t{1U} << (word & 63U);
            ++count_;
        }
    }
    [[nodiscard]] bool contains(const guest::GuestAddress pc) const noexcept {
        if (pc >= limit_ || (pc & 0x3U) != 0U) {
            return false;
        }
        const auto word = static_cast<std::size_t>(pc >> 2U);
        return ((bits_[word >> 6U] >> (word & 63U)) & 1U) != 0U;
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

  private:
    guest::GuestAddress limit_{};
    std::size_t count_{};
    std::vector<std::uint64_t> bits_;
};

// Why a burst handed control back to the outer loop. Exactly one per burst.
enum class ScalarBurstStop : std::uint8_t {
    Hook,     // the next PC is marked, or is a live UI-session boundary
    Horizon,  // a step, cycle or iteration deadline is reached at the next boundary
    Hardware, // the instruction touched device memory or ran a VU0 microprogram
    Result,   // the instruction did not simply execute (system call or fault)
    State,    // COP0 Status changed, or a polled guest RAM word changed
};
inline constexpr std::size_t kScalarBurstStopCount = 5U;

struct ScalarBurstStatistics {
    std::uint64_t bursts{};
    // Instructions executed inside bursts; the ordinary step that precedes
    // each burst is not included.
    std::uint64_t instructions{};
    // Native function entries dispatched from inside bursts (the guest
    // instructions they retire are counted with the native functions').
    std::uint64_t native_calls{};
    // Registered native function entries at which the replacement did not
    // apply and the outer iteration would only have stepped: the burst stepped
    // on instead of ending.
    std::uint64_t native_declines{};
    std::array<std::uint64_t, kScalarBurstStopCount> stops{};
    // The horizon each burst started with, by bit_width (bucket i holds the
    // bursts that started with bit_width(horizon) == i, the last bucket 2^15
    // and more): tells whether a deadline close to the burst start is what
    // keeps bursts short.
    std::array<std::uint64_t, 16> start_horizon{};

    void note_start(const std::uint64_t horizon) noexcept {
        ++start_horizon[std::min<std::size_t>(std::bit_width(horizon), start_horizon.size() - 1U)];
    }
    void note_stop(const ScalarBurstStop reason) noexcept {
        ++stops[static_cast<std::size_t>(reason)];
    }
    [[nodiscard]] std::uint64_t stop_count(const ScalarBurstStop reason) const noexcept {
        return stops[static_cast<std::size_t>(reason)];
    }
};

// Number of consecutive boundary values first, first + 1, ... that lie strictly
// before `limit`. Every deadline the outer loop tests fires at the first
// boundary at or after it, so a skipped boundary must be strictly before.
[[nodiscard]] constexpr std::uint64_t
scalar_burst_boundaries_before(const std::uint64_t first, const std::uint64_t limit) noexcept {
    return limit > first ? limit - first : 0U;
}

} // namespace takedown::runtime
