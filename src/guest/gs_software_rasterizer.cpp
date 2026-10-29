#include "takedown/guest/gs_software_rasterizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace takedown::guest {

namespace {

using GsBlockFootprint = std::bitset<kGsLocalMemoryBlocks>;

// Integer pixel rectangle, inclusive on all sides.
struct RasterBounds {
    int left{};
    int right{};
    int top{};
    int bottom{};
};

// Per-worker tallies, one cache line each so tile jobs on different workers
// never share a line. Slot 0 is the calling thread, slots 1..8 the workers.
struct alignas(64) RasterTileCounters {
    std::uint64_t covered_pixels{};
    std::uint64_t written_pixels{};
};

// A primitive's bounding box split into 64x8 pixel tiles aligned to the GS
// block grid.
struct RasterTileGrid {
    RasterBounds bounds{};
    int aligned_left{};
    int aligned_top{};
    unsigned int columns{};
    unsigned int rows{};
};

struct TextureCoordinateRange {
    std::uint32_t minimum{};
    std::uint32_t maximum{};
};

// Page geometry of a surface: page size in pixels, pages per row, and whether
// the blocks inside a page use the Z-buffer swizzle.
struct TexturePageLayout {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    bool depth_swizzled{};
};

// PSMCT32 framebuffer pages (64x32 pixels, 32 blocks each) covered by bounds.
[[nodiscard]] bool mark_framebuffer_pages(const GsLocalMemorySurface& surface,
                                          const RasterBounds bounds,
                                          GsBlockFootprint& footprint) {
    if (surface.pixel_storage_mode != kGsPsmct32) {
        return false;
    }
    if (surface.buffer_width == 0U) {
        return false;
    }
    if (surface.buffer_width > 63U) {
        return false;
    }
    if (surface.base_pointer > 16383U) {
        return false;
    }
    if (bounds.left < 0 || bounds.top < 0) {
        return false;
    }
    if (bounds.left > bounds.right || bounds.top > bounds.bottom) {
        return false;
    }
    if (static_cast<unsigned long long>(bounds.right) >=
        static_cast<unsigned long long>(surface.buffer_width) * 64U) {
        return false;
    }
    const unsigned long long first_page_x = static_cast<unsigned long long>(bounds.left) / 64U;
    const unsigned long long last_page_x = static_cast<unsigned long long>(bounds.right) / 64U;
    const unsigned long long first_page_y = static_cast<unsigned long long>(bounds.top) / 32U;
    const unsigned long long last_page_y = static_cast<unsigned long long>(bounds.bottom) / 32U;
    const unsigned long long last_page =
        static_cast<unsigned long long>(surface.buffer_width) * last_page_y + last_page_x;
    const unsigned long long final_block = surface.base_pointer + last_page * 32U + 31U;
    if (final_block > 16383U) {
        return false;
    }
    for (unsigned long long page_y = first_page_y; page_y <= last_page_y; ++page_y) {
        for (unsigned long long page_x = first_page_x; page_x <= last_page_x; ++page_x) {
            const unsigned long long page =
                static_cast<unsigned long long>(surface.buffer_width) * page_y + page_x;
            const unsigned long long first_block = surface.base_pointer + page * 32U;
            for (std::uint64_t block = 0U; block != 32U; ++block) {
                footprint.set(first_block + block);
            }
        }
    }
    return true;
}

// Height of a Z-buffer page: PSMZ32/24 pages are 64x32 pixels, PSMZ16/16S
// pages 64x64. Zero for formats that are not depth formats.
[[nodiscard]] constexpr unsigned int depth_page_height(const std::uint32_t psm) noexcept {
    if (psm == kGsPsmz32 || psm == kGsPsmz24) {
        return 32U;
    }
    if (psm == kGsPsmz16 || psm == kGsPsmz16s) {
        return 64U;
    }
    return 0U;
}

// Z-buffer pages covered by bounds. Depth pages swizzle their blocks (XOR 24).
[[nodiscard]] bool mark_depth_pages(const GsLocalMemorySurface& surface, const RasterBounds bounds,
                                    GsBlockFootprint& footprint) {
    // PSMZ32/24 pages are 64x32 pixels, PSMZ16/16S pages 64x64.
    const unsigned int page_height =
        surface.pixel_storage_mode == kGsPsmz32 || surface.pixel_storage_mode == kGsPsmz24 ? 32U
        : surface.pixel_storage_mode == kGsPsmz16 || surface.pixel_storage_mode == kGsPsmz16s
            ? 64U
            : 0U;
    if (page_height == 0U) {
        return false;
    }
    if (surface.buffer_width == 0U) {
        return false;
    }
    if (surface.buffer_width > 63U) {
        return false;
    }
    if (surface.base_pointer > 16383U) {
        return false;
    }
    if (bounds.left < 0 || bounds.top < 0) {
        return false;
    }
    if (bounds.left > bounds.right || bounds.top > bounds.bottom) {
        return false;
    }
    if (static_cast<unsigned long long>(bounds.right) >=
        static_cast<unsigned long long>(surface.buffer_width) * 64U) {
        return false;
    }
    const unsigned long long first_page_x = static_cast<unsigned long long>(bounds.left) / 64U;
    const unsigned long long last_page_x = static_cast<unsigned long long>(bounds.right) / 64U;
    const unsigned long long first_page_y =
        static_cast<unsigned long long>(bounds.top) / page_height;
    const unsigned long long last_page_y =
        static_cast<unsigned long long>(bounds.bottom) / page_height;
    const unsigned long long last_page =
        static_cast<unsigned long long>(surface.buffer_width) * last_page_y + last_page_x;
    const unsigned long long final_block = surface.base_pointer + last_page * 32U + 31U;
    if (final_block > 16383U) {
        return false;
    }
    for (unsigned long long page_y = first_page_y; page_y <= last_page_y; ++page_y) {
        for (unsigned long long page_x = first_page_x; page_x <= last_page_x; ++page_x) {
            const unsigned long long page =
                static_cast<unsigned long long>(surface.buffer_width) * page_y + page_x;
            const unsigned long long first_block = surface.base_pointer + page * 32U;
            for (std::uint64_t block = 0U; block != 32U; ++block) {
                footprint.set((first_block + block) ^ 24U);
            }
        }
    }
    return true;
}

// Blocks a draw writes: the framebuffer and, when depth is live, the Z-buffer.
// Fails when either cannot be addressed or when the two overlap, since tiles
// running in parallel would then race on shared blocks.
[[nodiscard]] bool mark_parallel_outputs(const GsRasterTarget& target, const RasterBounds bounds,
                                         GsBlockFootprint& footprint) noexcept {
    GsBlockFootprint framebuffer{};
    if (!mark_framebuffer_pages(target.surface, bounds, framebuffer)) {
        return false;
    }
    if (!target.depth.known) {
        footprint = framebuffer;
        return true;
    }
    GsBlockFootprint depth{};
    if (!mark_depth_pages(target.depth.surface, bounds, depth) || (framebuffer & depth).any()) {
        return false;
    }
    footprint = framebuffer | depth;
    return true;
}

// Indexed pages are twice as wide, so TBW counts half as many pages. A TBW of
// 1 rounds to zero pages, which only whole-texture footprints may accept.
[[nodiscard]] std::optional<TexturePageLayout>
indexed_page_layout(const std::uint32_t width, const std::uint32_t height, const std::uint32_t stride,
                    const bool allow_collapsed_stride) {
    if (stride == 0U) {
        if (!allow_collapsed_stride) {
            return std::nullopt;
        }
    }
    return TexturePageLayout{width, height, stride, false};
}

[[nodiscard]] std::optional<TexturePageLayout>
texture_page_layout(const GsLocalMemorySurface& surface, const bool allow_collapsed_indexed_stride) {
    if (surface.buffer_width == 0U || surface.buffer_width > 63U || surface.base_pointer > 16383U) {
        return std::nullopt;
    }
    switch (surface.pixel_storage_mode) {
    case kGsPsmct32:
    case kGsPsmct24:
    case kGsPsmt8h:
        return TexturePageLayout{64U, 32U, surface.buffer_width, false};
    case kGsPsmz32:
    case kGsPsmz24:
        return TexturePageLayout{64U, 32U, surface.buffer_width, true};
    case kGsPsmct16:
    case kGsPsmct16s:
        return TexturePageLayout{64U, 64U, surface.buffer_width, false};
    case kGsPsmz16:
    case kGsPsmz16s:
        return TexturePageLayout{64U, 64U, surface.buffer_width, true};
    case kGsPsmt8:
        return indexed_page_layout(128U, 64U, surface.buffer_width / 2U,
                                   allow_collapsed_indexed_stride);
    case kGsPsmt4:
        return indexed_page_layout(128U, 128U, surface.buffer_width / 2U,
                                   allow_collapsed_indexed_stride);
    default:
        return std::nullopt;
    }
}

// Marks every block a rectangular texel range of the surface can touch.
[[nodiscard]] bool mark_surface_region(const GsLocalMemorySurface& surface,
                                       const TextureCoordinateRange x,
                                       const TextureCoordinateRange y, GsBlockFootprint& footprint,
                                       const bool allow_collapsed_indexed_stride) {
    const auto layout = texture_page_layout(surface, allow_collapsed_indexed_stride);
    if (!layout || x.minimum > x.maximum || y.minimum > y.maximum) {
        return false;
    }
    const unsigned long long first_page_x = x.minimum / layout->width;
    const unsigned long long last_page_x = x.maximum / layout->width;
    const unsigned long long first_page_y = y.minimum / layout->height;
    const unsigned long long last_page_y = y.maximum / layout->height;
    for (unsigned long long page_y = first_page_y; page_y <= last_page_y; ++page_y) {
        for (unsigned long long page_x = first_page_x; page_x <= last_page_x; ++page_x) {
            const unsigned long long page = layout->stride * page_y + page_x;
            const unsigned long long first_block = surface.base_pointer + page * 32U;
            for (std::uint64_t block = 0U; block != 32U; ++block) {
                const unsigned long long unswizzled = first_block + block;
                const unsigned long long physical =
                    layout->depth_swizzled ? (unswizzled ^ 24U) & (kGsLocalMemoryBlocks - 1U)
                                           : unswizzled & (kGsLocalMemoryBlocks - 1U);
                [[assume(physical < kGsLocalMemoryBlocks)]];
                footprint.set(physical);
            }
        }
    }
    return true;
}

[[nodiscard]] bool footprints_overlap(const GsBlockFootprint& first,
                                      const GsBlockFootprint& second) noexcept {
    return (first & second).any();
}

// Marks one physical block; callers have already wrapped it into local memory.
void mark_block(const unsigned long long block, GsBlockFootprint& footprint) {
    [[assume(block < kGsLocalMemoryBlocks)]];
    footprint.set(block);
}

// Every block a textured draw may read: each mip level's whole surface plus
// the CLUT for indexed formats. Region clamp/repeat and oversized textures
// are not tracked and make the draw run serially.
[[nodiscard]] bool mark_parallel_texture(const GsRasterTexture& texture,
                                         GsBlockFootprint& footprint) noexcept {
    if (texture.width == 0U || texture.height == 0U || texture.width > 2048U ||
        texture.height > 2048U) {
        return false;
    }
    const GsRasterClamp clamp = decode_gs_raster_clamp(texture.clamp);
    if (clamp.wrap_s >= 2U || clamp.wrap_t >= 2U) {
        return false;
    }
    const auto mark_level = [&footprint](const GsLocalMemorySurface& surface,
                                         const std::uint32_t width,
                                         const std::uint32_t height) {
        const auto layout = texture_page_layout(surface, false);
        if (!layout) {
            return false;
        }
        const unsigned int pages_x = (width + layout->width - 1U) / layout->width;
        const unsigned int pages_y = (height + layout->height - 1U) / layout->height;
        if (pages_x == 0U || pages_y == 0U || pages_x > layout->stride) {
            return false;
        }
        for (std::uint64_t page_y = 0U; page_y < pages_y; ++page_y) {
            for (std::uint64_t page_x = 0U; page_x < pages_x; ++page_x) {
                const unsigned long long page = layout->stride * page_y + page_x;
                const unsigned long long first_block = surface.base_pointer + page * 32U;
                for (std::uint64_t block = 0U; block != 32U; ++block) {
                    mark_block((first_block + block) & (kGsLocalMemoryBlocks - 1U), footprint);
                }
            }
        }
        return true;
    };
    if (!mark_level(texture.surface, texture.width, texture.height)) {
        return false;
    }
    const std::uint32_t mip_levels = std::min<std::uint32_t>(texture.maximum_mip_level, 6U);
    for (std::uint32_t level = 1U; level <= mip_levels; ++level) {
        const std::uint32_t level_width = std::max(texture.width >> level, 1U);
        const std::uint32_t level_height = std::max(texture.height >> level, 1U);
        if (!mark_level(texture.mipmap_surfaces[level - 1U], level_width, level_height)) {
            return false;
        }
    }
    if (texture.surface.pixel_storage_mode == kGsPsmt8 ||
        texture.surface.pixel_storage_mode == kGsPsmt8h ||
        texture.surface.pixel_storage_mode == kGsPsmt4) {
        if (texture.clut_storage_mode2 ||
            (texture.clut_pixel_storage_mode != kGsPsmct32 &&
             texture.clut_pixel_storage_mode != kGsPsmct24) ||
            texture.clut_base_pointer > 16383U || !texture.clut) {
            return false;
        }
        // A 256-entry PSMCT32 CLUT occupies four blocks.
        for (std::uint64_t block = 0U; block != 4U; ++block) {
            mark_block((texture.clut_base_pointer + block) & (kGsLocalMemoryBlocks - 1U), footprint);
        }
    }
    return true;
}

// A textured draw can run tile-parallel when every block it writes and every
// block it may sample are known and the two sets do not overlap.
[[nodiscard]] bool textured_draw_parallel_safe(const GsRasterTarget& target, const RasterBounds bounds,
                                               const GsRasterTexture& texture) noexcept {
    GsBlockFootprint framebuffer_footprint{};
    GsBlockFootprint texture_footprint{};
    const bool output_safe = mark_parallel_outputs(target, bounds, framebuffer_footprint);
    const bool input_safe = mark_parallel_texture(texture, texture_footprint);
    return output_safe && input_safe && !footprints_overlap(framebuffer_footprint, texture_footprint);
}

// Tests one physical block; callers pass indices inside local memory.
[[nodiscard]] bool block_marked(const std::size_t block, const GsBlockFootprint& footprint) {
    [[assume(block < kGsLocalMemoryBlocks)]];
    return footprint.test(block);
}

// Texel range one CLAMP mode can reach on a level of the given size: repeat
// and clamp cover the level, region clamp the [minimum, maximum] window, and
// region repeat ((u & minimum) | maximum) the values from maximum up to
// maximum | minimum.
[[nodiscard]] std::optional<TextureCoordinateRange>
texture_level_range(const std::uint32_t size, const std::uint32_t mode,
                    const std::uint32_t minimum, const std::uint32_t maximum) {
    if (size == 0U) {
        return std::nullopt;
    }
    switch (mode) {
    case 0U:
    case 1U:
        return TextureCoordinateRange{0U, size - 1U};
    case 2U:
        if (maximum < minimum) {
            return std::nullopt;
        }
        return TextureCoordinateRange{minimum, maximum};
    case 3U:
        return TextureCoordinateRange{maximum, maximum | minimum};
    default:
        return std::nullopt;
    }
}

// Blocks a textured draw can sample, over every mip level it can select.
[[nodiscard]] bool mark_texture_reads(const GsRasterTexture& texture, GsBlockFootprint& footprint) {
    const GsRasterClamp clamp = decode_gs_raster_clamp(texture.clamp);
    const std::uint32_t levels =
        std::min({static_cast<std::uint32_t>(texture.maximum_mip_level),
                  static_cast<std::uint32_t>(texture.maximum_lod_level), 6U});
    for (std::uint32_t level = 0U; level <= levels; ++level) {
        const std::uint32_t width = std::max(texture.width >> level, 1U);
        const std::uint32_t height = std::max(texture.height >> level, 1U);
        const std::optional<TextureCoordinateRange> x =
            texture_level_range(width, clamp.wrap_s, clamp.min_u >> level, clamp.max_u >> level);
        const std::optional<TextureCoordinateRange> y =
            texture_level_range(height, clamp.wrap_t, clamp.min_v >> level, clamp.max_v >> level);
        const GsLocalMemorySurface& surface =
            level == 0U ? texture.surface : texture.mipmap_surfaces[level - 1U];
        if (!x || !y || !mark_surface_region(surface, *x, *y, footprint, false)) {
            return false;
        }
    }
    return true;
}

// Blocks a draw inside bounds writes: the framebuffer unless every bit is
// masked, and the Z-buffer when depth writes are on.
[[nodiscard]] bool mark_draw_outputs(const GsRasterTarget& target, const RasterBounds bounds,
                                     GsBlockFootprint& outputs) noexcept {
    if (bounds.left < 0 || bounds.top < 0) {
        return false;
    }
    if (bounds.left > bounds.right || bounds.top > bounds.bottom) {
        return false;
    }
    const TextureCoordinateRange x{static_cast<std::uint32_t>(bounds.left),
                                   static_cast<std::uint32_t>(bounds.right)};
    const TextureCoordinateRange y{static_cast<std::uint32_t>(bounds.top),
                                   static_cast<std::uint32_t>(bounds.bottom)};
    if (target.framebuffer_mask != 0xffffffffU &&
        !mark_surface_region(target.surface, x, y, outputs, false)) {
        return false;
    }
    if (target.depth.known && target.depth.write_enabled) {
        return mark_surface_region(target.depth.surface, x, y, outputs, false);
    }
    return true;
}

// Cheap aliasing test when footprints are not known: any texture level that
// starts at the framebuffer or Z-buffer base.
// True when a texture surface starts on the frame or Z buffer's base block.
[[nodiscard]] bool surface_aliases_target(const GsLocalMemorySurface& surface,
                                          const GsRasterTarget& target) noexcept {
    return surface.base_pointer == target.surface.base_pointer ||
           (target.depth.known && surface.base_pointer == target.depth.surface.base_pointer);
}

[[nodiscard]] bool texture_may_alias_target(const GsRasterTexture& texture,
                                            const GsRasterTarget& target) {
    if (surface_aliases_target(texture.surface, target)) {
        return true;
    }
    const std::uint32_t levels =
        std::min({static_cast<std::uint32_t>(texture.maximum_mip_level),
                  static_cast<std::uint32_t>(texture.maximum_lod_level), 6U});
    for (std::uint32_t level = 1U; level <= levels; ++level) {
        if (surface_aliases_target(texture.mipmap_surfaces[level - 1U], target)) {
            return true;
        }
    }
    return false;
}

// Integer division rounding toward negative infinity.
template <typename T> [[nodiscard]] constexpr T floor_divide(const T numerator, const T denominator) noexcept {
    const T quotient = numerator / denominator;
    const T remainder = numerator % denominator;
    return remainder < 0 ? quotient - 1 : quotient;
}

// Alpha blend operand selection (A, B, D colour; C alpha).
[[nodiscard]] constexpr int blend_color(const GsRasterBlendColorSelector selector,
                                        const std::uint8_t source,
                                        const std::uint8_t destination) noexcept {
    switch (selector) {
    case GsRasterBlendColorSelector::Source:
        return source;
    case GsRasterBlendColorSelector::Destination:
        return destination;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr int blend_alpha(const GsRasterAlphaBlend& blend, const std::uint8_t source,
                                        const std::uint8_t destination) noexcept {
    switch (blend.c) {
    case GsRasterBlendAlphaSelector::Source:
        return source;
    case GsRasterBlendAlphaSelector::Destination:
        return destination;
    case GsRasterBlendAlphaSelector::Fixed:
        return blend.fixed_alpha;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr bool is_indexed_format(const std::uint32_t psm) noexcept {
    return psm == kGsPsmt8 || psm == kGsPsmt8h || psm == kGsPsmt4;
}

// Applies one CLAMP wrap mode to a texel coordinate on a level of the given
// size: repeat, clamp, region clamp or region repeat.
[[nodiscard]] int wrap_texel(const int coordinate, const std::uint32_t size,
                             const std::uint32_t mode, const std::uint32_t minimum,
                             const std::uint32_t maximum) noexcept {
    if (size == 0U) {
        return 0;
    }
    switch (mode) {
    case 0U:
        return coordinate & static_cast<int>(size - 1U);
    case 1U:
        return std::clamp(coordinate, 0, static_cast<int>(size - 1U));
    case 2U:
        return std::clamp(coordinate, static_cast<int>(minimum), static_cast<int>(maximum));
    case 3U:
        return (coordinate & static_cast<int>(minimum)) | static_cast<int>(maximum);
    default:
        return 0;
    }
}

// Indexed texel through the CLUT snapshot. PSMT4 picks a 16-entry row with
// CSA; PSMT8/8H add CSA to the high nibble, saturating at the last row.
[[nodiscard]] std::optional<GsRgba8> read_clut_texel(const GsLocalMemory& memory,
                                                     const GsRasterTexture& texture,
                                                     const GsLocalMemorySurface& surface,
                                                     const std::uint32_t x,
                                                     const std::uint32_t y) noexcept {
    if (texture.clut_storage_mode2 || texture.clut_pixel_storage_mode > kGsPsmct24 ||
        !texture.clut) {
        return std::nullopt;
    }
    const auto index = memory.read_index(surface, x, y);
    if (!index) {
        return std::nullopt;
    }
    std::uint32_t logical_index = 0U;
    if (surface.pixel_storage_mode == kGsPsmt4) {
        logical_index =
            ((static_cast<std::uint32_t>(texture.clut_start) << 4U) & 0xffU) | (*index & 0x0fU);
    } else if (surface.pixel_storage_mode == kGsPsmt8 ||
               surface.pixel_storage_mode == kGsPsmt8h) {
        logical_index =
            (std::min<std::uint32_t>((*index >> 4U) + (texture.clut_start & 0x0fU), 15U) << 4U) |
            (*index & 0x0fU);
    } else {
        return std::nullopt;
    }
    return texture.clut->entries[logical_index];
}

// Rounds half up to the nearest integer.
[[nodiscard]] int round_to_int(const double value) noexcept {
    return static_cast<int>(std::floor(value + 0.5));
}

// Bilinear weight of a texel position: its fractional part in 1/16 steps.
[[nodiscard]] int texel_fraction_16(const double position) noexcept {
    const double fraction = position - std::floor(position);
    return std::clamp(static_cast<int>(std::floor(fraction * 16.0)), 0, 15);
}

// first + (second - first) * fraction / one, rounded toward negative infinity.
[[nodiscard]] std::uint8_t lerp_channel(const std::uint8_t first, const std::uint8_t second,
                                        const int fraction, const int one) noexcept {
    const int delta = fraction * (second - first);
    return static_cast<std::uint8_t>(first + floor_divide(delta, one));
}

// TFX MODULATE: texel * vertex / 128, saturated.
[[nodiscard]] std::uint8_t modulate_channel(const std::uint8_t texel,
                                            const std::uint8_t vertex) noexcept {
    return static_cast<std::uint8_t>(
        std::min((static_cast<unsigned int>(texel) * static_cast<unsigned int>(vertex)) >> 7U, 255U));
}

// Vertex position relative to the drawing offset (12.4 fixed point).
struct AdjustedVertex {
    int x{};
    int y{};
};

// A vertex position relative to the target's XYOFFSET, in 12.4 fixed point.
[[nodiscard]] AdjustedVertex adjust_vertex(const GsRasterVertex& vertex,
                                           const GsRasterTarget& target) noexcept {
    return AdjustedVertex{vertex.x_12_4 - target.offset_x_12_4, vertex.y_12_4 - target.offset_y_12_4};
}

// Splits clipped bounds into 64x8 tiles aligned to the framebuffer page grid.
[[nodiscard]] RasterTileGrid raster_tile_grid(const RasterBounds bounds) noexcept {
    const int aligned_left = (bounds.left >> 6) * 64;
    const int aligned_top = (bounds.top >> 3) * 8;
    const unsigned int columns = static_cast<unsigned int>((bounds.right - aligned_left) / 64 + 1);
    const unsigned int rows = static_cast<unsigned int>((bounds.bottom - aligned_top) / 8 + 1);
    return RasterTileGrid{bounds, aligned_left, aligned_top, columns, rows};
}

// Pixel bounds of one 64x8 tile of the grid, clipped to the primitive.
[[nodiscard]] RasterBounds raster_tile_bounds(const RasterTileGrid& grid, const unsigned long long tile) noexcept {
    const auto column = static_cast<int>(tile % grid.columns);
    const auto row = static_cast<int>(tile / grid.columns);
    const int tile_left = grid.aligned_left + column * 64;
    const int tile_top = grid.aligned_top + row * 8;
    return RasterBounds{std::max(tile_left, grid.bounds.left), std::min(tile_left + 63, grid.bounds.right),
                        std::max(tile_top, grid.bounds.top), std::min(tile_top + 7, grid.bounds.bottom)};
}

[[nodiscard]] constexpr std::uint64_t raster_pixel_count(const RasterBounds bounds) noexcept {
    return static_cast<std::uint64_t>(bounds.right - bounds.left + 1) *
           static_cast<std::uint64_t>(bounds.bottom - bounds.top + 1);
}

[[nodiscard]] constexpr std::uint64_t raster_tile_count(const RasterTileGrid& grid) noexcept {
    return static_cast<std::uint64_t>(grid.columns) * grid.rows;
}

// Linear interpolation of an 8-bit vertex attribute along a line.
[[nodiscard]] std::uint8_t interpolate_line_channel(const std::uint8_t first, const std::uint8_t second,
                                                    const double progress) noexcept {
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(static_cast<double>(first) +
                                     (static_cast<double>(second) - static_cast<double>(first)) *
                                         progress)),
        0, 255));
}

// Clamps an interpolated Z to the precision of the target's depth buffer.
[[nodiscard]] std::uint32_t clamp_depth_to_target(const GsRasterTarget& target,
                                                  const std::uint32_t z) noexcept {
    if (!target.depth.known) {
        return z;
    }
    switch (target.depth.surface.pixel_storage_mode) {
    case kGsPsmz24:
        return std::min(z, 0xffffffU);
    case kGsPsmz16:
    case kGsPsmz16s:
        return std::min(z, 0xffffU);
    default:
        return z;
    }
}

// Twice the signed area of (from, to, point): which side of the edge the
// point lies on, in 12.4 fixed point squared.
[[nodiscard]] constexpr long long edge_function(const int from_x, const int from_y, const int to_x,
                                                const int to_y, const int x, const int y) noexcept {
    return static_cast<long long>(to_x - from_x) * (y - from_y) -
           static_cast<long long>(to_y - from_y) * (x - from_x);
}

// Top-left fill rule: samples exactly on a top or left edge belong to the
// triangle, samples on bottom or right edges do not.
[[nodiscard]] constexpr bool is_top_left_edge(const int from_x, const int from_y, const int to_x,
                                              const int to_y, const long long area) noexcept {
    const int dx = area > 0 ? to_x - from_x : from_x - to_x;
    const int dy = area > 0 ? to_y - from_y : from_y - to_y;
    return dy < 0 || (dy == 0 && dx > 0);
}

// A sample is inside an edge when it lies on the triangle's side of it, or
// on the edge itself when the edge is inclusive.
[[nodiscard]] constexpr bool edge_covers_sample(const long long edge, const bool inclusive,
                                                const long long area) noexcept {
    return area > 0 ? edge > 0 || (inclusive && edge == 0) : edge < 0 || (inclusive && edge == 0);
}

// Gouraud colour channel from barycentric weights.
[[nodiscard]] std::uint8_t barycentric_channel(const std::uint8_t first, const std::uint8_t second,
                                               const std::uint8_t third, const double wa,
                                               const double wb, const double wc) noexcept {
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(wa * static_cast<int>(first) + wb * static_cast<int>(second) +
                                     wc * static_cast<int>(third))),
        0, 255));
}

