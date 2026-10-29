#include "takedown/recompiler/reviewed_function_map.hpp"

#include "takedown/content/elf_loader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <map>
#include <span>
#include <sstream>

namespace takedown::recompiler {
namespace {

using namespace std::string_view_literals;

// One source line (BOM and CR/LF stripped) with its 1-based line number.
struct TextLine {
    std::string_view text;
    std::size_t number{};
};

[[noreturn]] void invalid_map(const std::string& message) {
    throw ReviewedFunctionMapError(message);
}

[[noreturn]] void invalid_line(const std::size_t line, const std::string& message) {
    invalid_map("function map line " + std::to_string(line) + ": " + message);
}

// A file with no text still yields one empty line so later errors can name line 1.
std::vector<TextLine> text_lines(std::string_view text) {
    std::vector<TextLine> result;
    std::size_t line_number = 1;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line_number == 1 && line.size() > 2 && line[0] == static_cast<char>(0xEF) &&
            line[1] == static_cast<char>(0xBB) && line[2] == static_cast<char>(0xBF)) {
            line.remove_prefix(3);
        }
        result.push_back({line, line_number});
        ++line_number;
        if (newline == std::string_view::npos) {
            text = {};
        } else {
            text.remove_prefix(newline + 1);
        }
    }
    if (result.empty()) {
        result.push_back({{}, 1});
    }
    return result;
}

