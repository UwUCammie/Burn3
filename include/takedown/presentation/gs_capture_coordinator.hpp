#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace takedown::presentation {

// F8 captures are fenced to modeled guest VBlanks.  Semantic-only capture
// does not require a local renderer; a renderer merely adds an exact scanout
// and VRAM snapshot at the same boundary.
enum class GsCaptureBoundaryReadiness : std::uint8_t {
    MissingTarget,
    WaitingForBoundary,
    WaitingForSnapshot,
    Ready,
    MissedBoundary,
};

[[nodiscard]] constexpr std::optional<std::uint64_t> select_gs_capture_boundary(
    const bool semantic_last_mode_enabled, const std::uint64_t completed_vblanks,
    const bool frame_step_paused) noexcept {
    if (!semantic_last_mode_enabled) {
        return std::nullopt;
    }
    // Boundary zero has no immediately preceding completed interval.  Even if
    // a debugger is armed unusually early, wait for boundary one rather than
    // manufacturing zero as an error sentinel.
    if (frame_step_paused && completed_vblanks != 0U) {
        return completed_vblanks;
    }
    if (completed_vblanks == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return completed_vblanks + 1U;
}

[[nodiscard]] constexpr GsCaptureBoundaryReadiness evaluate_gs_capture_boundary(
    const std::optional<std::uint64_t> target_vblank,
    const std::uint64_t completed_vblanks, const bool local_renderer_enabled,
    const bool snapshot_completed, const std::uint64_t snapshot_vblank) noexcept {
    if (!target_vblank.has_value()) {
        return GsCaptureBoundaryReadiness::MissingTarget;
    }
    if (completed_vblanks < *target_vblank) {
        return GsCaptureBoundaryReadiness::WaitingForBoundary;
    }
    if (completed_vblanks > *target_vblank) {
        return GsCaptureBoundaryReadiness::MissedBoundary;
    }
    if (!local_renderer_enabled) {
        return GsCaptureBoundaryReadiness::Ready;
    }
    if (!snapshot_completed || snapshot_vblank < *target_vblank) {
        return GsCaptureBoundaryReadiness::WaitingForSnapshot;
    }
    return snapshot_vblank == *target_vblank
               ? GsCaptureBoundaryReadiness::Ready
               : GsCaptureBoundaryReadiness::MissedBoundary;
}

} // namespace takedown::presentation
