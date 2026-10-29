#pragma once

#include "takedown/guest/gs_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::guest {

// This is a deliberately diagnostic representation of GS local memory.  It
// uses linear row-major addressing so captured host-to-local transfers can be
// inspected before a real GS VRAM swizzle implementation exists.  It is not a
// PS2 GS VRAM emulator and must not be used as a renderer correctness source.
inline constexpr std::size_t kGsDiagnosticVramRawBytes = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kGsDiagnosticCoordinateLimit = 2048U;

enum class GsDiagnosticVramStatus : std::uint8_t {
    Applied,
    UnsupportedPixelStorageMode,
    ZeroDestinationBufferWidth,
    InvalidGeometry,
    InvalidPayload,
    AddressOutOfBounds,
};

[[nodiscard]] const char* to_string(GsDiagnosticVramStatus status) noexcept;

// A layout is deliberately keyed only by the fields that define its diagnostic
// destination surface.  The position and size belong to individual transfers.
struct GsDiagnosticVramLayoutKey {
    std::uint32_t destination_base_pointer{};
    std::uint32_t destination_buffer_width{};
    std::uint32_t destination_psm{};

    bool operator==(const GsDiagnosticVramLayoutKey&) const = default;
};

struct GsDiagnosticVramLayout {
    GsDiagnosticVramLayoutKey key{};
    std::uint32_t dirty_x{};
    std::uint32_t dirty_y{};
    std::uint32_t dirty_width{};
    std::uint32_t dirty_height{};
    std::uint64_t last_sequence{};
    std::uint64_t apply_count{};

    bool operator==(const GsDiagnosticVramLayout&) const = default;
};

struct GsDiagnosticVramApplyResult {
    GsDiagnosticVramStatus status{GsDiagnosticVramStatus::InvalidGeometry};
    std::size_t bytes_written{};
    // For AddressOutOfBounds this is the first diagnostic linear byte address
    // that did not fit in the 4 MiB backing.  It is zero for other results.
    std::uint64_t detail{};

    [[nodiscard]] bool applied() const noexcept {
        return status == GsDiagnosticVramStatus::Applied;
    }
};

struct GsDiagnosticVramDiagnostic {
    GsDiagnosticVramStatus status{GsDiagnosticVramStatus::InvalidGeometry};
    std::uint64_t sequence{};
    GsDiagnosticVramLayoutKey key{};
    std::uint64_t detail{};

    bool operator==(const GsDiagnosticVramDiagnostic&) const = default;
};

// The snapshot contains tightly packed RGBA8 pixels.  Its rectangle is the
// diagnostic layout's bounding dirty region; a union that crosses the 2048
// coordinate seam may therefore be larger than the individual transfers.
struct GsDiagnosticVramSnapshot {
    GsDiagnosticVramLayout layout{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba{};

    bool operator==(const GsDiagnosticVramSnapshot&) const = default;
};

class GsDiagnosticVram {
  public:
    GsDiagnosticVram();

    // Applies a supported captured host-to-local transfer after validating the
    // whole source and preflighting every destination address.  A rejected
    // transfer changes neither backing storage nor layout state.
    [[nodiscard]] GsDiagnosticVramApplyResult apply_host_to_local(const GsImageTransfer& transfer);

    [[nodiscard]] std::span<const std::uint8_t> raw_backing() const noexcept;
    [[nodiscard]] std::span<const GsDiagnosticVramLayout> layouts() const noexcept;
    [[nodiscard]] std::span<const GsDiagnosticVramDiagnostic> diagnostics() const noexcept;
    [[nodiscard]] const GsDiagnosticVramLayout* latest_layout() const noexcept;

    [[nodiscard]] std::optional<GsDiagnosticVramSnapshot>
    snapshot_for(const GsDiagnosticVramLayoutKey& key) const;
    [[nodiscard]] std::optional<GsDiagnosticVramSnapshot> latest_snapshot() const;

    void clear() noexcept;

  private:
    std::vector<std::uint8_t> raw_backing_{};
    std::vector<GsDiagnosticVramLayout> layouts_{};
    std::vector<GsDiagnosticVramDiagnostic> diagnostics_{};
    std::optional<std::size_t> latest_layout_index_{};
};

} // namespace takedown::guest
