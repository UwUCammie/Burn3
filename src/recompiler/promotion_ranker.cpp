#include "takedown/recompiler/promotion_ranker.hpp"

#include "takedown/content/elf_static_analysis.hpp"
#include "takedown/recompiler/reviewed_function_map.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace takedown::recompiler {

namespace {

[[noreturn]] void invalid_line(const std::filesystem::path& path, const std::uint64_t line,
                               const std::string& message) {
    throw PromotionRankerError(path.string() + ":" + std::to_string(line) + ": " + message);
}

[[nodiscard]] std::size_t kind_index(const std::string_view kind) {
    if (kind == "scalar") {
        return 0U;
    }
    if (kind == "compiled") {
        return 1U;
    }
    if (kind == "native") {
        return 2U;
    }
    if (kind == "hle") {
        return 3U;
    }
    throw PromotionRankerError("unknown EE PC sample kind: " + std::string(kind));
}

[[nodiscard]] std::vector<std::string_view> split_tabs(const std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0U;
    while (true) {
        const std::size_t tab = text.find('\t', start);
        if (tab == std::string_view::npos) {
            fields.push_back(text.substr(start));
            break;
        }
        fields.push_back(text.substr(start, tab - start));
        start = tab + 1U;
    }
    return fields;
}

[[nodiscard]] std::uint64_t parse_u64(const std::string_view field, const std::filesystem::path& path, const std::uint64_t line,
                        const std::string& name) {
    if (field.empty()) {
        invalid_line(path, line, name + " is empty");
    }
    std::uint64_t value = 0U;
    const char* const end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        invalid_line(path, line, name + " is not a decimal integer");
    }
    return value;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        throw PromotionRankerError("cannot stat " + path.string());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw PromotionRankerError("cannot stat " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw PromotionRankerError("cannot open " + path.string());
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(size))) {
        throw PromotionRankerError("cannot read complete " + path.string());
    }
    return text;
}

[[nodiscard]] std::vector<std::string_view> split_lines(const std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0U;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        lines.push_back(line);
        start = end + 1U;
    }
    return lines;
}

[[nodiscard]] std::string summary_value(const std::map<std::string, std::string>& summary, const std::string& key) {
    const auto found = summary.find(key);
    if (found == summary.end()) {
        throw PromotionRankerError("summary.tsv is missing the " + key + " row");
    }
    return found->second;
}

[[nodiscard]] PromotionPcSample parse_pc_row(const std::string_view* cells, const std::size_t column_kind,
                               const std::size_t column_pc, const std::size_t column_samples,
                               const std::optional<std::size_t> column_nanoseconds,
                               const std::optional<std::size_t> column_windows, const std::filesystem::path& path,
                               const std::uint64_t line) {
    PromotionPcSample sample;
    sample.kind = std::string(cells[column_kind]);
    kind_index(sample.kind);
    const std::string_view pc = cells[column_pc];
    if (pc.size() < 2U || pc[0] != '0' || (pc[1] != 'x' && pc[1] != 'X')) {
        invalid_line(path, line, std::string(pc) + " is not a 0x-prefixed hex address");
    }
    std::uint64_t address = 0U;
    const std::string_view digits = pc.substr(2U);
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || address > 0xFFFFFFFFULL) {
        invalid_line(path, line, std::string(pc) + " is not a 32-bit hex address");
    }
    sample.pc = static_cast<std::uint32_t>(address);
    sample.samples = parse_u64(cells[column_samples], path, line, "samples");
    if (column_nanoseconds.has_value()) {
        sample.host_nanoseconds = parse_u64(cells[*column_nanoseconds], path, line, "host_nanoseconds");
    }
    if (column_windows.has_value()) {
        sample.host_windows = parse_u64(cells[*column_windows], path, line, "windows");
    }
    return sample;
}

