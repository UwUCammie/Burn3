#pragma once

#include "takedown/content/elf_inspector.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace takedown::guest {
class EeMemory;
}

namespace takedown::content {

class ElfLoadError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct Elf32LoadSegment {
    std::uint32_t file_offset{};
    std::uint32_t virtual_address{};
    std::uint32_t physical_address{};
    std::uint32_t file_size{};
    std::uint32_t memory_size{};
    std::uint32_t flags{};
    std::uint32_t alignment{};

    [[nodiscard]] bool readable() const noexcept;
    [[nodiscard]] bool writable() const noexcept;
    [[nodiscard]] bool executable() const noexcept;
    [[nodiscard]] std::uint64_t address_end() const noexcept;
};

struct Elf32LoadPlan {
    Elf32MipsInfo executable;
    std::vector<Elf32LoadSegment> segments;

    [[nodiscard]] std::uint64_t required_memory_size() const noexcept;
    [[nodiscard]] bool contains_executable_file_range(std::uint32_t address,
                                                      std::size_t length) const noexcept;
};

// Parses and validates every PT_LOAD program header without loading guest code.
[[nodiscard]] Elf32LoadPlan read_ps2_elf_load_plan(const std::filesystem::path& file);

// Maps PT_LOAD data at its guest virtual address and zeroes each BSS tail
// through EeMemory's tracked bulk-write boundary. Accepting EeMemory directly
// prevents an escaped mutable RAM span from invalidating later code-page
// authentication.
[[nodiscard]] Elf32LoadPlan load_ps2_elf(const std::filesystem::path& file,
                                         guest::EeMemory& guest_memory);

} // namespace takedown::content
