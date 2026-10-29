#include "takedown/content/elf_static_analysis.hpp"

#include "takedown/content/elf_loader.hpp"
#include "takedown/guest/r5900_instruction.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string_view>

namespace takedown::content {
namespace {

// Bounds every table and the sum of scanned executable data so a hostile ELF
// cannot make the analyzer allocate unbounded memory.
constexpr std::uint64_t max_analysis_bytes = 64U * 1024U * 1024U;
constexpr std::uint16_t max_section_headers = 4096U;
constexpr std::uint32_t section_header_size = 40U;
constexpr std::uint32_t symbol_entry_size = 16U;

constexpr std::uint32_t sht_symtab = 2U;
constexpr std::uint32_t sht_strtab = 3U;
constexpr std::uint32_t sht_dynsym = 11U;
constexpr std::uint8_t stt_func = 2U;

constexpr std::string_view hex_digits = "0123456789abcdef";

struct SectionHeader {
    std::uint32_t type{};
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint32_t link{};
    std::uint32_t entry_size{};
};

struct FunctionBuilder {
    std::vector<ElfFunctionSymbol> symbols;
    bool is_entry_point{};
    bool is_symbol{};
    bool is_direct_call_target{};
    bool is_direct_jump_target{};
};

[[noreturn]] void invalid_analysis(const std::filesystem::path& file, const std::string& reason) {
    throw ElfStaticAnalysisError(file.string() + ": " + reason);
}

void read_exact(std::ifstream& input,
                const std::filesystem::path& file,
                const std::uint64_t offset,
                const std::span<std::uint8_t> destination,
                const std::string_view description) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        destination.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        invalid_analysis(file, std::string(description) + " is too large to read");
    }

    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(destination.size()));
    if (input.gcount() != static_cast<std::streamsize>(destination.size())) {
        invalid_analysis(file, "could not read complete " + std::string(description));
    }
}

bool fits_in_file(const std::uint64_t file_size,
                  const std::uint64_t offset,
                  const std::uint64_t size) noexcept {
    return offset <= file_size && size <= file_size - offset;
}

std::vector<std::uint8_t> read_bounded_table(std::ifstream& input,
                                                           const std::filesystem::path& file,
                                                           const SectionHeader& section,
                                                           const std::uint64_t file_size,
                                                           const std::string_view description) {
    if (section.size > max_analysis_bytes) {
        invalid_analysis(file, std::string(description) + " exceeds the analysis limit");
    }
    if (!fits_in_file(file_size, section.offset, section.size)) {
        invalid_analysis(file, std::string(description) + " extends beyond the file");
    }

    std::vector<std::uint8_t> bytes(section.size);
    if (!bytes.empty()) {
        read_exact(input, file, section.offset, bytes, description);
    }
    return bytes;
}

std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset] |
                                      static_cast<std::uint16_t>(bytes[offset + 1U] << 8U));
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::vector<SectionHeader> read_section_headers(std::ifstream& input,
                                                              const std::filesystem::path& file,
                                                              const std::uint64_t file_size) {
    std::array<std::uint8_t, 52> header{};
    read_exact(input, file, 0U, header, "ELF header");

    const auto section_offset = read_u32(header, 32U);
    const auto entry_size = read_u16(header, 46U);
    const auto count = read_u16(header, 48U);
    if (count == 0U) {
        return {};
    }
    if (count > max_section_headers) {
        invalid_analysis(file, "ELF has too many section headers");
    }
    if (entry_size != section_header_size) {
        invalid_analysis(file, "ELF section headers are not 40 bytes");
    }

    const auto table_size = static_cast<std::uint64_t>(count) * section_header_size;
    if (!fits_in_file(file_size, section_offset, table_size)) {
        invalid_analysis(file, "section header table extends beyond the file");
    }

    std::vector<SectionHeader> sections;
    sections.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        std::array<std::uint8_t, section_header_size> bytes{};
        const auto offset = section_offset + static_cast<std::uint64_t>(index) * section_header_size;
        read_exact(input, file, offset, bytes, "section header");
        sections.push_back({
            .type = read_u32(bytes, 4U),
            .offset = read_u32(bytes, 16U),
            .size = read_u32(bytes, 20U),
            .link = read_u32(bytes, 24U),
            .entry_size = read_u32(bytes, 36U),
        });
    }
    return sections;
}

