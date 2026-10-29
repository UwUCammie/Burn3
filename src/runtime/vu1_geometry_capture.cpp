#include "takedown/runtime/vu1_geometry_capture.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace takedown::runtime {
namespace {

// Little-endian fields, written byte by byte so no native layout leaks in.
void write_le(std::ostream& stream, std::uint64_t value, unsigned int bytes) {
    for (unsigned int i = 0; i < bytes; ++i) {
        stream.put(static_cast<char>(value & 0xffU));
        value >>= 8U;
    }
}

std::uint64_t read_le(std::istream& stream, unsigned int bytes) {
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < bytes; ++i) {
        const int c = stream.get();
        if (c == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated geometry case");
        }
        value |= static_cast<std::uint64_t>(c & 0xff) << (8U * i);
    }
    return value;
}

std::uint32_t read_u32(std::istream& stream) {
    return static_cast<std::uint32_t>(read_le(stream, 4U));
}

std::uint64_t read_u64(std::istream& stream) {
    return read_le(stream, 8U);
}

bool read_bool(std::istream& stream) {
    const std::uint64_t value = read_le(stream, 4U);
    if (value > 1U) {
        throw std::runtime_error("invalid geometry boolean");
    }
    return value != 0U;
}

void state_out(std::ostream& stream, const guest::Vu1State& state) {
    for (const auto& vector : state.vectors) {
        for (auto value : vector.words) {
            write_le(stream, value, 4U);
        }
    }
    for (auto value : state.accumulator.words) {
        write_le(stream, value, 4U);
    }
    for (auto value : state.control) {
        write_le(stream, value, 4U);
    }
    for (const auto* history : {&state.mac_flag_history, &state.clip_flag_history,
                                &state.status_flag_history}) {
        for (auto value : *history) {
            write_le(stream, value, 4U);
        }
    }
    write_le(stream, state.flag_history_initialized, 4U);
}

guest::Vu1State state_in(std::istream& stream) {
    guest::Vu1State state{};
    for (auto& vector : state.vectors) {
        for (auto& value : vector.words) {
            value = read_u32(stream);
        }
    }
    for (auto& value : state.accumulator.words) {
        value = static_cast<std::uint32_t>(read_le(stream, 4U));
    }
    for (auto& value : state.control) {
        value = static_cast<std::uint32_t>(read_le(stream, 4U));
    }
    for (auto* history : {&state.mac_flag_history, &state.clip_flag_history,
                          &state.status_flag_history}) {
        for (auto& value : *history) {
            value = read_u32(stream);
        }
    }
    state.flag_history_initialized = read_bool(stream);
    return state;
}

void read_memory(std::istream& stream, std::span<std::byte> memory) {
    stream.read(reinterpret_cast<char*>(memory.data()),
        static_cast<std::streamsize>(memory.size()));
    if (!stream) {
        throw std::runtime_error("truncated geometry memory");
    }
}

constexpr std::string_view kMagic{"B3GEO001"};


const std::filesystem::path& capture_directory() {
    static const std::filesystem::path directory = [] {
        const char* value = std::getenv("TAKEDOWN_VU1_GEOMETRY_CAPTURE_DIR");
        return value != nullptr ? std::filesystem::path{value} : std::filesystem::path{};
    }();
    return directory;
}

// Developer-only census of distinct (image, entry, resolved entry) launches,
// enabled by TAKEDOWN_VU1_TUPLE_CAPTURE_DIR.
struct TupleCensus {
    struct Tuple {
        std::uint64_t launches;
        std::uint64_t pairs;
        std::uint32_t cases;
    };

    std::filesystem::path directory{};
    std::map<std::tuple<std::uint64_t, std::uint16_t, std::uint16_t>, Tuple> tuples{};
    std::set<std::uint64_t> images{};
    Tuple* current{};
    std::uint64_t cases_written{};
    std::uint64_t launches_since_manifest{};

    TupleCensus() {
        const char* value = std::getenv("TAKEDOWN_VU1_TUPLE_CAPTURE_DIR");
        if (value != nullptr && *value != '\0') {
            directory = value;
            std::filesystem::create_directories(directory / "cases");
        }
    }

    ~TupleCensus() { write_manifest(); }