// Clamps a pixel coordinate to an inclusive scissor range.
[[nodiscard]] constexpr int clamp_coordinate(const int value, const std::uint16_t low,
                                             const std::uint16_t high) noexcept {
    return std::clamp(value, static_cast<int>(low), static_cast<int>(high));
}

// Integer division rounding toward positive infinity.
template <typename T> [[nodiscard]] constexpr T ceil_divide(const T numerator, const T denominator) noexcept {
    return -floor_divide(-numerator, denominator);
}

// Texture formats the software rasterizer can sample.
[[nodiscard]] constexpr bool sprite_format_extra(const std::uint32_t psm) noexcept {
    return is_indexed_format(psm) || psm == kGsPsmz16s;
}

[[nodiscard]] constexpr bool texture_format_supported(const std::uint32_t psm) noexcept {
    if (psm != kGsPsmct32 && psm != kGsPsmct24 && psm != kGsPsmz16) {
        return sprite_format_extra(psm);
    }
    return true;
}

// Level of detail: LOD = K - log2|Q| << L, or K alone with LCM fixed, clamped
// to MXL. A zero, infinite or NaN Q selects the smallest level.
[[nodiscard]] double texture_lod(const GsRasterTexture& texture, const double q) noexcept {
    if (texture.maximum_lod_level == 0U) {
        return 0.0;
    }
    double lod = static_cast<double>(texture.lod_bias_4) * 0.0625;
    if (!texture.fixed_lod) {
        if (!std::isfinite(q) || std::abs(q) < 1.0e-12) {
            return static_cast<double>(texture.maximum_lod_level);
        }
        lod += -std::log2(std::abs(q)) *
               static_cast<double>(1U << std::min<std::uint8_t>(texture.lod_scale, 3U));
    }
    return std::min(lod, static_cast<double>(texture.maximum_lod_level));
}

