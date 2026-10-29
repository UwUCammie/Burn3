#pragma once

#include "takedown/platform/parallel_gs_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace takedown::platform {

// One completed modeled VBlank interval of a paraLLEl-GS raw stream. The
// project's writer emits one Vsync packet per modeled VBlank; this harness
// attributes every decoded quadword to the interval it was submitted in so a
// corpus can be compared one guest frame at a time.
struct GsParityStreamInterval {
    std::uint64_t index{};      // 0-based count of preceding Vsync records.
    std::uint8_t phase{};       // Vsync phase byte (0 or 1).
    // Post-arbitration GIF quadwords submitted after the previous Vsync and
    // before this one, with their hardware path identity exactly as encoded.
    std::vector<ParallelGsStreamQuadword> quadwords{};
    // FNV-1a over the quadword bytes in submission order. Two intervals with
    // equal hashes carried identical command traffic.
    std::uint64_t fnv1a64{0xcbf29ce484222325ULL};
    // FNV-1a folded over the eight observed privileged-display slots of the
    // snapshot in this interval (zero when the stream carried no snapshot).
    std::uint64_t privileged_snapshot_hash{0xcbf29ce484222325ULL};
};

enum class GsParityStreamStatus : std::uint8_t {
    Complete,
    TruncatedPacket,
    TruncatedPayload,
    TruncatedPrivilegedRegisters,
    UnknownPacketType,
    InvalidGifPath,
};

struct GsParityStreamDecodeResult {
    GsParityStreamStatus status{GsParityStreamStatus::Complete};
    std::string detail{};
    std::vector<GsParityStreamInterval> intervals{};
    std::uint64_t total_quadwords{};
    std::uint64_t vblank_records{};
};

[[nodiscard]] const char* to_string(GsParityStreamStatus status) noexcept;

// Decodes the headerless packet grammar produced by ParallelGsRawStreamWriter:
//   Transfer:      u8(0), u8(path), u32(payload bytes), 16-byte GIF QWs
//   Vsync:         u8(1), u8(phase)
//   PrivRegisters: u8(3), one 0x2000-byte GS privileged-register snapshot
// The grammar is byte-aligned and self-delimiting, so no writer state is
// needed to read it back. A truncated or malformed stream returns the failure
// status together with every completely decoded interval.
[[nodiscard]] GsParityStreamDecodeResult
decode_gs_parity_raw_stream(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// Two-arm replay harness
// ---------------------------------------------------------------------------

enum class GsParityImageStatus : std::uint8_t {
    Rendered,
    ProjectionFailed,
    NoQuadwords,
};

[[nodiscard]] const char* to_string(GsParityImageStatus status) noexcept;

// Summary of replaying one command interval through the software GS renderer.
// The image is a projected view of the renderer's swizzled local memory after
// the final command of the interval, read as tightly packed RGBA8. A raw
// paraLLEl-GS stream does not carry CRTC display rectangles, so the harness
// projects an explicit surface instead of deriving one; two arms projected
// identically are pixel-comparable even when no privileged registers exist.
struct GsParityReplaySummary {
    std::uint64_t quadwords{};
    std::uint64_t register_writes{};
    std::uint64_t image_transfers{};
    std::uint64_t capture_diagnostics{};
    // FNV-1a over every decoded IMAGE payload (sequence, size, bytes).
    std::uint64_t image_fnv1a64{0xcbf29ce484222325ULL};
    GsParityImageStatus image_status{GsParityImageStatus::NoQuadwords};
    std::uint32_t image_width{};
    std::uint32_t image_height{};
    // FNV-1a over the projected RGBA bytes.
    std::uint64_t image_fnv1a64_pixels{0xcbf29ce484222325ULL};
    // Tightly packed RGBA8 projection (empty unless image_status == Rendered).
    std::vector<std::uint8_t> image_rgba{};
};

struct GsParityReplayConfig {
    // Projection rectangle in the surface's own pixel coordinates. The default
    // is the whole 640x448 NTSC framebuffer region at surface origin.
    std::uint32_t surface_base_pointer{};
    std::uint32_t surface_buffer_width{640U};
    std::uint32_t surface_pixel_storage_mode{0U}; // PSMCT32
    std::uint32_t x{0U};
    std::uint32_t y{0U};
    std::uint32_t width{640U};
    std::uint32_t height{448U};
};

// Replays one decoded command interval through one independent GsCapture +
// GsLiveRenderer pipeline (an own-thread-independent software-rasterizer arm)
// and returns its summary. Identical command input yields identical summaries.
[[nodiscard]] GsParityReplaySummary
replay_gs_parity_interval(const std::vector<ParallelGsStreamQuadword>& quadwords,
                          const GsParityReplayConfig& config);

struct GsParityPixelExample {
    std::uint32_t x{};
    std::uint32_t y{};
    std::array<std::uint8_t, 4U> first_rgba{};
    std::array<std::uint8_t, 4U> second_rgba{};
};

// Bounded pixel-diff between two RGBA8 projections. Geometry mismatches are
// reported without comparing pixels; matching geometry reports the differing
// pixel count, the maximum per-channel delta, and up to
// maximum_reported_pixels concrete examples.
struct GsParityPixelDiff {
    bool geometry_mismatch{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t compared_pixels{};
    std::uint64_t differing_pixels{};
    std::uint8_t max_channel_delta{};
    std::vector<GsParityPixelExample> examples{};
};

void compare_gs_parity_images(const std::vector<std::uint8_t>& first,
                              const std::vector<std::uint8_t>& second,
                              std::uint32_t width, std::uint32_t height,
                              std::size_t maximum_reported_pixels,
                              GsParityPixelDiff& diff);

// A whole-corpus case: one raw stream replayed on both arms. The command arm
// feeds GsCapture's decoded observer boundary; the raw arm feeds the identical
// quadwords through the GsRawGifObserver boundary so the transport seam the
// Vulkan consumer reads is exercised too. Green requires equal command counts,
// equal summaries, and a bounded or absent pixel diff.
struct GsParityCaseReport {
    std::filesystem::path stream_path{};
    GsParityStreamStatus decode_status{GsParityStreamStatus::Complete};
    std::string decode_detail{};
    std::uint64_t vblank_intervals{};
    std::uint64_t total_quadwords{};
    // Aggregate FNV-1a over every decoded quadword in stream order.
    std::uint64_t stream_fnv1a64{0xcbf29ce484222325ULL};
    // Per-interval FNV stream (one entry per decoded interval).
    std::vector<std::uint64_t> interval_fnv1a64{};
    GsParityReplaySummary command_arm{};
    GsParityReplaySummary raw_arm{};
    GsParityPixelDiff pixel_diff{};
    bool summaries_match{};
    bool stream_green{};
    std::string failure_detail{};
};

// Runs the two-arm comparison for one raw stream. maximum_reported_pixels
// bounds the per-case pixel-diff examples kept in memory.
[[nodiscard]] GsParityCaseReport
run_gs_parity_case(const std::filesystem::path& stream_path,
                   const GsParityReplayConfig& config,
                   std::size_t maximum_reported_pixels = 8U);

} // namespace takedown::platform
