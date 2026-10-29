#include "takedown/platform/gs_semantic_compare.hpp"

#include "takedown/guest/gs_capture.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <optional>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace takedown::platform {

namespace {

constexpr std::size_t kMaximumDecodedBytes = 512U * 1024U * 1024U;

[[noreturn]] void malformed_trace(const std::string& detail) {
    throw std::runtime_error("invalid GS semantic trace: " + detail);
}

[[noreturn]] void malformed_dump(const std::string& detail) {
    throw std::runtime_error("invalid PCSX2 GS dump for semantic comparison: " + detail);
}

void require_range(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                   const std::size_t count, const char* detail) {
    if (offset > bytes.size() || bytes.size() - offset < count) {
        malformed_dump(detail);
    }
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset, const char* detail) {
    require_range(bytes, offset, 4U, detail);
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void release_stream(ZSTD_DStream* const stream) noexcept {
    ZSTD_freeDStream(stream);
}

[[nodiscard]] std::vector<std::uint8_t> decompress_zstandard(const std::span<const std::uint8_t> input) {
    ZSTD_DStream* const stream = ZSTD_createDStream();
    if (stream == nullptr) {
        throw std::runtime_error("unable to initialize Zstandard GS-dump decompressor");
    }
    try {
        const std::size_t init_result = ZSTD_initDStream(stream);
        if (ZSTD_isError(init_result)) {
            throw std::runtime_error("unable to initialize Zstandard GS-dump decompressor: " +
                                     std::string(ZSTD_getErrorName(init_result)));
        }

        std::vector<std::uint8_t> output;
        output.reserve(std::min(input.size() * 8U, kMaximumDecodedBytes));
        std::array<std::uint8_t, 65536> buffer{};
        ZSTD_inBuffer compressed{input.data(), input.size(), 0U};
        std::size_t last_result = 1U;
        while (compressed.pos < compressed.size) {
            const std::size_t before = compressed.pos;
            ZSTD_outBuffer decoded{buffer.data(), buffer.size(), 0U};
            last_result = ZSTD_decompressStream(stream, &decoded, &compressed);
            if (ZSTD_isError(last_result)) {
                throw std::runtime_error("unable to decompress PCSX2 GS dump: " +
                                         std::string(ZSTD_getErrorName(last_result)));
            }
            if (decoded.pos > kMaximumDecodedBytes - output.size()) {
                throw std::runtime_error("PCSX2 GS dump exceeds the 512 MiB local-analysis limit");
            }
            output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(decoded.pos));
            if (compressed.pos == before && decoded.pos == 0U) {
                malformed_dump("Zstandard stream made no progress");
            }
        }
        if (last_result != 0U) {
            malformed_dump("truncated Zstandard stream");
        }
        release_stream(stream);
        return output;
    } catch (...) {
        release_stream(stream);
        throw;
    }
}

constexpr std::size_t kMaximumTraceBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::uint64_t kMaximumTraceEvents = 5'000'000U;

enum class SemanticKind : std::uint8_t {
    Tag,
    Register,
    Image,
};

// One comparable GS command. The meaning of each value depends on the kind:
// TAG: raw GIFtag low/high words; REG: address, value, packed high, source;
// IMAGE: byte count, payload hash, destination geometry, and the latched
// BITBLTBUF/TRXPOS/TRXREG/TRXDIR registers.
struct SemanticEvent {
    SemanticKind kind{};
    std::array<std::uint64_t, 13> values{};

    [[nodiscard]] bool operator==(const SemanticEvent&) const = default;
};

struct SemanticInterval {
    std::uint64_t index{};
    std::vector<SemanticEvent> events;
};

struct ParsedLiveTrace {
    std::vector<SemanticInterval> intervals{};
    bool event_limit_reached{};
};

struct ParsedDumpTrace {
    std::vector<SemanticInterval> intervals{};
    std::size_t decoder_diagnostics{};
};

constexpr std::size_t kMaximumInputBytes = 256U * 1024U * 1024U;
constexpr std::uint32_t kZstandardMagic = 0xFD2FB528U;

[[nodiscard]] std::string_view required_field(const std::string_view line, const std::string_view key) {
    const std::string needle = " " + std::string(key) + '=';
    const std::size_t found = line.find(needle);
    if (found == std::string_view::npos) {
        malformed_trace("record is missing " + std::string(key));
    }
    const auto value_begin = found + needle.size();
    const auto value_end = line.find(' ', value_begin);
    return line.substr(value_begin, value_end == std::string_view::npos ? std::string_view::npos
                                                                         : value_end - value_begin);
}

[[nodiscard]] std::uint64_t parse_unsigned(const std::string_view value, const std::string_view key,
                                           const int base) {
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        malformed_trace("invalid " + std::string(key));
    }
    return parsed;
}

