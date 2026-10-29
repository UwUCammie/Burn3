#pragma once

// Typed loads and stores of game data in guest memory, for adapters between
// engine code and the original game's memory layouts. They go through
// EeMemory's ordinary accessors, which keep every store visible to the
// runtime's bookkeeping (code-change detection, graphics fences).

#include "takedown/engine/math/vector.hpp"
#include "takedown/guest/ee_memory.hpp"

#include <bit>
#include <cstdint>

namespace takedown::runtime {

[[nodiscard]] inline float load_float(const guest::EeMemory& memory, const guest::GuestAddress address) {
    return std::bit_cast<float>(memory.read_u32_le(address));
}

inline void store_float(guest::EeMemory& memory, const guest::GuestAddress address, const float value) {
    memory.write_u32_le(address, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] inline engine::Vector4 load_vector4(const guest::EeMemory& memory,
                                                  const guest::GuestAddress address) {
    return {load_float(memory, address), load_float(memory, address + 4U),
            load_float(memory, address + 8U), load_float(memory, address + 12U)};
}

inline void store_vector4(guest::EeMemory& memory, const guest::GuestAddress address,
                          const engine::Vector4& value) {
    store_float(memory, address, value.x);
    store_float(memory, address + 4U, value.y);
    store_float(memory, address + 8U, value.z);
    store_float(memory, address + 12U, value.w);
}

// A 16-byte unit copied without interpreting it.
inline void copy_quadword(guest::EeMemory& memory, const guest::GuestAddress destination,
                          const guest::GuestAddress source) {
    for (guest::GuestAddress offset = 0U; offset < 16U; offset += 4U) {
        memory.write_u32_le(destination + offset, memory.read_u32_le(source + offset));
    }
}

} // namespace takedown::runtime
