#pragma once

#include "takedown/guest/gs_local_memory.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace takedown::guest {

struct GsRasterScissor {
    std::uint16_t left{};
    std::uint16_t right{};
    std::uint16_t top{};
    std::uint16_t bottom{};

    bool operator==(const GsRasterScissor&) const = default;
};

// TEST.ZTST depth-test methods. ps2tek lists 0=NEVER, 1=ALWAYS, 2=GEQUAL
// (pixel Z >= zbuffer Z passes), 3=GREATER (pixel Z > zbuffer Z passes).
// ZTE=0 disables the test entirely (always passes), matching the real GS
// where the depth unit is always active and the enable bit selects ALWAYS.
enum class GsRasterDepthMethod : std::uint8_t {
    Never = 0,
    Always = 1,
    GreaterEqual = 2,
    Greater = 3,
};

// TEST.ATE/ATST/AREF/AFAIL alpha-test state. ps2tek ATST order matches the
// raw two-bit/three-bit encoding directly.
enum class GsRasterAlphaTestMethod : std::uint8_t {
    Never = 0,
    Always = 1,
    Less = 2,
    LessEqual = 3,
    Equal = 4,
    GreaterEqual = 5,
    Greater = 6,
    NotEqual = 7,
};

struct GsRasterAlphaTest {
    bool enabled{};
    GsRasterAlphaTestMethod method{GsRasterAlphaTestMethod::Always};
    std::uint8_t reference{};
    // AFAIL: 0 = no updates, 1 = framebuffer only, 2 = zbuffer only,
    // 3 = RGB channels only.
    std::uint8_t fail_action{};

    bool operator==(const GsRasterAlphaTest&) const = default;
};

struct GsRasterDestinationAlphaTest {
    bool enabled{};
    // TEST.DATM: zero accepts destination alpha bit 7 clear; one accepts it set.
    bool match_one{};

    bool operator==(const GsRasterDestinationAlphaTest&) const = default;
};

struct GsRasterDepthState {
    GsLocalMemorySurface surface{};
    // True once a ZBUF register write has been observed. Until then no depth
    // reads, tests, or writes are performed; an unprogrammed z buffer has no
    // meaningful contents to compare against.
    bool known{};
    bool write_enabled{true};
    bool test_enabled{};
    GsRasterDepthMethod test_method{GsRasterDepthMethod::Always};

    bool operator==(const GsRasterDepthState&) const = default;
};

struct GsRasterTarget {
    GsLocalMemorySurface surface{};
    std::uint16_t offset_x_12_4{};
    std::uint16_t offset_y_12_4{};
    GsRasterScissor scissor{};
    std::uint32_t framebuffer_mask{};
    GsRasterDepthState depth{};
    GsRasterAlphaTest alpha_test{};
    GsRasterDestinationAlphaTest destination_alpha_test{};
    // COLCLAMP: 1 = 8-bit signed clamp (0..255), 0 = 8-bit AND wrap.
    bool colclamp{true};

    bool operator==(const GsRasterTarget&) const = default;
};

// Coordinates retain the GS's unsigned 12.4 screen-space representation.
// The rasterizer subtracts XYOFFSET before testing pixel centres.
struct GsRasterVertex {
    std::uint16_t x_12_4{};
    std::uint16_t y_12_4{};
    std::uint32_t z{};
    // XYZF carries F beside its 24-bit depth field. Keep the raw 0..255 fog
    // coefficient independent of Z so depth interpolation never consumes it.
    std::uint8_t fog{};
    GsRgba8 color{};
    // Fixed UV register values remain in their native unsigned 10.4 range,
    // while STQ sprites can produce signed coordinates outside the texture.
    // Keep the rasterizer's internal representation signed and unclipped so
    // CLAMP can apply repeat/region modes to the original coordinate.
    std::int32_t u_10_4{};
    std::int32_t v_10_4{};
    std::uint32_t s_bits{};
    std::uint32_t t_bits{};
    std::uint32_t q_bits{0x3f800000U};
};

enum class GsRasterBlendColorSelector : std::uint8_t {
    Source = 0,
    Destination = 1,
    Zero = 2,
    Reserved = 3,
};