[[nodiscard]] std::uint64_t parse_decimal_field(const std::string_view line, const std::string_view key) {
    return parse_unsigned(required_field(line, key), key, 10);
}

[[nodiscard]] std::uint64_t parse_hex_field(const std::string_view line, const std::string_view key) {
    return parse_unsigned(required_field(line, key), key, 16);
}

[[nodiscard]] std::uint64_t parse_register_source(const std::string_view value) {
    if (value == "tag-prim") {
        return static_cast<std::uint64_t>(guest::GsRegisterWriteSource::GifTagPrim);
    }
    if (value == "packed") {
        return static_cast<std::uint64_t>(guest::GsRegisterWriteSource::Packed);
    }
    if (value == "reglist") {
        return static_cast<std::uint64_t>(guest::GsRegisterWriteSource::Reglist);
    }
    if (value == "ad") {
        return static_cast<std::uint64_t>(guest::GsRegisterWriteSource::Ad);
    }
    malformed_trace("unknown register-write source");
}

[[nodiscard]] SemanticEvent parse_tag_event(const std::string_view record) {
    SemanticEvent event{};
    event.values[0] = parse_hex_field(record, "raw");
    event.values[1] = parse_hex_field(record, "regs");
    return event;
}

[[nodiscard]] SemanticEvent parse_register_event(const std::string_view record) {
    SemanticEvent event{};
    event.kind = SemanticKind::Register;
    event.values[0] = parse_hex_field(record, "addr");
    event.values[1] = parse_hex_field(record, "value");
    event.values[2] = parse_hex_field(record, "high");
    event.values[3] = parse_register_source(required_field(record, "source"));
    return event;
}

[[nodiscard]] SemanticEvent parse_image_event(const std::string_view record) {
    SemanticEvent event{};
    event.kind = SemanticKind::Image;
    event.values[0] = parse_decimal_field(record, "bytes");
    event.values[1] = parse_hex_field(record, "hash");
    event.values[2] = parse_hex_field(record, "dbp");
    event.values[3] = parse_hex_field(record, "dbw");
    event.values[4] = parse_hex_field(record, "dpsm");
    event.values[5] = parse_hex_field(record, "dsax");
    event.values[6] = parse_hex_field(record, "dsay");
    event.values[7] = parse_decimal_field(record, "width");
    event.values[8] = parse_decimal_field(record, "height");
    event.values[9] = parse_hex_field(record, "bitbltbuf");
    event.values[10] = parse_hex_field(record, "trxpos");
    event.values[11] = parse_hex_field(record, "trxreg");
    event.values[12] = parse_hex_field(record, "trxdir");
    return event;
}