// Texel span a sprite edge-to-edge UV range can touch, in whole texels.
// Bilinear sampling reaches half a texel further on each side.
[[nodiscard]] std::pair<long long, long long> sprite_texel_span(const std::int64_t first_coordinate,
                                                                const std::int64_t last_coordinate,
                                                                const bool linear) noexcept {
    const long long minimum = std::min(first_coordinate, last_coordinate);
    const long long maximum = std::max(first_coordinate, last_coordinate);
    if (!linear) {
        return {floor_divide(minimum, 16LL), floor_divide(maximum, 16LL)};
    }
    return {floor_divide(minimum - 8, 16LL), floor_divide(maximum - 8, 16LL) + 1};
}

// Texture coordinate (10.4) at a sample offset along a sprite edge.
[[nodiscard]] constexpr long long sprite_coordinate(const long long start, const long long delta,
                                                    const int offset, const int span) noexcept {
    return start + delta * offset / span;
}

// Exact texel footprint of an unmipped, clamped sprite. Mipmapped, repeating
// or degenerate sprites fall back to the conservative whole-texture footprint.
[[nodiscard]] bool mark_sprite_texture_reads(const GsRasterTexture& texture, const AdjustedVertex a,
                                             const AdjustedVertex b, const GsRasterVertex& first,
                                             const GsRasterVertex& second, const RasterBounds bounds,
                                             GsBlockFootprint& footprint) noexcept {
    if (texture.maximum_mip_level != 0U || texture.maximum_lod_level != 0U) {
        return false;
    }
    if (a.x == b.x || a.y == b.y) {
        return false;
    }
    const GsRasterClamp clamp = decode_gs_raster_clamp(texture.clamp);
    if (clamp.wrap_s != 1U && clamp.wrap_s != 2U) {
        return false;
    }
    if (clamp.wrap_t != 1U && clamp.wrap_t != 2U) {
        return false;
    }
    const long long delta_u = static_cast<long long>(second.u_10_4) - first.u_10_4;
    const long long delta_v = static_cast<long long>(second.v_10_4) - first.v_10_4;
    const int left_sample = bounds.left * 16;
    const int right_sample = bounds.right * 16;
    const int top_sample = bounds.top * 16;
    const int bottom_sample = bounds.bottom * 16;
    const auto [first_u, last_u, first_v, last_v] = std::array<long long, 4>{
        sprite_coordinate(first.u_10_4, delta_u, left_sample - a.x, b.x - a.x),
        sprite_coordinate(first.u_10_4, delta_u, right_sample - a.x, b.x - a.x),
        sprite_coordinate(first.v_10_4, delta_v, top_sample - a.y, b.y - a.y),
        sprite_coordinate(first.v_10_4, delta_v, bottom_sample - a.y, b.y - a.y)};
    const auto [minimum_u, maximum_u] =
        sprite_texel_span(first_u, last_u, texture.magnification_linear);
    const auto [minimum_v, maximum_v] =
        sprite_texel_span(first_v, last_v, texture.magnification_linear);
    if (minimum_u < std::numeric_limits<int>::min() || maximum_u > std::numeric_limits<int>::max() ||
        minimum_v < std::numeric_limits<int>::min() || maximum_v > std::numeric_limits<int>::max()) {
        return false;
    }
    const int minimum_x = wrap_texel(static_cast<int>(minimum_u), texture.width, clamp.wrap_s,
                                     clamp.min_u, clamp.max_u);
    const int maximum_x = wrap_texel(static_cast<int>(maximum_u), texture.width, clamp.wrap_s,
                                     clamp.min_u, clamp.max_u);
    const int minimum_y = wrap_texel(static_cast<int>(minimum_v), texture.height, clamp.wrap_t,
                                     clamp.min_v, clamp.max_v);
    const int maximum_y = wrap_texel(static_cast<int>(maximum_v), texture.height, clamp.wrap_t,
                                     clamp.min_v, clamp.max_v);
    if (minimum_x < 0 || minimum_x > maximum_x || minimum_y < 0 || minimum_y > maximum_y) {
        return false;
    }
    return mark_surface_region(
        texture.surface,
        TextureCoordinateRange{static_cast<std::uint32_t>(minimum_x),
                               static_cast<std::uint32_t>(maximum_x)},
        TextureCoordinateRange{static_cast<std::uint32_t>(minimum_y),
                               static_cast<std::uint32_t>(maximum_y)},
        footprint, false);
}


// Packs a colour into the PSMCT32 word layout (R in the low byte).
[[nodiscard]] inline unsigned int pack_rgba8(const GsRgba8 color) noexcept {
    return static_cast<unsigned int>(color.red) |
           (static_cast<unsigned int>(color.green) << 8U) |
           (static_cast<unsigned int>(color.blue) << 16U) |
           (static_cast<unsigned int>(color.alpha) << 24U);
}

} // namespace
// Fixed pool of rasterizer worker threads. run() publishes one job per
// primitive; workers and the calling thread (slot 0) pull tile indices from an
// atomic counter until every tile is done.
class GsSoftwareRasterizer::TilePool {
  public:
    explicit TilePool(const std::uint32_t requested_workers) {
        const std::uint32_t worker_count = std::min(requested_workers, 8U);
        workers_.reserve(worker_count);
        try {
            for (std::uint32_t slot = 1U; slot <= worker_count; ++slot) {
                workers_.emplace_back([this, slot] { worker_loop(slot); });
            }
        } catch (...) {
            // Threads that did start must be joined before the vector dies.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            start_.notify_all();
            for (auto& worker : workers_) {
                worker.join();
            }
            throw;
        }
    }

    ~TilePool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        start_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    TilePool(const TilePool&) = delete;
    TilePool& operator=(const TilePool&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    template <typename Job> void run(const std::size_t tile_count, Job& job) noexcept {
        if (workers_.empty()) {
            for (std::size_t tile = 0U; tile < tile_count; ++tile) {
                job(tile, 0U);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_context_ = &job;
            job_function_ = [](void* const context, const std::size_t tile, const std::uint32_t slot) {
                (*static_cast<Job*>(context))(tile, slot);
            };
            tile_count_ = tile_count;
            next_tile_.store(0U, std::memory_order_relaxed);
            pending_workers_ = workers_.size();
            ++generation_;
        }
        start_.notify_all();
        drain(0U);
        std::unique_lock<std::mutex> lock(mutex_);
        completed_.wait(lock, [this] { return pending_workers_ == 0U; });
    }

  private:
    void worker_loop(const std::uint32_t slot) noexcept {
        std::uint64_t observed_generation = 0U;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_.wait(lock, [this, observed_generation] {
                    return stopping_ || generation_ != observed_generation;
                });
                if (stopping_) {
                    return;
                }
                observed_generation = generation_;
            }
            drain(slot);
            std::lock_guard<std::mutex> done(mutex_);
            if (--pending_workers_ == 0U) {
                completed_.notify_one();
            }
        }
    }

    void drain(const std::uint32_t slot) {
        for (;;) {
            const std::size_t tile = next_tile_.fetch_add(1U, std::memory_order_relaxed);
            if (tile >= tile_count_) {
                return;
            }
            job_function_(job_context_, tile, slot);
        }
    }

    std::vector<std::thread> workers_{};
    std::mutex mutex_{};
    std::condition_variable start_{};
    std::condition_variable completed_{};
    std::atomic<std::size_t> next_tile_{0U};
    void* job_context_{};
    void (*job_function_)(void*, std::size_t, std::uint32_t){};
    std::size_t tile_count_{};
    std::size_t pending_workers_{};
    std::uint64_t generation_{};
    bool stopping_{};
};

const char* to_string(const GsRasterStatus status) noexcept {
    switch (status) {
    case GsRasterStatus::Drawn:
        return "drawn";
    case GsRasterStatus::Empty:
        return "empty primitive";
    case GsRasterStatus::UnsupportedFramebufferFormat:
        return "unsupported framebuffer format";
    case GsRasterStatus::UnsupportedAlphaBlend:
        return "unsupported alpha-blend selector";
    case GsRasterStatus::InvalidTarget:
        return "invalid raster target";
    }
    return "unknown raster status";
}

std::optional<GsRasterTarget> decode_gs_raster_target(const std::uint64_t frame,
                                                      const std::uint64_t xyoffset,
                                                      const std::uint64_t scissor,
                                                      const std::optional<std::uint64_t> zbuf,
                                                      const std::optional<std::uint64_t> test,
                                                      const std::uint64_t colclamp) noexcept {
    const auto buffer_width = static_cast<unsigned int>(frame >> 16U) & 63U;
    const auto left = static_cast<unsigned short>(scissor & 2047U);
    const auto right = static_cast<unsigned short>((scissor >> 16U) & 2047U);
    const auto top = static_cast<unsigned short>((scissor >> 32U) & 2047U);
    const auto bottom = static_cast<unsigned short>((scissor >> 48U) & 2047U);
    if (buffer_width == 0U || left > right || top > bottom) {
        return std::nullopt;
    }

    GsRasterTarget target{};
    target.surface.base_pointer = (static_cast<std::uint32_t>(frame) << 5U) & 16352U;
    target.surface.buffer_width = buffer_width;
    target.surface.pixel_storage_mode = static_cast<std::uint32_t>(frame >> 24U) & 63U;
    target.offset_x_12_4 = static_cast<std::uint16_t>(xyoffset);
    target.offset_y_12_4 = static_cast<std::uint16_t>(xyoffset >> 32U);
    target.scissor = {left, right, top, bottom};
    target.framebuffer_mask = static_cast<std::uint32_t>(frame >> 32U);

    if (zbuf) {
        const auto z_format = (static_cast<unsigned int>(*zbuf >> 24U) & 15U) | 48U;
        if ((z_format - 48U <= 2U) || z_format == kGsPsmz16s) {
            target.depth.surface.base_pointer = (static_cast<std::uint32_t>(*zbuf) << 5U) & 16352U;
            target.depth.surface.buffer_width = buffer_width;
            target.depth.surface.pixel_storage_mode = z_format;
            target.depth.known = true;
            target.depth.write_enabled = (*zbuf & 0x100000000ULL) == 0U;
        }
    }

    if (test) {
        target.alpha_test.enabled = (*test & 1U) != 0U;
        target.alpha_test.method = static_cast<GsRasterAlphaTestMethod>((*test >> 1U) & 7U);
        target.alpha_test.reference = static_cast<std::uint8_t>(*test >> 4U);
        target.alpha_test.fail_action = static_cast<std::uint8_t>((*test >> 12U) & 3U);
        target.destination_alpha_test.enabled =
            (*test & 0x4000U) != 0U && target.surface.pixel_storage_mode != 1U;
        target.destination_alpha_test.match_one = (*test & 0x8000U) != 0U;
        target.depth.test_enabled = (*test & 0x10000U) != 0U;
        target.depth.test_method = static_cast<GsRasterDepthMethod>((*test >> 17U) & 3U);
    }

    target.colclamp = (colclamp & 1U) != 0U;
    return target;
}

GsSoftwareRasterizer::GsSoftwareRasterizer(GsLocalMemory& memory,
                                           const GsRasterThreadingConfig threading)
    : memory_(memory), threading_config_(threading) {
    if (!threading.enabled) {
        return;
    }
    // Default: leave two hardware threads for the emulator and host.
    unsigned int worker_count = threading.worker_threads;
    if (worker_count == 0U) {
        const unsigned int hardware_threads = std::thread::hardware_concurrency();
        worker_count = hardware_threads > 2U ? hardware_threads - 2U : 0U;
    }
    worker_count = std::min(worker_count, 8U);
    if (worker_count != 0U) {
        tile_pool_ = std::make_unique<TilePool>(worker_count);
        threading_statistics_.worker_threads =
            static_cast<std::uint32_t>(tile_pool_->worker_count());
    }
}

GsSoftwareRasterizer::~GsSoftwareRasterizer() noexcept = default;

const GsRasterThreadingStatistics& GsSoftwareRasterizer::threading_statistics() const noexcept {
    return threading_statistics_;
}

void GsSoftwareRasterizer::reset_threading_statistics() noexcept {
    threading_statistics_.parallel_primitives = 0U;
    threading_statistics_.parallel_tiles = 0U;
    threading_statistics_.serial_primitives = 0U;
    threading_statistics_.small_primitive_fallbacks = 0U;
    threading_statistics_.hazard_fallbacks = 0U;
}