    void write_manifest() const {
        if (directory.empty()) {
            return;
        }
        std::ofstream out(directory / "manifest.tsv");
        out << "image\tentry\tresolved_entry\tlaunches\tpairs\n";
        for (const auto& [key, tuple] : tuples) {
            out << std::format("image-{:016x}.bin\t0x{:04x}\t0x{:04x}\t{}\t{}\n",
                std::get<0>(key), std::get<1>(key), std::get<2>(key), tuple.launches,
                tuple.pairs);
        }
    }

    bool note_launch(const guest::EeMemory& memory, const guest::Vu1State& state,
        const std::uint16_t entry) {
        const std::span<const std::byte> image = memory.view(kVu1WorkerMicroBase, kVu1WorkerMemorySize);
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t offset = 0; offset < image.size(); offset += 8U) {
            std::uint64_t word{};
            std::memcpy(&word, image.data() + offset, sizeof(word));
            hash = (hash ^ word) * 11400714819323198485ULL;
            hash ^= hash >> 29U;
        }
        auto resolved = static_cast<std::uint16_t>(entry & 2047U);
        const guest::vu1_detail::CachedVu1Pair launch =
            guest::vu1_detail::decode_vu1_pair_metadata(image, entry & 2047U);
        if (launch.lower && launch.lower->op == guest::vu1_detail::LowerOp::Jr && launch.lower->is == 15U) {
            resolved = state.read_integer(15U) & 2047U;
        }
        if (images.insert(hash).second) {
            std::ofstream out(directory / std::format("image-{:016x}.bin", hash),
                std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.data()),
                static_cast<std::streamsize>(image.size()));
        }
        const auto [it, inserted] = tuples.try_emplace(
            {hash, static_cast<std::uint16_t>(entry & 2047U), resolved});
        current = &it->second;
        const std::uint64_t ordinal = current->launches++;
        if (inserted || ++launches_since_manifest > 262143U) {
            launches_since_manifest = 0;
            write_manifest();
        }
        // Launch ordinals (zero-based) of each tuple whose full case is kept.
        static constexpr std::array<std::uint64_t, 8> kOrdinals{
            0U, 1U, 7U, 100U, 1000U, 10000U, 100000U, 1000000U};
        return std::ranges::find(kOrdinals, ordinal) != kOrdinals.end();
    }
};

TupleCensus& tuple_census() {
    static TupleCensus census;
    return census;
}

} // namespace

void flush_vu1_tuple_census() {
    tuple_census().write_manifest();
}

bool same_vu1_geometry_state(const guest::Vu1State& a, const guest::Vu1State& b) noexcept {
    return a.vectors == b.vectors && a.accumulator == b.accumulator &&
           a.control == b.control && a.mac_flag_history == b.mac_flag_history &&
           a.clip_flag_history == b.clip_flag_history &&
           a.status_flag_history == b.status_flag_history &&
           a.flag_history_initialized == b.flag_history_initialized;
}

void write_vu1_geometry_case(std::ostream& stream, const Vu1GeometryCase& data) {
    stream.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_le(stream, data.sequence, 8U);
    write_le(stream, data.initial_xgkick_live, 4U);
    write_le(stream, data.input.entry, 4U);
    write_le(stream, data.input.options.pair_limit, 8U);
    write_le(stream, data.input.vif1_top, 4U);
    write_le(stream, data.input.vif1_itop, 4U);
    write_le(stream, data.input.timed_vu1_execution_enabled, 4U);
    write_le(stream, static_cast<std::uint64_t>(data.input.xgkick_mode), 4U);
    write_le(stream, data.input.program_hash.low, 8U);
    write_le(stream, data.input.program_hash.high, 8U);
    state_out(stream, data.input.vu1_state);
    stream.write(reinterpret_cast<const char*>(data.input.micro_memory.data()),
        static_cast<std::streamsize>(data.input.micro_memory.size()));
    stream.write(reinterpret_cast<const char*>(data.input.vu_mem.data()),
        static_cast<std::streamsize>(data.input.vu_mem.size()));
    write_le(stream, static_cast<std::uint64_t>(data.result.status), 4U);
    write_le(stream, static_cast<std::uint64_t>(data.result.slot), 4U);
    write_le(stream, data.result.pc, 4U);
    write_le(stream, data.result.lower_word, 4U);
    write_le(stream, data.result.upper_word, 4U);
    write_le(stream, data.result.committed_pairs, 8U);
    write_le(stream, data.result.elapsed_cycles, 8U);
    write_le(stream, data.result.pipeline_stall_cycles, 8U);
    state_out(stream, data.output_state);
    stream.write(reinterpret_cast<const char*>(data.output_memory.data()),
        static_cast<std::streamsize>(data.output_memory.size()));
    if (!stream) {
        throw std::runtime_error("cannot write geometry case");
    }
}