enum class GsRasterBlendAlphaSelector : std::uint8_t {
    Source = 0,
    Destination = 1,
    Fixed = 2,
    Reserved = 3,
};

// GS ALPHA evaluates each RGB channel as (A - B) * C / 128 + D. Alpha is
// carried from the source fragment; FRAME.FBMSK is applied afterwards.
struct GsRasterAlphaBlend {
    bool enabled{};
    GsRasterBlendColorSelector a{GsRasterBlendColorSelector::Source};
    GsRasterBlendColorSelector b{GsRasterBlendColorSelector::Source};
    GsRasterBlendAlphaSelector c{GsRasterBlendAlphaSelector::Source};
    GsRasterBlendColorSelector d{GsRasterBlendColorSelector::Source};
    std::uint8_t fixed_alpha{};

    [[nodiscard]] constexpr bool supported() const noexcept {
        return !enabled || (a != GsRasterBlendColorSelector::Reserved &&
                            b != GsRasterBlendColorSelector::Reserved &&
                            c != GsRasterBlendAlphaSelector::Reserved &&
                            d != GsRasterBlendColorSelector::Reserved);
    }

    // Returns true only when the complete RGB equation is provably the source
    // colour without reading the destination. This includes GS reset ALPHA=0:
    // (Cs - Cs) * As / 128 + Cs.
    [[nodiscard]] constexpr bool source_copy_without_destination() const noexcept {
        if (!enabled) {
            return true;
        }
        if (!supported() || d != GsRasterBlendColorSelector::Source) {
            return false;
        }
        return a == b || (c == GsRasterBlendAlphaSelector::Fixed && fixed_alpha == 0U);
    }

    bool operator==(const GsRasterAlphaBlend&) const = default;
};

// PRIM.FGE applies FOGCOL to RGB after texture/color combination and before
// ALPHA blending. Alpha is deliberately absent from the fog colour equation.
struct GsRasterFog {
    bool enabled{};
    GsRgba8 color{};

    bool operator==(const GsRasterFog&) const = default;
};

[[nodiscard]] constexpr GsRasterAlphaBlend decode_gs_alpha_blend(const std::uint64_t alpha,
                                                                 const bool enabled) noexcept {
    return {
        .enabled = enabled,
        .a = static_cast<GsRasterBlendColorSelector>(alpha & 0x03U),
        .b = static_cast<GsRasterBlendColorSelector>((alpha >> 2U) & 0x03U),
        .c = static_cast<GsRasterBlendAlphaSelector>((alpha >> 4U) & 0x03U),
        .d = static_cast<GsRasterBlendColorSelector>((alpha >> 6U) & 0x03U),
        .fixed_alpha = static_cast<std::uint8_t>((alpha >> 32U) & 0xffU),
    };
}

struct GsRasterClamp {
    std::uint32_t wrap_s{};
    std::uint32_t wrap_t{};
    std::uint32_t min_u{};
    std::uint32_t max_u{};
    std::uint32_t min_v{};
    std::uint32_t max_v{};

    bool operator==(const GsRasterClamp&) const = default;
};

[[nodiscard]] constexpr GsRasterClamp decode_gs_raster_clamp(const std::uint64_t clamp) noexcept {
    return {
        .wrap_s = static_cast<std::uint32_t>(clamp & 0x03U),
        .wrap_t = static_cast<std::uint32_t>((clamp >> 2U) & 0x03U),
        .min_u = static_cast<std::uint32_t>((clamp >> 4U) & 0x3ffU),
        .max_u = static_cast<std::uint32_t>((clamp >> 14U) & 0x3ffU),
        .min_v = static_cast<std::uint32_t>((clamp >> 24U) & 0x3ffU),
        .max_v = static_cast<std::uint32_t>((clamp >> 34U) & 0x3ffU),
    };
}

// One immutable view of the GS's internal CLUT. Palette uploads copy local
// memory into this state when TEX0/TEX2.CLD requests a load; indexed draws
// retain the resulting snapshot so later palette loads cannot change work
// already submitted to the rasterizer.
struct GsRasterClut {
    std::array<GsRgba8, 256> entries{};
    std::uint64_t version{};
};

