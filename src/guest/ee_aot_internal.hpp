#pragma once

// Shared between src/guest/ee_aot.cpp (dispatcher and tables) and the tests
// that include it. The generated function shards (compiled as separate
// translation units) include only ee_aot_abi.hpp, which this header extends
// with everything that needs EeMemory. See include/takedown/guest/ee_aot.hpp.

// Defines the real ee_store, ee_device_changed, ... below (see ee_aot_abi.hpp).
#define TAKEDOWN_EE_AOT_RUNTIME_SIDE
#include "ee_aot_abi.hpp"

#include "takedown/guest/ee_aot.hpp"
#include "takedown/guest/ee_execution.hpp"
#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_instruction.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace takedown::guest::ee_aot_detail {

static_assert(kMainRamBytes == kEeMainRamSize);
static_assert(kScratchpadBytes == kEeScratchpadSize);

// ---- Runtime-side vocabulary (the out-of-line routes of the fast paths) -----

// Device state and polled RAM words the caller's burst checks after every
// instruction, plus stores into compiled code.
[[gnu::always_inline]] inline bool ee_device_changed(const EeAotContext& c) noexcept {
    return c.memory->hardware_activity_epoch() != c.hardware_epoch ||
           c.memory->ee_aot_code_write_epoch() != c.code_epoch ||
           c.memory->ee_aot_watch_epoch() != c.watch_epoch;
}
// `executed` is the run's count before the instruction at pc.
[[gnu::always_inline]] inline void ee_provenance_at(EeAotContext& c, const GuestAddress pc,
                                                    const std::uint64_t executed) noexcept {
    c.memory->set_current_ee_pc(pc);
    c.memory->set_current_guest_step(c.first_step + executed);
}

// Store with the bookkeeping of the ordinary path: main RAM records each
// 32-bit word, or each byte below a word as write_u8 does; the scratchpad
// records every byte. False when the bookkeeping cannot have moved an epoch
// ee_device_changed compares.
[[gnu::always_inline]] inline bool ee_store(EeAotContext& c, std::byte* p, const void* source,
                                            const std::uint32_t length) noexcept {
    const auto offset = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) -
                                                 reinterpret_cast<std::uintptr_t>(c.ram));
    if (offset >= kMainRamBytes) [[unlikely]] {
        std::memcpy(p, source, length);
        if (c.memory->ee_aot_scratchpad_store_records()) [[unlikely]] {
            ee_store_scratchpad(c, p, length);
            return true;
        }
        return false;
    }
    const auto* bytes = static_cast<const std::byte*>(source);
    bool recorded = false;
    if (length < 4U) {
        for (std::uint32_t index = 0U; index < length; ++index) {
            p[index] = bytes[index];
            recorded |= c.memory->ee_aot_record_main_ram_store(offset + index, 1U);
        }
        return recorded;
    }
    for (std::uint32_t word = 0U; word < length; word += 4U) {
        std::memcpy(p + word, bytes + word, 4U);
        recorded |= c.memory->ee_aot_record_main_ram_store(offset + word, 4U);
    }
    return recorded;
}
[[gnu::always_inline]] inline void ee_refresh_stores_plain(EeAotContext& c) noexcept {
    c.stores_plain = c.memory->ee_aot_stores_plain();
}
[[gnu::always_inline]] inline bool ee_store_allowed(const EeAotContext& c) noexcept {
    return !c.memory->ee_aot_store_needs_slow_path();
}

} // namespace takedown::guest::ee_aot_detail