std::string symbol_name(const std::filesystem::path& file,
                                      const std::span<const std::uint8_t> strings,
                                      const std::uint32_t offset) {
    if (offset >= strings.size()) {
        invalid_analysis(file, "function symbol name is outside its string table");
    }
    const auto begin = strings.begin() + offset;
    const auto end = std::find(begin, strings.end(), std::uint8_t{0});
    if (end == strings.end()) {
        invalid_analysis(file, "function symbol name is not terminated");
    }
    return std::string(reinterpret_cast<const char*>(std::to_address(begin)),
                       static_cast<std::size_t>(end - begin));
}

std::string json_string(const std::string_view value) {
    std::string result{'"'};
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (byte >= 0x20U && byte < 0x7FU) {
                result.push_back(static_cast<char>(byte));
            } else {
                result += "\\u00";
                result.push_back(hex_digits[byte >> 4U]);
                result.push_back(hex_digits[byte & 0x0FU]);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

// Seeds candidates from defined STT_FUNC symbols in every symbol table.
void add_symbol_functions(std::ifstream& input,
                          const std::filesystem::path& file,
                          const Elf32LoadPlan& load_plan,
                          const std::vector<SectionHeader>& sections,
                          const std::uint64_t file_size,
                          std::map<std::uint32_t, FunctionBuilder>& function_builders) {
    for (const auto& section : sections) {
        if (section.type != sht_symtab && section.type != sht_dynsym) {
            continue;
        }
        if (section.entry_size != symbol_entry_size || section.size % symbol_entry_size != 0U) {
            invalid_analysis(file, "ELF32 symbol table has an invalid entry size");
        }
        if (section.link >= sections.size() || sections[section.link].type != sht_strtab) {
            invalid_analysis(file, "ELF32 symbol table has an invalid string-table link");
        }

        const auto symbols = read_bounded_table(input, file, section, file_size, "symbol table");
        const auto strings =
            read_bounded_table(input, file, sections[section.link], file_size, "symbol string table");
        for (std::size_t offset = 0; offset < symbols.size(); offset += symbol_entry_size) {
            const auto entry = std::span<const std::uint8_t>(symbols).subspan(offset, symbol_entry_size);
            const auto info = entry[12];
            const auto section_index = read_u16(entry, 14U);
            if ((info & 0x0FU) != stt_func || section_index == 0U) {
                continue;
            }
            const auto address = read_u32(entry, 4U);
            if ((address & 3U) != 0U || !load_plan.contains_executable_file_range(address, 4U)) {
                continue;
            }

            auto& builder = function_builders[address];
            builder.is_symbol = true;
            builder.symbols.push_back({symbol_name(file, strings, read_u32(entry, 0U)), read_u32(entry, 8U)});
        }
    }
}

// Lists the independent reasons a candidate exists, in a fixed order.
void write_evidence(std::ostringstream& output, const ElfFunctionCandidate& function) {
    output << '[';
    bool first = true;
    const auto append = [&](const std::string_view value) {
        if (!first) {
            output << ',';
        }
        output << json_string(value);
        first = false;
    };
    if (function.is_entry_point) {
        append("entry");
    }
    if (function.is_symbol) {
        append("symbol");
    }
    if (function.is_direct_call_target) {
        append("direct_call");
    }
    if (function.is_direct_jump_target) {
        append("direct_jump");
    }
    output << ']';
}

// Prefers a real symbol name, then the ELF entry, then a stable address name.
std::string suggested_name(const std::uint32_t address, const FunctionBuilder& builder) {
    const auto named = std::find_if(builder.symbols.begin(), builder.symbols.end(),
                                    [](const ElfFunctionSymbol& symbol) { return !symbol.name.empty(); });
    if (named != builder.symbols.end()) {
        return named->name;
    }
    if (builder.is_entry_point) {
        return "entry";
    }
    return "sub_" + hex32(address).substr(2);
}

} // namespace

ElfStaticAnalysis analyze_ps2_elf_static(const std::filesystem::path& file) {
    const auto load_plan = read_ps2_elf_load_plan(file);
    if ((load_plan.executable.entry_point & 3U) != 0U ||
        !load_plan.contains_executable_file_range(load_plan.executable.entry_point, 4U)) {
        invalid_analysis(file, "ELF entry point is not a complete aligned instruction");
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw ElfStaticAnalysisError("could not open ELF for static analysis: " + file.string());
    }

    std::map<std::uint32_t, FunctionBuilder> function_builders;
    function_builders[load_plan.executable.entry_point].is_entry_point = true;

    const auto sections = read_section_headers(input, file, load_plan.executable.file_size);
    add_symbol_functions(input, file, load_plan, sections, load_plan.executable.file_size, function_builders);

    ElfStaticAnalysis result{};
    result.entry_point = load_plan.executable.entry_point;

    std::uint64_t executable_bytes = 0;
    for (const auto& segment : load_plan.segments) {
        if (!segment.executable() || segment.file_size < 4U) {
            continue;
        }
        executable_bytes += segment.file_size;
        if (executable_bytes > max_analysis_bytes) {
            invalid_analysis(file, "file-backed executable data exceeds the analysis limit");
        }

        std::vector<std::uint8_t> bytes(segment.file_size);
        read_exact(input, file, segment.file_offset, bytes, "executable segment");

        // Start at the first word-aligned guest address inside the segment.
        const auto alignment_skip = static_cast<std::uint64_t>((0U - segment.virtual_address) & 3U);
        for (std::size_t offset = alignment_skip; offset + 4U <= bytes.size(); offset += 4U) {
            const auto address64 = static_cast<std::uint64_t>(segment.virtual_address) + offset;
            if (address64 > std::numeric_limits<std::uint32_t>::max()) {
                break;
            }
            const auto address = static_cast<std::uint32_t>(address64);
            const auto word = read_u32(bytes, offset);
            ++result.scanned_word_count;

            const auto instruction = guest::decode_r5900(address, word);
            const bool is_call = instruction.kind == guest::R5900InstructionKind::JumpAndLink;
            const bool is_jump = instruction.kind == guest::R5900InstructionKind::Jump;
            if ((!is_call && !is_jump) ||
                (instruction.target & 3U) != 0U ||
                !load_plan.contains_executable_file_range(instruction.target, 4U)) {
                continue;
            }

            auto& builder = function_builders[instruction.target];
            const ElfDirectTransfer transfer{.site = address, .target = instruction.target};
            if (is_call) {
                builder.is_direct_call_target = true;
                result.calls.push_back(transfer);
            } else {
                builder.is_direct_jump_target = true;
                result.jump_candidates.push_back(transfer);
            }
        }
    }

    std::sort(result.calls.begin(), result.calls.end());
    std::sort(result.jump_candidates.begin(), result.jump_candidates.end());

    result.functions.reserve(function_builders.size());
    for (auto& [address, builder] : function_builders) {
        std::sort(builder.symbols.begin(), builder.symbols.end());
        builder.symbols.erase(std::unique(builder.symbols.begin(), builder.symbols.end()),
                              builder.symbols.end());
        result.functions.push_back({
            .address = address,
            .suggested_name = suggested_name(address, builder),
            .symbols = std::move(builder.symbols),
            .is_entry_point = builder.is_entry_point,
            .is_symbol = builder.is_symbol,
            .is_direct_call_target = builder.is_direct_call_target,
            .is_direct_jump_target = builder.is_direct_jump_target,
        });
    }
    return result;
}

std::string elf_static_analysis_json(const ElfStaticAnalysis& analysis) {
    std::ostringstream output;
    output << "{\n"
           << "  \"format\": \"takedown-elf-static-analysis-v1\",\n"
           << "  \"entry_point\": " << json_string(hex32(analysis.entry_point)) << ",\n"
           << "  \"scanned_words\": " << analysis.scanned_word_count << ",\n"
           << "  \"functions\": [\n";
    for (std::size_t index = 0; index < analysis.functions.size(); ++index) {
        const auto& function = analysis.functions[index];
        output << "    {\"address\":" << json_string(hex32(function.address))
               << ",\"suggested_name\":" << json_string(function.suggested_name) << ",\"evidence\":";
        write_evidence(output, function);
        output << ",\"symbols\":[";
        for (std::size_t symbol_index = 0; symbol_index < function.symbols.size(); ++symbol_index) {
            const auto& symbol = function.symbols[symbol_index];
            output << "{\"name\":" << json_string(symbol.name) << ",\"size\":" << symbol.size << '}';
            if (symbol_index + 1U != function.symbols.size()) {
                output << ',';
            }
        }
        output << "]}";
        if (index + 1U != analysis.functions.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n"
           << "  \"calls\": [\n";
    for (std::size_t index = 0; index < analysis.calls.size(); ++index) {
        const auto& transfer = analysis.calls[index];
        output << "    {\"site\":" << json_string(hex32(transfer.site))
               << ",\"target\":" << json_string(hex32(transfer.target)) << '}';
        if (index + 1U != analysis.calls.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n"
           << "  \"jump_candidates\": [\n";
    for (std::size_t index = 0; index < analysis.jump_candidates.size(); ++index) {
        const auto& transfer = analysis.jump_candidates[index];
        output << "    {\"site\":" << json_string(hex32(transfer.site))
               << ",\"target\":" << json_string(hex32(transfer.target)) << '}';
        if (index + 1U != analysis.jump_candidates.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n"
           << "}\n";
    return output.str();
}

} // namespace takedown::content