struct GsRasterTexture {
    GsLocalMemorySurface surface{};
    std::uint32_t width{};
    std::uint32_t height{};
    // TEX1 selects separate filters for magnification and minification.
    bool magnification_linear{};
    bool minification_linear{};
    // MMIN 3/5 interpolate between adjacent mip levels. The within-level
    // filter remains independently selected by minification_linear.
    bool mipmap_linear{};
    // TEX1.MXL controls the LOD sign used to select MMAG versus MMIN even when
    // MMIN does not address a mip chain. MIPTBP1/2 provide the local-memory
    // surface for levels one through six. The live path supports rounded
    // selection (MMIN 2/4) and adjacent-level interpolation (MMIN 3/5), while
    // retaining the raw MXL independently for correct base-level filtering.
    std::uint8_t maximum_lod_level{};
    std::array<GsLocalMemorySurface, 6> mipmap_surfaces{};
    std::uint8_t maximum_mip_level{};
    bool fixed_lod{};
    std::uint8_t lod_scale{};
    std::int16_t lod_bias_4{};
    // TEX0 palette fields. They are ignored by direct-colour textures. The
    // first persistent indexed path deliberately supports only CSM1
    // PSMCT32/24 palettes; unsupported combinations remain deferred by the
    // live renderer instead of being sampled with invented semantics.
    std::uint32_t clut_base_pointer{};
    std::uint32_t clut_pixel_storage_mode{kGsPsmct32};
    bool clut_storage_mode2{};
    std::uint8_t clut_start{};
    std::shared_ptr<const GsRasterClut> clut{};
    // TEXA.TA0/TA1 and AEM are needed when a 16-bit color or depth surface
    // is reinterpreted as a texture. Framebuffer reads deliberately use a
    // different expansion path.
    std::uint8_t alpha{0x80U};
    std::uint8_t alpha_high{0x80U};
    bool alpha_is_zero_for_black{};
    bool texture_alpha_enabled{true};
    bool modulate{};
    GsRasterAlphaBlend alpha_blend{};
    std::uint64_t clamp{};

    bool operator==(const GsRasterTexture&) const = default;
};

enum class GsRasterStatus : std::uint8_t {
    Drawn,
    Empty,
    UnsupportedFramebufferFormat,
    UnsupportedAlphaBlend,
    InvalidTarget,
};

struct GsRasterResult {
    GsRasterStatus status{GsRasterStatus::InvalidTarget};
    std::uint64_t covered_pixels{};
    std::uint64_t written_pixels{};
};

// Large primitives can be split into non-overlapping GS-block-aligned tiles.
// A value of zero for worker_threads selects a conservative host-dependent
// count (hardware concurrency minus the calling thread, capped internally).
// Disabling the pool is useful for exact serial/parallel verification.
struct GsRasterThreadingConfig {
    bool enabled{true};
    std::uint32_t worker_threads{};
    // 2026-09-02 race-route telemetry: 21.2M of 21.4M primitives fell back to
    // serial rasterization with the previous 32Ki-pixel threshold while the GS
    // worker queue saturated (9.3M producer waits, peak = queue capacity).
    // 8Ki pixels keeps the per-primitive tile-pool fan-out (measured 40-120us)
    // well below the ~32ns/pixel serial cost of the primitives it admits, and
    // the tile-count floor below still prevents sub-worker splits.
    std::uint64_t minimum_candidate_pixels{8U * 1024U};
};

struct GsRasterThreadingStatistics {
    std::uint32_t worker_threads{};
    std::uint64_t parallel_primitives{};
    std::uint64_t parallel_tiles{};
    std::uint64_t serial_primitives{};
    std::uint64_t small_primitive_fallbacks{};
    std::uint64_t hazard_fallbacks{};
};

[[nodiscard]] const char* to_string(GsRasterStatus status) noexcept;

// Decodes the context registers needed by every draw target. FRAME.FBP and
// ZBUF.ZBP are converted from their 2048-word units to GsLocalMemory's
// 256-byte blocks (the same physical 8192-byte stride). TEST contributes the
// alpha and depth test state; COLCLAMP the blend clamp mode. Omitting ZBUF
// (the common case for pre-depth frames and unit callers) leaves depth
// disabled; omitting TEST leaves both tests disabled.
[[nodiscard]] std::optional<GsRasterTarget>
decode_gs_raster_target(std::uint64_t frame, std::uint64_t xyoffset, std::uint64_t scissor,
                        std::optional<std::uint64_t> zbuf = std::nullopt,
                        std::optional<std::uint64_t> test = std::nullopt,
                        std::uint64_t colclamp = 1U) noexcept;

