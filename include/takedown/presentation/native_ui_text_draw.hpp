#pragma once

#include "takedown/guest/gs_texture_snapshot.hpp"
#include "takedown/presentation/native_ui_rectangle_batch.hpp"
#include "takedown/runtime/slus_21050_ui_text_draw_guest.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace takedown::guest {

struct GsDrawingContextState;
struct GsPrimitiveAssemblerState;

} // namespace takedown::guest

namespace takedown::presentation {

enum class NativeUiTextDrawAmbientStateStatus : std::uint8_t {
    Matches,
    Incomplete,
    Mismatch,
};

enum class NativeUiTextDrawEffectiveStateStatus : std::uint8_t {
    Supported,
    Incomplete,
    Unsupported,
};

// Checks the observed effective sprite/STQ/Context1 profile at the worker's
// FIFO boundary, reusing the GS PRMODECONT/PRMODE decoder. FBA1, PABE, SCANMSK,
// and DTHE must all be observed disabled; this makes no dither-irrelevance
// assumption. Unknown required sources take precedence over unsupported bits.
// This supplements ambient-state matching, not callback/geometry provenance
// or rasterizer parity. The public snapshot transport does not apply it.
[[nodiscard]] NativeUiTextDrawEffectiveStateStatus
classify_native_ui_text_draw_effective_state(
    const guest::GsPrimitiveAssemblerState& actual) noexcept;

// First validates the snapshot's redundant raw/decoded UI fields, then compares
// the ordered raw Context1 latches and global COLCLAMP. TEX0 is deliberately
// excluded: store-resident draws use the immutable image acquired for their
// residency generation. Target identity and dimensions have no corresponding
// live GS latch beyond FRAME, SCISSOR, and XYOFFSET. Target identity and the
// cached-XYOFFSET-derived packet offset remain trusted upstream inputs; live
// activation must authenticate their association at its packet boundary.
[[nodiscard]] NativeUiTextDrawAmbientStateStatus classify_native_ui_text_draw_ambient_state(
    const guest::GsDrawingContextState& actual, std::optional<std::uint64_t> actual_colclamp,
    const runtime::slus_21050::UiDrawStateSnapshot& expected) noexcept;

// Renderer-neutral texture-combine behavior admitted by the first native UI
// text profile. Extend this enum only when the matching GS behavior has a
// differential and backend parity contract.
enum class NativeUiTextureFunction : std::uint8_t {
    Modulate,
};

// The first profile admits only draws for which ordered TEST semantics prevent
// every depth read and write. vertex_z remains explicit so a later depth-aware
// profile cannot accidentally inherit a hidden packet word.
enum class NativeUiDepthMode : std::uint8_t {
    NoReadOrWrite,
};

// Image-free renderer request used by the store-resident path. The
// eventual sink value adds the exact immutable registry image at the command's
// FIFO position. Keeping this separate prevents a producer from supplying an
// unauthenticated image while allowing repeated draws to share residency.
struct NativeUiTextDrawRequest {
    runtime::slus_21050::UiTextDrawBatchSubmission draw;
    runtime::slus_21050::TextRasterResidencyToken raster_generation;
    NativeUiTextureFunction texture_function{NativeUiTextureFunction::Modulate};
    NativeUiDepthMode depth_mode{NativeUiDepthMode::NoReadOrWrite};
    std::uint32_t vertex_z{};
    // Owned callback-boundary values; presence alone does not authenticate them.
    std::optional<NativeUiTextConversionInput> conversion{};
};

// One complete retained UI text callback. The public snapshot submission path
// copies both this request base and raster_image into worker-owned immutable
// storage. The store-resident path copies the request during
// preparation, then attaches the registry-owned canonical image when the FIFO
// command executes. The sink may retain the complete value after its callback.
struct NativeUiTextDraw : NativeUiTextDrawRequest {
    std::shared_ptr<const guest::GsTextureImageSnapshot> raster_image;
};

// Returns the retained byte charge for an image-free request: its owning value
// and all nested geometry allocation capacities. Per-command allocator metadata
// is covered by the worker command-count bound. Malformed values or arithmetic
// overflow return nullopt.
[[nodiscard]] std::optional<std::size_t>
native_ui_text_draw_request_retained_bytes(const NativeUiTextDrawRequest& request) noexcept;

// Returns the retained byte charge for one queued draw: the owning value, every
// nested geometry allocation, and the referenced canonical image. Per-command
// and shared-control metadata is omitted and bounded by the separate command
// limit. Nested allocator bookkeeping follows the charged line/geometry
// capacities. Images are deliberately counted once per queued command because a
// residency release may remove registry accounting while commands still retain
// that image. Malformed values or arithmetic overflow return nullopt.
[[nodiscard]] std::optional<std::size_t>
native_ui_text_draw_retained_bytes(const NativeUiTextDraw& draw) noexcept;

class NativeUiTextDrawSink {
  public:
    virtual ~NativeUiTextDrawSink() = default;

    // Called on the sole GS worker thread at worker_serial's exact primary-FIFO
    // position. The shared immutable draw may be retained asynchronously.
    virtual void on_native_ui_text_draw(std::uint64_t worker_serial,
                                        std::shared_ptr<const NativeUiTextDraw> draw) = 0;
};

} // namespace takedown::presentation