bool GsSoftwareRasterizer::local_memory_is_authoritative_for_texture(
    const GsLocalMemorySurface& surface, const std::uint32_t width,
    const std::uint32_t height) const noexcept {
    const auto snapshot_reads = texture_snapshot_block_footprint(surface, width, height);
    if (!snapshot_reads) {
        return false;
    }
    // Local memory is authoritative unless a still-valid snapshot block
    // overlaps the texture.
    return !aliased_texture_snapshot_valid_blocks_.any() ||
           !footprints_overlap(*snapshot_reads, aliased_texture_snapshot_valid_blocks_);
}

std::optional<GsLocalMemoryBlockFootprint>
GsSoftwareRasterizer::texture_snapshot_block_footprint(const GsLocalMemorySurface& surface,
                                                       const std::uint32_t width,
                                                       const std::uint32_t height) const noexcept {
    if (width == 0U || height == 0U || width > 2048U || height > 2048U) {
        return std::nullopt;
    }
    GsBlockFootprint snapshot_reads{};
    const TextureCoordinateRange x{.maximum = width - 1U};
    const TextureCoordinateRange y{.maximum = height - 1U};
    if (!mark_surface_region(surface, x, y, snapshot_reads, true)) {
        return std::nullopt;
    }
    return snapshot_reads;
}

void GsSoftwareRasterizer::invalidate_texture_cache() noexcept {
    aliased_texture_snapshot_valid_blocks_.reset();
    aliased_texture_previous_draw_read_blocks_.reset();
    aliased_texture_batch_output_blocks_.reset();
    aliased_texture_previous_draw_reads_precise_ = false;
    aliased_texture_snapshot_batch_state_.reset();
}

void GsSoftwareRasterizer::invalidate_texture_cache_region(const GsLocalMemorySurface& surface,
                                                           const std::uint32_t x,
                                                           const std::uint32_t y,
                                                           const std::uint32_t width,
                                                           const std::uint32_t height) noexcept {
    if (width == 0U || height == 0U) {
        return;
    }
    if (aliased_texture_snapshot_valid_blocks_.none()) {
        return;
    }
    const unsigned long long right = x + static_cast<unsigned long long>(width) - 1U;
    const unsigned long long bottom = y + static_cast<unsigned long long>(height) - 1U;
    if (right > 4294967295ULL || bottom > 4294967295ULL) {
        invalidate_texture_cache();
        return;
    }
    GsBlockFootprint written{};
    const TextureCoordinateRange horizontal{x, static_cast<std::uint32_t>(right)};
    const TextureCoordinateRange vertical{y, static_cast<std::uint32_t>(bottom)};
    if (!mark_surface_region(surface, horizontal, vertical, written, false)) {
        invalidate_texture_cache();
        return;
    }
    // Blocks rewritten by this transfer no longer match the cached snapshot.
    aliased_texture_snapshot_valid_blocks_ &= ~written;
    if (aliased_texture_snapshot_valid_blocks_.none()) {
        invalidate_texture_cache();
    }
}

// Returns the local memory a textured draw should sample. A draw that cannot
// read anything it (or the current batch) writes samples live memory. Otherwise
// it samples a snapshot taken before the batch started writing, so texels
// written by earlier primitives of the same batch read their old values like
// they would on the GS, which samples the texture cache filled at batch start.
const GsLocalMemory& GsSoftwareRasterizer::texture_source_for_draw(
    const GsRasterTarget& target, const GsRasterTexture& texture,
    const TextureDrawClass draw_class, const std::bitset<kGsLocalMemoryBlocks>* const draw_texture_reads,
    const std::int32_t left, const std::int32_t right, const std::int32_t top,
    const std::int32_t bottom) noexcept {
    // A snapshot only stays valid for the batch (target, texture and draw
    // class) it was taken for.
    if (aliased_texture_snapshot_valid_blocks_.any() &&
        (!aliased_texture_snapshot_batch_state_ ||
         !(aliased_texture_snapshot_batch_state_->target == target &&
           aliased_texture_snapshot_batch_state_->texture == texture) ||
         aliased_texture_snapshot_batch_state_->draw_class != draw_class)) {
        invalidate_texture_cache();
    }

    const RasterBounds bounds{left, right, top, bottom};
    GsBlockFootprint texture_reads{};
    GsBlockFootprint outputs{};
    const bool precise_reads = draw_texture_reads != nullptr;
    // The caller's exact footprint, or every block the texture can sample.
    const bool reads_known = draw_texture_reads != nullptr
                                 ? (texture_reads = *draw_texture_reads, true)
                                 : mark_texture_reads(texture, texture_reads);
    const bool outputs_known = mark_draw_outputs(target, bounds, outputs);

    // Within a batch, a precise draw that reads different blocks than the
    // previous one and overlaps what the batch already wrote needs a fresh
    // snapshot.
    if (aliased_texture_snapshot_valid_blocks_.any() && reads_known && precise_reads &&
        aliased_texture_previous_draw_reads_precise_ &&
        texture_reads != aliased_texture_previous_draw_read_blocks_ &&
        footprints_overlap(texture_reads, aliased_texture_batch_output_blocks_)) {
        invalidate_texture_cache();
    }

    const bool reads_own_outputs =
        reads_known && outputs_known && footprints_overlap(texture_reads, outputs);
    const bool cache_supplies_reads =
        reads_known ? footprints_overlap(texture_reads, aliased_texture_snapshot_valid_blocks_)
                    : aliased_texture_snapshot_valid_blocks_.any();
    if (!cache_supplies_reads && !reads_own_outputs &&
        !texture_may_alias_target(texture, target)) {
        return memory_;
    }

    if (!reads_known) {
        // Unknown footprint: snapshot all of local memory.
        if (!aliased_texture_snapshot_valid_blocks_.any()) {
            static_cast<void>(aliased_texture_snapshot_.load_raw_backing(memory_.raw_backing()));
            aliased_texture_snapshot_valid_blocks_.set();
        } else if (!aliased_texture_snapshot_valid_blocks_.all()) {
            for (std::size_t block = 0U; block != kGsLocalMemoryBlocks; ++block) {
                if (block_marked(block, aliased_texture_snapshot_valid_blocks_)) {
                    continue;
                }
                const auto offset = block * kGsLocalMemoryBlockBytes;
                std::copy_n(memory_.raw_backing_.begin() + offset, kGsLocalMemoryBlockBytes,
                            aliased_texture_snapshot_.raw_backing_.begin() + offset);
            }
            aliased_texture_snapshot_valid_blocks_.set();
        }
        if (!aliased_texture_snapshot_batch_state_) {
            aliased_texture_snapshot_batch_state_ = TextureBatchState{target, texture, draw_class};
        }
        aliased_texture_previous_draw_read_blocks_.reset();
        aliased_texture_previous_draw_reads_precise_ = false;
        if (outputs_known) {
            aliased_texture_batch_output_blocks_ |= outputs;
        } else {
            aliased_texture_batch_output_blocks_.set();
        }
        return aliased_texture_snapshot_;
    }

    // Copy only the blocks this draw reads that the snapshot does not have yet.
    const auto missing = texture_reads & ~aliased_texture_snapshot_valid_blocks_;
    for (std::size_t block = 0U; block != kGsLocalMemoryBlocks; ++block) {
        if (!block_marked(block, missing)) {
            continue;
        }
        const auto offset = block * kGsLocalMemoryBlockBytes;
        std::copy_n(memory_.raw_backing_.begin() + offset, kGsLocalMemoryBlockBytes,
                    aliased_texture_snapshot_.raw_backing_.begin() + offset);
    }
    aliased_texture_snapshot_valid_blocks_ |= texture_reads;
    if (!aliased_texture_snapshot_batch_state_) {
        aliased_texture_snapshot_batch_state_ = TextureBatchState{target, texture, draw_class};
    }
    if (draw_texture_reads != nullptr) {
        aliased_texture_previous_draw_read_blocks_ = texture_reads;
        aliased_texture_previous_draw_reads_precise_ = true;
    } else {
        aliased_texture_previous_draw_read_blocks_.reset();
        aliased_texture_previous_draw_reads_precise_ = false;
    }
    if (outputs_known) {
        aliased_texture_batch_output_blocks_ |= outputs;
    } else {
        aliased_texture_batch_output_blocks_.set();
    }
    return aliased_texture_snapshot_;
}

GsRasterStatus GsSoftwareRasterizer::validate_target(const GsRasterTarget& target) const noexcept {
    const auto psm = target.surface.pixel_storage_mode;
    if (psm > 2U && psm != kGsPsmct16s) {
        return GsRasterStatus::UnsupportedFramebufferFormat;
    }
    if (psm != kGsPsmct32 && psm != kGsPsmct16) {
        if (psm != kGsPsmct16s) {
            if (target.framebuffer_mask != 0U) {
                return GsRasterStatus::UnsupportedFramebufferFormat;
            }
        }
    }
    if (target.surface.buffer_width == 0U || target.surface.base_pointer > 16383U ||
        target.scissor.left > target.scissor.right || target.scissor.top > target.scissor.bottom) {
        return GsRasterStatus::InvalidTarget;
    }
    return GsRasterStatus::Drawn;
}

bool GsSoftwareRasterizer::write_fragment(const GsRasterTarget& target, const std::uint32_t x,
                                          const std::uint32_t y, const GsRgba8 source,
                                          const bool rgb_only) noexcept {
    if (target.framebuffer_mask == 0xffffffffU) {
        // Every bit is protected: nothing can change.
        return false;
    }
    presentation::Rgba8 result = source;
    if (target.framebuffer_mask != 0U) {
        // FBMSK bits keep the destination's bit.
        const auto destination = memory_.read_rgba8(target.surface, x, y);
        if (!destination) {
            return false;
        }
        const unsigned int current = pack_rgba8(*destination);
        const unsigned int combined = (current & target.framebuffer_mask) |
                                      (pack_rgba8(source) & ~target.framebuffer_mask);
        result = {static_cast<std::uint8_t>(combined), static_cast<std::uint8_t>(combined >> 8U),
                  static_cast<std::uint8_t>(combined >> 16U),
                  static_cast<std::uint8_t>(combined >> 24U)};
    }
    if (rgb_only) {
        const auto destination = memory_.read_rgba8(target.surface, x, y);
        if (!destination) {
            return false;
        }
        result.alpha = destination->alpha;
    }
    return memory_.write_rgba8(target.surface, x, y, result) == GsLocalMemoryStatus::Applied;
}

void GsSoftwareRasterizer::write_depth_fragment(const GsRasterTarget& target, const std::uint32_t x,
                                                const std::uint32_t y,
                                                const std::uint32_t z) noexcept {
    if (target.depth.known && target.depth.write_enabled) {
        memory_.write_z(target.depth.surface, x, y, z);
    }
}

void GsSoftwareRasterizer::write_depth_fragment_at(const GsRasterTarget& target,
                                                   const std::size_t byte,
                                                   const std::uint32_t z) noexcept {
    memory_.write_z_at(target.depth.surface.pixel_storage_mode, byte, z);
}

std::uint8_t GsSoftwareRasterizer::alpha_test_decision(const GsRasterAlphaTest& alpha_test,
                                                       const std::uint8_t alpha) const noexcept {
    constexpr std::uint8_t kPass = 4U;
    if (!alpha_test.enabled) {
        return kPass;
    }
    bool passes = false;
    switch (alpha_test.method) {
    case GsRasterAlphaTestMethod::Never:
        passes = false;
        break;
    case GsRasterAlphaTestMethod::Less:
        passes = alpha_test.reference > alpha;
        break;
    case GsRasterAlphaTestMethod::LessEqual:
        passes = alpha_test.reference >= alpha;
        break;
    case GsRasterAlphaTestMethod::Equal:
        passes = alpha_test.reference == alpha;
        break;
    case GsRasterAlphaTestMethod::GreaterEqual:
        passes = alpha_test.reference <= alpha;
        break;
    case GsRasterAlphaTestMethod::Greater:
        passes = alpha_test.reference < alpha;
        break;
    case GsRasterAlphaTestMethod::NotEqual:
        passes = alpha_test.reference != alpha;
        break;
    default:
        passes = true;
        break;
    }
    return passes ? kPass : alpha_test.fail_action;
}

bool GsSoftwareRasterizer::destination_alpha_test_passes(const GsRasterTarget& target,
                                                         const std::uint32_t x,
                                                         const std::uint32_t y) const noexcept {
    if (!target.destination_alpha_test.enabled) {
        return true;
    }
    // DATM: pass when the destination alpha's top bit equals the mode.
    const auto destination = memory_.read_rgba8(target.surface, x, y);
    return destination && (static_cast<std::int8_t>(destination->alpha) < 0) ==
                              target.destination_alpha_test.match_one;
}

// ZTST against the stored depth. writable_byte is the depth pixel's byte when
// the fragment should write Z, or kGsLocalMemoryBytes when it must not.
GsSoftwareRasterizer::DepthTestResult
GsSoftwareRasterizer::depth_test(const GsRasterTarget& target, const std::uint32_t x,
                                 const std::uint32_t y, const std::uint32_t z) const noexcept {
    if (!target.depth.known) {
        return DepthTestResult{.passes = true};
    }
    const GsRasterDepthMethod method =
        target.depth.test_enabled ? target.depth.test_method : GsRasterDepthMethod::Always;
    bool compare_stored_depth = false;
    switch (method) {
    case GsRasterDepthMethod::Never:
        return DepthTestResult{};
    case GsRasterDepthMethod::GreaterEqual:
    case GsRasterDepthMethod::Greater:
        compare_stored_depth = true;
        break;
    default:
        break;
    }
    if (!compare_stored_depth && !target.depth.write_enabled) {
        // An always-passing test with no Z write leaves the buffer untouched.
        return DepthTestResult{.passes = true};
    }
    const unsigned long long byte = GsLocalMemory::depth_pixel_byte(target.depth.surface, x, y);
    if (byte >= kGsLocalMemoryBytes) {
        return DepthTestResult{.passes = true};
    }
    if (!compare_stored_depth) {
        return DepthTestResult{.passes = true,
                               .writable_byte =
                                   target.depth.write_enabled ? byte : kGsLocalMemoryBytes};
    }
    // Compare at the buffer's precision: PSMZ24 keeps 24 bits, PSMZ16/16S 16.
    const unsigned int depth_psm = target.depth.surface.pixel_storage_mode;
    const unsigned int compared_z = depth_psm == kGsPsmz24 ? (z & 0xffffffU)
                                    : (depth_psm == kGsPsmz16 || depth_psm == kGsPsmz16s)
                                        ? (z & 0xffffU)
                                        : z;
    const unsigned int stored = memory_.read_z_at(depth_psm, byte);
    const bool passes = method == GsRasterDepthMethod::GreaterEqual ? stored <= compared_z
                                                                    : stored < compared_z;
    return DepthTestResult{.passes = passes,
                           .writable_byte = passes && target.depth.write_enabled
                                                ? byte
                                                : kGsLocalMemoryBytes};
}

