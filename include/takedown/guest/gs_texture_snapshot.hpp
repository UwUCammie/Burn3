#pragma once

#include "takedown/guest/gs_local_memory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace takedown::presentation {
class GsTextureResidencyRegistry;
}

namespace takedown::guest {

struct GsRasterClut;
class GsLiveRenderer;

// Canonical row-major texels for a direct-colour GS texture. Alpha already
// reflects TEXA and TEX0.TCC, so consumers do not need GS register semantics.
using GsDirectTexturePlane = presentation::DirectTexturePlane;

// Canonical row-major indices plus the complete palette visible to those
// indices. PSMT4 owns 16 entries; PSMT8 and PSMT8H own 256. The palette has
// already applied CSM1/CSA mapping, TEXA, and TEX0.TCC.
using GsIndexedTexturePlane = presentation::IndexedTexturePlane;

using GsTextureImagePlane = presentation::TextureImagePlane;

// An owned, renderer-neutral base-level image. Equality deliberately excludes
// raw TEX0/TEXA values and CLUT version counters: only decoded dimensions,
// source format, texels, and the effective indexed palette define the image.
struct GsTextureImageSnapshot {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t source_pixel_storage_mode{};
    GsTextureImagePlane plane;

    bool operator==(const GsTextureImageSnapshot&) const = default;
};

// A move-only immutable image plus its renderer-owned source proof. Construction
// is sealed so a checkpoint captured at a later point cannot be paired with an
// older or different image. The registry may update only the hidden proof;
// image() remains immutable and keeps the canonical payload renderer-neutral.
class GsTextureImageResidencyCapture final {
  public:
    GsTextureImageResidencyCapture(const GsTextureImageResidencyCapture&) = delete;
    GsTextureImageResidencyCapture& operator=(const GsTextureImageResidencyCapture&) = delete;
    GsTextureImageResidencyCapture(GsTextureImageResidencyCapture&&) noexcept = default;
    GsTextureImageResidencyCapture& operator=(GsTextureImageResidencyCapture&&) = delete;

    [[nodiscard]] const GsTextureImageSnapshot& image() const noexcept { return image_; }

  private:
    friend class GsLiveRenderer;
    friend class presentation::GsTextureResidencyRegistry;

    GsTextureImageResidencyCapture(GsTextureImageSnapshot image,
                                   std::optional<std::uint64_t> source_tex0,
                                   GsLocalMemoryMutationCheckpoint local_memory_checkpoint,
                                   std::optional<std::uint64_t> direct_texa) noexcept
        : image_(std::move(image)), source_tex0_(source_tex0),
          local_memory_checkpoint_(local_memory_checkpoint), direct_texa_(direct_texa) {}

    GsTextureImageSnapshot image_;
    // A production capture retains this permanent bit-exact binding even when
    // tracking is unavailable. Registry-only legacy snapshots leave it empty,
    // remain unauthenticated, and cannot pass source validation.
    std::optional<std::uint64_t> source_tex0_;
    GsLocalMemoryMutationCheckpoint local_memory_checkpoint_;
    std::optional<std::uint64_t> direct_texa_;
};

struct GsTextureImageValidationResult {
    bool current{};
    bool texture_content_scan_skipped{};
    // True when a later global write required narrowing through the
    // conservative physical-block footprint before deciding currentness.
    bool mutation_block_scan_performed{};
};

// Checks the bounded descriptor and external TEXA/CLUT prerequisites without
// reading local memory or allocating the canonical image.
[[nodiscard]] bool can_snapshot_gs_texture_image(std::uint64_t tex0, bool texa_available,
                                                 bool effective_clut_available) noexcept;

// Decodes one base-level TEX0 image against an immutable point-in-time view of
// GS local memory and the already-loaded internal CLUT. This bounded profile
// supports PSMCT32, PSMCT24, PSMZ16, PSMZ16S, PSMT4, PSMT8, and PSMT8H with
// TW/TH <= 11. It does not inspect TEX1, resolve mip levels, or interpret TFX.
// It also does not inspect the software rasterizer's active framebuffer-
// feedback cache; callers must serialize the read and prove non-aliasing or a
// preceding cache boundary.
[[nodiscard]] std::optional<GsTextureImageSnapshot>
snapshot_gs_texture_image(const GsLocalMemory& memory, std::uint64_t tex0,
                          std::optional<std::uint64_t> texa,
                          std::shared_ptr<const GsRasterClut> effective_clut = {});

// Checks every part of a canonical image comparison which does not require a
// local-memory texel/index read. This validates the descriptor and snapshot
// shape and compares the complete effective palette for indexed images without
// allocating. Direct-image callers which use this as a local-memory fast path
// must separately retain and compare any TEXA generation which affects their
// pinned image.
[[nodiscard]] bool
gs_texture_image_non_memory_state_matches(std::uint64_t tex0, std::optional<std::uint64_t> texa,
                                          const GsRasterClut* effective_clut,
                                          const GsTextureImageSnapshot& expected) noexcept;

// Compares the currently decoded base-level image with an owned snapshot
// without allocating or copying either plane. The descriptor, TEXA, and CLUT
// rules are identical to snapshot_gs_texture_image(); malformed snapshots and
// unsupported descriptors compare unequal.
[[nodiscard]] bool gs_texture_image_matches(const GsLocalMemory& memory, std::uint64_t tex0,
                                            std::optional<std::uint64_t> texa,
                                            const GsRasterClut* effective_clut,
                                            const GsTextureImageSnapshot& expected) noexcept;

} // namespace takedown::guest