// Only space and tab count as padding; any other byte is data.
std::string_view trim_ascii(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// Decimal or 0x-prefixed hex, with nothing else on the field.
std::uint64_t parse_unsigned(const std::string_view text, const std::size_t line,
                                           const std::string_view field_name) {
    auto value_text = trim_ascii(text);
    int base = 10;
    if (value_text.size() > 1 && value_text[0] == '0' &&
        (value_text[1] == 'x' || value_text[1] == 'X')) {
        base = 16;
        value_text.remove_prefix(2);
    }
    if (value_text.empty()) {
        invalid_line(line, std::string(field_name) + " is empty");
    }

    std::uint64_t value = 0;
    const auto parsed = std::from_chars(value_text.data(), value_text.data() + value_text.size(),
                                        value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != value_text.data() + value_text.size()) {
        invalid_line(line, std::string(field_name) + " is not an unsigned integer");
    }
    return value;
}

// Splits one CSV record. Quoted fields may contain commas and doubled quotes;
// unquoted fields are trimmed. Multiline names are rejected earlier.
std::vector<std::string> parse_csv_line(const TextLine& line) {
    std::vector<std::string> fields;
    std::size_t cursor = 0;
    while (true) {
        std::string field;
        if (cursor < line.text.size() && line.text[cursor] == '"') {
            ++cursor;
            bool closed = false;
            while (cursor < line.text.size()) {
                const char character = line.text[cursor++];
                if (character != '"') {
                    field.push_back(character);
                    continue;
                }
                if (cursor < line.text.size() && line.text[cursor] == '"') {
                    field.push_back('"');
                    ++cursor;
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed) {
                invalid_line(line.number,
                             "unterminated quoted CSV field; multiline names are unsupported");
            }
            while (cursor < line.text.size() &&
                   (line.text[cursor] == ' ' || line.text[cursor] == '\t')) {
                ++cursor;
            }
            if (cursor < line.text.size() && line.text[cursor] != ',') {
                invalid_line(line.number, "unexpected data after a quoted CSV field");
            }
        } else {
            const auto comma = line.text.find(',', cursor);
            const auto end = comma == std::string_view::npos ? line.text.size() : comma;
            field = std::string(trim_ascii(line.text.substr(cursor, end - cursor)));
            cursor = end;
        }
        fields.push_back(std::move(field));
        if (cursor == line.text.size()) {
            break;
        }
        ++cursor;
        if (cursor == line.text.size()) {
            fields.emplace_back();
            break;
        }
    }
    return fields;
}

void validate_name(const std::string& name, const std::size_t line) {
    if (name.empty()) {
        invalid_line(line, "function Name is empty");
    }
    for (const unsigned char byte : name) {
        if (byte <= 31U || byte == 127U) {
            invalid_line(line, "function Name contains a control character");
        }
    }
}

ReviewedFunctionExtent parse_function_row(const TextLine& line,
                                                        const bool has_code_hash) {
    const std::vector<std::string> fields = parse_csv_line(line);
    const std::size_t expected_fields = has_code_hash ? 5U : 4U;
    if (fields.size() != expected_fields) {
        invalid_line(line.number, "expected " + std::to_string(expected_fields) +
                                      " CSV fields, found " + std::to_string(fields.size()));
    }
    validate_name(fields[0], line.number);

    const std::uint64_t start = parse_unsigned(fields[1], line.number, "Start");
    const std::uint64_t end = parse_unsigned(fields[2], line.number, "End");
    const std::uint64_t size = parse_unsigned(fields[3], line.number, "Size");
    if (start > 0xFFFFFFFFULL) {
        invalid_line(line.number, "Start is outside the 32-bit EE address space");
    }
    if (end > 0x100000000ULL || start >= end) {
        invalid_line(line.number, "End must be after Start within the 32-bit EE address space");
    }
    const auto span_size = end - start;
    if (size != span_size) {
        invalid_line(line.number, "Size does not equal End-Start; discontiguous Ghidra bodies "
                                  "must be reviewed and split before import");
    }
    if (span_size > 0xFFFFFFFFULL) {
        invalid_line(line.number, "function extent is too large");
    }
    if ((start & 3U) != 0U || (end & 3U) != 0U) {
        invalid_line(line.number, "function extent is not EE instruction aligned");
    }
    return {fields[0], static_cast<guest::GuestAddress>(start),
            static_cast<std::uint32_t>(span_size),
            has_code_hash ? parse_unsigned(fields[4], line.number, "CodeFnv1a64") : 0U};
}

// A function-count ceiling keeps a hostile or corrupt map from exhausting memory.
constexpr std::size_t kMaxFunctions = 100000U;

// Sorts by start address and rejects anything that is not a plain, aligned,
// non-overlapping set of EE extents. A load plan, when given, must also contain
// every extent in one file-backed executable segment.
void validate_structure(std::vector<ReviewedFunctionExtent>& functions,
                        const guest::GuestAddress entry_point,
                        const content::Elf32LoadPlan* load_plan) {
    if (functions.empty()) {
        invalid_map("function map contains no functions");
    }
    if (functions.size() > kMaxFunctions) {
        invalid_map("function map exceeds the function-count safety limit");
    }
    std::sort(functions.begin(), functions.end(),
              [](const ReviewedFunctionExtent& left, const ReviewedFunctionExtent& right) {
                  return left.start_address < right.start_address;
              });

    bool contains_entry = false;
    std::uint64_t previous_end = 0;
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const ReviewedFunctionExtent& function = functions[index];
        if (function.byte_size == 0 || (function.start_address & 3U) != 0U ||
            (function.byte_size & 3U) != 0U || function.end_address() > 0x100000000ULL) {
            invalid_map("function " + function.name + " has an invalid EE extent");
        }
        if (index != 0 && previous_end > function.start_address) {
            invalid_map("function " + function.name + " overlaps the preceding reviewed extent");
        }
        if (load_plan != nullptr &&
            !load_plan->contains_executable_file_range(function.start_address,
                                                        function.byte_size)) {
            invalid_map("function " + function.name +
                        " is not wholly contained in one file-backed executable PT_LOAD segment");
        }
        contains_entry = contains_entry || function.start_address == entry_point;
        previous_end = function.end_address();
    }
    if (!contains_entry) {
        invalid_map("function map does not contain a function starting at the ELF entry point");
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        invalid_map("could not inspect function map " + path.string() + ": " + error.message());
    }
    if (file_size > 16U * 1024U * 1024U) {
        invalid_map("function map exceeds the 16 MiB input limit: " + path.string());
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        invalid_map("could not open function map: " + path.string());
    }
    std::string result(file_size, '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
        invalid_map("could not read complete function map: " + path.string());
    }
    return result;
}

// 64-bit FNV-1a, enough to notice a changed revision; not a security hash.
class Fnv1a64 {
  public:
    void add(const unsigned char byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }

    void add_u32(const std::uint32_t value) noexcept {
        for (unsigned int shift = 0; shift != 32U; shift += 8U) {
            add(static_cast<unsigned char>((value >> shift) & 0xFFU));
        }
    }

    void add_text(const std::string_view text) noexcept {
        for (const unsigned char byte : text) {
            add(byte);
        }
    }

    std::uint64_t value() const noexcept {
        return value_;
    }

  private:
    std::uint64_t value_ = 14695981039346656037ULL;
};

void add_file_bytes(std::ifstream& input, const std::filesystem::path& file,
                    const std::uint64_t offset, std::uint64_t byte_count, Fnv1a64& hash) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) {
        invalid_map("could not seek in ELF while fingerprinting: " + file.string());
    }

    std::array<unsigned char, 65536> buffer{};
    while (byte_count != 0) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(byte_count, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            invalid_map("could not read executable bytes while fingerprinting: " + file.string());
        }
        for (const unsigned char byte : std::span<const unsigned char>(buffer.data(), chunk)) {
            hash.add(byte);
        }
        byte_count -= chunk;
    }
}

