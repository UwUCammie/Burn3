#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace takedown::content {

class ElfInspectionError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct Elf32MipsInfo {
    std::uint16_t object_type{};
    std::uint16_t machine{};
    std::uint32_t entry_point{};
    std::uint32_t program_header_offset{};
    std::uint16_t program_header_entry_size{};
    std::uint16_t program_header_count{};
    std::uint64_t file_size{};
};

// Validates the ELF identity, 32-bit class, little-endian byte order, MIPS
// machine value, and fixed header before returning the fields needed by the
// recompiler.
[[nodiscard]] Elf32MipsInfo inspect_ps2_elf(const std::filesystem::path& file);

} // namespace takedown::content
