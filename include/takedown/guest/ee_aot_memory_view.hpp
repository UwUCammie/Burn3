#pragma once

// The part of EeMemory that EE ahead-of-time compiled code reads directly on
// its store fast path (src/guest/ee_aot_abi.hpp). It is a plain data struct
// owned by EeMemory and handed to the generated code by pointer, so the
// generated translation units depend on this small header and not on
// ee_memory.hpp (which pulls in most of the runtime). Keep it free of any
// other takedown include, and change it rarely: every generated shard
// rebuilds when it changes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace takedown::guest {

inline constexpr std::size_t kEeMainRamCodePageSize = 4U * 1024U;
// MainRamCodePageTracker::PageState::Watched (checked by ee_memory.cpp).
inline constexpr std::uint8_t kEeAotWatchedPageState = 1U;
inline constexpr std::size_t kEeAotWatchRanges = 2U;

struct EeAotMemoryView {
    // The code-page tracker's mutation generation and per-page states.
    std::uint64_t* mutation_generation{};
    const std::uint8_t* page_states{};
    // Main-RAM words holding compiled guest code: bit i of `code_words`
    // stands for the word at physical `code_begin` + 4i.
    std::size_t code_begin{};
    std::size_t code_end{};
    const std::uint64_t* code_words{};
    // Physical main-RAM ranges the caller polls between instructions.
    std::array<std::pair<std::size_t, std::size_t>, kEeAotWatchRanges> watch{};
    // Live conditions of the scratchpad store fast path, as the addresses of
    // the state itself (refreshed by EeMemory::ee_aot_memory_view()): the
    // PATH2 source marker count and the slots of the write observer and the
    // VIF1 chain observer (each holds a pointer; non-null means present).
    const std::size_t* vif1_marker_count{};
    const void* write_observer_slot{};
    const void* chain_observer_slot{};
};

static_assert(kEeAotWatchRanges == 2U);

// The fast path of a direct main-RAM store of `length` bytes (1, 2, 4, 8 or
// 16, aligned to its size) at `offset`, given that no observer, PATH2 marker,
// write watch or VIF1 writer log is in use (EeMemory::ee_aot_stores_plain):
// every record call the ordinary path makes for it (one per byte below a
// word, else one per word) would only advance the mutation generation, which
// is then advanced by that many. True means exactly that was done and the
// caller writes the bytes; false leaves everything untouched and the caller
// takes the ordinary route.
[[gnu::always_inline]] inline bool ee_aot_plain_main_ram_store(const EeAotMemoryView& view, const std::size_t offset,
                                                               const std::size_t length) noexcept {
    const auto generation = *view.mutation_generation;
    const auto records = length < 4U ? length : length / 4U;
    const auto end = offset + length;
    bool code_clear = true;
    if (offset + length > view.code_begin && offset < view.code_end) {
        // Inside the code range: test each word's bit; a store that
        // straddles either edge of the range is left to the ordinary path.
        if (offset < view.code_begin || end > view.code_end) [[unlikely]] {
            return false;
        }
        const auto first_word = (offset - view.code_begin) / 4U;
        std::uint64_t hit = 0U;
        for (std::size_t word = 0U; word < (length + 3U) / 4U; ++word) {
            const auto index = first_word + word;
            hit |= view.code_words[index / 64U] >> (index % 64U);
        }
        code_clear = (hit & 1U) == 0U;
    }
    if (generation - 1U < ~std::uint64_t{0U} - 16U && code_clear &&
        view.page_states[offset / kEeMainRamCodePageSize] != kEeAotWatchedPageState &&
        !(offset < view.watch[0].second && end > view.watch[0].first) &&
        !(offset < view.watch[1].second && end > view.watch[1].first)) [[likely]] {
        *view.mutation_generation = generation + records;
        return true;
    }
    return false;
}

// Whether a scratchpad store needs anything beyond the bytes (see
// EeMemory::ee_aot_store_needs_slow_path and ee_aot_scratchpad_store_records):
// a PATH2 marker, a write observer or a VIF1 chain observer is live.
[[gnu::always_inline]] inline bool ee_aot_scratchpad_store_needs_slow_path(const EeAotMemoryView& view) noexcept {
    const void* write_observer;
    const void* chain_observer;
    std::memcpy(&write_observer, view.write_observer_slot, sizeof(write_observer));
    std::memcpy(&chain_observer, view.chain_observer_slot, sizeof(chain_observer));
    return *view.vif1_marker_count != 0U || write_observer != nullptr || chain_observer != nullptr;
}

} // namespace takedown::guest