std::vector<content::Elf32LoadSegment>
executable_segments(const content::Elf32LoadPlan& load_plan) {
    std::vector<content::Elf32LoadSegment> result;
    for (const content::Elf32LoadSegment& segment : load_plan.segments) {
        if (segment.executable()) {
            result.push_back(segment);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const content::Elf32LoadSegment& left, const content::Elf32LoadSegment& right) {
                  if (left.virtual_address != right.virtual_address) {
                      return left.virtual_address < right.virtual_address;
                  }
                  return left.file_offset < right.file_offset;
              });
    return result;
}

EeExecutableFingerprint
fingerprint_executable(const std::filesystem::path& elf_file,
                       const content::Elf32LoadPlan& load_plan) {
    const auto segments = executable_segments(load_plan);
    if (segments.empty()) {
        invalid_map("ELF has no executable PT_LOAD segment");
    }
    if (segments.size() > 0xFFFFFFFFULL) {
        invalid_map("ELF executable segment count cannot be represented");
    }

    std::ifstream input(elf_file, std::ios::binary);
    if (!input) {
        invalid_map("could not open ELF while fingerprinting: " + elf_file.string());
    }

    Fnv1a64 hash;
    hash.add_text(std::string_view("takedown-ee-executable-v1\0", 26));
    hash.add_u32(load_plan.executable.entry_point);
    hash.add_u32(static_cast<std::uint32_t>(segments.size()));

    std::uint64_t executable_bytes = 0;
    for (const content::Elf32LoadSegment& segment : segments) {
        hash.add_u32(segment.virtual_address);
        hash.add_u32(segment.file_size);
        hash.add_u32(segment.memory_size);
        hash.add_u32(segment.flags);
        executable_bytes += segment.file_size;
        add_file_bytes(input, elf_file, segment.file_offset, segment.file_size, hash);
    }
    return {load_plan.executable.entry_point, executable_bytes,
            static_cast<std::uint32_t>(segments.size()), hash.value()};
}