class GsSoftwareRasterizer {
  public:
    explicit GsSoftwareRasterizer(GsLocalMemory& memory,
                                  GsRasterThreadingConfig threading = {});
    ~GsSoftwareRasterizer() noexcept;

    GsSoftwareRasterizer(const GsSoftwareRasterizer&) = delete;
    GsSoftwareRasterizer& operator=(const GsSoftwareRasterizer&) = delete;
    GsSoftwareRasterizer(GsSoftwareRasterizer&&) = delete;
    GsSoftwareRasterizer& operator=(GsSoftwareRasterizer&&) = delete;

    // First-stage solid-color paths. Texture, depth, alpha-test, and blending
    // are deliberately separate later stages; these functions establish real
    // fixed-coordinate primitive coverage and FRAME writes. The optional
    // blend state lets solid draws participate in the ALPHA equation when
    // ABE is set; the gouraud flag interpolates vertex colors instead of
    // using the primitive's final (provoking) vertex colour.
    [[nodiscard]] GsRasterResult draw_solid_point(const GsRasterTarget& target,
                                                  const GsRasterVertex& vertex,
                                                  GsRasterAlphaBlend alpha_blend = {}) noexcept;
    [[nodiscard]] GsRasterResult draw_solid_line(const GsRasterTarget& target,
                                                 const GsRasterVertex& first,
                                                 const GsRasterVertex& second, bool gouraud = false,
                                                 bool antialias = false,
                                                 GsRasterAlphaBlend alpha_blend = {},
                                                 GsRasterFog fog = {}) noexcept;
    [[nodiscard]] GsRasterResult draw_solid_sprite(const GsRasterTarget& target,
                                                   const GsRasterVertex& first,
                                                   const GsRasterVertex& second,
                                                   GsRasterAlphaBlend alpha_blend = {}) noexcept;
    [[nodiscard]] GsRasterResult
    draw_solid_triangle(const GsRasterTarget& target, const GsRasterVertex& first,
                        const GsRasterVertex& second, const GsRasterVertex& third,
                        bool gouraud = false, GsRasterAlphaBlend alpha_blend = {}) noexcept;
    [[nodiscard]] GsRasterResult draw_textured_sprite(const GsRasterTarget& target,
                                                      const GsRasterTexture& texture,
                                                      const GsRasterVertex& first,
                                                      const GsRasterVertex& second) noexcept;
    [[nodiscard]] GsRasterResult
    draw_textured_triangle(const GsRasterTarget& target, const GsRasterTexture& texture,
                           const GsRasterVertex& first, const GsRasterVertex& second,
                           const GsRasterVertex& third, bool gouraud) noexcept;

    [[nodiscard]] const GsRasterThreadingStatistics& threading_statistics() const noexcept;
    void reset_threading_statistics() noexcept;

    // Reports whether every page in a complete base-level texture snapshot
    // would come from current local memory. An active framebuffer-feedback
    // epoch can retain older contents for some physical GS blocks. Unsupported
    // footprints conservatively return false while such an epoch is active.
    [[nodiscard]] bool
    local_memory_is_authoritative_for_texture(const GsLocalMemorySurface& surface,
                                              std::uint32_t width,
                                              std::uint32_t height) const noexcept;

    // Returns a conservative complete-page footprint for the canonical
    // base-level snapshot decoder. This deliberately includes the decoder's
    // narrow TBW=1 indexed-row convention and circular GS block wrapping.
    [[nodiscard]] std::optional<GsLocalMemoryBlockFootprint>
    texture_snapshot_block_footprint(const GsLocalMemorySurface& surface, std::uint32_t width,
                                     std::uint32_t height) const noexcept;

    // GS texture reads are cached across primitive boundaries in one draw-state
    // batch. TEXFLUSH, reset, and a real draw-batch boundary discard every
    // cached page; transfer writes need only discard the pages they overwrite.
    // Framebuffer writes within the same batch deliberately do neither so a
    // multi-primitive framebuffer-feedback pass sees one stable source image.
    void invalidate_texture_cache() noexcept;
    void invalidate_texture_cache_region(const GsLocalMemorySurface& surface, std::uint32_t x,
                                         std::uint32_t y, std::uint32_t width,
                                         std::uint32_t height) noexcept;

