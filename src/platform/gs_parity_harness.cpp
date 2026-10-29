#include "takedown/platform/gs_parity_harness.hpp"

#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_live_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace takedown::platform {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

// Whole-file reader for the headerless raw stream grammar.
class StreamCursor {
public:
    explicit StreamCursor(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("could not open GS parity raw stream: " + path.string());
        }
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0) {
            throw std::runtime_error("could not size GS parity raw stream: " + path.string());
        }
        input.seekg(0, std::ios::beg);
        bytes_.resize(static_cast<std::size_t>(end));
        if (!bytes_.empty() &&
            !input.read(reinterpret_cast<char*>(bytes_.data()),
                        static_cast<std::streamsize>(bytes_.size()))) {
            throw std::runtime_error("could not read GS parity raw stream: " + path.string());
        }
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

    [[nodiscard]] const std::uint8_t* take(std::size_t count) noexcept {
        const auto* data = bytes_.data() + position_;
        position_ += count;
        return data;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_{};
};

[[nodiscard]] std::uint64_t fnv1a_u64(std::uint64_t hash, std::uint64_t value) noexcept {
    for (unsigned int byte = 0U; byte < 8U; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFU;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

[[nodiscard]] std::uint64_t fnv1a_bytes(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

// One independent capture + renderer pipeline.
struct ReplayArm {
    ReplayArm() { capture.set_command_observer(&renderer); }

    // Snapshots what the capture decoded so the capture can be dropped first.
    void collect() {
        register_writes.assign(capture.register_writes().begin(), capture.register_writes().end());
        image_transfers.assign(capture.image_transfers().begin(),
                               capture.image_transfers().end());
        diagnostic_count = capture.diagnostics().size();
    }

    // Folds the collected counts and image payloads into the summary.
    void describe(GsParityReplaySummary& summary) const {
        summary.register_writes = register_writes.size();
        summary.image_transfers = image_transfers.size();
        summary.capture_diagnostics = diagnostic_count;
        summary.image_fnv1a64 = kFnvOffsetBasis;
        for (const auto& transfer : image_transfers) {
            summary.image_fnv1a64 = fnv1a_u64(summary.image_fnv1a64, transfer.sequence);
            summary.image_fnv1a64 = fnv1a_u64(summary.image_fnv1a64, transfer.pixels.size());
            summary.image_fnv1a64 = fnv1a_u64(
                summary.image_fnv1a64, fnv1a_bytes(transfer.pixels.data(), transfer.pixels.size()));
        }
    }

    guest::GsCapture capture;
    guest::GsLiveRenderer renderer;
    std::vector<guest::GsRegisterWrite> register_writes;
    std::vector<guest::GsImageTransfer> image_transfers;
    std::size_t diagnostic_count{};
};

bool project_surface(const guest::GsLocalMemory& memory, const GsParityReplayConfig& config,
                     GsParityReplaySummary& summary) {
    const guest::GsLocalMemorySurface surface{config.surface_base_pointer,
                                              config.surface_buffer_width,
                                              config.surface_pixel_storage_mode};
    summary.image_width = config.width;
    summary.image_height = config.height;
    const auto pixel_count = static_cast<std::size_t>(config.width) * config.height;
    summary.image_rgba.assign(pixel_count * 4U, 0U);
    for (std::uint32_t row = 0U; row < config.height; ++row) {
        for (std::uint32_t column = 0U; column < config.width; ++column) {
            const auto pixel = memory.read_rgba8(surface, config.x + column, config.y + row);
            if (!pixel) {
                return false;
            }
            const auto destination = (static_cast<std::size_t>(row) * config.width + column) * 4U;
            summary.image_rgba[destination] = pixel->red;
            summary.image_rgba[destination + 1U] = pixel->green;
            summary.image_rgba[destination + 2U] = pixel->blue;
            summary.image_rgba[destination + 3U] = pixel->alpha;
        }
    }
    summary.image_fnv1a64_pixels = fnv1a_bytes(summary.image_rgba.data(), summary.image_rgba.size());
    return true;
}

// RECOVERY: not byte-identical in GIMPLE; named SSA version suffixes are offset by ~17 vs the original (cause not identified).
[[nodiscard]] GsParityReplaySummary replay_arm_summary(
    const std::vector<ParallelGsStreamQuadword>& quadwords, const GsParityReplayConfig& config) {
    GsParityReplaySummary summary{};
    summary.quadwords = quadwords.size();
    if (quadwords.empty()) {
        return summary;
    }

    ReplayArm arm;
    for (const auto& item : quadwords) {
        arm.capture.submit_gif_quadword(item.path, item.quadword);
    }
    arm.collect();

    arm.describe(summary);

    if (project_surface(arm.renderer.local_memory(), config, summary)) {
        summary.image_status = GsParityImageStatus::Rendered;
    } else {
        summary.image_status = GsParityImageStatus::ProjectionFailed;
        summary.image_rgba.clear();
        summary.image_fnv1a64_pixels = kFnvOffsetBasis;
    }
    return summary;
}

} // namespace

const char* to_string(const GsParityStreamStatus status) noexcept {
    switch (status) {
    case GsParityStreamStatus::Complete:
        return "complete";
    case GsParityStreamStatus::TruncatedPacket:
        return "truncated packet header";
    case GsParityStreamStatus::TruncatedPayload:
        return "truncated transfer payload";
    case GsParityStreamStatus::TruncatedPrivilegedRegisters:
        return "truncated privileged-register snapshot";
    case GsParityStreamStatus::UnknownPacketType:
        return "unknown packet type";
    case GsParityStreamStatus::InvalidGifPath:
        return "invalid GIF path";
    }
    return "unknown GS parity stream status";
}

const char* to_string(const GsParityImageStatus status) noexcept {
    switch (status) {
    case GsParityImageStatus::Rendered:
        return "rendered";
    case GsParityImageStatus::ProjectionFailed:
        return "projection failed";
    case GsParityImageStatus::NoQuadwords:
        return "no quadwords";
    }
    return "unknown GS parity image status";
}

GsParityStreamDecodeResult decode_gs_parity_raw_stream(const std::filesystem::path& path) {
    StreamCursor cursor(path);
    GsParityStreamDecodeResult result;
    std::uint64_t interval_hash = kFnvOffsetBasis;
    std::uint64_t privileged_hash = kFnvOffsetBasis;

    const auto open_interval = [&result]() {
        // Hashes are sealed when the interval closes, so they start at zero.
        result.intervals.push_back({.fnv1a64 = 0U, .privileged_snapshot_hash = 0U});
    };
    // Seals the open interval's hashes so a failure keeps every complete one.
    const auto fail = [&result, &interval_hash, &privileged_hash](GsParityStreamStatus status,
                                                                  std::string detail) {
        if (!result.intervals.empty()) {
            result.intervals.back().fnv1a64 = interval_hash;
            result.intervals.back().privileged_snapshot_hash = privileged_hash;
        }
        result.status = status;
        result.detail = std::move(detail);
        return result;
    };

    while (cursor.remaining() != 0U) {
        const auto packet_type = *cursor.take(1U);
        if (packet_type == 1U) {
            if (cursor.remaining() < 1U) {
                return fail(GsParityStreamStatus::TruncatedPacket,
                            "Vsync record ended without its phase byte");
            }
            const auto phase = *cursor.take(1U);
            if (result.intervals.empty()) {
                open_interval();
            }
            result.intervals.back().fnv1a64 = interval_hash;
            result.intervals.back().privileged_snapshot_hash = privileged_hash;
            result.intervals.back().index = result.vblank_records;
            result.intervals.back().phase = phase;
            ++result.vblank_records;
            interval_hash = kFnvOffsetBasis;
            privileged_hash = kFnvOffsetBasis;
            open_interval();
            continue;
        }
        if (packet_type == 3U) {
            if (cursor.remaining() < 0x2000U) {
                return fail(GsParityStreamStatus::TruncatedPrivilegedRegisters,
                            "privileged-register snapshot ends before 0x2000 bytes");
            }
            const auto* snapshot = cursor.take(0x2000U);
            // The eight display-related privileged registers (PMODE, SMODE1,
            // DISPFB1/DISPLAY1, DISPFB2/DISPLAY2, BGCOLOR), 8 bytes each.
            const std::array<std::size_t, 8U> kObservedOffsets{0U,   16U,  96U,  112U,
                                                              128U, 144U, 160U, 224U};
            for (const auto offset : kObservedOffsets) {
                privileged_hash ^= fnv1a_bytes(snapshot + offset, 8U) * kFnvPrime;
            }
            continue;
        }
        if (packet_type != 0U) {
            return fail(GsParityStreamStatus::UnknownPacketType,
                        "packet type " + std::to_string(static_cast<int>(packet_type)) +
                            " is not part of the raw stream grammar");
        }

        if (cursor.remaining() < 5U) {
            return fail(GsParityStreamStatus::TruncatedPacket,
                        "transfer record ended before its 5-byte header");
        }
        const auto path_byte = *cursor.take(1U);
        const std::uint32_t payload_bytes = read_u32(cursor.take(4U));
        if (payload_bytes % 16U != 0U) {
            return fail(GsParityStreamStatus::TruncatedPayload,
                        "transfer payload length " + std::to_string(payload_bytes) +
                            " is not a whole quadword count");
        }
        if (payload_bytes > cursor.remaining()) {
            return fail(GsParityStreamStatus::TruncatedPayload,
                        "transfer payload of " + std::to_string(payload_bytes) +
                            " bytes extends past the end of the stream");
        }
        if (path_byte > 2U) {
            return fail(GsParityStreamStatus::InvalidGifPath,
                        "transfer carries GIF path byte " +
                            std::to_string(static_cast<int>(path_byte)));
        }
        const auto payload = static_cast<std::uint64_t>(payload_bytes);
        const auto quadword_count = payload >> 4U;
        if (result.intervals.empty()) {
            open_interval();
        }
        auto& interval = result.intervals.back();
        for (std::uint32_t quadword = 0U; quadword < quadword_count; ++quadword) {
            const auto* data = cursor.take(16U);
            guest::GsQuadword words{};
            for (std::size_t word = 0U; word < 4U; ++word) {
                words[word] = read_u32(data + word * 4U);
            }
            interval.quadwords.push_back({static_cast<guest::GifPath>(path_byte), words});
            interval_hash ^= fnv1a_bytes(data, 16U) * kFnvPrime;
            ++result.total_quadwords;
        }
    }

    if (!result.intervals.empty()) {
        result.intervals.back().fnv1a64 = interval_hash;
        result.intervals.back().privileged_snapshot_hash = privileged_hash;
    }
    result.status = GsParityStreamStatus::Complete;
    return result;
}

GsParityReplaySummary replay_gs_parity_interval(
    const std::vector<ParallelGsStreamQuadword>& quadwords, const GsParityReplayConfig& config) {
    return replay_arm_summary(quadwords, config);
}

void compare_gs_parity_images(const std::vector<std::uint8_t>& first,
                              const std::vector<std::uint8_t>& second, const std::uint32_t width,
                              const std::uint32_t height, const std::size_t maximum_reported_pixels,
                              GsParityPixelDiff& diff) {
    if (first.size() != second.size()) {
        diff.geometry_mismatch = true;
        return;
    }
    diff.width = width;
    diff.height = height;
    diff.compared_pixels = first.size() / 4U;
    diff.max_channel_delta = 0U;
    for (std::size_t offset = 0U; offset + 3U < first.size(); offset += 4U) {
        bool differing = false;
        for (unsigned int channel = 0U; channel < 4U; ++channel) {
            const auto left = first[offset + channel];
            const auto right = second[offset + channel];
            const auto delta = left > right ? static_cast<std::uint8_t>(left - right)
                                            : static_cast<std::uint8_t>(right - left);
            if (delta != 0U) {
                diff.max_channel_delta = std::max(diff.max_channel_delta, delta);
                differing = true;
            }
        }
        if (!differing) {
            continue;
        }
        ++diff.differing_pixels;
        if (diff.examples.size() < maximum_reported_pixels) {
            const auto pixel_index = offset / 4U;
            const auto x = static_cast<unsigned int>(pixel_index % width);
            const auto y = static_cast<unsigned int>(pixel_index / width);
            diff.examples.push_back(GsParityPixelExample{
                x,
                y,
                {first[offset], first[offset + 1U], first[offset + 2U], first[offset + 3U]},
                {second[offset], second[offset + 1U], second[offset + 2U], second[offset + 3U]}});
        }
    }
}

GsParityCaseReport run_gs_parity_case(const std::filesystem::path& stream_path,
                                      const GsParityReplayConfig& config,
                                      const std::size_t maximum_reported_pixels) {
    GsParityCaseReport report;
    report.stream_path = stream_path;

    const auto decoded = decode_gs_parity_raw_stream(stream_path);
    report.decode_status = decoded.status;
    report.decode_detail = decoded.detail;
    report.vblank_intervals = decoded.vblank_records;
    report.total_quadwords = decoded.total_quadwords;
    report.stream_fnv1a64 = kFnvOffsetBasis;
    for (const auto& interval : decoded.intervals) {
        report.interval_fnv1a64.push_back(interval.fnv1a64);
        report.stream_fnv1a64 = fnv1a_u64(report.stream_fnv1a64, interval.fnv1a64);
    }
    if (decoded.status != GsParityStreamStatus::Complete) {
        report.failure_detail = decoded.detail;
        return report;
    }

    std::vector<ParallelGsStreamQuadword> quadwords{};
    quadwords.reserve(decoded.total_quadwords);
    for (const auto& interval : decoded.intervals) {
        quadwords.insert(quadwords.end(), interval.quadwords.begin(), interval.quadwords.end());
    }

    report.command_arm = replay_gs_parity_interval(quadwords, config);
    report.raw_arm = replay_gs_parity_interval(quadwords, config);
    report.summaries_match = report.command_arm.quadwords == report.raw_arm.quadwords &&
                             report.command_arm.register_writes == report.raw_arm.register_writes &&
                             report.command_arm.image_transfers == report.raw_arm.image_transfers &&
                             report.command_arm.capture_diagnostics ==
                                 report.raw_arm.capture_diagnostics &&
                             report.command_arm.image_fnv1a64 == report.raw_arm.image_fnv1a64 &&
                             report.command_arm.image_status == report.raw_arm.image_status;
    if (report.command_arm.image_status == GsParityImageStatus::Rendered &&
        report.raw_arm.image_status == GsParityImageStatus::Rendered) {
        compare_gs_parity_images(report.command_arm.image_rgba, report.raw_arm.image_rgba,
                                 report.command_arm.image_width, report.command_arm.image_height,
                                 maximum_reported_pixels, report.pixel_diff);
        report.command_arm.image_rgba.clear();
        report.raw_arm.image_rgba.clear();
    }

    report.stream_green = report.summaries_match && !report.pixel_diff.geometry_mismatch &&
                          report.pixel_diff.differing_pixels == 0U;
    if (!report.stream_green && report.failure_detail.empty()) {
        if (!report.summaries_match) {
            report.failure_detail = "the two replay arms summarized the same command stream differently";
        } else if (report.pixel_diff.geometry_mismatch) {
            report.failure_detail = "the two replay arms projected different image sizes";
        } else {
            report.failure_detail =
                "the two replay arms produced " +
                std::to_string(report.pixel_diff.differing_pixels) +
                " differing pixels (max channel delta " +
                std::to_string(report.pixel_diff.max_channel_delta) + ")";
        }
    }
    return report;
}

} // namespace takedown::platform