[[nodiscard]] std::vector<PromotionPcSample> parse_pc_table(const std::filesystem::path& path, const PromotionRankerLimits& limits) {
    const std::string text = read_text_file(path);
    const auto lines = split_lines(text);
    if (lines.empty()) {
        throw PromotionRankerError(path.string() + " is empty (no header row)");
    }
    const auto header = split_tabs(lines[0]);
    std::optional<std::size_t> column_kind;
    std::optional<std::size_t> column_pc;
    std::optional<std::size_t> column_samples;
    std::optional<std::size_t> column_nanoseconds;
    std::optional<std::size_t> column_windows;
    for (std::size_t column = 0U; column < header.size(); ++column) {
        if (header[column] == "kind") {
            column_kind = column;
        } else if (header[column] == "pc") {
            column_pc = column;
        } else if (header[column] == "samples") {
            column_samples = column;
        } else if (header[column] == "host_nanoseconds") {
            column_nanoseconds = column;
        } else if (header[column] == "windows") {
            column_windows = column;
        }
    }
    if (!column_kind.has_value() || !column_pc.has_value() || !column_samples.has_value()) {
        invalid_line(path, 1U, "PC table header must contain kind, pc, and samples columns");
    }
    std::vector<PromotionPcSample> rows;
    for (std::size_t index = 1U; index < lines.size(); ++index) {
        const std::uint64_t line = index + 1U;
        if (lines[index].empty()) {
            continue;
        }
        if (rows.size() >= limits.maximum_rows) {
            invalid_line(path, line, "PC table row count exceeds the row-count limit");
        }
        const auto cells = split_tabs(lines[index]);
        if (cells.size() < header.size()) {
            invalid_line(path, line, "row has fewer columns than the header");
        }
        rows.push_back(parse_pc_row(cells.data(), *column_kind, *column_pc, *column_samples, column_nanoseconds,
                                    column_windows, path, line));
    }
    return rows;
}

} // namespace


[[nodiscard]] std::string_view to_string(const PromotionClass promotion_class) noexcept {
    switch (promotion_class) {
    case PromotionClass::LeafArithmetic:
        return "leaf-arithmetic";
    case PromotionClass::MemoryFamily:
        return "memory-family";
    case PromotionClass::PacketBuilder:
        return "packet-builder";
    case PromotionClass::VtableSubsystem:
        return "vtable-subsystem";
    case PromotionClass::AlreadyPromoted:
        return "already-promoted";
    case PromotionClass::DispatchEntry:
        return "dispatch-entry";
    case PromotionClass::Unclassified:
        return "unclassified";
    }
    return "unknown";
}

[[nodiscard]] PromotionProfile parse_ee_pc_profile(const std::filesystem::path& profile_directory,
                                     const PromotionRankerLimits& limits) {
    if (profile_directory.empty()) {
        throw PromotionRankerError("--ee-pc-profile directory path is empty");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(profile_directory, error)) {
        throw PromotionRankerError("EE PC profile path is not a directory: " + profile_directory.string());
    }
    const std::string summary_text = read_text_file(profile_directory / "summary.tsv");
    const auto summary_lines = split_lines(summary_text);
    if (summary_lines.empty() || summary_lines[0] != "field\tvalue") {
        throw PromotionRankerError("summary must begin with the field/value header");
    }
    std::map<std::string, std::string> summary;
    for (std::size_t index = 1U; index < summary_lines.size(); ++index) {
        if (summary_lines[index].empty()) {
            continue;
        }
        const auto cells = split_tabs(summary_lines[index]);
        if (cells.size() < 2U) {
            throw PromotionRankerError("summary row has no value column");
        }
        summary.emplace(std::string(cells[0]), std::string(cells[1]));
    }
    PromotionProfile profile;
    const std::filesystem::path summary_path = profile_directory / "summary.tsv";
    profile.host_nanoseconds = parse_u64(summary_value(summary, "host_nanoseconds"), summary_path, 0U, "host_nanoseconds");
    profile.samples = parse_u64(summary_value(summary, "samples"), summary_path, 0U, "samples");
    profile.retired_instructions =
        parse_u64(summary_value(summary, "retired_instructions"), summary_path, 0U, "retired_instructions");
    profile.hot_pcs = parse_pc_table(profile_directory / "hot-pcs.tsv", limits);
    profile.hot_host_pcs = parse_pc_table(profile_directory / "hot-host-pcs.tsv", limits);
    return profile;
}