// RECOVERY: statement-for-statement identical to the original GIMPLE; only GCC's
// global temporary counter (SR.<n>) differs by a constant offset because the
// original translation unit created a few more unnamed temporaries earlier on
// (some helper that was fully inlined away and could not be identified).
[[nodiscard]] ParsedLiveTrace parse_live_trace(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    if (error || file_bytes - 1U > kMaximumTraceBytes - 1U) {
        throw std::runtime_error("unable to read a non-empty GS semantic trace within the 512 MiB limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open GS semantic trace");
    }

    ParsedLiveTrace parsed;
    std::string line;
    bool header_seen = false;
    bool footer_seen = false;
    std::size_t total_events = 0U;
    std::optional<std::size_t> active_interval;
    while (std::getline(input, line)) {
        if (line.size() > kMaximumLineBytes) {
            malformed_trace("line exceeds the 64 KiB bound");
        }
        if (!header_seen) {
            if (!line.starts_with("# takedown-gs-semantic-v1 ")) {
                malformed_trace("missing v1 header");
            }
            header_seen = true;
            continue;
        }
        if (line.starts_with("# end ")) {
            footer_seen = true;
            parsed.event_limit_reached = required_field(line, "limit") == "1";
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto sequence_end = line.find(' ');
        if (sequence_end == std::string::npos) {
            malformed_trace("event record has no type");
        }
        const std::string_view record{line.data() + sequence_end + 1U, line.size() - sequence_end - 1U};

        if (record.starts_with("VBLANK ")) {
            const auto phase = required_field(record, "phase");
            if (phase == "begin") {
                parsed.intervals.push_back({parse_decimal_field(record, "index")});
                active_interval = parsed.intervals.size() - 1U;
            } else if (phase == "end") {
                active_interval.reset();
            } else {
                malformed_trace("unknown VBlank phase");
            }
            continue;
        }

        std::optional<SemanticEvent> event;
        if (record.starts_with("TAG ")) {
            event = parse_tag_event(record);
        } else if (record.starts_with("REG ")) {
            event = parse_register_event(record);
        } else if (record.starts_with("IMAGE ")) {
            event = parse_image_event(record);
        } else {
            continue;
        }

        if (!active_interval) {
            parsed.intervals.push_back({});
            active_interval = parsed.intervals.size() - 1U;
        }
        parsed.intervals[*active_interval].events.push_back(*event);
        if (++total_events > kMaximumTraceEvents) {
            malformed_trace("more than 5,000,000 comparable events");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("unable to read complete GS semantic trace");
    }
    if (!header_seen || !footer_seen) {
        malformed_trace("trace is incomplete; close the runtime before comparing it");
    }

    std::vector<SemanticInterval> nonempty;
    for (auto& interval : parsed.intervals) {
        if (!interval.events.empty()) {
            nonempty.push_back(std::move(interval));
        }
    }
    parsed.intervals = std::move(nonempty);
    if (parsed.intervals.empty()) {
        malformed_trace("trace contains no TAG, REG, or IMAGE records");
    }
    return parsed;
}

template <std::size_t First>
[[nodiscard]] std::uint64_t quadword_u64(const guest::GsQuadword& quadword) {
    return static_cast<std::uint64_t>(quadword[First]) | (static_cast<std::uint64_t>(quadword[First + 1U]) << 32U);
}

// Observes the GIF stream of a PCSX2 dump after the capture's arbitration
// seam and keeps only the comparable commands.
class DumpSemanticCollector final : public guest::GsRawGifObserver, public guest::GsCommandObserver {
  public:
    DumpSemanticCollector(const std::uint64_t bitbltbuf, const std::uint64_t trxpos,
                          const std::uint64_t trxreg, const std::uint64_t trxdir)
        : bitbltbuf_(bitbltbuf), trxpos_(trxpos), trxreg_(trxreg), trxdir_(trxdir) {}

    using guest::GsCommandObserver::on_gs_image_transfer;

    void on_gs_gif_quadword(const guest::GifPath path, const guest::GsQuadword& quadword) override {
        unsigned long long& remaining = payload_remaining_[static_cast<std::size_t>(path)];
        if (remaining != 0U) {
            --remaining;
            return;
        }
        const auto low = quadword_u64<0U>(quadword);
        const auto high = quadword_u64<2U>(quadword);
        const unsigned int loops = quadword[0] & 0x7FFFU;
        const unsigned int format = static_cast<unsigned int>(low >> 58U) & 3U;
        unsigned int registers = static_cast<unsigned int>(low >> 60U);
        if (registers == 0U) {
            registers = 16U;
        }
        switch (format) {
        case 0U:
            remaining = static_cast<std::uint64_t>(loops) * registers;
            break;
        case 1U:
            remaining = (static_cast<std::uint64_t>(loops) * registers + 1U) >> 1U;
            break;
        case 2U:
        case 3U:
            remaining = loops;
            break;
        default:
            break;
        }
        SemanticEvent event{};
        event.values[0] = low;
        event.values[1] = high;
        events_.push_back(event);
    }

    void on_gs_gif_reset() noexcept override {
        payload_remaining_ = {};
    }

    void on_gs_register_write(const guest::GsRegisterWrite& write) override {
        switch (write.address) {
        case 0x50U:
            bitbltbuf_ = write.value;
            break;
        case 0x51U:
            trxpos_ = write.value;
            break;
        case 0x52U:
            trxreg_ = write.value;
            break;
        case 0x53U:
            trxdir_ = write.value;
            break;
        default:
            break;
        }
        SemanticEvent event{};
        event.kind = SemanticKind::Register;
        event.values[0] = write.address;
        event.values[1] = write.value;
        event.values[2] = write.packed_high;
        event.values[3] = static_cast<std::uint64_t>(write.source);
        events_.push_back(event);
    }

    void on_gs_image_transfer(const guest::GsImageTransfer& transfer) override {
        SemanticEvent event{};
        event.kind = SemanticKind::Image;
        event.values[0] = transfer.pixels.size();
        unsigned long long hash = 14695981039346656037ULL;
        for (const auto byte : transfer.pixels) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        event.values[1] = hash;
        event.values[2] = transfer.destination_base_pointer;
        event.values[3] = transfer.destination_buffer_width;
        event.values[4] = transfer.destination_psm;
        event.values[5] = transfer.destination_x;
        event.values[6] = transfer.destination_y;
        event.values[7] = transfer.width;
        event.values[8] = transfer.height;
        event.values[9] = bitbltbuf_;
        event.values[10] = trxpos_;
        event.values[11] = trxreg_;
        event.values[12] = trxdir_;
        events_.push_back(event);
    }

    [[nodiscard]] std::vector<SemanticEvent> take_events() {
        return std::exchange(events_, {});
    }

  private:
    std::array<unsigned long long, 3> payload_remaining_{};
    std::vector<SemanticEvent> events_{};
    std::uint64_t bitbltbuf_{};
    std::uint64_t trxpos_{};
    std::uint64_t trxreg_{};
    std::uint64_t trxdir_{};
};

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path, const char* const what,
                                                  const std::uintmax_t maximum_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size - 1U >= maximum_bytes) {
        throw std::runtime_error(std::string("unable to read a non-empty ") + what +
                                 " within its local-analysis size limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(std::string("unable to open ") + what);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        throw std::runtime_error(std::string("unable to read complete ") + what);
    }
    return bytes;
}

[[nodiscard]] std::uint64_t read_u64(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                                     const char* const detail) {
    require_range(bytes, offset, 8U, detail);
    std::uint64_t value = 0;
    for (unsigned int byte = 0; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] bool is_zstandard(const std::span<const std::uint8_t> bytes) {
    return bytes.size() > 3U && read_u32(bytes, 0U, "truncated compression marker") == kZstandardMagic;
}

// RECOVERY: statement-for-statement identical to the original GIMPLE; only GCC's
// global temporary counter (SR.<n>) differs by a constant offset because the
// original translation unit created a few more unnamed temporaries earlier on
// (some helper that was fully inlined away and could not be identified).
[[nodiscard]] ParsedDumpTrace parse_dump_trace(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> input = read_file(path, "PCSX2 GS dump", kMaximumInputBytes);
    const std::vector<std::uint8_t> decoded = is_zstandard(input) ? decompress_zstandard(input) : input;
    const std::span<const std::uint8_t> bytes{decoded};
    if (bytes.size() < 44U) {
        malformed_dump("truncated modern header");
    }
    if (read_u32(bytes, 0U, "truncated header marker") != 0xFFFFFFFFU) {
        malformed_dump("legacy or unrecognized header; capture a current Shift+F8 dump");
    }
    const auto header_bytes = static_cast<std::uint64_t>(read_u32(bytes, 4U, "truncated modern header size"));
    if (header_bytes <= 35U) {
        malformed_dump("modern header is smaller than its fixed fields");
    }
    require_range(bytes, 8U, header_bytes, "truncated modern header");
    const std::uint32_t state_version = read_u32(bytes, 8U, "truncated state version");
    const auto state_bytes = static_cast<std::uint64_t>(read_u32(bytes, 12U, "truncated state size"));
    if (state_version != 9U || state_bytes != 4194813U) {
        malformed_dump("semantic comparison requires the exact GSState v9 layout");
    }
    auto cursor = header_bytes + 8U;
    require_range(bytes, cursor, state_bytes, "truncated serialized GS state");
    const std::span<const std::uint8_t> state = bytes.subspan(cursor, state_bytes);
    cursor += state_bytes;
    require_range(bytes, cursor, 8192U, "truncated initial privileged-register snapshot");
    cursor += 8192U;

    guest::GsReplayStateSeed seed;
    seed.local_memory.resize(guest::kGsReplayStateLocalMemoryBytes);
    seed.bitbltbuf = read_u64(state, 84U, "truncated BITBLTBUF state");
    seed.trxdir = read_u64(state, 92U, "truncated TRXDIR state");
    seed.trxpos = read_u64(state, 100U, "truncated TRXPOS state");
    seed.trxreg = read_u64(state, 108U, "truncated TRXREG state");
    seed.transfer = {read_u32(state, 412U, "truncated transfer total"),
                     read_u32(state, 416U, "truncated transfer start"),
                     read_u32(state, 420U, "truncated transfer end"), state[424] != 0U};
    for (std::size_t path_index = 0; path_index < 4U; ++path_index) {
        const std::uint64_t tag = read_u64(state, 4194729U + path_index * 20U, "truncated serialized GIF path state");
        if ((tag & 0x7FFFU) != 0U) {
            ++seed.active_gif_paths;
        }
    }

    guest::GsCapture capture;
    capture.set_image_history_enabled(false);
    const auto restore_status = capture.restore_idle_state(seed);
    if (restore_status != guest::GsReplayStateRestoreStatus::Applied) {
        malformed_dump("capture starts at a non-idle GS transport boundary (" +
                       std::string(guest::to_string(restore_status)) + ')');
    }
    DumpSemanticCollector collector{seed.bitbltbuf, seed.trxpos, seed.trxreg, seed.trxdir};
    capture.set_diagnostic_raw_gif_observer(&collector);
    capture.set_diagnostic_command_observer(&collector);

    ParsedDumpTrace parsed;
    std::size_t interval_index = 0;
    while (cursor < bytes.size()) {
        const auto packet = bytes[cursor++];
        switch (packet) {
        case 0: {
            require_range(bytes, cursor, 5U, "truncated GS-transfer packet");
            const auto path_index = bytes[cursor];
            const auto payload_bytes =
                static_cast<std::uint64_t>(read_u32(bytes, cursor + 1U, "truncated GS-transfer payload size"));
            cursor += 5U;
            require_range(bytes, cursor, payload_bytes, "truncated GS-transfer payload");
            if (path_index != 3U) {
                malformed_dump("comparison currently requires PCSX2's generic ordered transfer cursor");
            }
            if ((payload_bytes & 15U) != 0U) {
                malformed_dump("GS-transfer payload is not quadword-aligned");
            }
            for (std::size_t payload_offset = 0; payload_offset < payload_bytes; payload_offset += 16U) {
                capture.submit_gif_quadword(
                    guest::GifPath::Path3,
                    guest::GsQuadword{read_u32(bytes, cursor + payload_offset, "truncated GIF quadword lane 0"),
                                      read_u32(bytes, cursor + payload_offset + 4U, "truncated GIF quadword lane 1"),
                                      read_u32(bytes, cursor + payload_offset + 8U, "truncated GIF quadword lane 2"),
                                      read_u32(bytes, cursor + payload_offset + 12U, "truncated GIF quadword lane 3")});
            }
            cursor += payload_bytes;
            break;
        }
        case 1:
            require_range(bytes, cursor, 1U, "truncated VSync packet");
            cursor += 1U;
            parsed.intervals.push_back({interval_index++, collector.take_events()});
            break;
        case 2:
            require_range(bytes, cursor, 4U, "truncated FIFO-read packet");
            cursor += 4U;
            break;
        case 3:
            require_range(bytes, cursor, 8192U, "truncated privileged-register packet");
            cursor += 8192U;
            break;
        default:
            malformed_dump("unknown packet type");
        }
    }
    auto trailing = collector.take_events();
    if (!trailing.empty()) {
        parsed.intervals.push_back({interval_index, std::move(trailing)});
    }
    parsed.decoder_diagnostics = capture.diagnostics().size();

    std::vector<SemanticInterval> nonempty;
    for (auto& interval : parsed.intervals) {
        if (!interval.events.empty()) {
            nonempty.push_back(std::move(interval));
        }
    }
    parsed.intervals = std::move(nonempty);
    if (parsed.intervals.empty()) {
        malformed_dump("dump contains no comparable TAG, REG, or IMAGE records");
    }
    return parsed;
}

[[nodiscard]] const char* register_source_name(const std::uint64_t source) {
    switch (static_cast<guest::GsRegisterWriteSource>(source)) {
    case guest::GsRegisterWriteSource::GifTagPrim:
        return "tag-prim";
    case guest::GsRegisterWriteSource::Packed:
        return "packed";
    case guest::GsRegisterWriteSource::Reglist:
        return "reglist";
    case guest::GsRegisterWriteSource::Ad:
        return "ad";
    }
    return "unknown";
}

[[nodiscard]] std::string describe_event(const SemanticEvent& event) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    switch (event.kind) {
    case SemanticKind::Tag:
        output << "TAG raw=" << std::setw(16) << event.values[0] << " regs=" << std::setw(16) << event.values[1];
        break;
    case SemanticKind::Register:
        output << "REG source=" << register_source_name(event.values[3]) << " addr=" << std::setw(2) << event.values[0] << " value=" << std::setw(16)
               << event.values[1] << " high=" << std::setw(16) << event.values[2];
        break;
    case SemanticKind::Image:
        output << std::dec << "IMAGE bytes=" << event.values[0] << std::hex << " hash=" << std::setw(16)
               << event.values[1] << " dbp=" << std::setw(4) << event.values[2] << " dbw=" << std::setw(2)
               << event.values[3] << " dpsm=" << std::setw(2) << event.values[4] << " dsax=" << std::setw(3)
               << event.values[5] << " dsay=" << std::setw(3) << event.values[6] << std::dec
               << " width=" << event.values[7] << " height=" << event.values[8] << std::hex
               << " bitbltbuf=" << std::setw(16) << event.values[9] << " trxpos=" << std::setw(16) << event.values[10]
               << " trxreg=" << std::setw(16) << event.values[11] << " trxdir=" << std::setw(16) << event.values[12];
        break;
    }
    return output.str();
}


[[nodiscard]] std::size_t matching_prefix(const std::span<const SemanticEvent> left,
                                          const std::span<const SemanticEvent> right) noexcept {
    std::size_t matched = 0;
    const std::size_t limit = std::min(left.size(), right.size());
    while (matched < limit && left[matched] == right[matched]) {
        ++matched;
    }
    return matched;
}

[[nodiscard]] std::vector<SemanticEvent> events_of_kind(const std::span<const SemanticEvent> events,
                                                         const SemanticKind kind) {
    std::vector<SemanticEvent> selected;
    for (const SemanticEvent& event : events) {
        if (event.kind == kind) {
            selected.push_back(event);
        }
    }
    return selected;
}
}  // namespace