// GS alpha blending: Cv = ((A - B) * C >> 7) + D per colour channel, where A,
// B and D select the source colour, destination colour or zero and C the
// source alpha, destination alpha or FIX. The shift is arithmetic, so negative
// products round toward negative infinity. Alpha passes through unblended.
std::optional<GsRgba8> GsSoftwareRasterizer::apply_alpha_blend(const GsRasterTarget& target,
                                                               const GsRasterAlphaBlend& blend,
                                                               const GsRgba8& source,
                                                               const std::uint32_t x,
                                                               const std::uint32_t y) const noexcept {
    if (!blend.enabled || blend.source_copy_without_destination()) {
        return source;
    }
    const auto destination = memory_.read_rgba8(target.surface, x, y);
    if (!destination) {
        return std::nullopt;
    }
    const auto blend_channel = [&target, &blend, &source,
                                &destination](const std::uint8_t source_channel,
                                              const std::uint8_t destination_channel) {
        const int a = blend_color(blend.a, source_channel, destination_channel);
        const int b = blend_color(blend.b, source_channel, destination_channel);
        const int c = blend_alpha(blend, source.alpha, destination->alpha);
        const int d = blend_color(blend.d, source_channel, destination_channel);
        const int value = d + floor_divide((a - b) * c, 128);
        // COLCLAMP clamps to 0..255; without it the result wraps.
        return target.colclamp ? static_cast<std::uint8_t>(std::min(std::max(value, 0), 255))
                               : static_cast<std::uint8_t>(value);
    };
    return GsRgba8{blend_channel(source.red, destination->red),
                   blend_channel(source.green, destination->green),
                   blend_channel(source.blue, destination->blue), source.alpha};
}

// Shades one textured fragment: picks the mip level from LOD (nearest or
// trilinear), samples it with the MAG/MIN filter and CLAMP wrap, expands
// indexed and 16/24-bit texels through the CLUT and TEXA, then applies TFX
// (MODULATE or DECAL) and TCC.
std::optional<GsRgba8> GsSoftwareRasterizer::shade_texture_fragment(
    const GsLocalMemory& texture_memory, const GsRasterTarget& target,
    const GsRasterTexture& texture, const GsRasterClamp& clamp, const double texture_x,
    const double texture_y, const double lod, const GsRgba8 color) const noexcept {
    static_cast<void>(target);
    const std::uint32_t maximum_level =
        std::min({static_cast<std::uint32_t>(texture.maximum_mip_level),
                  static_cast<std::uint32_t>(texture.maximum_lod_level), 6U});
    // LOD > 0 minifies; MXL 0 always magnifies.
    const bool minifying = texture.maximum_lod_level != 0U && std::isfinite(lod) && lod > 0.0;
    const bool linear_filter =
        minifying ? texture.minification_linear : texture.magnification_linear;

    // Samples one mip level at the fragment's texel position.
    // RECOVERY: matches the original statement for statement except for the
    // block layout around the texel-read selection and the alpha-from-TEXA
    // test, which GCC orders differently from this source.
    const auto sample_level = [&texture_memory, &texture, &clamp, texture_x,
                               texture_y](const std::uint32_t level,
                                          const bool linear) -> std::optional<GsRgba8> {
        const GsLocalMemorySurface& surface =
            level != 0U ? texture.mipmap_surfaces[level - 1U] : texture.surface;
        const std::uint32_t level_width = std::max(texture.width >> level, 1U);
        const std::uint32_t level_height = std::max(texture.height >> level, 1U);
        const GsRasterClamp scaled_clamp{clamp.wrap_s,         clamp.wrap_t,
                                         clamp.min_u >> level, clamp.max_u >> level,
                                         clamp.min_v >> level, clamp.max_v >> level};
        const double level_texture_x = std::ldexp(texture_x, -static_cast<int>(level));
        const double level_texture_y = std::ldexp(texture_y, -static_cast<int>(level));
        const bool alpha_from_texa =
            surface.pixel_storage_mode == kGsPsmct24 ||
            (is_indexed_format(surface.pixel_storage_mode) &&
             texture.clut_pixel_storage_mode == kGsPsmct24);

        const auto sample_texel = [&texture_memory, &texture, &surface, &scaled_clamp,
                                   level_width, level_height,
                                   alpha_from_texa](const int x,
                                                    const int y) -> std::optional<GsRgba8> {
            const int wrapped_x = wrap_texel(x, level_width, scaled_clamp.wrap_s,
                                             scaled_clamp.min_u, scaled_clamp.max_u);
            const int wrapped_y = wrap_texel(y, level_height, scaled_clamp.wrap_t,
                                             scaled_clamp.min_v, scaled_clamp.max_v);
            // PSMZ16 textures are read through the colour path with TEXA.
            const bool depth_as_color = surface.pixel_storage_mode == kGsPsmz16 ||
                                        surface.pixel_storage_mode == kGsPsmz16s;
            const unsigned long long raw_texa =
                static_cast<unsigned long long>(texture.alpha) |
                (static_cast<unsigned long long>(texture.alpha_is_zero_for_black) << 15U) |
                (static_cast<unsigned long long>(texture.alpha_high) << 32U);
            std::optional<GsRgba8> sampled =
                is_indexed_format(surface.pixel_storage_mode)
                    ? read_clut_texel(texture_memory, texture, surface,
                                      static_cast<std::uint32_t>(wrapped_x),
                                      static_cast<std::uint32_t>(wrapped_y))
                : depth_as_color
                    ? texture_memory.read_texture_rgba8(surface,
                                                        static_cast<std::uint32_t>(wrapped_x),
                                                        static_cast<std::uint32_t>(wrapped_y), raw_texa)
                    : texture_memory.read_rgba8(surface, static_cast<std::uint32_t>(wrapped_x),
                                                static_cast<std::uint32_t>(wrapped_y));
            if (!sampled) {
                return std::nullopt;
            }
            if (alpha_from_texa) {
                // AEM: black texels become transparent; others take TA0.
                sampled->alpha = texture.alpha_is_zero_for_black && sampled->red == 0U &&
                                         sampled->green == 0U && sampled->blue == 0U
                                     ? 0U
                                     : texture.alpha;
            }
            if (!texture.texture_alpha_enabled) {
                sampled->alpha = 0x80U;
            }
            return sampled;
        };

        if (!linear) {
            return sample_texel(static_cast<int>(std::floor(level_texture_x)),
                                static_cast<int>(std::floor(level_texture_y)));
        }
        // Bilinear: texel centres sit at +0.5, weights in 1/16 steps.
        const double shifted_x = level_texture_x - 0.5;
        const double shifted_y = level_texture_y - 0.5;
        const int first_x = static_cast<int>(std::floor(shifted_x));
        const int first_y = static_cast<int>(std::floor(shifted_y));
        const int second_x = first_x != std::numeric_limits<int>::max() ? first_x + 1 : first_x;
        const int second_y = first_y != std::numeric_limits<int>::max() ? first_y + 1 : first_y;
        const auto top_left = sample_texel(first_x, first_y);
        const auto top_right = sample_texel(second_x, first_y);
        const auto bottom_left = sample_texel(first_x, second_y);
        const auto bottom_right = sample_texel(second_x, second_y);
        if (!top_left || !top_right || !bottom_left || !bottom_right) {
            return std::nullopt;
        }
        const auto bilerp = [fraction_x = texel_fraction_16(shifted_x),
                             fraction_y = texel_fraction_16(shifted_y)](const std::uint8_t top_left_channel,
                                                     const std::uint8_t top_right_channel,
                                                     const std::uint8_t bottom_left_channel,
                                                     const std::uint8_t bottom_right_channel) {
            const std::uint8_t top = lerp_channel(top_left_channel, top_right_channel, fraction_x, 16);
            const std::uint8_t bottom =
                lerp_channel(bottom_left_channel, bottom_right_channel, fraction_x, 16);
            return lerp_channel(top, bottom, fraction_y, 16);
        };
        return GsRgba8{
            bilerp(top_left->red, top_right->red, bottom_left->red, bottom_right->red),
            bilerp(top_left->green, top_right->green, bottom_left->green, bottom_right->green),
            bilerp(top_left->blue, top_right->blue, bottom_left->blue, bottom_right->blue),
            bilerp(top_left->alpha, top_right->alpha, bottom_left->alpha, bottom_right->alpha)};
    };

    std::optional<GsRgba8> sampled;
    if (maximum_level != 0U && minifying && texture.mipmap_linear) {
        // Trilinear: LOD in 16.16 fixed point picks two levels and a 15-bit
        // blend weight between them.
        const long long fixed_lod = std::min(
            std::max(static_cast<long long>(std::nearbyint(
                         std::min(std::max(lod, 0.0), static_cast<double>(maximum_level)) * 65536.0)),
                     0LL),
            static_cast<long long>(maximum_level) * 65536 - 1);
        const unsigned int first_level = static_cast<unsigned int>(fixed_lod >> 16);
        const unsigned int second_level = first_level + 1U;
        const std::optional<GsRgba8> first_sample =
            sample_level(first_level, texture.minification_linear);
        const std::optional<GsRgba8> second_sample =
            sample_level(second_level, texture.minification_linear);
        if (!first_sample || !second_sample) {
            return std::nullopt;
        }
        const int fraction_15 = static_cast<int>(fixed_lod >> 1) & 0x7fff;
        sampled = GsRgba8{
            lerp_channel(first_sample->red, second_sample->red, fraction_15, 32768),
            lerp_channel(first_sample->green, second_sample->green, fraction_15, 32768),
            lerp_channel(first_sample->blue, second_sample->blue, fraction_15, 32768),
            lerp_channel(first_sample->alpha, second_sample->alpha, fraction_15, 32768)};
    } else {
        std::uint32_t level = 0U;
        if (maximum_level != 0U && minifying) {
            const int nearest = round_to_int(lod);
            level = static_cast<std::uint32_t>(
                std::clamp(nearest, 0, static_cast<int>(maximum_level)));
        }
        sampled = sample_level(level, linear_filter);
    }
    if (!sampled) {
        return std::nullopt;
    }
    GsRgba8 source = *sampled;
    if (texture.modulate) {
        source = GsRgba8{modulate_channel(source.red, color.red),
                         modulate_channel(source.green, color.green),
                         modulate_channel(source.blue, color.blue),
                         modulate_channel(source.alpha, color.alpha)};
    } else if (!texture.texture_alpha_enabled) {
        source.alpha = color.alpha;
    }
    return source;
}

GsRasterResult GsSoftwareRasterizer::draw_solid_point(const GsRasterTarget& target,
                                                      const GsRasterVertex& vertex,
                                                      const GsRasterAlphaBlend alpha_blend) noexcept {
    if (aliased_texture_snapshot_batch_state_) {
        // A solid primitive is a draw-batch boundary for the texture cache.
        invalidate_texture_cache();
    }
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    // Vertices are 12.4 fixed point; +8 rounds to the nearest pixel centre.
    const AdjustedVertex adjusted = adjust_vertex(vertex, target);
    const int x = (adjusted.x + 8) / 16;
    const int y = (adjusted.y + 8) / 16;
    if (x < static_cast<int>(target.scissor.left) || x > static_cast<int>(target.scissor.right) ||
        y < static_cast<int>(target.scissor.top) || y > static_cast<int>(target.scissor.bottom)) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    auto mutation_batch = memory_.begin_mutation_batch();
    ++threading_statistics_.serial_primitives;
    ++threading_statistics_.small_primitive_fallbacks;
    const auto pixel_x = static_cast<std::uint32_t>(x);
    const auto pixel_y = static_cast<std::uint32_t>(y);
    GsRasterResult result{GsRasterStatus::Drawn, 1U, 0U};
    if (!destination_alpha_test_passes(target, pixel_x, pixel_y)) {
        return result;
    }
    const auto decision = alpha_test_decision(target.alpha_test, vertex.color.alpha);
    if (decision == 0U) {
        return result;
    }
    if (decision == 2U) {
        // AFAIL zbuffer-only: depth updates, the framebuffer is untouched.
        const DepthTestResult depth = depth_test(target, pixel_x, pixel_y, vertex.z);
        if (depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
            write_depth_fragment_at(target, depth.writable_byte, vertex.z);
        }
        return result;
    }
    const DepthTestResult depth = depth_test(target, pixel_x, pixel_y, vertex.z);
    if (!depth.passes) {
        return result;
    }
    auto source = vertex.color;
    const auto blended = apply_alpha_blend(target, alpha_blend, source, pixel_x, pixel_y);
    source = blended.value_or(source);
    if (decision == 1U) {
        // FB_ONLY: colour without depth.
        if (write_fragment(target, pixel_x, pixel_y, source)) {
            result.written_pixels = 1U;
        }
        return result;
    }
    if (decision == 3U) {
        // RGB_ONLY: keep the destination alpha, no depth.
        if (write_fragment(target, pixel_x, pixel_y, source, true)) {
            result.written_pixels = 1U;
        }
        return result;
    }
    if (write_fragment(target, pixel_x, pixel_y, source)) {
        result.written_pixels = 1U;
    }
    if (depth.writable_byte < kGsLocalMemoryBytes) {
        write_depth_fragment_at(target, depth.writable_byte, vertex.z);
    }
    return result;
}