[[nodiscard]] PromotionFunctionMap load_promotion_function_map(const std::filesystem::path& elf_file,
                                                 const std::optional<std::filesystem::path>& reviewed_manifest_file) {
    PromotionFunctionMap map;
    if (reviewed_manifest_file.has_value()) {
        const ReviewedFunctionMap reviewed = read_reviewed_function_map_manifest(*reviewed_manifest_file);
        map.executable_fnv1a64 = reviewed.executable.fnv1a64;
        map.entry_point = reviewed.executable.entry_point;
        for (const ReviewedFunctionExtent& extent : reviewed.functions) {
            map.reviewed.push_back(ReviewedMapFunction{extent.name, extent.start_address, extent.byte_size});
        }
    }
    const content::ElfStaticAnalysis analysis = content::analyze_ps2_elf_static(elf_file);
    if (!reviewed_manifest_file.has_value()) {
        map.entry_point = analysis.entry_point;
    }
    for (std::size_t index = 0U; index < analysis.functions.size(); ++index) {
        const content::ElfFunctionCandidate& candidate = analysis.functions[index];
        std::uint32_t size = 0U;
        if (index + 1U < analysis.functions.size()) {
            size = analysis.functions[index + 1U].address - candidate.address;
        }
        map.static_analysis.push_back(ReviewedMapFunction{candidate.suggested_name, candidate.address, size});
    }
    return map;
}



namespace {

// Sorted function extents for attributing a PC to its containing function.
struct ExtentIndex {
    struct Entry {
        std::uint64_t begin{};
        std::uint64_t end{};
        std::size_t function{};
    };

    std::vector<Entry> entries;

    static ExtentIndex build(const std::vector<ReviewedMapFunction>& functions) {
        ExtentIndex index;
        for (std::size_t function = 0U; function < functions.size(); ++function) {
            const std::uint64_t begin = functions[function].address;
            const std::uint64_t end = begin + functions[function].byte_size;
            if (functions[function].byte_size != 0U) {
                index.entries.push_back(Entry{begin, end, function});
            }
        }
        std::sort(index.entries.begin(), index.entries.end(),
                  [](const Entry& left, const Entry& right) { return left.begin < right.begin; });
        return index;
    }

    [[nodiscard]] std::optional<std::size_t> find(const std::uint64_t pc) const {
        auto after = std::upper_bound(entries.begin(), entries.end(), pc,
                                      [](const std::uint64_t value, const Entry& entry) { return value < entry.begin; });
        if (after == entries.begin()) {
            return std::nullopt;
        }
        const Entry& entry = *(after - 1);
        if (pc < entry.end) {
            return entry.function;
        }
        return std::nullopt;
    }
};

} // namespace


namespace {

[[nodiscard]] std::string lowercase(const std::string& text) {
    std::string lower = text;
    for (char& character : lower) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return lower;
}

[[nodiscard]] bool contains_any(const std::string& text, const std::vector<std::string_view>& words) {
    return std::any_of(words.begin(), words.end(),
                       [&text](const std::string_view word) { return text.find(word) != std::string::npos; });
}

// Name-based family: leaf arithmetic first, then vtable-driven subsystems, memory, and packet builders.
[[nodiscard]] PromotionClass classify_name(const std::string& name) {
    const std::string lower = lowercase(name);
    static const std::vector<std::string_view> leaf_words{"fabs", "floor", "ceil", "sqrt", "clamp", "min", "max", "abs"};
    static const std::vector<std::string_view> vtable_words{"vtable", "vtbl", "dispatch", "subsystem",
                                                            "render", "scene", "object", "manager"};
    static const std::vector<std::string_view> memory_words{"memset", "memcpy", "memmove", "copy", "fill", "mix"};
    static const std::vector<std::string_view> packet_words{"packet", "vif", "gif", "dma", "xgkick", "template",
                                                            "ring", "flush"};
    if (contains_any(lower, leaf_words)) {
        return PromotionClass::LeafArithmetic;
    }
    if (contains_any(lower, vtable_words)) {
        return PromotionClass::VtableSubsystem;
    }
    if (contains_any(lower, memory_words)) {
        return PromotionClass::MemoryFamily;
    }
    if (contains_any(lower, packet_words)) {
        return PromotionClass::PacketBuilder;
    }
    return PromotionClass::Unclassified;
}

} // namespace