std::uint64_t fingerprint_function(std::ifstream& input,
                                                 const std::filesystem::path& elf_file,
                                                 const content::Elf32LoadPlan& load_plan,
                                                 const ReviewedFunctionExtent& function) {
    const auto segment = std::find_if(
        load_plan.segments.begin(), load_plan.segments.end(),
        [&](const content::Elf32LoadSegment& candidate) {
            return candidate.executable() &&
                   load_plan.contains_executable_file_range(function.start_address,
                                                            function.byte_size) &&
                   function.start_address >= candidate.virtual_address &&
                   function.end_address() <=
                       static_cast<std::uint64_t>(candidate.virtual_address) +
                           candidate.file_size;
        });
    if (segment == load_plan.segments.end()) {
        invalid_map("function " + function.name +
                    " is outside file-backed executable data while hashing");
    }

    Fnv1a64 hash;
    hash.add_text(std::string_view("takedown-ee-function-v1\0", 24));
    hash.add_u32(function.start_address);
    hash.add_u32(function.byte_size);
    const auto file_offset = static_cast<std::uint64_t>(segment->file_offset) +
                             (function.start_address - segment->virtual_address);
    add_file_bytes(input, elf_file, file_offset, function.byte_size, hash);
    return hash.value();
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string hex64(const std::uint64_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

// Quotes a name only when a comma or quote would otherwise change the record.
std::string csv_field(const std::string_view value) {
    if (value.find_first_of(",\"") == std::string_view::npos) {
        return std::string(value);
    }
    std::string result{'"'};
    for (const char character : value) {
        if (character == '"') {
            result += "\"\"";
        } else {
            result.push_back(character);
        }
    }
    result.push_back('"');
    return result;
}

constexpr std::string_view kManifestHeader = "# takedown-reviewed-function-map-v1";
constexpr std::string_view kCsvHeader = "Name,Start,End,Size,CodeFnv1a64";

// Parses the Ghidra Name,Start,End,Size export into validated extents.
std::vector<ReviewedFunctionExtent>
parse_ghidra_csv(const std::string_view text, const guest::GuestAddress entry_point,
                          const content::Elf32LoadPlan& load_plan) {
    const std::vector<TextLine> lines = text_lines(text);
    std::size_t header_index = 0;
    while (header_index < lines.size() && trim_ascii(lines[header_index].text).empty()) {
        ++header_index;
    }
    if (header_index == lines.size() ||
        trim_ascii(lines[header_index].text) != "Name,Start,End,Size"sv) {
        invalid_map("Ghidra function CSV must begin with Name,Start,End,Size");
    }

    std::vector<ReviewedFunctionExtent> functions;
    for (std::size_t index = header_index + 1; index < lines.size(); ++index) {
        if (trim_ascii(lines[index].text).empty()) {
            continue;
        }
        functions.push_back(parse_function_row(lines[index], false));
        if (functions.size() > kMaxFunctions) {
            invalid_map("function map exceeds the function-count safety limit");
        }
    }
    validate_structure(functions, entry_point, &load_plan);
    return functions;
}

} // namespace

ReviewedFunctionMap import_ghidra_function_map_csv(const std::filesystem::path& elf_file,
                                                   const std::filesystem::path& csv_file) {
    const content::Elf32LoadPlan load_plan = content::read_ps2_elf_load_plan(elf_file);
    ReviewedFunctionMap result{
        fingerprint_executable(elf_file, load_plan),
        parse_ghidra_csv(read_text_file(csv_file), load_plan.executable.entry_point,
                                  load_plan)};

    std::ifstream input(elf_file, std::ios::binary);
    if (!input) {
        invalid_map("could not open ELF while hashing functions: " + elf_file.string());
    }
    for (ReviewedFunctionExtent& function : result.functions) {
        function.code_fnv1a64 = fingerprint_function(input, elf_file, load_plan, function);
    }
    return result;
}

std::string reviewed_function_map_manifest(const ReviewedFunctionMap& function_map) {
    // Sorted copy so the manifest is stable regardless of input order.
    auto functions = function_map.functions;
    validate_structure(functions, function_map.executable.entry_point, nullptr);

    std::ostringstream output;
    output << kManifestHeader << '\n'
           << "# entry_point=" << hex32(function_map.executable.entry_point) << '\n'
           << "# executable_bytes=" << function_map.executable.executable_file_bytes << '\n'
           << "# executable_segments=" << function_map.executable.executable_segment_count << '\n'
           << "# executable_fnv1a64=" << hex64(function_map.executable.fnv1a64) << '\n'
           << "# function_count=" << functions.size() << '\n'
           << kCsvHeader << '\n';
    for (const ReviewedFunctionExtent& function : functions) {
        output << csv_field(function.name) << ',' << hex32(function.start_address) << ','
               << hex64(function.end_address()) << ',' << function.byte_size << ','
               << hex64(function.code_fnv1a64) << '\n';
    }
    return output.str();
}

ReviewedFunctionMap parse_reviewed_function_map_manifest(const std::string_view manifest) {
    static constexpr std::array<std::string_view, 5> kKnownKeys{
        "entry_point", "executable_bytes", "executable_segments", "executable_fnv1a64",
        "function_count"};

    const std::vector<TextLine> lines = text_lines(manifest);
    std::size_t cursor = 0;
    while (cursor < lines.size() && trim_ascii(lines[cursor].text).empty()) {
        ++cursor;
    }
    if (cursor == lines.size() || trim_ascii(lines[cursor].text) != kManifestHeader) {
        invalid_map("reviewed function map has no takedown-reviewed-function-map-v1 header");
    }
    ++cursor;

    // key -> (value text, source line) so later errors can name the line.
    std::map<std::string, std::pair<std::string, std::uint64_t>> metadata;
    while (cursor < lines.size()) {
        const auto line = trim_ascii(lines[cursor].text);
        if (line.empty()) {
            ++cursor;
            continue;
        }
        if (line == kCsvHeader) {
            break;
        }
        if (!line.starts_with("# ")) {
            invalid_line(lines[cursor].number, "expected manifest metadata or CSV header");
        }
        const auto assignment = line.substr(2);
        const auto equals = assignment.find('=');
        if (equals == std::string_view::npos) {
            invalid_line(lines[cursor].number, "manifest metadata has no '=' separator");
        }
        const std::string key(trim_ascii(assignment.substr(0, equals)));
        const std::string value(trim_ascii(assignment.substr(equals + 1)));
        if (std::find(kKnownKeys.begin(), kKnownKeys.end(), key) == kKnownKeys.end()) {
            invalid_line(lines[cursor].number, "unknown manifest metadata key " + key);
        }
        if (!metadata
                 .emplace(key, std::pair<std::string, std::uint64_t>{value, lines[cursor].number})
                 .second) {
            invalid_line(lines[cursor].number, "duplicate manifest metadata key " + key);
        }
        ++cursor;
    }
    if (cursor == lines.size()) {
        invalid_map("reviewed function map has no " + std::string(kCsvHeader) + " CSV header");
    }
    ++cursor;

    const auto metadata_value = [&metadata](const std::string& key) -> const auto& {
        const auto found = metadata.find(key);
        if (found == metadata.end()) {
            invalid_map("reviewed function map is missing metadata key " + key);
        }
        return found->second;
    };
    const auto& entry_point_value = metadata_value("entry_point");
    const auto& executable_bytes_value = metadata_value("executable_bytes");
    const auto& executable_segments_value = metadata_value("executable_segments");
    const auto& executable_fnv1a64_value = metadata_value("executable_fnv1a64");
    const auto& function_count_value = metadata_value("function_count");

    const std::uint64_t entry_point =
        parse_unsigned(entry_point_value.first, entry_point_value.second, "entry_point");
    const std::uint64_t executable_segments = parse_unsigned(
        executable_segments_value.first, executable_segments_value.second, "executable_segments");
    const std::uint64_t function_count =
        parse_unsigned(function_count_value.first, function_count_value.second, "function_count");
    if (entry_point > 0xFFFFFFFFULL || executable_segments > 0xFFFFFFFFULL ||
        function_count > kMaxFunctions) {
        invalid_map("reviewed function map metadata exceeds its supported range");
    }

    ReviewedFunctionMap result{
        {static_cast<guest::GuestAddress>(entry_point),
         parse_unsigned(executable_bytes_value.first, executable_bytes_value.second,
                        "executable_bytes"),
         static_cast<std::uint32_t>(executable_segments),
         parse_unsigned(executable_fnv1a64_value.first, executable_fnv1a64_value.second,
                        "executable_fnv1a64")}};

    for (; cursor < lines.size(); ++cursor) {
        if (trim_ascii(lines[cursor].text).empty()) {
            continue;
        }
        result.functions.push_back(parse_function_row(lines[cursor], true));
        if (result.functions.size() > kMaxFunctions) {
            invalid_map("function map exceeds the function-count safety limit");
        }
    }
    if (function_count != result.functions.size()) {
        invalid_map("function_count metadata does not match the number of CSV records");
    }
    validate_structure(result.functions, result.executable.entry_point, nullptr);
    return result;
}

ReviewedFunctionMap read_reviewed_function_map_manifest(const std::filesystem::path& manifest_file) {
    return parse_reviewed_function_map_manifest(read_text_file(manifest_file));
}

void verify_reviewed_function_map(const std::filesystem::path& elf_file,
                                  const ReviewedFunctionMap& function_map) {
    const content::Elf32LoadPlan load_plan = content::read_ps2_elf_load_plan(elf_file);
    auto functions = function_map.functions;
    validate_structure(functions, load_plan.executable.entry_point, &load_plan);

    const EeExecutableFingerprint executable = fingerprint_executable(elf_file, load_plan);
    if (executable != function_map.executable) {
        invalid_map("reviewed function map executable fingerprint does not match " +
                    elf_file.string());
    }

    std::ifstream input(elf_file, std::ios::binary);
    if (!input) {
        invalid_map("could not open ELF while verifying functions: " + elf_file.string());
    }
    for (const ReviewedFunctionExtent& function : functions) {
        if (fingerprint_function(input, elf_file, load_plan, function) != function.code_fnv1a64) {
            invalid_map("function code fingerprint mismatch at " + hex32(function.start_address) +
                        " (" + function.name + ")");
        }
    }
}

} // namespace takedown::recompiler