// Lines follow the GS diamond-exit rule: a pixel is drawn when the line
// leaves the diamond inscribed in it. The major axis steps one pixel at a
// time; a decision variable in 1/512 pixel units tracks the minor axis.
// Antialiased lines (AA1) split coverage between the two nearest pixels.
GsRasterResult GsSoftwareRasterizer::draw_solid_line(const GsRasterTarget& target,
                                                     const GsRasterVertex& first,
                                                     const GsRasterVertex& second, const bool gouraud,
                                                     const bool antialias,
                                                     const GsRasterAlphaBlend alpha_blend,
                                                     const GsRasterFog fog) noexcept {
    if (aliased_texture_snapshot_batch_state_) {
        // A solid primitive is a draw-batch boundary for the texture cache.
        invalidate_texture_cache();
    }
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    const AdjustedVertex a = adjust_vertex(first, target);
    const AdjustedVertex b = adjust_vertex(second, target);
    const int delta_x_12_4 = b.x - a.x;
    const int delta_y_12_4 = b.y - a.y;
    if (delta_x_12_4 == 0 && delta_y_12_4 == 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    const float x0 = static_cast<float>(a.x) * 0.0625F;
    const float y0 = static_cast<float>(a.y) * 0.0625F;
    const float x1 = static_cast<float>(b.x) * 0.0625F;
    const float y1 = static_cast<float>(b.y) * 0.0625F;
    const float delta_x = x1 - x0;
    const float delta_y = y1 - y0;
    const bool step_x = std::abs(delta_x) >= std::abs(delta_y);
    const bool positive_x = delta_x >= 0.0F;
    const bool positive_y = delta_y >= 0.0F;
    const int direction_x = positive_x ? 1 : -1;
    const int direction_y = positive_y ? 1 : -1;
    float rounded_x0 = std::floor(x0 + 0.5F);
    float rounded_y0 = std::floor(y0 + 0.5F);
    float rounded_x1 = std::floor(x1 + 0.5F);
    float rounded_y1 = std::floor(y1 + 0.5F);

    // True when the line leaves the diamond of the pixel nearest an endpoint
    // in its direction of travel, so that pixel belongs to the line.
    const auto endpoint_exits_diamond = [step_x, positive_x, positive_y](const float offset_x,
                                                                         const float offset_y) {
        const float distance = std::abs(offset_x) + std::abs(offset_y);
        if (distance < 0.5F) {
            return false;
        }
        if (step_x) {
            const bool exits_in_direction = positive_x ? offset_x > 0.0F : offset_x < 0.0F;
            return exits_in_direction && (distance > 0.5F || offset_y >= 0.0F);
        }
        const bool exits_in_direction = positive_y ? offset_y > 0.0F : offset_y < 0.0F;
        return exits_in_direction && (distance > 0.5F || offset_x >= 0.0F);
    };
    const bool draw_first = !endpoint_exits_diamond(x0 - rounded_x0, y0 - rounded_y0);
    const bool draw_last = endpoint_exits_diamond(x1 - rounded_x1, y1 - rounded_y1);
    if (!draw_first) {
        rounded_x0 += step_x ? static_cast<float>(direction_x) : 0.0F;
        rounded_y0 += step_x ? 0.0F : static_cast<float>(direction_y);
    }
    if (!draw_last) {
        rounded_x1 -= step_x ? static_cast<float>(direction_x) : 0.0F;
        rounded_y1 -= step_x ? 0.0F : static_cast<float>(direction_y);
    }
    const int raster_x0 = static_cast<int>(rounded_x0);
    const int raster_y0 = static_cast<int>(rounded_y0);
    const int raster_x1 = static_cast<int>(rounded_x1);
    const int raster_y1 = static_cast<int>(rounded_y1);
    const int remaining_major =
        step_x ? (raster_x1 - raster_x0) * direction_x : (raster_y1 - raster_y0) * direction_y;
    if (remaining_major < 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    const float major_delta = std::abs(step_x ? delta_x : delta_y);
    const int scale_d = static_cast<int>(major_delta * 512.0F);
    if (scale_d == 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }
    const float scale_d_float = static_cast<float>(scale_d);
    const int delta_d = static_cast<int>((step_x ? delta_y : delta_x) * 512.0F);
    int decision_d = static_cast<int>((step_x ? y0 - rounded_y0 : x0 - rounded_x0) * scale_d_float);
    int raster_x = raster_x0;
    int raster_y = raster_y0;
    const float prestep = step_x ? static_cast<float>(direction_x) * (rounded_x0 - x0)
                                 : static_cast<float>(direction_y) * (rounded_y0 - y0);
    decision_d += static_cast<int>(static_cast<float>(delta_d) * prestep);
    // Moves one pixel along the minor axis (sign +1 or -1).
    const auto step_dependent = [&decision_d, &raster_x, &raster_y, step_x, scale_d](const int sign) {
        decision_d -= sign * scale_d;
        raster_x += step_x ? 0 : sign;
        raster_y += step_x ? sign : 0;
    };
    // Bring the decision variable into [-scale/2, scale/2).
    while (decision_d >= scale_d / 2) {
        step_dependent(1);
    }
    while (decision_d < -scale_d / 2) {
        step_dependent(-1);
    }

    auto mutation_batch = memory_.begin_mutation_batch();
    GsRasterResult result{GsRasterStatus::Drawn, 0U, 0U};
    const auto interpolate_z = [&first, &second](const double progress) {
        const long double z =
            static_cast<long double>(first.z) +
            (static_cast<long double>(second.z) - static_cast<long double>(first.z)) *
                static_cast<long double>(progress);
        return static_cast<std::uint32_t>(std::llround(std::clamp(z, 0.0L, 4294967295.0L)));
    };
    // FOG blends towards FOGCOL: 255 keeps the colour, 0 is pure fog.
    const auto fog_channel = [](const std::uint8_t fog_color, const std::uint8_t value,
                                const std::uint8_t factor) {
        const int product = (static_cast<int>(value) - static_cast<int>(fog_color)) * factor;
        const int contribution = product >= 0 ? product / 256 : -((255 - product) / 256);
        return static_cast<std::uint8_t>(std::clamp(fog_color + contribution, 0, 255));
    };
    const auto inside_scissor = [&target](const int x, const int y) {
        return x >= static_cast<int>(target.scissor.left) && x <= static_cast<int>(target.scissor.right) &&
               y >= static_cast<int>(target.scissor.top) && y <= static_cast<int>(target.scissor.bottom);
    };

    float major_progress = prestep;
    for (int major_step = 0; major_step <= remaining_major; ++major_step) {
        const double progress = static_cast<double>(major_progress / major_delta);
        auto interpolated_color = second.color;
        if (gouraud) {
            interpolated_color.red =
                interpolate_line_channel(first.color.red, interpolated_color.red, progress);
            interpolated_color.green =
                interpolate_line_channel(first.color.green, interpolated_color.green, progress);
            interpolated_color.blue =
                interpolate_line_channel(first.color.blue, interpolated_color.blue, progress);
            interpolated_color.alpha =
                interpolate_line_channel(first.color.alpha, interpolated_color.alpha, progress);
        }
        const std::uint32_t z = clamp_depth_to_target(target, interpolate_z(progress));
        const std::uint8_t fog_factor = interpolate_line_channel(first.fog, second.fog, progress);

        const auto emit_fragment = [this, &target, &alpha_blend, antialias, &result, &inside_scissor,
                                    &fog, &fog_channel, interpolated_color, fog_factor,
                                    z](const std::int32_t x, const std::int32_t y,
                                       const std::uint32_t coverage) {
            if (!inside_scissor(x, y)) {
                return;
            }
            ++result.covered_pixels;
            auto source = interpolated_color;
            if (antialias && (!alpha_blend.enabled || source.alpha == 0x80U)) {
                // AA1: coverage replaces alpha (16-bit coverage to 0..127).
                source.alpha = static_cast<std::uint8_t>(coverage >> 9U);
            }
            const auto pixel_x = static_cast<std::uint32_t>(x);
            const auto pixel_y = static_cast<std::uint32_t>(y);
            if (!destination_alpha_test_passes(target, pixel_x, pixel_y)) {
                return;
            }
            const auto alpha_decision = alpha_test_decision(target.alpha_test, source.alpha);
            if (alpha_decision == 0U) {
                return;
            }
            if (alpha_decision == 2U) {
                // ZB_ONLY: the failed test still updates depth.
                const DepthTestResult depth = depth_test(target, pixel_x, pixel_y, z);
                if (!antialias && depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
                    write_depth_fragment_at(target, depth.writable_byte, z);
                }
                return;
            }
            const DepthTestResult depth = depth_test(target, pixel_x, pixel_y, z);
            if (!depth.passes) {
                return;
            }
            if (fog.enabled) {
                source.red = fog_channel(fog.color.red, source.red, fog_factor);
                source.green = fog_channel(fog.color.green, source.green, fog_factor);
                source.blue = fog_channel(fog.color.blue, source.blue, fog_factor);
            }
            source = apply_alpha_blend(target, alpha_blend, source, pixel_x, pixel_y).value_or(source);
            if (alpha_decision == 1U) {
                // FB_ONLY: colour without depth.
                if (write_fragment(target, pixel_x, pixel_y, source)) {
                    ++result.written_pixels;
                }
                return;
            }
            if (alpha_decision == 3U) {
                // RGB_ONLY: keep the destination alpha, no depth.
                if (write_fragment(target, pixel_x, pixel_y, source, true)) {
                    ++result.written_pixels;
                }
                return;
            }
            if (write_fragment(target, pixel_x, pixel_y, source)) {
                ++result.written_pixels;
            }
            // Antialiased lines never write Z.
            if (!antialias && depth.writable_byte < kGsLocalMemoryBytes) {
                write_depth_fragment_at(target, depth.writable_byte, z);
            }
        };

        if (antialias) {
            const float coverage_float =
                std::abs(static_cast<float>(decision_d) / scale_d_float) * 65535.0F;
            const int coverage = std::min(static_cast<int>(coverage_float), 65535);
            const int dependent_offset = decision_d >= 0 ? 1 : -1;
            emit_fragment(raster_x, raster_y, 65535U - static_cast<std::uint32_t>(coverage));
            emit_fragment(raster_x + (step_x ? 0 : dependent_offset),
                          raster_y + (step_x ? dependent_offset : 0),
                          static_cast<std::uint32_t>(coverage));
        } else {
            emit_fragment(raster_x, raster_y, 65535U);
        }
        if (major_step == remaining_major) {
            break;
        }
        major_progress += 1.0F;
        decision_d += delta_d;
        raster_x += step_x ? direction_x : 0;
        raster_y += step_x ? 0 : direction_y;
        const bool positive_dependent = step_x ? positive_y : positive_x;
        if (positive_dependent) {
            if (decision_d >= scale_d / 2) {
                step_dependent(1);
            }
        } else if (decision_d < -scale_d / 2) {
            step_dependent(-1);
        }
    }

    if (result.covered_pixels == 0U) {
        result.status = GsRasterStatus::Empty;
        return result;
    }
    ++threading_statistics_.serial_primitives;
    ++threading_statistics_.small_primitive_fallbacks;
    return result;
}

// Flat sprite: colour from the second vertex, Z interpolated across X.
GsRasterResult GsSoftwareRasterizer::draw_solid_sprite(const GsRasterTarget& target,
                                                       const GsRasterVertex& first,
                                                       const GsRasterVertex& second,
                                                       const GsRasterAlphaBlend alpha_blend) noexcept {
    if (aliased_texture_snapshot_batch_state_) {
        // A solid primitive is a draw-batch boundary for the texture cache.
        invalidate_texture_cache();
    }
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    const AdjustedVertex a = adjust_vertex(first, target);
    const AdjustedVertex b = adjust_vertex(second, target);
    const int minimum_x = std::min(a.x, b.x);
    const int maximum_x = std::max(a.x, b.x);
    const int minimum_y = std::min(a.y, b.y);
    const int maximum_y = std::max(a.y, b.y);
    if (minimum_x == maximum_x || minimum_y == maximum_y) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }
    // Pixels whose centres (x * 16 + 8) lie in [minimum, maximum).
    const int left = ceil_divide(minimum_x - 8, 16);
    int right = ceil_divide(maximum_x - 8, 16) - 1;
    const int top = ceil_divide(minimum_y - 8, 16);
    int bottom = ceil_divide(maximum_y - 8, 16) - 1;
    const int clipped_left = clamp_coordinate(left, target.scissor.left, target.scissor.right);
    const int clipped_right = clamp_coordinate(right, target.scissor.left, target.scissor.right);
    const int clipped_top = clamp_coordinate(top, target.scissor.top, target.scissor.bottom);
    const int clipped_bottom = clamp_coordinate(bottom, target.scissor.top, target.scissor.bottom);
    if (clipped_right < clipped_left || clipped_bottom < clipped_top) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    const int delta_x = b.x - a.x;
    auto mutation_batch = memory_.begin_mutation_batch();
    const auto rasterize = [this, &target, &first, &second, &alpha_blend, &a,
                            delta_x](const RasterBounds bounds, RasterTileCounters& counters) {
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int sample_x = x * 16 + 8;
                ++counters.covered_pixels;
                if (!destination_alpha_test_passes(target, static_cast<std::uint32_t>(x),
                                                   static_cast<std::uint32_t>(y))) {
                    continue;
                }
                const auto decision = alpha_test_decision(target.alpha_test, second.color.alpha);
                if (decision == 0U) {
                    continue;
                }
                const unsigned int z = static_cast<unsigned int>(std::lround(
                    static_cast<double>(first.z) +
                    (static_cast<double>(second.z) - static_cast<double>(first.z)) *
                        static_cast<double>(sample_x - a.x) / static_cast<double>(delta_x)));
                if (decision == 2U) {
                    // ZB_ONLY: the failed test still updates depth.
                    const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                             static_cast<std::uint32_t>(y), z);
                    if (depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
                        write_depth_fragment_at(target, depth.writable_byte, z);
                    }
                    continue;
                }
                const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                         static_cast<std::uint32_t>(y), z);
                if (!depth.passes) {
                    continue;
                }
                auto source = second.color;
                const auto blended = apply_alpha_blend(target, alpha_blend, source,
                                                       static_cast<std::uint32_t>(x),
                                                       static_cast<std::uint32_t>(y));
                source = blended.value_or(source);
                if (decision == 1U) {
                    // FB_ONLY: colour without depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), source)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (decision == 3U) {
                    // RGB_ONLY: keep the destination alpha, no depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), source, true)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (write_fragment(target, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   source)) {
                    ++counters.written_pixels;
                }
                if (depth.writable_byte < kGsLocalMemoryBytes) {
                    write_depth_fragment_at(target, depth.writable_byte, z);
                }
            }
        }
    };

    GsRasterResult result{GsRasterStatus::Drawn, 0U, 0U};
    const RasterBounds bounds{clipped_left, clipped_right, clipped_top, clipped_bottom};
    const RasterTileGrid grid = raster_tile_grid(bounds);
    const bool parallel_candidate =
        tile_pool_ != nullptr &&
        raster_pixel_count(bounds) >= threading_config_.minimum_candidate_pixels &&
        raster_tile_count(grid) >=
            (static_cast<unsigned long long>(static_cast<std::uint32_t>(tile_pool_->worker_count())) + 1U) * 2U;
    bool hazard_free = false;
    if (parallel_candidate) {
        GsBlockFootprint framebuffer_footprint{};
        hazard_free = mark_parallel_outputs(target, bounds, framebuffer_footprint);
    }
    if (hazard_free) {
        std::array<RasterTileCounters, 9> counters{};
        auto run_tile = [&grid, &rasterize, &counters](const std::size_t tile, const std::uint32_t slot) {
            rasterize(raster_tile_bounds(grid, tile), counters[slot]);
        };
        tile_pool_->run(static_cast<std::size_t>(grid.columns) * grid.rows, run_tile);
        for (const auto& tile_counters : counters) {
            result.covered_pixels += tile_counters.covered_pixels;
            result.written_pixels += tile_counters.written_pixels;
        }
        ++threading_statistics_.parallel_primitives;
        threading_statistics_.parallel_tiles += raster_tile_count(grid);
    } else {
        RasterTileCounters counters{};
        rasterize(bounds, counters);
        result.covered_pixels = counters.covered_pixels;
        result.written_pixels = counters.written_pixels;
        ++threading_statistics_.serial_primitives;
        if (!parallel_candidate) {
            ++threading_statistics_.small_primitive_fallbacks;
        } else {
            ++threading_statistics_.hazard_fallbacks;
        }
    }
    return result;
}