Vu1GeometryCase read_vu1_geometry_case(std::istream& stream) {
    for (char value : kMagic) {
        const int actual = stream.get();
        if (value != actual) {
            throw std::runtime_error("invalid geometry case version");
        }
    }
    Vu1GeometryCase data{};
    data.sequence = read_u64(stream);
    data.initial_xgkick_live = read_bool(stream);
    const std::uint32_t entry = read_u32(stream);
    const std::uint64_t pair_limit = read_u64(stream);
    if (entry > 2047U || pair_limit == 0U || pair_limit > 1048576U) {
        throw std::runtime_error("invalid geometry run limits");
    }
    data.input.entry = static_cast<std::uint16_t>(entry);
    data.input.options.pair_limit = pair_limit;
    data.input.vif1_top = read_u32(stream);
    data.input.vif1_itop = read_u32(stream);
    data.input.timed_vu1_execution_enabled = read_bool(stream);
    const std::uint32_t xgkick_mode = read_u32(stream);
    if (xgkick_mode > 1U) {
        throw std::runtime_error("invalid geometry XGKICK mode");
    }
    data.input.xgkick_mode = static_cast<guest::Vu1XgkickMode>(xgkick_mode);
    data.input.program_hash.low = read_u64(stream);
    data.input.program_hash.high = read_u64(stream);
    data.input.vu1_state = state_in(stream);
    read_memory(stream, data.input.micro_memory);
    read_memory(stream, data.input.vu_mem);
    const std::uint32_t status = read_u32(stream);
    const std::uint32_t slot = read_u32(stream);
    const std::uint32_t pc = read_u32(stream);
    if (status > 4U || slot > 3U || pc > 2047U) {
        throw std::runtime_error("invalid geometry result");
    }
    data.result.status = static_cast<guest::Vu1RunStatus>(status);
    data.result.slot = static_cast<guest::Vu1Slot>(slot);
    data.result.pc = static_cast<std::uint16_t>(pc);
    data.result.lower_word = read_u32(stream);
    data.result.upper_word = read_u32(stream);
    data.result.committed_pairs = read_u64(stream);
    data.result.elapsed_cycles = read_u64(stream);
    data.result.pipeline_stall_cycles = read_u64(stream);
    data.output_state = state_in(stream);
    read_memory(stream, data.output_memory);
    if (stream.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("trailing geometry case data");
    }
    return data;
}

