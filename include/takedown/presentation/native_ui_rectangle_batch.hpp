#pragma once

#include "takedown/presentation/texture_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace takedown::presentation {

struct NativeUiTextDraw;

// These are explicit inputs from the callback's packet conversion boundary.
// No default color scale or duplicated position vector is inferred. Supplying
// values is not authentication; the producer must bind them to the draw.
struct NativeUiTextConversionInput {
    std::uint32_t color_scale_bits{};
    std::array<std::uint32_t, 4U> global_position_offset_bits{};

    bool operator==(const NativeUiTextConversionInput&) const = default;
};

enum class NativeUiTextureFilter : std::uint8_t { Nearest, Linear };
enum class NativeUiTextureAddressMode : std::uint8_t { Repeat, ClampToEdge };
enum class NativeUiBlendColor : std::uint8_t { Source, Destination, Zero };
enum class NativeUiBlendFactor : std::uint8_t { SourceAlpha, DestinationAlpha, Constant };

// RGB selectors describe (A - B) * C / 128 + D with clamped channel output.
// The intended integer model floors negative products during division; this
// compiler does not execute blending, and backend rounding remains a required
// pixel-parity proof. Alpha is the source alpha independently of the RGB equation.
struct NativeUiRgbBlendEquation {
    NativeUiBlendColor a{};
    NativeUiBlendColor b{};
    NativeUiBlendFactor c{};
    NativeUiBlendColor d{};
    std::uint8_t constant{};

    bool operator==(const NativeUiRgbBlendEquation&) const = default;
};

struct NativeUiRectangleState {
    std::uint64_t target_identity{};
    std::int32_t target_width{};
    std::int32_t target_height{};
    std::int16_t target_origin_x{};
    std::int16_t target_origin_y{};
    // Effective framebuffer coordinates, right/bottom exclusive. Empty bounds
    // remain explicit; no geometry is removed by the compiler.
    std::int32_t scissor_left{};
    std::int32_t scissor_top{};
    std::int32_t scissor_right{};
    std::int32_t scissor_bottom{};
    // One bits preserve the destination bit, applied after shading/blending.
    std::uint32_t framebuffer_preserve_mask{};
    NativeUiRgbBlendEquation rgb_blend;
    NativeUiTextureFilter magnification_filter{};
    NativeUiTextureFilter minification_filter{};
    NativeUiTextureAddressMode horizontal_address{};
    NativeUiTextureAddressMode vertical_address{};

    bool operator==(const NativeUiRectangleState&) const = default;
};

// Two opposite sprite corners in framebuffer coordinates, in 1/16 pixel
// units. UV words are normalized binary32 coordinates with Q fixed to one;
// they are preserved exactly, including signed zero. A pixel renderer still
// needs the proved sprite sampling/coverage rules; treating these as ordinary
// interpolated triangles is not a pixel-parity implementation.
struct NativeUiRectangle {
    std::int32_t x0_16{};
    std::int32_t y0_16{};
    std::int32_t x1_16{};
    std::int32_t y1_16{};
    std::array<std::uint32_t, 4U> texture_coordinate_bits{};

    bool operator==(const NativeUiRectangle&) const = default;
};

struct NativeUiRectangleRange {
    std::size_t first_rectangle{};
    std::size_t rectangle_count{};
    Rgba8 color;

    bool operator==(const NativeUiRectangleRange&) const = default;
};

// One callback only, retaining line -> pass -> rectangle order. All ranges
// share explicit state and one immutable image generation. Adjacent equal
// colors can share a range; separate callbacks are never coalesced here.
// Shading modulates each texture channel by min(255, texel * color / 128)
// with integer truncation. There are no depth reads/writes, mip levels, fog,
// alpha/destination tests, or framebuffer format conversion in this profile.
struct NativeUiRectangleBatch {
    NativeUiRectangleState state;
    std::uint64_t raster_generation{};
    TextureImageView image;
    std::vector<NativeUiRectangle> rectangles;
    std::vector<NativeUiRectangleRange> ranges;
};

struct NativeUiRectangleCompileLimits {
    // Work counts lines, passes, output rectangles, and indexed image entries
    // examined during validation. Limits are checked before materialization.
    std::size_t maximum_work_items{8U * 1024U * 1024U};
    std::size_t maximum_output_rectangles{1024U * 1024U};
    std::size_t maximum_output_ranges{65536U};
    // Includes batch/vector capacities and retained immutable image capacity,
    // although the image is shared rather than copied. Allocator metadata is
    // excluded. Allocation failures propagate; ordinary rejection is nullopt.
    std::size_t maximum_retained_bytes{64U * 1024U * 1024U};
};

// Called once per complete batch on the sole GS worker at its primary FIFO
// position. Retained batches are immutable; consumers own their retention budget.
// A sink must not enqueue or wait on this same worker from its callback.
class NativeUiRectangleBatchSink {
  public:
    virtual ~NativeUiRectangleBatchSink() = default;
    virtual void
    on_native_ui_rectangle_batch(std::uint64_t worker_serial,
                                 std::shared_ptr<const NativeUiRectangleBatch> batch) = 0;
};

// Pure callback-local lowering with no FIFO, residency-store, or renderer
// effects. The compatibility adapter accepts only canonical 32-bit targets,
// the reviewed no-depth/clamped Modulate profile, base-level nearest/linear
// sampling with matching minification/magnification filters, repeat/clamp
// addressing, defined blend selectors, and finite emitted UVs representable as
// signed 32-bit 1/16 texel coordinates after Q=1 scaling by the image extent.
// Raw position/color/conversion bits use the reviewed VU arithmetic rules.
// It returns
// either a complete owned batch or nullopt, never a partial publication.
// The input image must already be immutable, as guaranteed by the worker's
// owned snapshot/residency delivery. Sharing a caller's independently mutable
// image would not establish that ownership contract.
// State checks cover only fields supplied by NativeUiTextDraw. The resident
// worker separately checks effective sprite/STQ/Context1 controls and observed
// disabled FBA1/PABE/SCANMSK/DTHE at the FIFO boundary. Those observations are
// not carried here, and the public snapshot transport bypasses that guard;
// this pure compiler alone cannot authenticate the effective profile. Dithering
// is required disabled; no target-format irrelevance is assumed.
// Conversion input, target/resource identity, and callback ordering still need
// authenticated producer association before any live renderer activation.
[[nodiscard]] std::optional<NativeUiRectangleBatch>
compile_native_ui_text_draw(const NativeUiTextDraw& draw,
                            const NativeUiTextConversionInput& conversion,
                            const NativeUiRectangleCompileLimits& limits = {});

} // namespace takedown::presentation
