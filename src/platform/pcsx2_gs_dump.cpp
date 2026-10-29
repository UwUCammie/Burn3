#include "takedown/platform/pcsx2_gs_dump.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace takedown::platform {

namespace {

constexpr std::size_t kMaximumInputBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumDecodedBytes = 512U * 1024U * 1024U;
constexpr std::uint32_t kZstandardMagic = 0xFD2FB528U;
constexpr std::size_t kCrtcSnapshotBytes = 8192U;
constexpr std::size_t kGsStateTailOffset = 80U;
constexpr std::size_t kGsStateTailPathBytes = 20U;

// RECOVERY: body matches; only GCC's retvalptr temporary numbering differs.
[[noreturn]] void malformed_dump(const std::string& detail) {
    throw std::runtime_error("invalid PCSX2 GS dump: " + detail);
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                       const char* message) {
    if (bytes.size() < offset || bytes.size() - offset < 4U) {
        malformed_dump(message);
    }
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t read_u64(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                       const char* message) {
    if (bytes.size() < offset || bytes.size() - offset < 8U) {
        malformed_dump(message);
    }
    std::uint64_t value = 0;
    for (unsigned int byte = 0U; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

struct ZstdStreamDeleter {
    void operator()(ZSTD_DStream* stream) const noexcept {
        ZSTD_freeDStream(stream);
    }
};

// RECOVERY: the original is a .constprop clone (GCC specialised a constant
// argument); this build keeps a standalone copy, so the call sites differ.
[[nodiscard]] Pcsx2GsDumpCrtcRegisters read_crtc_registers(const std::span<const std::uint8_t> registers) {
    if (registers.size() < kCrtcSnapshotBytes) {
        malformed_dump("truncated GS privileged-register snapshot");
    }
    Pcsx2GsDumpCrtcRegisters crtc{};
    crtc.pmode = read_u64(registers, 0x00U, "truncated PMODE register");
    crtc.smode2 = read_u64(registers, 0x20U, "truncated SMODE2 register");
    crtc.dispfb1 = read_u64(registers, 0x70U, "truncated DISPFB1 register");
    crtc.display1 = read_u64(registers, 0x80U, "truncated DISPLAY1 register");
    crtc.dispfb2 = read_u64(registers, 0x90U, "truncated DISPFB2 register");
    crtc.display2 = read_u64(registers, 0xA0U, "truncated DISPLAY2 register");
    crtc.bgcolor = read_u64(registers, 0xE0U, "truncated BGCOLOR register");
    return crtc;
}

// RECOVERY: the original copies the error string through a temporary
// std::string that this build does not reproduce.
[[nodiscard]] std::vector<std::uint8_t> decompress_zstandard(const std::span<const std::uint8_t> input) {
    std::unique_ptr<ZSTD_DStream, ZstdStreamDeleter> stream(ZSTD_createDStream());
    if (!stream) {
        throw std::runtime_error("unable to initialize Zstandard GS-dump decompressor");
    }
    const std::size_t init_result = ZSTD_initDStream(stream.get());
    if (ZSTD_isError(init_result)) {
        throw std::runtime_error(
            std::string(ZSTD_getErrorName(init_result)).insert(0, "unable to initialize Zstandard GS-dump decompressor: "));
    }

    std::vector<std::uint8_t> output;
    output.reserve(std::min(input.size() * 8U, kMaximumDecodedBytes));
    std::array<std::uint8_t, 65536> buffer{};
    ZSTD_inBuffer compressed{input.data(), input.size(), 0U};
    std::size_t last_result = 1U;
    while (compressed.pos < compressed.size) {
        const std::size_t before = compressed.pos;
        ZSTD_outBuffer decoded{buffer.data(), buffer.size(), 0U};
        last_result = ZSTD_decompressStream(stream.get(), &decoded, &compressed);
        if (ZSTD_isError(last_result)) {
            throw std::runtime_error(
                std::string(ZSTD_getErrorName(last_result)).insert(0, "unable to decompress PCSX2 GS dump: "));
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
    return output;
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size - 1U > kMaximumInputBytes - 1U) {
        throw std::runtime_error("unable to read a non-empty PCSX2 GS dump within the 256 MiB local-analysis limit");
    }
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open PCSX2 GS dump");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("unable to read complete PCSX2 GS dump");
    }
    return bytes;
}

// RECOVERY: GIF path analyzer and packet walker rebuilt from the GIMPLE
// structure and string/constant evidence; statement-level identity is not
// established for this function.
enum class GifPhase : std::uint8_t {
    Tag = 0,
    Packed = 1,
    RegList = 2,
    Image = 3,
};

struct GifPathState {
    GifPhase phase{GifPhase::Tag};
    std::uint64_t register_descriptors{};
    std::uint32_t register_count{};
    std::uint32_t register_index{};
    std::uint64_t payload_quadwords_remaining{};
    std::uint64_t register_values_remaining{};
};

struct GifAnalyzer {
    Pcsx2GsDumpGifStatistics* statistics_{};
    std::array<GifPathState, 4> paths_{};
    std::uint8_t current_primitive_{};
    bool primitive_known_{};
};

// Applies one A+D (or direct descriptor) register write to the statistics.
void count_register_write(GifAnalyzer& gif, const std::uint32_t address, const std::uint64_t lo,
                          const std::uint64_t hi) {
    auto& stats = *gif.statistics_;
    switch (address) {
    case 0:  // PRIM
        gif.current_primitive_ = static_cast<std::uint8_t>(lo & 7U);
        gif.primitive_known_ = true;
        ++stats.primitive_state_writes[gif.current_primitive_];
        break;
    case 4:
    case 5:
    case 12:
    case 13:
        if (((hi >> 47U) & 1U) != 0U) {
            ++stats.adc_suppressed_kicks;
            ++stats.nonkicking_xyz_writes;
        } else {
            ++stats.draw_kicks;
            if (gif.primitive_known_) {
                ++stats.draw_kicks_by_primitive[gif.current_primitive_];
            } else {
                ++stats.draw_kicks_without_primitive;
            }
        }
        break;
    default:
        break;
    }
}

// Consumes one 16-byte GIF quadword on a path.
void consume_gif_quadword(GifAnalyzer& gif, GifPathState& path, const std::span<const std::uint8_t> payload,
                          const std::size_t offset) {
    auto& stats = *gif.statistics_;
    const std::uint64_t lo = read_u64(payload, offset, "truncated GIF tag");
    const std::uint64_t hi = read_u64(payload, offset + 8U, "truncated GIF descriptors");

    switch (path.phase) {
    case GifPhase::Tag: {
        const std::uint64_t loops = lo & 0x7FFFU;
        const auto format = static_cast<std::uint8_t>((lo >> 58U) & 3U);
        const auto count = static_cast<std::uint32_t>(lo >> 60U);
        const std::uint32_t register_count = count == 0U ? 16U : count;
        if (loops == 0U) {
            ++stats.zero_loop_tags;
        }
        ++stats.gif_tags_by_format[format];
        if (format == 0U && ((lo >> 46U) & 1U) != 0U) {
            gif.current_primitive_ = static_cast<std::uint8_t>((lo >> 47U) & 7U);
            gif.primitive_known_ = true;
            ++stats.primitive_state_writes[gif.current_primitive_];
        }
        path = GifPathState{};
        path.register_descriptors = hi;
        path.register_count = register_count;
        if (format == 0U) {
            path.payload_quadwords_remaining = loops * register_count;
            path.register_values_remaining = loops * register_count;
            path.phase = path.payload_quadwords_remaining != 0U ? GifPhase::Packed : GifPhase::Tag;
        } else if (format == 1U) {
            path.payload_quadwords_remaining = (loops * register_count + 1U) >> 1U;
            path.phase = path.payload_quadwords_remaining != 0U ? GifPhase::RegList : GifPhase::Tag;
        } else {
            path.payload_quadwords_remaining = loops;
            path.phase = path.payload_quadwords_remaining != 0U ? GifPhase::Image : GifPhase::Tag;
        }
        break;
    }
    case GifPhase::Packed: {
        const auto descriptor = static_cast<std::uint32_t>((path.register_descriptors >> (path.register_index * 4U)) & 15U);
        ++stats.direct_register_descriptor_counts[descriptor];
        if (descriptor == 0xEU) {
            const auto address = static_cast<std::uint32_t>(hi & 127U);
            ++stats.register_writes_by_address[address];
            count_register_write(gif, address, lo, hi);
        } else if (descriptor != 0xFU) {
            count_register_write(gif, descriptor, lo, hi);
        }
        if (++path.register_index == path.register_count) {
            path.register_index = 0U;
        }
        --path.payload_quadwords_remaining;
        --path.register_values_remaining;
        if (path.payload_quadwords_remaining == 0U) {
            path.phase = GifPhase::Tag;
        }
        break;
    }
    case GifPhase::RegList: {
        const auto first = static_cast<std::uint32_t>((path.register_descriptors >> (path.register_index * 4U)) & 15U);
        ++stats.direct_register_descriptor_counts[first];
        count_register_write(gif, first, lo, 0U);
        if (++path.register_index == path.register_count) {
            path.register_index = 0U;
        }
        if (path.register_values_remaining == 0U || path.register_count > 1U) {
            const auto second = static_cast<std::uint32_t>((path.register_descriptors >> (path.register_index * 4U)) & 15U);
            ++stats.direct_register_descriptor_counts[second];
            count_register_write(gif, second, hi, 0U);
            if (++path.register_index == path.register_count) {
                path.register_index = 0U;
            }
        }
        --path.payload_quadwords_remaining;
        if (path.payload_quadwords_remaining == 0U) {
            path.phase = GifPhase::Tag;
        }
        break;
    }
    case GifPhase::Image:
        ++stats.image_payload_quadwords;
        --path.payload_quadwords_remaining;
        if (path.payload_quadwords_remaining == 0U) {
            path.phase = GifPhase::Tag;
        }
        break;
    }
}

}  // namespace

namespace {

// RECOVERY: header, GS-state tail and packet-walk layout follow the GIMPLE;
// the GIF path analyzer above is a reconstruction.
Pcsx2GsDumpInspection inspect_uncompressed_pcsx2_gs_dump(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 44U) {
        malformed_dump("truncated modern header");
    }
    if (read_u32(bytes, 0U, "truncated header marker") != 0xFFFFFFFFU) {
        malformed_dump("legacy or unrecognized header; capture a current Shift+F8 dump");
    }
    const std::uint32_t header_size = read_u32(bytes, 4U, "truncated modern header size");
    if (header_size <= 35U) {
        malformed_dump("modern header is smaller than its fixed fields");
    }
    if (header_size > bytes.size() - 8U) {
        malformed_dump("truncated modern header payload");
    }

    Pcsx2GsDumpInspection result{};
    result.state_version = read_u32(bytes, 8U, "truncated state version");
    result.state_bytes = read_u32(bytes, 12U, "truncated state size");
    const std::uint32_t serial_offset = read_u32(bytes, 16U, "truncated serial offset");
    const std::uint32_t serial_size = read_u32(bytes, 20U, "truncated serial size");
    const std::uint32_t screenshot_offset = read_u32(bytes, 36U, "truncated screenshot offset");
    const std::uint32_t screenshot_size = read_u32(bytes, 40U, "truncated screenshot size");
    if (header_size < serial_offset || serial_size > header_size - serial_offset) {
        malformed_dump("serial range leaves the header");
    }
    if (header_size < screenshot_offset || screenshot_size > header_size - screenshot_offset) {
        malformed_dump("screenshot range leaves the header");
    }

    std::size_t cursor = static_cast<std::size_t>(header_size) + 8U;
    if (cursor > bytes.size() || result.state_bytes > bytes.size() - cursor) {
        malformed_dump("truncated serialized GS state");
    }
    const auto serialized_state = bytes.subspan(cursor, result.state_bytes);
    cursor += result.state_bytes;
    if (cursor > bytes.size() || kCrtcSnapshotBytes > bytes.size() - cursor) {
        malformed_dump("truncated initial privileged-register snapshot");
    }
    result.initial_crtc = read_crtc_registers(bytes.subspan(cursor));
    result.final_crtc = result.initial_crtc;
    cursor += kCrtcSnapshotBytes;

    GifAnalyzer gif;
    gif.statistics_ = &result.gif;
    std::array<GifPathState, 4> restored_paths{};
    if (result.state_version == 9U) {
        if (serialized_state.size() < 84U) {
            malformed_dump("serialized v9 GS state is too small for its GIF-path tail");
        }
        if (read_u32(serialized_state, 0U, "truncated serialized GS state version") != 9U) {
            malformed_dump("v9 dump header does not contain a v9 serialized GS state");
        }
        gif.current_primitive_ = static_cast<std::uint8_t>(read_u64(serialized_state, 4U, "truncated serialized GS primitive") & 7U);
        gif.primitive_known_ = true;
        const std::size_t tail_offset = read_u32(serialized_state, kGsStateTailOffset, "truncated serialized GIF tag");
        for (std::size_t path = 0U; path != 4U; ++path) {
            const std::size_t path_offset = tail_offset + path * kGsStateTailPathBytes;
            if (tail_offset > serialized_state.size() ||
                serialized_state.size() - tail_offset < (path + 1U) * kGsStateTailPathBytes) {
                malformed_dump("serialized v9 GS state is too small for its GIF-path tail");
            }
            const std::uint64_t tag = read_u64(serialized_state, path_offset, "truncated serialized GIF tag");
            const std::uint64_t descriptors =
                read_u64(serialized_state, path_offset + 8U, "truncated serialized GIF descriptors");
            const std::uint32_t index =
                read_u32(serialized_state, path_offset + 16U, "truncated serialized GIF register index");
            GifPathState state{};
            const std::uint64_t loops = tag & 0x7FFFU;
            if (loops != 0U) {
                const auto format = static_cast<std::uint8_t>((tag >> 58U) & 3U);
                const auto count = static_cast<std::uint32_t>(tag >> 60U);
                const std::uint32_t register_count = count == 0U ? 16U : count;
                if (index >= register_count) {
                    malformed_dump("serialized GIF register index is outside its tag descriptor list");
                }
                state.register_descriptors = descriptors;
                state.register_count = register_count;
                state.register_index = index;
                if (format == 0U) {
                    state.payload_quadwords_remaining = loops * register_count;
                    state.phase = GifPhase::Packed;
                } else if (format == 1U) {
                    state.payload_quadwords_remaining = (loops * register_count + 1U) >> 1U;
                    state.phase = GifPhase::RegList;
                } else {
                    state.payload_quadwords_remaining = loops;
                    state.phase = GifPhase::Image;
                }
            }
            restored_paths[path] = state;
        }
        gif.paths_ = restored_paths;
        result.gif.statistics_available = true;
    }

    while (cursor < bytes.size()) {
        const std::uint8_t packet_id = bytes[cursor];
        switch (packet_id) {
        case 0U: {  // GS transfer: id, path, payload size, payload.
            if (cursor + 6U > bytes.size()) {
                malformed_dump("truncated GS-transfer packet");
            }
            const std::uint8_t path = bytes[cursor + 1U];
            const std::uint32_t payload_size = read_u32(bytes, cursor + 2U, "truncated GS-transfer payload size");
            if (payload_size > bytes.size() - (cursor + 6U)) {
                malformed_dump("truncated GS-transfer payload");
            }
            ++result.transfer_packets;
            const std::size_t payload_begin = cursor + 6U;
            if (path > 3U) {
                ++result.gif.unsupported_transfer_path_packets;
            } else {
                ++result.gif.transfer_packets_by_path[path];
                result.gif.transfer_bytes_by_path[path] += payload_size;
            }
            if (!result.gif.statistics_available || path > 3U) {
                if (result.skipped_transfer_bytes > ~static_cast<std::uint64_t>(payload_size)) {
                    malformed_dump("GS-transfer metadata overflow");
                }
                result.skipped_transfer_bytes += payload_size;
            } else {
                if (path == 0U && gif.paths_[0].phase != GifPhase::Tag) {
                    gif.paths_[0] = GifPathState{};
                    ++result.gif.path0_incomplete_resets;
                }
                if ((payload_size & 15U) != 0U) {
                    malformed_dump("GIF transfer payload is not quadword-aligned");
                }
                for (std::size_t offset = 0U; offset < payload_size; offset += 16U) {
                    consume_gif_quadword(gif, gif.paths_[path], bytes.subspan(payload_begin), offset);
                }
            }
            cursor = payload_begin + payload_size;
            break;
        }
        case 1U:  // VSync
            if (cursor + 2U > bytes.size()) {
                malformed_dump("truncated VSync packet");
            }
            ++result.vsync_packets;
            cursor += 2U;
            break;
        case 2U:  // FIFO read
            if (cursor + 5U > bytes.size()) {
                malformed_dump("truncated FIFO-read packet");
            }
            ++result.fifo_read_packets;
            cursor += 5U;
            break;
        case 3U:  // privileged register snapshot
            if (cursor + 8193U > bytes.size()) {
                malformed_dump("truncated privileged-register packet");
            }
            ++result.privileged_register_packets;
            result.final_crtc = read_crtc_registers(bytes.subspan(cursor + 1U));
            result.final_crtc_came_from_packet = true;
            cursor += 8193U;
            break;
        default:
            malformed_dump("unknown packet type");
        }
    }

    if (result.gif.statistics_available) {
        for (const auto& path : gif.paths_) {
            if (path.phase != GifPhase::Tag) {
                ++result.gif.incomplete_paths;
            }
        }
    }
    return result;
}

}  // namespace

const char* to_string(const Pcsx2GsDumpCompression compression) noexcept {
    switch (compression) {
    case Pcsx2GsDumpCompression::Uncompressed:
        return "uncompressed";
    case Pcsx2GsDumpCompression::Zstandard:
        return "zstandard";
    }
    return "unknown";
}

// RECOVERY: the compression-marker test is folded differently than in the
// original (which keeps a span copy around the marker read).
Pcsx2GsDumpInspection inspect_pcsx2_gs_dump_bytes(const std::span<const std::uint8_t> input) {
    if (input.empty()) {
        malformed_dump("empty input");
    }
    if (input.size() > kMaximumInputBytes) {
        throw std::runtime_error("PCSX2 GS dump exceeds the 256 MiB local-analysis limit");
    }
    const bool zstandard = input.size() > 3U && read_u32(input, 0U, "truncated compression marker") == kZstandardMagic;
    const auto decoded = zstandard ? decompress_zstandard(input)
                                   : std::vector<std::uint8_t>(input.begin(), input.end());
    if (decoded.size() > kMaximumDecodedBytes) {
        throw std::runtime_error("PCSX2 GS dump exceeds the 512 MiB local-analysis limit");
    }
    Pcsx2GsDumpInspection result = inspect_uncompressed_pcsx2_gs_dump(decoded);
    result.compression = zstandard ? Pcsx2GsDumpCompression::Zstandard : Pcsx2GsDumpCompression::Uncompressed;
    result.input_bytes = input.size();
    result.decoded_bytes = decoded.size();
    return result;
}

Pcsx2GsDumpInspection inspect_pcsx2_gs_dump(const std::filesystem::path& path) {
    const auto input = read_file(path);
    return inspect_pcsx2_gs_dump_bytes(input);
}

}  // namespace takedown::platform