// Flat or Gouraud triangle. Pixel centres (x * 16 + 8) are tested against
// the three edge functions with the top-left fill rule; colour and Z are
// interpolated with barycentric weights.
GsRasterResult GsSoftwareRasterizer::draw_solid_triangle(const GsRasterTarget& target,
                                                         const GsRasterVertex& first,
                                                         const GsRasterVertex& second,
                                                         const GsRasterVertex& third, const bool gouraud,
                                                         const GsRasterAlphaBlend alpha_blend) noexcept {
    if (aliased_texture_snapshot_batch_state_) {
        // A solid primitive is a draw-batch boundary for the texture cache.
        invalidate_texture_cache();
    }
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    const AdjustedVertex a = adjust_vertex(first, target);
    const AdjustedVertex b = adjust_vertex(second, target);
    const AdjustedVertex c = adjust_vertex(third, target);
    const long long area = edge_function(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }
    // Pixels whose centres lie inside the vertices' bounding box.
    const int left = ceil_divide(std::min({a.x, b.x, c.x}) - 8, 16);
    const int right = floor_divide(std::max({a.x, b.x, c.x}) - 8, 16);
    const int top = ceil_divide(std::min({a.y, b.y, c.y}) - 8, 16);
    const int bottom = floor_divide(std::max({a.y, b.y, c.y}) - 8, 16);
    const int clipped_left = clamp_coordinate(left, target.scissor.left, target.scissor.right);
    const int clipped_right = clamp_coordinate(right, target.scissor.left, target.scissor.right);
    const int clipped_top = clamp_coordinate(top, target.scissor.top, target.scissor.bottom);
    const int clipped_bottom = clamp_coordinate(bottom, target.scissor.top, target.scissor.bottom);
    if (clipped_right < clipped_left || clipped_bottom < clipped_top) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    const double inverse_area = 1.0 / static_cast<double>(area);
    const bool ab_inclusive = is_top_left_edge(a.x, a.y, b.x, b.y, area);
    const bool bc_inclusive = is_top_left_edge(b.x, b.y, c.x, c.y, area);
    const bool ca_inclusive = is_top_left_edge(c.x, c.y, a.x, a.y, area);
    auto mutation_batch = memory_.begin_mutation_batch();
    const auto rasterize = [this, &target, &first, &second, &third, &alpha_blend, &a, &b, &c, area,
                            inverse_area, ab_inclusive, bc_inclusive, ca_inclusive,
                            gouraud](const RasterBounds bounds, RasterTileCounters& counters) {
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            const int sample_y = y * 16 + 8;
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int sample_x = x * 16 + 8;
                const long long ab = edge_function(a.x, a.y, b.x, b.y, sample_x, sample_y);
                const long long bc = edge_function(b.x, b.y, c.x, c.y, sample_x, sample_y);
                const long long ca = edge_function(c.x, c.y, a.x, a.y, sample_x, sample_y);
                const bool inside = edge_covers_sample(ab, ab_inclusive, area) &&
                                    edge_covers_sample(bc, bc_inclusive, area) &&
                                    edge_covers_sample(ca, ca_inclusive, area);
                if (!inside) {
                    continue;
                }
                ++counters.covered_pixels;
                const double wa = static_cast<double>(bc) * inverse_area;
                const double wb = static_cast<double>(ca) * inverse_area;
                const double wc = static_cast<double>(ab) * inverse_area;
                GsRgba8 source;
                if (gouraud) {
                    source = GsRgba8{barycentric_channel(first.color.red, second.color.red,
                                                          third.color.red, wa, wb, wc),
                                      barycentric_channel(first.color.green, second.color.green,
                                                          third.color.green, wa, wb, wc),
                                      barycentric_channel(first.color.blue, second.color.blue,
                                                          third.color.blue, wa, wb, wc),
                                      barycentric_channel(first.color.alpha, second.color.alpha,
                                                          third.color.alpha, wa, wb, wc)};
                } else {
                    source = third.color;
                }
                if (!destination_alpha_test_passes(target, static_cast<std::uint32_t>(x),
                                                   static_cast<std::uint32_t>(y))) {
                    continue;
                }
                const auto decision = alpha_test_decision(target.alpha_test, source.alpha);
                if (decision == 0U) {
                    continue;
                }
                const unsigned int z = static_cast<unsigned int>(
                    std::lround(static_cast<double>(first.z) * wa + static_cast<double>(second.z) * wb +
                                static_cast<double>(third.z) * wc));
                if (decision == 2U) {
                    // ZB_ONLY: the failed test still updates depth.
                    const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                             static_cast<std::uint32_t>(y), z);
                    if (depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
                        write_depth_fragment_at(target, depth.writable_byte, z);
                    }
                    continue;
                }
                const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                         static_cast<std::uint32_t>(y), z);
                if (!depth.passes) {
                    continue;
                }
                const auto blended = apply_alpha_blend(target, alpha_blend, source,
                                                       static_cast<std::uint32_t>(x),
                                                       static_cast<std::uint32_t>(y));
                source = blended.value_or(source);
                if (decision == 1U) {
                    // FB_ONLY: colour without depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), source)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (decision == 3U) {
                    // RGB_ONLY: keep the destination alpha, no depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), source, true)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (write_fragment(target, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   source)) {
                    ++counters.written_pixels;
                }
                if (depth.writable_byte < kGsLocalMemoryBytes) {
                    write_depth_fragment_at(target, depth.writable_byte, z);
                }
            }
        }
    };

    GsRasterResult result{GsRasterStatus::Drawn, 0U, 0U};
    const RasterBounds bounds{clipped_left, clipped_right, clipped_top, clipped_bottom};
    const RasterTileGrid grid = raster_tile_grid(bounds);
    const bool parallel_candidate =
        tile_pool_ != nullptr &&
        raster_pixel_count(bounds) >= threading_config_.minimum_candidate_pixels &&
        raster_tile_count(grid) >=
            (static_cast<unsigned long long>(static_cast<std::uint32_t>(tile_pool_->worker_count())) + 1U) * 2U;
    bool hazard_free = false;
    if (parallel_candidate) {
        GsBlockFootprint framebuffer_footprint{};
        hazard_free = mark_parallel_outputs(target, bounds, framebuffer_footprint);
    }
    if (hazard_free) {
        std::array<RasterTileCounters, 9> counters{};
        auto run_tile = [&grid, &rasterize, &counters](const std::size_t tile, const std::uint32_t slot) {
            rasterize(raster_tile_bounds(grid, tile), counters[slot]);
        };
        tile_pool_->run(static_cast<std::size_t>(grid.columns) * grid.rows, run_tile);
        for (const auto& tile_counters : counters) {
            result.covered_pixels += tile_counters.covered_pixels;
            result.written_pixels += tile_counters.written_pixels;
        }
        ++threading_statistics_.parallel_primitives;
        threading_statistics_.parallel_tiles += raster_tile_count(grid);
    } else {
        RasterTileCounters counters{};
        rasterize(bounds, counters);
        result.covered_pixels = counters.covered_pixels;
        result.written_pixels = counters.written_pixels;
        ++threading_statistics_.serial_primitives;
        if (!parallel_candidate) {
            ++threading_statistics_.small_primitive_fallbacks;
        } else {
            ++threading_statistics_.hazard_fallbacks;
        }
    }
    return result;
}

// Textured sprite: UV (fixed 10.4) interpolated linearly across the sprite
// from the first to the second vertex; Q, colour, fog and Z come from the
// second vertex. Pixels are sampled at their centres (12.4 + 8).
GsRasterResult GsSoftwareRasterizer::draw_textured_sprite(const GsRasterTarget& target,
                                                          const GsRasterTexture& texture,
                                                          const GsRasterVertex& first,
                                                          const GsRasterVertex& second) noexcept {
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    if (texture.width == 0U || texture.height == 0U ||
        !texture_format_supported(texture.surface.pixel_storage_mode)) {
        return GsRasterResult{GsRasterStatus::UnsupportedFramebufferFormat, 0U, 0U};
    }
    if (!texture.alpha_blend.supported()) {
        return GsRasterResult{GsRasterStatus::UnsupportedAlphaBlend, 0U, 0U};
    }
    const AdjustedVertex a = adjust_vertex(first, target);
    const AdjustedVertex b = adjust_vertex(second, target);
    const int minimum_x = std::min(a.x, b.x);
    const int maximum_x = std::max(a.x, b.x);
    const int minimum_y = std::min(a.y, b.y);
    const int maximum_y = std::max(a.y, b.y);
    const int delta_x = b.x - a.x;
    const int delta_y = b.y - a.y;
    if (delta_x == 0 || delta_y == 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }
    // Pixels whose centres lie in [minimum, maximum).
    const int left = ceil_divide(minimum_x - 8, 16);
    int right = ceil_divide(maximum_x - 8, 16) - 1;
    const int top = ceil_divide(minimum_y - 8, 16);
    int bottom = ceil_divide(maximum_y - 8, 16) - 1;
    const int clipped_left = clamp_coordinate(left, target.scissor.left, target.scissor.right);
    const int clipped_right = clamp_coordinate(right, target.scissor.left, target.scissor.right);
    const int clipped_top = clamp_coordinate(top, target.scissor.top, target.scissor.bottom);
    const int clipped_bottom = clamp_coordinate(bottom, target.scissor.top, target.scissor.bottom);
    if (clipped_left > clipped_right || clipped_top > clipped_bottom) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    const long long delta_u = static_cast<long long>(second.u_10_4) - first.u_10_4;
    const long long delta_v = static_cast<long long>(second.v_10_4) - first.v_10_4;
    const double lod =
        texture_lod(texture, static_cast<double>(std::bit_cast<float>(second.q_bits)));
    const GsRasterClamp clamp = decode_gs_raster_clamp(texture.clamp);
    GsBlockFootprint sprite_texture_reads{};
    const RasterBounds bounds{clipped_left, clipped_right, clipped_top, clipped_bottom};
    const bool sprite_reads_known =
        mark_sprite_texture_reads(texture, a, b, first, second, bounds, sprite_texture_reads);
    const GsLocalMemory& texture_memory = texture_source_for_draw(
        target, texture, TextureDrawClass::Sprite,
        sprite_reads_known ? &sprite_texture_reads : nullptr, clipped_left, clipped_right,
        clipped_top, clipped_bottom);
    auto mutation_batch = memory_.begin_mutation_batch();

    const auto rasterize = [this, &texture_memory, &target, &texture, &first, &second, &clamp, &a,
                            delta_x, delta_y, delta_u, delta_v,
                            lod](const RasterBounds bounds, RasterTileCounters& counters) {
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            const int sample_y = y * 16;
            const long long v_fixed =
                first.v_10_4 + static_cast<long long>(sample_y - a.y) * delta_v / delta_y;
            const double texture_y = static_cast<double>(v_fixed) * 0.0625;
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int sample_x = x * 16;
                const long long u_fixed =
                    first.u_10_4 + static_cast<long long>(sample_x - a.x) * delta_u / delta_x;
                const double texture_x = static_cast<double>(u_fixed) * 0.0625;
                const auto source = shade_texture_fragment(texture_memory, target, texture, clamp,
                                                           texture_x, texture_y, lod, second.color);
                if (!source) {
                    return false;
                }
                ++counters.covered_pixels;
                if (!destination_alpha_test_passes(target, static_cast<std::uint32_t>(x),
                                                   static_cast<std::uint32_t>(y))) {
                    continue;
                }
                const auto decision = alpha_test_decision(target.alpha_test, source->alpha);
                if (decision == 0U) {
                    continue;
                }
                const unsigned int z = static_cast<unsigned int>(std::lround(
                    static_cast<double>(first.z) +
                    (static_cast<double>(second.z) - static_cast<double>(first.z)) *
                        static_cast<double>(sample_x - a.x) / static_cast<double>(delta_x)));
                if (decision == 2U) {
                    // ZB_ONLY: the failed test still updates depth.
                    const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                             static_cast<std::uint32_t>(y), z);
                    if (depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
                        write_depth_fragment_at(target, depth.writable_byte, z);
                    }
                    continue;
                }
                const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                         static_cast<std::uint32_t>(y), z);
                if (!depth.passes) {
                    continue;
                }
                auto color = *source;
                const auto blended = apply_alpha_blend(target, texture.alpha_blend, color,
                                                       static_cast<std::uint32_t>(x),
                                                       static_cast<std::uint32_t>(y));
                color = blended.value_or(color);
                if (decision == 1U) {
                    // FB_ONLY: colour without depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), color)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (decision == 3U) {
                    // RGB_ONLY: keep the destination alpha, no depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), color, true)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (write_fragment(target, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   color)) {
                    ++counters.written_pixels;
                }
                if (depth.writable_byte < kGsLocalMemoryBytes) {
                    write_depth_fragment_at(target, depth.writable_byte, z);
                }
            }
        }
        return true;
    };

    GsRasterResult result{GsRasterStatus::Drawn, 0U, 0U};
    const RasterTileGrid grid =
        raster_tile_grid(bounds);
    const bool parallel_candidate =
        tile_pool_ != nullptr &&
        static_cast<unsigned long long>(clipped_right - clipped_left + 1) *
                static_cast<unsigned long long>(clipped_bottom - clipped_top + 1) >=
            threading_config_.minimum_candidate_pixels &&
        static_cast<unsigned long long>(grid.columns) * grid.rows >=
            (static_cast<unsigned long long>(static_cast<std::uint32_t>(tile_pool_->worker_count())) + 1U) * 2U;
    bool parallel_safe = false;
    if (parallel_candidate) {
        GsBlockFootprint framebuffer_footprint{};
        GsBlockFootprint texture_footprint{};
        const bool output_safe = mark_parallel_outputs(target, bounds, framebuffer_footprint);
        const bool input_safe = mark_parallel_texture(texture, texture_footprint);
        parallel_safe = output_safe && input_safe &&
                        !footprints_overlap(framebuffer_footprint, texture_footprint);
    }
    if (parallel_safe) {
        std::array<RasterTileCounters, 9> counters{};
        std::atomic_bool unsupported{false};
        auto run_tile = [&grid, &rasterize, &counters, &unsupported](const std::size_t tile,
                                                                     const std::uint32_t slot) {
            if (!rasterize(raster_tile_bounds(grid, tile), counters[slot])) {
                unsupported.store(true, std::memory_order_relaxed);
            }
        };
        tile_pool_->run(static_cast<std::size_t>(grid.columns) * grid.rows, run_tile);
        for (const auto& tile_counters : counters) {
            result.covered_pixels += tile_counters.covered_pixels;
            result.written_pixels += tile_counters.written_pixels;
        }
        ++threading_statistics_.parallel_primitives;
        threading_statistics_.parallel_tiles += raster_tile_count(grid);
        if (unsupported.load(std::memory_order_relaxed)) {
            result.status = GsRasterStatus::UnsupportedFramebufferFormat;
        }
    } else {
        RasterTileCounters counters{};
        const bool supported = rasterize(bounds, counters);
        result.covered_pixels = counters.covered_pixels;
        result.written_pixels = counters.written_pixels;
        if (!supported) {
            result.status = GsRasterStatus::UnsupportedFramebufferFormat;
        }
        ++threading_statistics_.serial_primitives;
        if (!parallel_candidate) {
            ++threading_statistics_.small_primitive_fallbacks;
        } else {
            ++threading_statistics_.hazard_fallbacks;
        }
    }
    return result;
}