  private:
    enum class TextureDrawClass : std::uint8_t {
        Sprite,
        FlatTriangle,
        GouraudTriangle,
    };

    struct TextureBatchState {
        GsRasterTarget target{};
        GsRasterTexture texture{};
        TextureDrawClass draw_class{TextureDrawClass::Sprite};

        bool operator==(const TextureBatchState&) const = default;
    };

    struct DepthTestResult {
        bool passes{};
        // Less than kGsLocalMemoryBytes only when a passing fragment must
        // update a valid Z-buffer pixel. The address is resolved during the
        // read/test and reused after the framebuffer write so the GS swizzle
        // is not calculated twice.
        std::size_t writable_byte{kGsLocalMemoryBytes};
    };

    [[nodiscard]] GsRasterStatus validate_target(const GsRasterTarget& target) const noexcept;
    [[nodiscard]] bool write_fragment(const GsRasterTarget& target, std::uint32_t x,
                                      std::uint32_t y, GsRgba8 source,
                                      bool rgb_only = false) noexcept;
    void write_depth_fragment(const GsRasterTarget& target, std::uint32_t x, std::uint32_t y,
                              std::uint32_t z) noexcept;
    void write_depth_fragment_at(const GsRasterTarget& target, std::size_t byte,
                                 std::uint32_t z) noexcept;
    // TEST.ATE evaluation. Returns 0..3 carrying the AFAIL action when the
    // pixel fails, or -1-sentinel (4) when it passes and should proceed.
    [[nodiscard]] std::uint8_t alpha_test_decision(const GsRasterAlphaTest& alpha_test,
                                                   std::uint8_t alpha) const noexcept;
    [[nodiscard]] bool destination_alpha_test_passes(const GsRasterTarget& target,
                                                     std::uint32_t x,
                                                     std::uint32_t y) const noexcept;
    [[nodiscard]] DepthTestResult depth_test(const GsRasterTarget& target, std::uint32_t x,
                                             std::uint32_t y, std::uint32_t z) const noexcept;
    [[nodiscard]] std::optional<GsRgba8>
    apply_alpha_blend(const GsRasterTarget& target, const GsRasterAlphaBlend& blend,
                      const GsRgba8& source, std::uint32_t x, std::uint32_t y) const noexcept;
    [[nodiscard]] std::optional<GsRgba8>
    shade_texture_fragment(const GsLocalMemory& texture_memory, const GsRasterTarget& target,
                           const GsRasterTexture& texture, const GsRasterClamp& clamp,
                           double texture_x, double texture_y, double lod,
                           GsRgba8 color) const noexcept;
    // A cached GS texture page retains its contents while later primitives in
    // the same draw-state batch write back into local memory. Return ordinary
    // backing for disjoint draws, or populate/reuse the stable aliased source.
    [[nodiscard]] const GsLocalMemory&
    texture_source_for_draw(const GsRasterTarget& target, const GsRasterTexture& texture,
                            TextureDrawClass draw_class,
                            const std::bitset<kGsLocalMemoryBlocks>* draw_texture_reads,
                            std::int32_t left, std::int32_t right, std::int32_t top,
                            std::int32_t bottom) noexcept;

    class TilePool;

    GsLocalMemory& memory_;
    GsLocalMemory aliased_texture_snapshot_{};
    std::bitset<kGsLocalMemoryBlocks> aliased_texture_snapshot_valid_blocks_{};
    std::bitset<kGsLocalMemoryBlocks> aliased_texture_previous_draw_read_blocks_{};
    std::bitset<kGsLocalMemoryBlocks> aliased_texture_batch_output_blocks_{};
    bool aliased_texture_previous_draw_reads_precise_{};
    std::optional<TextureBatchState> aliased_texture_snapshot_batch_state_{};
    GsRasterThreadingConfig threading_config_{};
    GsRasterThreadingStatistics threading_statistics_{};
    std::unique_ptr<TilePool> tile_pool_{};
};

} // namespace takedown::guest
