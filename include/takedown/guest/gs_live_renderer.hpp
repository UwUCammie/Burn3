#pragma once

#include "takedown/guest/gs_capture.hpp"
#include "takedown/guest/gs_local_memory.hpp"
#include "takedown/guest/gs_primitive_assembler.hpp"
#include "takedown/guest/gs_replay_state.hpp"
#include "takedown/guest/gs_software_rasterizer.hpp"
#include "takedown/guest/gs_texture_snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace takedown::guest {

// A bounded, stable classification for draw attempts which do not produce a
// supported raster result. The legacy aggregate counters below remain the
// compatibility surface; this enum supplies enough detail to identify the
// dominant missing state or unsupported GS feature in one retail run.
enum class GsDrawRejectionReason : std::uint8_t {
    UnsupportedTopology,
    MissingFrame,
    MissingXyoffset,
    MissingScissor,
    MissingTex0,
    MissingRgbaq,
    MissingUv,
    MissingStq,
    MissingAlpha,
    InvalidFramebufferWidth,
    InvalidScissor,
    UnsupportedFixedUvTriangle,
    InvalidStq,
    UnsupportedFramebufferTexturePair,
    OversizedTexture,
    UnsupportedTextureFunction,
    MissingTexa,
    UnsupportedClutPixelStorageMode,
    UnsupportedClutStorageMode,
    EmptyRasterization,
    UnsupportedFramebufferFormat,
    UnsupportedAlphaBlend,
    InvalidRasterTarget,
    Count,
};

[[nodiscard]] const char* to_string(GsDrawRejectionReason reason) noexcept;

inline constexpr std::size_t kGsPrimitiveTopologyCount = 8U;
inline constexpr std::size_t kGsDrawRejectionReasonCount =
    static_cast<std::size_t>(GsDrawRejectionReason::Count);
inline constexpr std::size_t kGsPixelStorageModeCount = 64U;
inline constexpr std::size_t kGsClutPixelStorageModeCount = 16U;

struct GsLiveRendererStatistics {
    std::uint64_t register_writes{};
    std::uint64_t image_transfers_applied{};
    std::uint64_t image_transfers_rejected{};
    std::uint64_t local_to_local_transfers_applied{};
    std::uint64_t local_to_local_transfers_rejected{};
    std::uint64_t gs_soft_resets{};
    std::uint64_t drawable_kicks{};
    std::uint64_t adc_suppressed_positions{};
    std::uint64_t nonkicking_positions{};
    std::uint64_t solid_points_drawn{};
    std::uint64_t solid_lines_drawn{};
    std::uint64_t solid_sprites_drawn{};
    std::uint64_t solid_triangles_drawn{};
    std::uint64_t presentation_sprites_drawn{};
    std::uint64_t textured_sprites_drawn{};
    std::uint64_t textured_triangles_drawn{};
    std::uint64_t textured_primitives_deferred{};
    std::uint64_t empty_primitives{};
    std::uint64_t incomplete_state_primitives{};
    std::uint64_t unsupported_primitives{};
    std::uint64_t covered_pixels{};
    std::uint64_t written_pixels{};
    std::uint64_t framebuffer_revision{};
    // Intra-primitive host parallelism is observational only. Each draw still
    // reaches a full barrier before the next GS primitive is accepted.
    std::uint32_t raster_worker_threads{};
    std::uint64_t raster_parallel_primitives{};
    std::uint64_t raster_parallel_tiles{};
    std::uint64_t raster_serial_primitives{};
    std::uint64_t raster_small_primitive_fallbacks{};
    std::uint64_t raster_hazard_fallbacks{};
    std::uint64_t clut_loads_applied{};
    std::uint64_t clut_loads_skipped{};
    std::uint64_t clut_loads_deferred{};
    std::uint64_t clut_version{};
    std::array<std::uint64_t, 8> clut_loads_by_condition{};
    std::array<std::uint64_t, 8> clut_skips_by_condition{};
    std::array<std::uint64_t, 8> clut_deferred_by_condition{};
    // Deferral reason breakdown for textured draws: by texture PSM (0..63)
    // and by texture-function/TFX mode (0..3).
    std::array<std::uint64_t, 64> deferred_texture_psm{};
    std::array<std::uint64_t, 4> deferred_texture_tfx{};
    // Census of textured primitives that were actually rasterized, broken
    // down by texture PSM (0..63), colour function (0..3), and CLUT format
    // (0..15). This makes per-screen font/sprite paths observable: a screen
    // whose glyphs rely on a particular indexed storage or palette format
    // shows up here, complementing the deferred counts above.
    std::array<std::uint64_t, 64> drawn_texture_psm{};
    std::array<std::uint64_t, 4> drawn_texture_tfx{};
    std::array<std::uint64_t, 16> drawn_texture_clut{};
    // Every array is fixed-size and observational. The four topology
    // histograms sum to their corresponding legacy aggregate counter. A
    // raster-level unsupported textured draw is intentionally both
    // unsupported and deferred, matching the pre-existing aggregate totals.
    std::array<std::uint64_t, kGsPrimitiveTopologyCount> deferred_primitives_by_topology{};
    std::array<std::uint64_t, kGsPrimitiveTopologyCount> empty_primitives_by_topology{};
    std::array<std::uint64_t, kGsPrimitiveTopologyCount> incomplete_state_primitives_by_topology{};
    std::array<std::uint64_t, kGsPrimitiveTopologyCount> unsupported_primitives_by_topology{};
    // One primary reason is recorded per rejected draw attempt. PSM
    // histograms are populated only when the corresponding register state is
    // known, so missing-state counts are not disguised as storage mode zero.
    std::array<std::uint64_t, kGsDrawRejectionReasonCount> rejected_primitives_by_reason{};
    std::array<std::uint64_t, kGsPixelStorageModeCount> rejected_framebuffer_psm{};
    std::array<std::uint64_t, kGsPixelStorageModeCount> rejected_texture_psm{};
    std::array<std::uint64_t, kGsClutPixelStorageModeCount> rejected_clut_psm{};
};

