#pragma once

#include "takedown/guest/gs_local_memory.hpp"
#include "takedown/guest/gs_privileged_display.hpp"

#include <cstdint>
#include <vector>

namespace takedown::presentation {

// The first real scanout boundary: one GS display circuit backed
// by project-owned swizzled GS local memory. Primitive rendering and CRTC
// circuit composition intentionally remain outside this small foundation.
enum class GsScanoutPlanStatus : std::uint8_t {
    Ready,
    MissingPrivilegedRegisters,
    NoEnabledCircuit,
    DualCircuitCompositionUnsupported,
    InterlacedSourceRequiresConversion,
    UnsupportedPixelStorageMode,
    ZeroFramebufferWidth,
    InvalidOutputGeometry,
};

[[nodiscard]] const char* to_string(GsScanoutPlanStatus status) noexcept;

struct GsScanoutPlan {
    GsScanoutPlanStatus status{GsScanoutPlanStatus::MissingPrivilegedRegisters};
    std::uint32_t circuit{};
    guest::GsLocalMemorySurface source_surface{};
    std::uint32_t source_x{};
    std::uint32_t source_y{};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::uint32_t crtc_width{};
    std::uint32_t crtc_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t horizontal_repeat{};
    std::uint32_t vertical_repeat{};
    // An interlaced CRTC never reaches the host as fields. The first host
    // conversion reads the complete rendered framebuffer and presents it as
    // one progressive frame, matching modern-display policy.
    bool progressive_frame_extraction{};

    [[nodiscard]] bool ready() const noexcept { return status == GsScanoutPlanStatus::Ready; }
};

enum class GsScanoutRenderStatus : std::uint8_t {
    Rendered,
    InvalidPlan,
    FrameTooLarge,
};

[[nodiscard]] const char* to_string(GsScanoutRenderStatus status) noexcept;

struct GsScanoutFrame {
    GsScanoutRenderStatus status{GsScanoutRenderStatus::InvalidPlan};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba{};

    [[nodiscard]] bool rendered() const noexcept {
        return status == GsScanoutRenderStatus::Rendered;
    }
};

// Derives a bounded progressive host-frame plan from observed privileged GS
// registers. DISPFB.FBP is converted from its 8 KiB hardware unit to the
// 256-byte block unit used by GsLocalMemory. It never derives host timing or
// submits a field. Interlaced CRTC timing is converted by extracting the full
// rendered framebuffer into a progressive host frame.
[[nodiscard]] GsScanoutPlan
plan_gs_scanout(const guest::GsPrivilegedDisplayState& display_state) noexcept;

// Reads only the selected plan's logical visible source region. MAGH/MAGV are
// CRTC pixel-clock repeats, not extra host content, so the modern host frame
// contains one RGBA8 pixel per source pixel.
[[nodiscard]] GsScanoutFrame render_gs_scanout(const guest::GsLocalMemory& memory,
                                               const GsScanoutPlan& plan);

} // namespace takedown::presentation