[[nodiscard]] PromotionQueue rank_promotion_candidates(const PromotionProfile& profile, const PromotionFunctionMap& function_map,
                                         const PromotionRankerLimits& limits) {
    std::vector<PromotionCandidate> candidates;
    candidates.reserve(function_map.reviewed.size() + function_map.static_analysis.size());
    for (const ReviewedMapFunction& function : function_map.reviewed) {
        PromotionCandidate candidate;
        candidate.address = function.address;
        candidate.byte_size = function.byte_size;
        candidate.name = function.name;
        candidate.source = "reviewed";
        candidates.push_back(std::move(candidate));
    }
    for (const ReviewedMapFunction& function : function_map.static_analysis) {
        PromotionCandidate candidate;
        candidate.address = function.address;
        candidate.byte_size = function.byte_size;
        candidate.name = function.name;
        candidate.source = "static-analysis";
        candidates.push_back(std::move(candidate));
    }
    const ExtentIndex reviewed_index = ExtentIndex::build(function_map.reviewed);
    const ExtentIndex static_index = ExtentIndex::build(function_map.static_analysis);
    const std::size_t reviewed_count = function_map.reviewed.size();

    PromotionQueue queue;
    std::map<std::uint32_t, PromotionQueue::UnmappedPage> unmapped;
    for (const PromotionPcSample& row : profile.hot_pcs) {
        const std::size_t kind = kind_index(row.kind);
        std::optional<std::size_t> owner;
        if (const auto found = reviewed_index.find(row.pc); found.has_value()) {
            owner = *found;
        } else if (const auto found_static = static_index.find(row.pc); found_static.has_value()) {
            owner = reviewed_count + *found_static;
        }
        if (!owner.has_value()) {
            const std::uint32_t begin = row.pc & ~0xFFFU;
            PromotionQueue::UnmappedPage& page = unmapped[begin];
            page.begin = begin;
            page.samples += row.samples;
            page.host_nanoseconds += row.host_nanoseconds;
            page.by_kind[kind] += row.samples;
            continue;
        }
        PromotionCandidate& candidate = candidates[*owner];
        candidate.samples += row.samples;
        candidate.host_nanoseconds += row.host_nanoseconds;
        candidate.host_windows += row.host_windows;
        candidate.by_kind[kind] += row.samples;
        if (kind == 0U || kind == 3U) {
            candidate.exact_pcs += 1U;
        } else {
            candidate.dispatch_entry_pcs += 1U;
        }
        queue.attributed_samples += row.samples;
        queue.attributed_host_nanoseconds += row.host_nanoseconds;
    }
    if (unmapped.size() > limits.maximum_unmapped_pages) {
        throw PromotionRankerError("unmapped page count exceeds the limit");
    }

    for (PromotionCandidate& candidate : candidates) {
        if (candidate.samples == 0U) {
            continue;
        }
        candidate.promotion_class = classify_name(candidate.name);
        if (candidate.exact_pcs == 0U && candidate.dispatch_entry_pcs != 0U) {
            candidate.promotion_class = PromotionClass::DispatchEntry;
        }
        candidate.host_percent_of_profile_x100 =
            profile.host_nanoseconds == 0U ? 0U : candidate.host_nanoseconds * 10000U / profile.host_nanoseconds;
        queue.ranked.push_back(candidate);
    }
    std::stable_sort(queue.ranked.begin(), queue.ranked.end(),
                     [](const PromotionCandidate& left, const PromotionCandidate& right) {
                         if (left.host_nanoseconds != right.host_nanoseconds) {
                             return left.host_nanoseconds > right.host_nanoseconds;
                         }
                         return left.address < right.address;
                     });
    for (const auto& [begin, page] : unmapped) {
        queue.unmapped_pages.push_back(page);
    }
    std::stable_sort(queue.unmapped_pages.begin(), queue.unmapped_pages.end(),
                     [](const PromotionQueue::UnmappedPage& left, const PromotionQueue::UnmappedPage& right) {
                         return left.host_nanoseconds > right.host_nanoseconds;
                     });
    queue.profile_host_nanoseconds = profile.host_nanoseconds;
    queue.profile_samples = profile.samples;
    queue.attributed_host_nanoseconds = queue.attributed_host_nanoseconds;
    return queue;
}

