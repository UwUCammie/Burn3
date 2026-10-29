#include "takedown/presentation/stats_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>
#include <string>

namespace takedown::presentation {

namespace {

struct GlyphRows {
    std::array<const char*, 7> rows;
};

// 5x7 glyph rows; '#' marks an ink cell and any other character is blank.
GlyphRows glyph_rows(const char character) noexcept {
    GlyphRows rows;
    switch (character) {
    case '0':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case '1':
        rows.rows[0] = "..#..";
        rows.rows[1] = ".##..";
        rows.rows[2] = "..#..";
        rows.rows[3] = "..#..";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".###.";
        break;
    case '2':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "....#";
        rows.rows[3] = "...#.";
        rows.rows[4] = "..#..";
        rows.rows[5] = ".#...";
        rows.rows[6] = "#####";
        break;
    case '3':
        rows.rows[0] = ".####";
        rows.rows[1] = "#...#";
        rows.rows[2] = "....#";
        rows.rows[3] = "...#.";
        rows.rows[4] = "....#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".####";
        break;
    case '4':
        rows.rows[0] = "..#..";
        rows.rows[1] = ".##..";
        rows.rows[2] = "#.#..";
        rows.rows[3] = "..#..";
        rows.rows[4] = "#####";
        rows.rows[5] = "..#..";
        rows.rows[6] = "..#..";
        break;
    case '5':
        rows.rows[0] = "#####";
        rows.rows[1] = "#....";
        rows.rows[2] = "#....";
        rows.rows[3] = "#####";
        rows.rows[4] = "....#";
        rows.rows[5] = "....#";
        rows.rows[6] = "#####";
        break;
    case '6':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = ".#..#";
        rows.rows[6] = "..##.";
        break;
    case '7':
        rows.rows[0] = "#####";
        rows.rows[1] = "....#";
        rows.rows[2] = "...#.";
        rows.rows[3] = "..#..";
        rows.rows[4] = ".#...";
        rows.rows[5] = ".#...";
        rows.rows[6] = ".#...";
        break;
    case '8':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = ".###.";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case '9':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = ".###.";
        rows.rows[4] = "....#";
        rows.rows[5] = "....#";
        rows.rows[6] = ".####";
        break;
    case 'A':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#####";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = "#...#";
        break;
    case 'B':
        rows.rows[0] = "####.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "####.";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = "####.";
        break;
    case 'C':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#....";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case 'D':
        rows.rows[0] = "####.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = "####.";
        break;
    case 'E':
        rows.rows[0] = "#####";
        rows.rows[1] = "#....";
        rows.rows[2] = "#....";
        rows.rows[3] = "#####";
        rows.rows[4] = "#....";
        rows.rows[5] = "#....";
        rows.rows[6] = "#####";
        break;
    case 'F':
        rows.rows[0] = "#####";
        rows.rows[1] = "#....";
        rows.rows[2] = "#....";
        rows.rows[3] = "####.";
        rows.rows[4] = "#....";
        rows.rows[5] = "#....";
        rows.rows[6] = "#....";
        break;
    case 'G':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = ".#..#";
        rows.rows[5] = "..##.";
        rows.rows[6] = "..###";
        break;
    case 'H':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#####";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = "#...#";
        break;
    case 'I':
        rows.rows[0] = ".###.";
        rows.rows[1] = "..#..";
        rows.rows[2] = "..#..";
        rows.rows[3] = "..#..";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".###.";
        break;
    case 'J':
        rows.rows[0] = ".###.";
        rows.rows[1] = "....#";
        rows.rows[2] = "....#";
        rows.rows[3] = "....#";
        rows.rows[4] = "....#";
        rows.rows[5] = "....#";
        rows.rows[6] = ".####";
        break;
    case 'K':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#..#.";
        rows.rows[2] = "#.#..";
        rows.rows[3] = "##...";
        rows.rows[4] = "#.#..";
        rows.rows[5] = "#..#.";
        rows.rows[6] = "#...#";
        break;
    case 'L':
        rows.rows[0] = "#....";
        rows.rows[1] = "#....";
        rows.rows[2] = "#....";
        rows.rows[3] = "#....";
        rows.rows[4] = "#....";
        rows.rows[5] = "#....";
        rows.rows[6] = "#####";
        break;
    case 'M':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "##.##";
        rows.rows[3] = "#.#.#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = "#...#";
        break;
    case 'N':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "##..#";
        rows.rows[3] = "#.#.#";
        rows.rows[4] = "#..##";
        rows.rows[5] = "#...#";
        rows.rows[6] = "#...#";
        break;
    case 'O':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case 'P':
        rows.rows[0] = "####.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "####.";
        rows.rows[4] = "#....";
        rows.rows[5] = "#....";
        rows.rows[6] = "#....";
        break;
    case 'Q':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".####";
        break;
    case 'R':
        rows.rows[0] = "####.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "####.";
        rows.rows[4] = "#.#..";
        rows.rows[5] = "#..#.";
        rows.rows[6] = "#...#";
        break;
    case 'S':
        rows.rows[0] = ".###.";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#....";
        rows.rows[3] = ".###.";
        rows.rows[4] = "....#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case 'T':
        rows.rows[0] = "#####";
        rows.rows[1] = "..#..";
        rows.rows[2] = "..#..";
        rows.rows[3] = "..#..";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = "..#..";
        break;
    case 'U':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = "#...#";
        rows.rows[5] = "#...#";
        rows.rows[6] = ".###.";
        break;
    case 'V':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#...#";
        rows.rows[3] = "#...#";
        rows.rows[4] = ".#.#.";
        rows.rows[5] = "..#..";
        rows.rows[6] = "..#..";
        break;
    case 'W':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = "#.#.#";
        rows.rows[3] = "##.##";
        rows.rows[4] = "##.##";
        rows.rows[5] = "#.#.#";
        rows.rows[6] = "#...#";
        break;
    case 'X':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = ".#.#.";
        rows.rows[3] = "..#..";
        rows.rows[4] = ".#.#.";
        rows.rows[5] = "#...#";
        rows.rows[6] = "#...#";
        break;
    case 'Y':
        rows.rows[0] = "#...#";
        rows.rows[1] = "#...#";
        rows.rows[2] = ".#.#.";
        rows.rows[3] = "..#..";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = "..#..";
        break;
    case 'Z':
        rows.rows[0] = "#####";
        rows.rows[1] = "...#.";
        rows.rows[2] = "..#..";
        rows.rows[3] = ".#...";
        rows.rows[4] = ".#...";
        rows.rows[5] = "..#..";
        rows.rows[6] = "...#.";
        break;
    case ':':
        rows.rows[0] = ".....";
        rows.rows[1] = "..#..";
        rows.rows[2] = "..#..";
        rows.rows[3] = ".....";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".....";
        break;
    case '/':
        rows.rows[0] = "....#";
        rows.rows[1] = "...#.";
        rows.rows[2] = "..#..";
        rows.rows[3] = ".#...";
        rows.rows[4] = "#....";
        rows.rows[5] = ".....";
        rows.rows[6] = ".....";
        break;
    case '-':
        rows.rows[0] = ".....";
        rows.rows[1] = ".....";
        rows.rows[2] = ".....";
        rows.rows[3] = "#####";
        rows.rows[4] = ".....";
        rows.rows[5] = ".....";
        rows.rows[6] = ".....";
        break;
    case '_':
        rows.rows[0] = ".....";
        rows.rows[1] = ".....";
        rows.rows[2] = ".....";
        rows.rows[3] = ".....";
        rows.rows[4] = ".....";
        rows.rows[5] = ".....";
        rows.rows[6] = "#####";
        break;
    case '.':
        rows.rows[0] = ".....";
        rows.rows[1] = ".....";
        rows.rows[2] = ".....";
        rows.rows[3] = ".....";
        rows.rows[4] = ".....";
        rows.rows[5] = "..#..";
        rows.rows[6] = "..#..";
        break;
    case '%':
        rows.rows[0] = ".....";
        rows.rows[1] = "#...#";
        rows.rows[2] = ".#.#.";
        rows.rows[3] = "..#..";
        rows.rows[4] = ".....";
        rows.rows[5] = ".....";
        rows.rows[6] = ".....";
        break;
    case '<':
        rows.rows[0] = "...#.";
        rows.rows[1] = "..#..";
        rows.rows[2] = ".#...";
        rows.rows[3] = "#....";
        rows.rows[4] = ".#...";
        rows.rows[5] = "..#..";
        rows.rows[6] = "...#.";
        break;
    case '>':
        rows.rows[0] = ".#...";
        rows.rows[1] = "..#..";
        rows.rows[2] = "...#.";
        rows.rows[3] = "....#";
        rows.rows[4] = "...#.";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".#...";
        break;
    case '(':
        rows.rows[0] = "...#.";
        rows.rows[1] = "..#..";
        rows.rows[2] = ".#...";
        rows.rows[3] = ".#...";
        rows.rows[4] = ".#...";
        rows.rows[5] = "..#..";
        rows.rows[6] = "...#.";
        break;
    case ')':
        rows.rows[0] = ".#...";
        rows.rows[1] = "..#..";
        rows.rows[2] = "...#.";
        rows.rows[3] = "...#.";
        rows.rows[4] = "...#.";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".#...";
        break;
    case ',':
        rows.rows[0] = ".....";
        rows.rows[1] = ".....";
        rows.rows[2] = ".....";
        rows.rows[3] = ".....";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".#...";
        break;
    case '+':
        rows.rows[0] = ".....";
        rows.rows[1] = "..#..";
        rows.rows[2] = "..#..";
        rows.rows[3] = "#####";
        rows.rows[4] = "..#..";
        rows.rows[5] = "..#..";
        rows.rows[6] = ".....";
        break;
    default:
        rows.rows[0] = ".....";
        rows.rows[1] = ".....";
        rows.rows[2] = ".....";
        rows.rows[3] = ".....";
        rows.rows[4] = ".....";
        rows.rows[5] = ".....";
        rows.rows[6] = ".....";
        break;
    }
    return rows;
}

} // namespace


namespace {

// Formats one decimal place without any locale-dependent state.
std::string one_decimal(const double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

} // namespace

void draw_stats_text(const std::span<std::uint8_t> rgba, const std::uint32_t width,
                     const std::uint32_t height, const std::uint32_t x, const std::uint32_t y,
                     const std::string_view text, const std::uint8_t red,
                     const std::uint8_t green, const std::uint8_t blue) noexcept {
    std::uint32_t cursor_x = x;
    for (const char character : text) {
        const GlyphRows glyph = glyph_rows(character);
        for (std::uint32_t row = 0U; row != 7U; ++row) {
            const char* const row_cells = glyph.rows[row];
            for (std::uint32_t column = 0U; column != 5U; ++column) {
                if (row_cells[column] != '#') {
                    continue;
                }
                if (cursor_x + column >= width || row + y >= height) {
                    continue;
                }
                const std::size_t offset = (static_cast<std::size_t>(row + y) * width + (cursor_x + column)) * 4U;
                rgba[offset] = red;
                rgba[offset + 1U] = green;
                rgba[offset + 2U] = blue;
                rgba[offset + 3U] = 255U;
            }
        }
        cursor_x += 6U;
    }
}

std::string format_measured_fps(const std::uint64_t frames, const double elapsed_seconds) noexcept {
    if (frames == 0U || !(elapsed_seconds > 0.0)) {
        return "0.0";
    }
    const double fps = static_cast<double>(frames) / elapsed_seconds;
    return one_decimal(fps);
}

void composite_stats_overlay(const std::span<std::uint8_t> rgba, const std::uint32_t width,
                             const std::uint32_t height, const StatsOverlayValues& values) noexcept {
    if (width == 0U || height == 0U) {
        return;
    }
    if (rgba.size() / 4U < static_cast<std::size_t>(width) * height) {
        return;
    }
    const std::string fps_line = "FPS: VIS " + one_decimal(values.visual_fps) + " AVG3S " +
                                 one_decimal(values.visual_average_fps) + " GUEST " +
                                 one_decimal(values.guest_fps) + " AVG3S " +
                                 one_decimal(values.guest_average_fps) + " PRESENT " +
                                 one_decimal(values.present_fps) + " AVG3S " +
                                 one_decimal(values.present_average_fps);
    const std::string sprites_line = "SPR: SOLID " + std::to_string(values.solid_sprites) +
                                     " SHAPED " + std::to_string(values.presentation_sprites) +
                                     " TEXTURED " + std::to_string(values.textured_sprites);
    const std::string triangles_line = "TRI: SOLID " + std::to_string(values.solid_triangles) +
                                       " TEXTURED " + std::to_string(values.textured_triangles);
    const std::string pixels_line = "PIX: " + std::to_string(values.covered_pixels) + " WRITTEN " +
                                    std::to_string(values.written_pixels) + " COVERED";
    const std::string uploads_line = "UPLOADS: " + std::to_string(values.image_transfers_applied) +
                                     " KICKS: " + std::to_string(values.drawable_kicks) +
                                     " DEFERRED: " + std::to_string(values.textured_primitives_deferred);
    std::size_t widest_line = 0U;
    for (const std::string& line : {fps_line, sprites_line, triangles_line, pixels_line, uploads_line}) {
        widest_line = std::max(widest_line, line.size());
    }
    const std::uint32_t panel_width = static_cast<std::uint32_t>(widest_line) * 6U + 15U;
    for (std::uint32_t y = 8U; y < 68U; ++y) {
        for (std::uint32_t x = 8U; x < panel_width + 8U; ++x) {
            if (width <= x || height <= y) {
                continue;
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[offset] = 10U;
            rgba[offset + 1U] = 14U;
            rgba[offset + 2U] = 20U;
            rgba[offset + 3U] = 255U;
        }
    }
    draw_stats_text(rgba, width, height, 12U, 12U, fps_line, 111U, 224U, 141U);
    draw_stats_text(rgba, width, height, 12U, 21U, sprites_line, 111U, 224U, 141U);
    draw_stats_text(rgba, width, height, 12U, 30U, triangles_line, 111U, 224U, 141U);
    draw_stats_text(rgba, width, height, 12U, 39U, pixels_line, 111U, 224U, 141U);
    draw_stats_text(rgba, width, height, 12U, 48U, uploads_line, 111U, 224U, 141U);
}

void HostFpsTracker::start_session(const std::uint64_t now_nanoseconds) noexcept {
    reset();
    average_.start(now_nanoseconds);
}

void HostFpsTracker::note_frame(const std::uint64_t now_nanoseconds) noexcept {
    average_.note_frame(now_nanoseconds);
    if (total_frames_ != 18446744073709551615U) {
        ++total_frames_;
    }
    samples_[write_index_] = now_nanoseconds;
    write_index_ = (write_index_ + 1U) % kWindowSize;
    if (count_ <= 59U) {
        ++count_;
    }
}

double HostFpsTracker::fps() const noexcept {
    if (count_ <= 1U) {
        return 0.0;
    }
    const std::uint64_t newest = samples_[(write_index_ + 59U) % kWindowSize];
    if (count_ <= 1U) {
        return 0.0;
    }
    const std::size_t first_index = (write_index_ - count_ + kWindowSize) % kWindowSize;
    const std::uint64_t cutoff = std::max<std::uint64_t>(newest, 1000000000U) - 1000000000U;
    std::size_t first_recent_offset = 0U;
    while (count_ > first_recent_offset &&
           cutoff > samples_[(first_index + first_recent_offset) % kWindowSize]) {
        ++first_recent_offset;
    }
    const std::size_t recent_count = count_ - first_recent_offset;
    if (recent_count <= 1U) {
        return 0.0;
    }
    const std::uint64_t oldest = samples_[(first_index + first_recent_offset) % kWindowSize];
    const std::uint64_t newest_check = samples_[(write_index_ + 59U) % kWindowSize];
    if (oldest >= newest_check || newest < newest_check) {
        return 0.0;
    }
    const double elapsed_seconds = static_cast<double>(newest - oldest) / 1.0e9;
    return static_cast<double>(recent_count - 1U) / elapsed_seconds;
}

double HostFpsTracker::fps(const std::uint64_t now_nanoseconds) const noexcept {
    if (count_ <= 1U) {
        return 0.0;
    }
    const std::size_t first_index = (write_index_ - count_ + kWindowSize) % kWindowSize;
    const std::uint64_t cutoff = std::max<std::uint64_t>(now_nanoseconds, 1000000000U) - 1000000000U;
    std::size_t first_recent_offset = 0U;
    while (count_ > first_recent_offset &&
           samples_[(first_index + first_recent_offset) % kWindowSize] < cutoff) {
        ++first_recent_offset;
    }
    const std::size_t recent_count = count_ - first_recent_offset;
    if (recent_count <= 1U) {
        return 0.0;
    }
    const std::uint64_t oldest = samples_[(first_index + first_recent_offset) % kWindowSize];
    const std::uint64_t newest = samples_[(write_index_ + 59U) % kWindowSize];
    if (oldest >= newest || now_nanoseconds < newest) {
        return 0.0;
    }
    const double elapsed_seconds = static_cast<double>(now_nanoseconds - oldest) / 1.0e9;
    return static_cast<double>(recent_count - 1U) / elapsed_seconds;
}

double HostFpsTracker::average_fps() const noexcept {
    if (count_ == 0U) {
        return 0.0;
    }
    const std::uint64_t newest = samples_[(write_index_ + 59U) % kWindowSize];
    return average_.fps(newest);
}

double HostFpsTracker::average_fps(const std::uint64_t now_nanoseconds) const noexcept {
    return average_.fps(now_nanoseconds);
}

std::uint64_t HostFpsTracker::total_frames() const noexcept {
    return total_frames_;
}

void HostFpsTracker::reset() noexcept {
    count_ = 0U;
    write_index_ = 0U;
    average_.reset();
    total_frames_ = 0U;
}

} // namespace takedown::presentation