// Textured triangle: STQ is interpolated with barycentric weights and
// divided per pixel (perspective-correct), then sampled through
// shade_texture_fragment. Colour is flat (third vertex) or Gouraud.
GsRasterResult GsSoftwareRasterizer::draw_textured_triangle(const GsRasterTarget& target,
                                                            const GsRasterTexture& texture,
                                                            const GsRasterVertex& first,
                                                            const GsRasterVertex& second,
                                                            const GsRasterVertex& third,
                                                            const bool gouraud) noexcept {
    const GsRasterStatus target_status = validate_target(target);
    if (target_status != GsRasterStatus::Drawn) {
        return GsRasterResult{target_status, 0U, 0U};
    }
    if (texture.width == 0U || texture.height == 0U ||
        (texture.surface.pixel_storage_mode != kGsPsmct32 &&
         texture.surface.pixel_storage_mode != kGsPsmct24 &&
         texture.surface.pixel_storage_mode != kGsPsmt8 &&
         texture.surface.pixel_storage_mode != kGsPsmt8h &&
         texture.surface.pixel_storage_mode != kGsPsmt4)) {
        return GsRasterResult{GsRasterStatus::UnsupportedFramebufferFormat, 0U, 0U};
    }
    if (!texture.alpha_blend.supported()) {
        return GsRasterResult{GsRasterStatus::UnsupportedAlphaBlend, 0U, 0U};
    }
    const AdjustedVertex a = adjust_vertex(first, target);
    const AdjustedVertex b = adjust_vertex(second, target);
    const AdjustedVertex c = adjust_vertex(third, target);
    const long long area = edge_function(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }
    // Pixels whose centres lie inside the vertices' bounding box.
    const int left = ceil_divide(std::min({a.x, b.x, c.x}) - 8, 16);
    const int right = floor_divide(std::max({a.x, b.x, c.x}) - 8, 16);
    const int top = ceil_divide(std::min({a.y, b.y, c.y}) - 8, 16);
    const int bottom = floor_divide(std::max({a.y, b.y, c.y}) - 8, 16);
    const int clipped_left = clamp_coordinate(left, target.scissor.left, target.scissor.right);
    const int clipped_right = clamp_coordinate(right, target.scissor.left, target.scissor.right);
    const int clipped_top = clamp_coordinate(top, target.scissor.top, target.scissor.bottom);
    const int clipped_bottom = clamp_coordinate(bottom, target.scissor.top, target.scissor.bottom);
    const double s0 = static_cast<double>(std::bit_cast<float>(first.s_bits));
    const double s1 = static_cast<double>(std::bit_cast<float>(second.s_bits));
    const double s2 = static_cast<double>(std::bit_cast<float>(third.s_bits));
    const double t0 = static_cast<double>(std::bit_cast<float>(first.t_bits));
    const double t1 = static_cast<double>(std::bit_cast<float>(second.t_bits));
    const double t2 = static_cast<double>(std::bit_cast<float>(third.t_bits));
    const double q0 = static_cast<double>(std::bit_cast<float>(first.q_bits));
    const double q1 = static_cast<double>(std::bit_cast<float>(second.q_bits));
    const double q2 = static_cast<double>(std::bit_cast<float>(third.q_bits));
    if (clipped_left > clipped_right || clipped_top > clipped_bottom ||
        !std::isfinite(s0 + s1 + s2 + t0 + t1 + t2 + q0 + q1 + q2)) {
        return GsRasterResult{GsRasterStatus::Empty, 0U, 0U};
    }

    auto mutation_batch = memory_.begin_mutation_batch();
    const double inverse_area = 1.0 / static_cast<double>(area);
    const bool bc_inclusive = is_top_left_edge(b.x, b.y, c.x, c.y, area);
    const bool ca_inclusive = is_top_left_edge(c.x, c.y, a.x, a.y, area);
    const bool ab_inclusive = is_top_left_edge(a.x, a.y, b.x, b.y, area);
    const GsRasterClamp clamp = decode_gs_raster_clamp(texture.clamp);
    const GsLocalMemory& texture_memory = texture_source_for_draw(
        target, texture, gouraud ? TextureDrawClass::GouraudTriangle : TextureDrawClass::FlatTriangle,
        nullptr, clipped_left, clipped_right, clipped_top, clipped_bottom);
    const auto rasterize = [this, &texture_memory, &target, &texture, &first, &second, &third, &clamp,
                            &a, &b, &c, area, inverse_area, s0, s1, s2, t0, t1, t2, q0, q1, q2,
                            bc_inclusive, ca_inclusive, ab_inclusive,
                            gouraud](const RasterBounds bounds, RasterTileCounters& counters) {
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            const int sample_y = y * 16 + 8;
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int sample_x = x * 16 + 8;
                const long long bc = edge_function(b.x, b.y, c.x, c.y, sample_x, sample_y);
                const long long ca = edge_function(c.x, c.y, a.x, a.y, sample_x, sample_y);
                const long long ab = edge_function(a.x, a.y, b.x, b.y, sample_x, sample_y);
                const bool inside = edge_covers_sample(bc, bc_inclusive, area) &&
                                    edge_covers_sample(ca, ca_inclusive, area) &&
                                    edge_covers_sample(ab, ab_inclusive, area);
                if (!inside) {
                    continue;
                }
                const double wa = static_cast<double>(bc) * inverse_area;
                const double wb = static_cast<double>(ca) * inverse_area;
                const double wc = static_cast<double>(ab) * inverse_area;
                const double q = q0 * wa + q1 * wb + q2 * wc;
                if (!std::isfinite(q) || std::abs(q) < 1.0e-12) {
                    continue;
                }
                const double s = (s0 * wa + s1 * wb + s2 * wc) / q;
                const double t = (t0 * wa + t1 * wb + t2 * wc) / q;
                if (!std::isfinite(s) || !std::isfinite(t)) {
                    continue;
                }
                const double texture_x = static_cast<double>(texture.width) * s;
                const double texture_y = static_cast<double>(texture.height) * t;
                const auto color =
                    gouraud ? GsRgba8{barycentric_channel(first.color.red, second.color.red,
                                                          third.color.red, wa, wb, wc),
                                      barycentric_channel(first.color.green, second.color.green,
                                                          third.color.green, wa, wb, wc),
                                      barycentric_channel(first.color.blue, second.color.blue,
                                                          third.color.blue, wa, wb, wc),
                                      barycentric_channel(first.color.alpha, second.color.alpha,
                                                          third.color.alpha, wa, wb, wc)}
                            : third.color;
                const auto source = shade_texture_fragment(texture_memory, target, texture, clamp,
                                                           texture_x, texture_y, texture_lod(texture, q),
                                                           color);
                if (!source) {
                    continue;
                }
                ++counters.covered_pixels;
                if (!destination_alpha_test_passes(target, static_cast<std::uint32_t>(x),
                                                   static_cast<std::uint32_t>(y))) {
                    continue;
                }
                const auto decision = alpha_test_decision(target.alpha_test, source->alpha);
                if (decision == 0U) {
                    continue;
                }
                const unsigned int z = static_cast<unsigned int>(
                    std::lround(static_cast<double>(first.z) * wa + static_cast<double>(second.z) * wb +
                                static_cast<double>(third.z) * wc));
                if (decision == 2U) {
                    // ZB_ONLY: the failed test still updates depth.
                    const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                             static_cast<std::uint32_t>(y), z);
                    if (depth.passes && depth.writable_byte < kGsLocalMemoryBytes) {
                        write_depth_fragment_at(target, depth.writable_byte, z);
                    }
                    continue;
                }
                const DepthTestResult depth = depth_test(target, static_cast<std::uint32_t>(x),
                                                         static_cast<std::uint32_t>(y), z);
                if (!depth.passes) {
                    continue;
                }
                auto color_out = *source;
                const auto blended = apply_alpha_blend(target, texture.alpha_blend, color_out,
                                                       static_cast<std::uint32_t>(x),
                                                       static_cast<std::uint32_t>(y));
                color_out = blended.value_or(color_out);
                if (decision == 1U) {
                    // FB_ONLY: colour without depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), color_out)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (decision == 3U) {
                    // RGB_ONLY: keep the destination alpha, no depth.
                    if (write_fragment(target, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), color_out, true)) {
                        ++counters.written_pixels;
                    }
                    continue;
                }
                if (write_fragment(target, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                   color_out)) {
                    ++counters.written_pixels;
                }
                if (depth.writable_byte < kGsLocalMemoryBytes) {
                    write_depth_fragment_at(target, depth.writable_byte, z);
                }
            }
        }
    };

    GsRasterResult result{GsRasterStatus::Drawn, 0U, 0U};
    const RasterBounds bounds{clipped_left, clipped_right, clipped_top, clipped_bottom};
    const RasterTileGrid grid = raster_tile_grid(bounds);
    const bool parallel_candidate =
        tile_pool_ != nullptr &&
        static_cast<unsigned long long>(clipped_right - clipped_left + 1) *
                static_cast<unsigned long long>(clipped_bottom - clipped_top + 1) >=
            threading_config_.minimum_candidate_pixels &&
        static_cast<unsigned long long>(grid.columns) * grid.rows >=
            (static_cast<unsigned long long>(static_cast<std::uint32_t>(tile_pool_->worker_count())) + 1U) * 2U;
    bool parallel_safe = false;
    if (parallel_candidate) {
        GsBlockFootprint framebuffer_footprint{};
        GsBlockFootprint texture_footprint{};
        const bool output_safe = mark_parallel_outputs(target, bounds, framebuffer_footprint);
        const bool input_safe = mark_parallel_texture(texture, texture_footprint);
        parallel_safe = output_safe && input_safe &&
                        !footprints_overlap(framebuffer_footprint, texture_footprint);
    }
    if (parallel_safe) {
        std::array<RasterTileCounters, 9> counters{};
        auto run_tile = [&grid, &rasterize, &counters](const std::size_t tile, const std::uint32_t slot) {
            rasterize(raster_tile_bounds(grid, tile), counters[slot]);
        };
        tile_pool_->run(static_cast<std::size_t>(grid.columns) * grid.rows, run_tile);
        for (const auto& tile_counters : counters) {
            result.covered_pixels += tile_counters.covered_pixels;
            result.written_pixels += tile_counters.written_pixels;
        }
        ++threading_statistics_.parallel_primitives;
        threading_statistics_.parallel_tiles += raster_tile_count(grid);
    } else {
        RasterTileCounters counters{};
        rasterize(bounds, counters);
        result.covered_pixels = counters.covered_pixels;
        result.written_pixels = counters.written_pixels;
        ++threading_statistics_.serial_primitives;
        if (!parallel_candidate) {
            ++threading_statistics_.small_primitive_fallbacks;
        } else {
            ++threading_statistics_.hazard_fallbacks;
        }
    }
    return result;
}

} // namespace takedown::guest