// Classifies whether the live local-memory backing is authoritative for one
// canonical base-level snapshot at the current ordered renderer prefix.
enum class GsTextureSnapshotSourceStatus : std::uint8_t {
    CurrentLocalMemory,
    UnsupportedTexture,
    FeedbackCacheConflict,
};

// First live renderer bridge. GsCapture owns GIF parsing; this class receives
// every decoded command synchronously, canonicalizes PACKED attributes,
// assembles sprite/triangle-strip vertices, and writes the conservative solid
// subset into the same swizzled memory used by texture uploads and scanout.
class GsLiveRenderer final : public GsCommandObserver {
  public:
    GsLiveRenderer();

    void on_gs_gif_tag_begin(GifPath path) override;
    void on_gs_reset() override;
    void on_gs_register_write(const GsRegisterWrite& write) override;
    void on_gs_image_transfer(const GsImageTransfer& transfer) override;

    [[nodiscard]] const GsLocalMemory& local_memory() const noexcept;
    [[nodiscard]] GsLocalMemory& local_memory() noexcept;
    [[nodiscard]] const GsLiveRendererStatistics& statistics() const noexcept;

    // Returns the ordered Context1 register latches. The borrowed view's
    // contents may change on the next drawing-state write or reset, so inspect
    // it immediately on this renderer's owning thread.
    [[nodiscard]] const GsDrawingContextState& current_context1_state() const noexcept;
    // Sole rendering-thread view, including effective-attribute sources and
    // observed global controls. Read only at an ordered command boundary.
    [[nodiscard]] const GsPrimitiveAssemblerState& current_drawing_state() const noexcept;

    // Returns the exact ordered TEX0 value currently tracked for one drawing
    // context. An empty result means no TEX0 has reached that context since
    // construction or the most recent drawing-state reset.
    [[nodiscard]] std::optional<std::uint64_t>
    current_tex0(GsDrawingContext context) const noexcept;

    // Returns the exact ordered global COLCLAMP value, or no value when no
    // write has reached this renderer since construction/reset.
    [[nodiscard]] std::optional<std::uint64_t> current_colclamp() const noexcept;

    // Copies one base-level image from this renderer's current ordered local-
    // memory, TEXA, and immutable internal-CLUT state. An active framebuffer-
    // feedback cache is not observed, so the caller must prove non-aliasing or
    // a preceding cache boundary.
    [[nodiscard]] std::optional<GsTextureImageSnapshot>
    snapshot_texture_image(std::uint64_t tex0) const;

    // Exact, allocation-free comparison against this renderer's current
    // ordered local memory, TEXA, and effective CLUT state. The caller must
    // separately prove that local memory is authoritative for the footprint.
    [[nodiscard]] bool texture_image_matches(std::uint64_t tex0,
                                             const GsTextureImageSnapshot& expected) const noexcept;

    // Creates an immutable image and its source proof in one ordered operation.
    // Validation retains the original raw TEX0 binding, uses a conservative
    // per-block unchanged proof when possible, runs the exact comparator for
    // every inconclusive proof, and refreshes the hidden checkpoint only after
    // one of those complete validations succeeds.
    [[nodiscard]] std::optional<GsTextureImageResidencyCapture>
    capture_texture_image_for_residency(std::uint64_t tex0);
    [[nodiscard]] GsTextureImageValidationResult
    validate_texture_image_for_residency(std::uint64_t tex0,
                                         GsTextureImageResidencyCapture& capture) const noexcept;

    // Checks descriptor support and the complete base-level page footprint
    // without copying texels. FeedbackCacheConflict includes any footprint
    // which cannot be proved disjoint while a feedback epoch is active.
    [[nodiscard]] GsTextureSnapshotSourceStatus
    classify_texture_snapshot_source(std::uint64_t tex0) const noexcept;

