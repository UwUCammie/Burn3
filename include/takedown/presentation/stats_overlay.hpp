#pragma once

// Host-side on-screen stats overlay for the Burnout 3 recompilation runtime.
//
// This is a developer/diagnostic aid ONLY. It composites a small FPS / sprite
// / triangle / pixel readout into the presented window frame, independent of
// and invisible to the guest game logic and the retail framebuffer the guest
// owns. The overlay draws directly into the host's private RGBA copy of each
// presented frame using a built-in 5x7 bitmap pixel font; there are no font
// files, no SDL_ttf, and no new link dependencies (the runtime links only SDL3
// plus clean-room sources).
//
// Frame layout (top-left, buffer coordinates; column 0 is the left edge of the
// presented window, row 0 its top):
//
//     FPS: VIS 57.3 AVG3S 56.8 GUEST 59.9 AVG3S 58.1 PRESENT 59.9 AVG3S 59.7
//     SPR: SOLID 13498 SHAPED 12980 TEXTURED 51592
//     TRI: SOLID 12842 TEXTURED 108087
//     PIX: 187725279 WRITTEN 813447 COVERED
//     UPLOADS: 83748 KICKS: 507037 DEFERRED: 1
//
// Each row occupies kStatsLineStride pixel rows (the 7 font rows + 2 spacer
// rows) and every glyph cell is kStatsCellStride columns wide (5 font columns
// + 1 spacer column). The panel rectangle spans the widest line + a pad.

#include "takedown/presentation/rolling_fps.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takedown::presentation {

// Metrics of the built-in 5x7 bitmap font (glyph cell + intercell spacing).
constexpr std::uint32_t kStatsGlyphWidth = 5U;
constexpr std::uint32_t kStatsGlyphHeight = 7U;
constexpr std::uint32_t kStatsCellStride = 6U;  // 5 columns + 1 spacer column
constexpr std::uint32_t kStatsLineStride = 9U;  // 7 rows + 2 spacer rows

// Renders a monospace string into the RGBA buffer at ((x, y). (x and y name
// the top-left of the first glyph's cell.) Only the pixel cells a glyph's
// strokes occupy are rewritten; all other cells -- the inter-glyph spacer
// column, the space character, and any cell outside the text -- are left
// exactly as they were. Drawing is clipped to the buffer extent. The four
// channels of every ink cell are set to (red, green, blue, 0xff); the buffer
// alpha is preserved elsewhere.
void draw_stats_text(std::span<std::uint8_t> rgba, std::uint32_t width,
                     std::uint32_t height, std::uint32_t x, std::uint32_t y,
                     std::string_view text, std::uint8_t red, std::uint8_t green,
                     std::uint8_t blue) noexcept;

// The numbers shown by the overlay. All come from presentable host telemetry
// (GsLiveRendererStatistics) plus the host-measured byte-changing scanout rate.
struct StatsOverlayValues {
    // VIS counts byte-changing scanouts, GUEST counts modeled guest VBlank
    // boundaries completed per host second, and PRESENT counts actual SDL
    // presentation calls. The matching averages cover the
    // last three seconds of host time, including silence.
    double visual_fps{0.0};
    double visual_average_fps{0.0};
    double guest_fps{0.0};
    double guest_average_fps{0.0};
    double present_fps{0.0};
    double present_average_fps{0.0};
    std::uint64_t solid_sprites{};
    std::uint64_t presentation_sprites{};
    std::uint64_t textured_sprites{};
    std::uint64_t solid_triangles{};
    std::uint64_t textured_triangles{};
    std::uint64_t written_pixels{};
    std::uint64_t covered_pixels{};
    std::uint64_t image_transfers_applied{};
    std::uint64_t drawable_kicks{};
    std::uint64_t textured_primitives_deferred{};
};

// Composites the stats overlay into the top-left of the RGBA buffer: a dark
// panel is stamped first, then the labelled lines are drawn over it in a
// readable ink colour. The buffer is the host's private copy of the presented
// frame; mutating it never reaches guest memory. No-op when the buffer is
// empty or its dimensions are inconsistent.
void composite_stats_overlay(std::span<std::uint8_t> rgba, std::uint32_t width,
                             std::uint32_t height, const StatsOverlayValues& values) noexcept;

// Formats a measured frame rate with one decimal place. Returns "0.0" when
// elapsed_seconds is not strictly positive (which also covers the empty/zero
// case without any division). The frames count is only a guard, never the
// numerator of an unguarded expression.
std::string format_measured_fps(std::uint64_t frames, double elapsed_seconds) noexcept;

// A tiny host-side sliding-window tracker for newly displayed scanouts. The
// timestamped fps(now) overload includes silence since the last sample, so a
// frozen visual does not retain an old healthy rate forever. Both overloads
// guard against sparse/non-increasing clocks and never divide by zero.
class HostFpsTracker {
  public:
    // Starts the rolling average at an explicit host timestamp. This
    // is useful when several differently-defined frame streams must share one
    // denominator. It records no frame and clears the rolling window.
    void start_session(std::uint64_t now_nanoseconds) noexcept;
    void note_frame(std::uint64_t now_nanoseconds) noexcept;
    [[nodiscard]] double fps() const noexcept;
    [[nodiscard]] double fps(std::uint64_t now_nanoseconds) const noexcept;
    [[nodiscard]] double average_fps() const noexcept;
    [[nodiscard]] double average_fps(std::uint64_t now_nanoseconds) const noexcept;
    [[nodiscard]] std::uint64_t total_frames() const noexcept;
    void reset() noexcept;

  private:
    static constexpr std::size_t kWindowSize = 60U;
    std::array<std::uint64_t, kWindowSize> samples_{};
    std::size_t count_{0U};
    std::size_t write_index_{0U};
    RollingFpsAverage average_;
    std::uint64_t total_frames_{0U};
};

} // namespace takedown::presentation
