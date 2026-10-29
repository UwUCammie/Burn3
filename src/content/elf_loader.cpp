#include "takedown/content/elf_loader.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>

namespace takedown::content {
namespace {

constexpr std::uint16_t max_program_headers = 128U;
constexpr std::uint16_t program_header_size = 32U;
constexpr std::uint32_t pt_load = 1U;
constexpr std::uint32_t pf_x = 1U;
constexpr std::uint32_t pf_w = 2U;
constexpr std::uint32_t pf_r = 4U;
constexpr std::uint64_t guest_address_limit = 0x1'0000'0000ULL;

[[noreturn]] void invalid_segment(const std::filesystem::path& file,
                                  const std::size_t index,
                                  const std::string& reason) {
    throw ElfLoadError(file.string() + ": invalid PT_LOAD " + std::to_string(index) + ": " +
                       reason);
}

std::uint32_t read_u32(const std::array<std::uint8_t, program_header_size>& bytes,
                       const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool is_power_of_two(const std::uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

void reject_overlapping_segments(const std::filesystem::path& file,
                                 const std::vector<Elf32LoadSegment>& segments) {
    // Loading overlapping segments would make the result depend on header order.
    for (std::size_t left = 0; left < segments.size(); ++left) {
        if (segments[left].memory_size == 0U) {
            continue;
        }
        for (std::size_t right = left + 1U; right < segments.size(); ++right) {
            if (segments[right].memory_size == 0U) {
                continue;
            }
            if (segments[left].virtual_address < segments[right].address_end() &&
                segments[right].virtual_address < segments[left].address_end()) {
                throw ElfLoadError(file.string() +
                                   ": overlapping PT_LOAD memory ranges are unsupported");
            }
        }
    }
}

} // namespace

bool Elf32LoadSegment::readable() const noexcept {
    return (flags & pf_r) != 0U;
}

bool Elf32LoadSegment::writable() const noexcept {
    return (flags & pf_w) != 0U;
}

bool Elf32LoadSegment::executable() const noexcept {
    return (flags & pf_x) != 0U;
}

std::uint64_t Elf32LoadSegment::address_end() const noexcept {
    return static_cast<std::uint64_t>(virtual_address) + memory_size;
}

std::uint64_t Elf32LoadPlan::required_memory_size() const noexcept {
    std::uint64_t result = 0;
    for (const auto& segment : segments) {
        if (segment.memory_size != 0U) {
            result = std::max(result, segment.address_end());
        }
    }
    return result;
}

bool Elf32LoadPlan::contains_executable_file_range(const std::uint32_t address,
                                                   const std::size_t length) const noexcept {
    const auto begin = static_cast<std::uint64_t>(address);
    const auto end = begin + length;
    if (end < begin) {
        return false;
    }
    return std::any_of(segments.begin(), segments.end(), [&](const Elf32LoadSegment& segment) {
        const auto segment_begin = static_cast<std::uint64_t>(segment.virtual_address);
        const auto segment_end = segment_begin + segment.file_size;
        return segment.executable() && begin >= segment_begin && end <= segment_end;
    });
}

Elf32LoadPlan read_ps2_elf_load_plan(const std::filesystem::path& file) {
    Elf32LoadPlan plan{};
    plan.executable = inspect_ps2_elf(file);
    if (plan.executable.program_header_count == 0U) {
        throw ElfLoadError(file.string() + ": ELF has no program headers");
    }
    if (plan.executable.program_header_count > max_program_headers) {
        throw ElfLoadError(file.string() + ": ELF has too many program headers");
    }
    if (plan.executable.program_header_entry_size != program_header_size) {
        throw ElfLoadError(file.string() + ": ELF program headers are not 32 bytes");
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw ElfLoadError("could not open ELF for segment parsing: " + file.string());
    }

    for (std::uint16_t index = 0; index < plan.executable.program_header_count; ++index) {
        const std::uint64_t offset =
            plan.executable.program_header_offset +
            static_cast<std::uint64_t>(index) * plan.executable.program_header_entry_size;
        input.seekg(static_cast<std::streamoff>(offset));
        std::array<std::uint8_t, program_header_size> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw ElfLoadError(file.string() + ": could not read program header " +
                               std::to_string(index));
        }

        if (read_u32(bytes, 0U) != pt_load) {
            continue;
        }

        Elf32LoadSegment segment{
            .file_offset = read_u32(bytes, 4U),
            .virtual_address = read_u32(bytes, 8U),
            .physical_address = read_u32(bytes, 12U),
            .file_size = read_u32(bytes, 16U),
            .memory_size = read_u32(bytes, 20U),
            .flags = read_u32(bytes, 24U),
            .alignment = read_u32(bytes, 28U),
        };

        if (segment.memory_size < segment.file_size) {
            invalid_segment(file, index, "file size exceeds memory size");
        }
        const std::uint64_t file_end =
            static_cast<std::uint64_t>(segment.file_offset) + segment.file_size;
        if (file_end > plan.executable.file_size) {
            invalid_segment(file, index, "file data extends beyond the ELF");
        }
        if (segment.address_end() > guest_address_limit) {
            invalid_segment(file, index, "guest address range overflows 32 bits");
        }
        if (segment.alignment > 1U) {
            if (!is_power_of_two(segment.alignment)) {
                invalid_segment(file, index, "alignment is not a power of two");
            }
            if (segment.virtual_address % segment.alignment !=
                segment.file_offset % segment.alignment) {
                invalid_segment(file, index, "file and virtual addresses are misaligned");
            }
        }
        plan.segments.push_back(segment);
    }

    if (plan.segments.empty()) {
        throw ElfLoadError(file.string() + ": ELF has no PT_LOAD segments");
    }
    reject_overlapping_segments(file, plan.segments);

    if (!plan.contains_executable_file_range(plan.executable.entry_point, 1U)) {
        throw ElfLoadError(file.string() + ": entry point is outside executable PT_LOAD data");
    }
    return plan;
}

// RECOVERY: the only remaining difference from the original object is the
// operand order of one addition (virtual_address + memory_size) inside the
// inlined required_memory_size(); it comes from GCC's SSA numbering, not the source.
Elf32LoadPlan load_ps2_elf(const std::filesystem::path& file, guest::EeMemory& guest_memory) {
    auto plan = read_ps2_elf_load_plan(file);
    if (plan.required_memory_size() > guest_memory.size()) {
        throw ElfLoadError(file.string() + ": PT_LOAD data does not fit in supplied guest RAM");
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw ElfLoadError("could not open ELF for loading: " + file.string());
    }

    // Read every segment before touching guest RAM so a short read cannot
    // leave a partially loaded executable behind.
    std::vector<std::vector<std::byte>> file_data(plan.segments.size());
    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        const auto& segment = plan.segments[index];
        if (segment.file_size == 0U) {
            continue;
        }
        auto& data = file_data[index];
        data.resize(segment.file_size);
        input.seekg(static_cast<std::streamoff>(segment.file_offset));
        input.read(reinterpret_cast<char*>(data.data()),
                   static_cast<std::streamsize>(segment.file_size));
        if (input.gcount() != static_cast<std::streamsize>(segment.file_size)) {
            throw ElfLoadError(file.string() + ": short read while loading a PT_LOAD segment");
        }
    }

    for (std::size_t index = 0; index < plan.segments.size(); ++index) {
        const auto& segment = plan.segments[index];
        if (segment.memory_size == 0U) {
            continue;
        }
        const auto& data = file_data[index];
        if (!data.empty()) {
            guest_memory.write_main_ram_bytes(segment.virtual_address, data);
        }
        const auto bss_size = static_cast<std::uint64_t>(segment.memory_size - segment.file_size);
        if (bss_size != 0U) {
            guest_memory.fill_main_ram_bytes(segment.virtual_address + segment.file_size, bss_size,
                                             std::byte{0});
        }
    }
    return plan;
}

} // namespace takedown::content