std::size_t PromotionQueue::accepted_count() const noexcept {
    return ranked.size();
}

std::size_t PromotionQueue::promotable_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(ranked.begin(), ranked.end(), [](const PromotionCandidate& candidate) {
        return candidate.promotion_class != PromotionClass::AlreadyPromoted &&
               candidate.promotion_class != PromotionClass::DispatchEntry;
    }));
}


namespace {

[[nodiscard]] std::string hex32(const std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08x", static_cast<unsigned int>(value));
    return text;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw PromotionRankerError("cannot create " + path.string());
    }
    output << text;
    output.flush();
    if (!output) {
        throw PromotionRankerError("cannot write complete " + path.string());
    }
}

} // namespace

void write_promotion_queue_artifacts(const std::filesystem::path& output_directory,
                                     const PromotionQueue& queue,
                                     const PromotionRankerLimits& limits) {
    static_cast<void>(limits);
    if (output_directory.empty()) {
        throw PromotionRankerError("promotion queue output directory path is empty");
    }
    std::error_code error;
    if (std::filesystem::exists(output_directory, error)) {
        throw PromotionRankerError("promotion queue output directory already exists: " + output_directory.string());
    }
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        throw PromotionRankerError("cannot create promotion queue output directory: " + output_directory.string());
    }

    std::ostringstream queue_text;
    queue_text << "rank\taddress\tsize\tname\tsource\tclass\tsamples\thost_nanoseconds\twindows\tscalar\tcompiled\t"
                  "native\thle\texact_pcs\tdispatch_entry_pcs\thost_percent_x100\n";
    for (std::size_t index = 0U; index < queue.ranked.size(); ++index) {
        const PromotionCandidate& row = queue.ranked[index];
        queue_text << index + 1U << '\t' << hex32(row.address) << '\t' << row.byte_size << '\t' << row.name << '\t'
                   << row.source << '\t' << to_string(row.promotion_class) << '\t' << row.samples << '\t'
                   << row.host_nanoseconds << '\t' << row.host_windows << '\t' << row.by_kind[0] << '\t'
                   << row.by_kind[1] << '\t' << row.by_kind[2] << '\t' << row.by_kind[3] << '\t' << row.exact_pcs
                   << '\t' << row.dispatch_entry_pcs << '\t' << row.host_percent_of_profile_x100 << '\n';
    }
    write_text(output_directory / "promotion-queue.tsv", queue_text.str());

    const std::uint64_t attributed_percent =
        queue.profile_host_nanoseconds == 0U ? 0U : queue.attributed_host_nanoseconds * 10000U / queue.profile_host_nanoseconds;
    std::ostringstream summary;
    summary << "field\tvalue\n";
    summary << "profile_host_nanoseconds\t" << queue.profile_host_nanoseconds << '\n';
    summary << "profile_samples\t" << queue.profile_samples << '\n';
    summary << "attributed_samples\t" << queue.attributed_samples << '\n';
    summary << "attributed_host_nanoseconds\t" << queue.attributed_host_nanoseconds << '\n';
    summary << "attributed_host_percent_x100\t" << attributed_percent << '\n';
    summary << "candidates\t" << queue.accepted_count() << '\n';
    summary << "promotable_candidates\t" << queue.promotable_count() << '\n';
    summary << "unmapped_pages\t" << queue.unmapped_pages.size() << '\n';
    for (std::size_t index = 0U; index < queue.unmapped_pages.size(); ++index) {
        const PromotionQueue::UnmappedPage& page = queue.unmapped_pages[index];
        summary << "unmapped_page[" << index << "]\t" << hex32(page.begin) << '\t' << page.samples << '\t'
                << page.host_nanoseconds << '\n';
    }
    write_text(output_directory / "promotion-summary.tsv", summary.str());
}


} // namespace takedown::recompiler