std::unique_ptr<Vu1GeometryCase> begin_vu1_geometry_capture(const guest::EeMemory& memory,
    const guest::Vu1State& state, const std::uint16_t entry, const bool timed_execution,
    const bool xgkick_live, const bool native_strip_admitted) {
    TupleCensus& census = tuple_census();
    if (!census.directory.empty()) {
        if (!census.note_launch(memory, state, entry)) {
            return nullptr;
        }
        static std::uint64_t tuple_sequence = 0;
        auto data = std::make_unique<Vu1GeometryCase>();
        data->sequence = tuple_sequence++;
        data->initial_xgkick_live = xgkick_live;
        data->input.vu1_state = state;
        data->input.entry = entry;
        data->input.vif1_top = memory.read_u32_le(kVu1WorkerVif1Top);
        data->input.vif1_itop = memory.read_u32_le(kVu1WorkerVif1Itop);
        data->input.timed_vu1_execution_enabled = timed_execution;
        data->input.xgkick_mode = memory.vu1_xgkick_mode();
        std::ranges::copy(memory.view(kVu1WorkerDataBase, kVu1WorkerMemorySize),
            data->input.vu_mem.begin());
        std::ranges::copy(memory.view(kVu1WorkerMicroBase, kVu1WorkerMemorySize),
            data->input.micro_memory.begin());
        return data;
    }
    if (capture_directory().empty()) {
        return nullptr;
    }
    static const bool successors = [] {
        const char* value = std::getenv("TAKEDOWN_VU1_GEOMETRY_CAPTURE_SUCCESSORS");
        return value != nullptr && std::string_view{value} == "1";
    }();
    static const std::optional<std::uint16_t> fallback_entry = []() -> std::optional<std::uint16_t> {
        const char* value = std::getenv("TAKEDOWN_VU1_GEOMETRY_CAPTURE_FALLBACK_ENTRY");
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        const std::string_view text{value};
        unsigned int entry_value = 0;
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), entry_value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            entry_value > 2047U) {
            throw std::invalid_argument(
                "geometry fallback capture entry must be a decimal value below 2048");
        }
        return static_cast<std::uint16_t>(entry_value);
    }();
    static const std::optional<std::uint64_t> selected_sequence =
        []() -> std::optional<std::uint64_t> {
        const char* value = std::getenv("TAKEDOWN_VU1_GEOMETRY_CAPTURE_SEQUENCE");
        if (value == nullptr || *value == '\0') {
            return std::nullopt;
        }
        const std::string_view text{value};
        std::uint64_t sequence_value = 0;
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), sequence_value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            throw std::invalid_argument(
                "geometry capture sequence must be an unsigned decimal value");
        }
        return sequence_value;
    }();
    if (static_cast<unsigned int>(successors) +
            static_cast<unsigned int>(fallback_entry.has_value()) +
            static_cast<unsigned int>(selected_sequence.has_value()) >
        1U) {
        throw std::invalid_argument("select only one geometry capture selector");
    }
    if (!successors && !fallback_entry && !selected_sequence &&
        !memory.vu1_program_cache().profiling_enabled()) {
        return nullptr;
    }
    static std::uint64_t sequence = 0;
    const std::uint64_t index = sequence++;
    if (selected_sequence) {
        if (index != *selected_sequence) {
            return nullptr;
        }
    } else if (fallback_entry) {
        static std::uint32_t captured = 0;
        if (native_strip_admitted || *fallback_entry != entry || captured >= 64U) {
            return nullptr;
        }
        ++captured;
    } else if (successors) {
        static bool previous_native = false;
        static std::uint32_t remaining = 0;
        static std::uint32_t captured = 0;
        if (previous_native && !native_strip_admitted) {
            remaining = 8U;
        }
        previous_native = native_strip_admitted;
        if (native_strip_admitted) {
            remaining = 0U;
        }
        if (remaining == 0U || captured >= 64U) {
            return nullptr;
        }
        --remaining;
        ++captured;
    } else if (index >= 4096U || (index & 63U) != 0U) {
        return nullptr;
    }
    auto data = std::make_unique<Vu1GeometryCase>();
    data->sequence = index;
    data->initial_xgkick_live = xgkick_live;
    data->input.vu1_state = state;
    data->input.entry = entry;
    data->input.vif1_top = memory.read_u32_le(kVu1WorkerVif1Top);
    data->input.vif1_itop = memory.read_u32_le(kVu1WorkerVif1Itop);
    data->input.timed_vu1_execution_enabled = timed_execution;
    data->input.xgkick_mode = memory.vu1_xgkick_mode();
    std::ranges::copy(memory.view(kVu1WorkerDataBase, kVu1WorkerMemorySize),
        data->input.vu_mem.begin());
    std::ranges::copy(memory.view(kVu1WorkerMicroBase, kVu1WorkerMemorySize),
        data->input.micro_memory.begin());
    return data;
}

void finish_vu1_geometry_capture(std::unique_ptr<Vu1GeometryCase> data,
    const guest::EeMemory& memory, const guest::Vu1State& state,
    const guest::Vu1RunResult& result) {
    TupleCensus& census = tuple_census();
    if (!census.directory.empty() && census.current != nullptr) {
        census.current->pairs += result.committed_pairs;
        census.current = nullptr;
    }
    if (data == nullptr) {
        return;
    }
    data->result = result;
    data->output_state = state;
    std::ranges::copy(memory.view(kVu1WorkerDataBase, kVu1WorkerMemorySize),
        data->output_memory.begin());
    const std::filesystem::path directory = !census.directory.empty()
        ? census.directory / "cases"
        : capture_directory();
    std::filesystem::create_directories(directory);
    std::ostringstream name;
    name << "geometry-" << std::setfill('0') << std::setw(6) << data->sequence << ".b3geo";
    const std::filesystem::path path = directory / name.str();
    if (std::filesystem::exists(path)) {
        throw std::runtime_error("refusing to overwrite geometry case");
    }
    std::ofstream stream(path, std::ios::binary);
    write_vu1_geometry_case(stream, *data);
}

} // namespace takedown::runtime
