#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace takedown::platform {

// This is deliberately a metadata-only view of a PCSX2 GS dump. It never
// exposes the serialized GS state, GIF packet payloads, local GS memory, or a
// rendered image. The CRTC slots are enough to validate a future scanout
// implementation without turning dumps into a content-import path.
enum class Pcsx2GsDumpCompression : std::uint8_t {
    Uncompressed,
    Zstandard,
};

[[nodiscard]] const char* to_string(Pcsx2GsDumpCompression compression) noexcept;

struct Pcsx2GsDumpCrtcRegisters {
    std::uint64_t pmode{};
    std::uint64_t smode2{};
    std::uint64_t dispfb1{};
    std::uint64_t display1{};
    std::uint64_t dispfb2{};
    std::uint64_t display2{};
    std::uint64_t bgcolor{};

    bool operator==(const Pcsx2GsDumpCrtcRegisters&) const = default;
};

struct Pcsx2GsDumpGifStatistics {
    // The tag-level counters below are available only when the serialized GS
    // state uses the known v9 layout. Transfer framing remains useful for
    // other state versions, but it is not enough to safely reconstruct a GIF
    // stream that may begin in the middle of a tag.
    bool statistics_available{};

    // PCSX2 serializes internal GSState parsing cursors, not hardware GIF-path
    // provenance. Slots 0, 1, and 2 are the legacy Transfer1/Transfer2/
    // Transfer3 cursors. Slot 3 is the generic/new cursor used after modern
    // Gif_Unit arbitration has already produced an ordered GSPacket; it must
    // not be interpreted as hardware PATH1. The legacy slot-zero cursor has a
    // hardware-specific incomplete-transfer reset at each packet boundary.
    std::array<std::uint64_t, 4> transfer_packets_by_path{};
    std::array<std::uint64_t, 4> transfer_bytes_by_path{};
    // PACKED, REGLIST, IMAGE, and IMAGE2 respectively.
    std::array<std::uint64_t, 4> gif_tags_by_format{};
    // PACKED/REGLIST four-bit direct descriptor values. This is distinct from
    // A+D's effective seven-bit GS register address below.
    std::array<std::uint64_t, 16> direct_register_descriptor_counts{};
    // A+D effective seven-bit GS register-address counts. No values are
    // retained, and direct descriptors never contribute to this array.
    std::array<std::uint64_t, 128> register_writes_by_address{};
    // GS PRIM values 0 through 7. The second array attributes eligible
    // XYZF2/XYZ2 vertex/draw-kick commands to the active PRIM topology; it
    // does not represent host renderer draw calls or pixels.
    std::array<std::uint64_t, 8> primitive_state_writes{};
    std::array<std::uint64_t, 8> draw_kicks_by_primitive{};
    std::uint64_t image_payload_quadwords{};
    std::uint64_t draw_kicks{};
    std::uint64_t draw_kicks_without_primitive{};
    std::uint64_t zero_loop_tags{};
    std::uint64_t adc_suppressed_kicks{};
    std::uint64_t nonkicking_xyz_writes{};
    std::uint64_t path0_incomplete_resets{};
    std::uint64_t unsupported_transfer_path_packets{};
    std::uint64_t incomplete_paths{};
};

struct Pcsx2GsDumpInspection {
    Pcsx2GsDumpCompression compression{Pcsx2GsDumpCompression::Uncompressed};
    std::size_t input_bytes{};
    std::size_t decoded_bytes{};
    std::uint32_t state_version{};
    std::uint32_t state_bytes{};
    std::size_t transfer_packets{};
    std::uint64_t skipped_transfer_bytes{};
    std::size_t vsync_packets{};
    std::size_t fifo_read_packets{};
    std::size_t privileged_register_packets{};
    Pcsx2GsDumpCrtcRegisters initial_crtc{};
    Pcsx2GsDumpCrtcRegisters final_crtc{};
    bool final_crtc_came_from_packet{};
    Pcsx2GsDumpGifStatistics gif{};
};

// Accepts a PCSX2 .gs stream or a Zstandard-compressed .gs.zst stream. Input
// is bounded and decompressed only in process memory; the returned structure
// contains display metadata only.
[[nodiscard]] Pcsx2GsDumpInspection
inspect_pcsx2_gs_dump_bytes(std::span<const std::uint8_t> input);

// Convenience file wrapper for the local-analysis CLI. It does not create
// sidecar files or write decompressed content to disk.
[[nodiscard]] Pcsx2GsDumpInspection inspect_pcsx2_gs_dump(const std::filesystem::path& path);

} // namespace takedown::platform