    // Atomically installs an idle decoded seed. It never routes serialized
    // registers through on_gs_register_write(), since doing that to TRXDIR can
    // repeat an already-completed GS-to-GS move and corrupt frozen VRAM.
    [[nodiscard]] GsReplayStateRestoreStatus restore_idle_state(const GsReplayStateSeed& seed);

    void reset() noexcept;

  private:
    [[nodiscard]] std::optional<GsPrimitiveKickSnapshot>
    forward_register_write(const GsRegisterWrite& write) noexcept;
    void handle_kick(const GsPrimitiveKickSnapshot& kick, bool suppress_draw = false) noexcept;
    void draw_point(const GsPrimitiveKickSnapshot& point) noexcept;
    void draw_line(const GsPrimitiveKickSnapshot& first,
                   const GsPrimitiveKickSnapshot& second) noexcept;
    void draw_sprite(const GsPrimitiveKickSnapshot& first,
                     const GsPrimitiveKickSnapshot& second) noexcept;
    void draw_triangle(const GsPrimitiveKickSnapshot& first, const GsPrimitiveKickSnapshot& second,
                       const GsPrimitiveKickSnapshot& third) noexcept;
    void apply_raster_result(const GsRasterResult& result, GsPrimitiveTopology topology,
                             std::uint32_t framebuffer_psm,
                             std::optional<std::uint32_t> texture_psm = std::nullopt,
                             std::optional<std::uint32_t> clut_psm = std::nullopt) noexcept;
    void record_deferred(GsPrimitiveTopology topology, GsDrawRejectionReason reason,
                         std::optional<std::uint32_t> framebuffer_psm = std::nullopt,
                         std::optional<std::uint32_t> texture_psm = std::nullopt,
                         std::optional<std::uint32_t> clut_psm = std::nullopt) noexcept;
    void record_incomplete(GsPrimitiveTopology topology, GsDrawRejectionReason reason,
                           std::optional<std::uint32_t> framebuffer_psm = std::nullopt,
                           std::optional<std::uint32_t> texture_psm = std::nullopt,
                           std::optional<std::uint32_t> clut_psm = std::nullopt) noexcept;
    void record_rejection_details(GsDrawRejectionReason reason,
                                  std::optional<std::uint32_t> framebuffer_psm,
                                  std::optional<std::uint32_t> texture_psm,
                                  std::optional<std::uint32_t> clut_psm) noexcept;
    void census_texture(std::uint32_t texture_psm, std::uint32_t texture_function,
                        std::uint32_t clut_format) noexcept;
    void process_clut_write(std::uint8_t address);
    void process_transfer_write(const GsRegisterWrite& write) noexcept;
    void load_csm1_clut(std::uint64_t tex0);
    void reset_drawing_state() noexcept;
    void reset_pending_vertices() noexcept;

    [[nodiscard]] static GsRasterVertex raster_vertex(const GsVertexState& vertex) noexcept;
    [[nodiscard]] static std::uint64_t canonical_packed_rgba(std::uint64_t low, std::uint64_t high,
                                                             std::uint32_t q_bits) noexcept;
    [[nodiscard]] static std::uint64_t canonical_packed_uv(std::uint64_t low) noexcept;
    [[nodiscard]] static std::uint64_t
    canonical_packed_position(std::uint8_t address, std::uint64_t low, std::uint64_t high) noexcept;

    GsLocalMemory local_memory_{};
    GsPrimitiveAssembler assembler_{};
    GsSoftwareRasterizer rasterizer_;
    std::array<GsPrimitiveKickSnapshot, 3> pending_vertices_{};
    std::size_t pending_vertex_count_{};
    std::optional<GsPrimitiveTopology> pending_topology_{};
    GsLiveRendererStatistics statistics_{};
    std::uint64_t texa_{};
    bool texa_known_{};
    std::shared_ptr<const GsRasterClut> clut_{};
    std::array<std::uint32_t, 2> clut_cbp_slots_{};
    std::uint64_t next_clut_version_{};
    std::uint64_t bitbltbuf_{};
    std::uint64_t trxpos_{};
    std::uint64_t trxreg_{};
    // GIF's PACKED Q latch is transport state, distinct from the canonical
    // RGBAQ GS register retained by the primitive assembler.
    std::uint32_t packed_q_bits_{0x3f800000U};
    // COLCLAMP latched from GS register 0x46; GS reset clears it (8-bit AND).
    std::uint64_t colclamp_{};
    bool colclamp_known_{};
    // FOGCOL latched from canonical A+D register 0x3d. Only RGB bits 0..23
    // participate in the fog equation; GS reset restores black.
    std::uint64_t fogcol_{};
};

} // namespace takedown::guest