const char* to_string(const GsSemanticComparisonStatus status) noexcept {
    switch (status) {
    case GsSemanticComparisonStatus::ExactMatch:
        return "exact match";
    case GsSemanticComparisonStatus::CapturedPrefixMatch:
        return "captured prefix match";
    case GsSemanticComparisonStatus::Diverged:
        return "diverged";
    }
    return "unknown";
}

GsSemanticComparison compare_gs_semantic_trace_with_pcsx2_dump(const std::filesystem::path& live_trace_path,
                                                               const std::filesystem::path& pcsx2_dump_path,
                                                               const std::size_t context_events) {
    const ParsedLiveTrace live = parse_live_trace(live_trace_path);
    const ParsedDumpTrace reference = parse_dump_trace(pcsx2_dump_path);

    std::size_t best_live = 0;
    std::size_t best_reference = 0;
    std::size_t best_matched = 0;
    bool have_best = false;
    bool best_exact = false;
    for (std::size_t live_index = 0; live_index < live.intervals.size(); ++live_index) {
        for (std::size_t reference_index = 0; reference_index < reference.intervals.size(); ++reference_index) {
            const SemanticInterval& live_candidate = live.intervals[live_index];
            const SemanticInterval& reference_candidate = reference.intervals[reference_index];
            const unsigned long long matched = matching_prefix(live_candidate.events, reference_candidate.events);
            const bool exact = matched == live_candidate.events.size() &&
                               matched == reference_candidate.events.size();
            if (!have_best || matched > best_matched || (matched == best_matched && exact && !best_exact)) {
                best_live = live_index;
                best_reference = reference_index;
                best_matched = matched;
                best_exact = exact;
            }
            have_best = true;
        }
    }

    const SemanticInterval& live_interval = live.intervals[best_live];
    const SemanticInterval& reference_interval = reference.intervals[best_reference];
    GsSemanticComparison comparison{
        .live_vblank = live_interval.index,
        .pcsx2_interval = reference_interval.index,
        .live_interval_events = live_interval.events.size(),
        .pcsx2_interval_events = reference_interval.events.size(),
        .matched_prefix_events = best_matched,
        .live_intervals_considered = live.intervals.size(),
        .pcsx2_intervals_considered = reference.intervals.size(),
        .live_trace_event_limit_reached = live.event_limit_reached,
        .pcsx2_decoder_diagnostics = reference.decoder_diagnostics,
    };
    if (best_exact) {
        comparison.status = GsSemanticComparisonStatus::ExactMatch;
    } else if (live.event_limit_reached && best_matched == live_interval.events.size() &&
               best_matched <= reference_interval.events.size()) {
        comparison.status = GsSemanticComparisonStatus::CapturedPrefixMatch;
    }

    const auto context_begin = best_matched > context_events ? best_matched - context_events : 0U;
    for (auto index = context_begin; index < best_matched; ++index) {
        comparison.matching_context.push_back("event " + std::to_string(index) + ": " +
                                              describe_event(live_interval.events[index]));
    }
    if (best_matched < live_interval.events.size()) {
        comparison.live_mismatch = "event " + std::to_string(best_matched) + ": " +
                                   describe_event(live_interval.events[best_matched]);
    }
    if (best_matched < reference_interval.events.size()) {
        comparison.pcsx2_mismatch = "event " + std::to_string(best_matched) + ": " +
                                    describe_event(reference_interval.events[best_matched]);
    }

    std::vector<std::vector<SemanticEvent>> live_images;
    live_images.reserve(live.intervals.size());
    for (const SemanticInterval& interval : live.intervals) {
        live_images.push_back(events_of_kind(interval.events, SemanticKind::Image));
    }
    std::vector<std::vector<SemanticEvent>> reference_images;
    reference_images.reserve(reference.intervals.size());
    for (const SemanticInterval& interval : reference.intervals) {
        reference_images.push_back(events_of_kind(interval.events, SemanticKind::Image));
    }

    std::size_t image_best_live = 0;
    std::size_t image_best_reference = 0;
    std::size_t image_best_matched = 0;
    bool have_image_best = false;
    bool image_best_exact = false;
    for (std::size_t live_index = 0; live_index < live_images.size(); ++live_index) {
        if (live_images[live_index].empty()) {
            continue;
        }
        for (std::size_t reference_index = 0; reference_index < reference_images.size(); ++reference_index) {
            if (reference_images[reference_index].empty()) {
                continue;
            }
            const unsigned long long matched = matching_prefix(live_images[live_index], reference_images[reference_index]);
            const bool exact = matched == live_images[live_index].size() &&
                               matched == reference_images[reference_index].size();
            if (!have_image_best || matched > image_best_matched ||
                (matched == image_best_matched && exact && !image_best_exact)) {
                have_image_best = true;
                image_best_live = live_index;
                image_best_reference = reference_index;
                image_best_matched = matched;
                image_best_exact = exact;
            }
        }
    }

    if (have_image_best) {
        const std::vector<SemanticEvent>& live_uploads = live_images[image_best_live];
        const std::vector<SemanticEvent>& reference_uploads = reference_images[image_best_reference];
        comparison.image_comparison_available = true;
        comparison.image_live_vblank = live.intervals[image_best_live].index;
        comparison.image_pcsx2_interval = reference.intervals[image_best_reference].index;
        comparison.live_image_transfers = live_uploads.size();
        comparison.pcsx2_image_transfers = reference_uploads.size();
        comparison.matched_image_prefix = image_best_matched;
        if (image_best_exact) {
            comparison.image_status = GsSemanticComparisonStatus::ExactMatch;
        } else if (live.event_limit_reached && image_best_matched == live_uploads.size() &&
                   image_best_matched <= reference_uploads.size()) {
            comparison.image_status = GsSemanticComparisonStatus::CapturedPrefixMatch;
        } else {
            comparison.image_status = GsSemanticComparisonStatus::Diverged;
        }
        const auto image_context_begin =
            image_best_matched > context_events ? image_best_matched - context_events : 0U;
        for (auto index = image_context_begin; index < image_best_matched; ++index) {
            comparison.image_matching_context.push_back("upload " + std::to_string(index) + ": " +
                                                        describe_event(live_uploads[index]));
        }
        if (image_best_matched < live_uploads.size()) {
            comparison.live_image_mismatch = "upload " + std::to_string(image_best_matched) + ": " +
                                             describe_event(live_uploads[image_best_matched]);
        }
        if (image_best_matched < reference_uploads.size()) {
            comparison.pcsx2_image_mismatch = "upload " + std::to_string(image_best_matched) + ": " +
                                              describe_event(reference_uploads[image_best_matched]);
        }
    }
    return comparison;
}

}  // namespace takedown::platform
