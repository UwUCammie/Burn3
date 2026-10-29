#include "takedown/content/elf_inspector.hpp"

#include <array>
#include <fstream>
#include <string>

namespace takedown::content {
namespace {

constexpr std::size_t elf32_header_size = 52U;
constexpr std::uint16_t elf_machine_mips = 8U;

[[noreturn]] void invalid_elf(const std::filesystem::path& file, const std::string& reason) {
    throw ElfInspectionError(file.string() + ": " + reason);
}

std::uint16_t read_u16(const std::array<std::uint8_t, elf32_header_size>& bytes,
                       const std::size_t offset) noexcept {
    const auto low = static_cast<std::uint16_t>(bytes[offset]);
    const auto high = static_cast<std::uint16_t>(bytes[offset + 1U]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

std::uint32_t read_u32(const std::array<std::uint8_t, elf32_header_size>& bytes,
                       const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

} // namespace

Elf32MipsInfo inspect_ps2_elf(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input) {
        throw ElfInspectionError("could not open boot executable: " + file.string());
    }

    const std::streampos end = input.tellg();
    if (end < 0) {
        throw ElfInspectionError("could not determine boot executable size: " + file.string());
    }
    const auto file_size = static_cast<unsigned long long>(end);
    if (file_size < elf32_header_size) {
        invalid_elf(file, "file is smaller than an ELF32 header");
    }

    input.seekg(0);
    std::array<std::uint8_t, elf32_header_size> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        invalid_elf(file, "could not read the complete ELF32 header");
    }

    if (header[0] != 0x7FU || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        invalid_elf(file, "missing ELF magic");
    }
    if (header[4] != 1U) {
        invalid_elf(file, "executable is not ELF32");
    }
    if (header[5] != 1U) {
        invalid_elf(file, "executable is not little-endian");
    }
    if (header[6] != 1U) {
        invalid_elf(file, "unsupported ELF identity version");
    }

    Elf32MipsInfo info{};
    info.object_type = read_u16(header, 16U);
    info.machine = read_u16(header, 18U);
    const auto version = read_u32(header, 20U);
    info.entry_point = read_u32(header, 24U);
    info.program_header_offset = read_u32(header, 28U);
    const auto header_size = read_u16(header, 40U);
    info.program_header_entry_size = read_u16(header, 42U);
    info.program_header_count = read_u16(header, 44U);
    info.file_size = file_size;

    if (info.machine != elf_machine_mips) {
        invalid_elf(file, "ELF machine is not MIPS");
    }
    if (version != 1U) {
        invalid_elf(file, "unsupported ELF header version");
    }
    if (header_size < elf32_header_size) {
        invalid_elf(file, "invalid ELF header size");
    }

    const std::uint64_t table_size =
        static_cast<std::uint64_t>(info.program_header_entry_size) * info.program_header_count;
    if (info.program_header_count != 0U &&
        (info.program_header_offset > file_size || file_size < table_size ||
         info.program_header_offset > file_size - table_size)) {
        invalid_elf(file, "program header table extends beyond the file");
    }

    return info;
}

} // namespace takedown::content
