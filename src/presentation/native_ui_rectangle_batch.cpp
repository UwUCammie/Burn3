#include "takedown/presentation/native_ui_rectangle_batch.hpp"

#include "takedown/presentation/gs_texture_residency.hpp"
#include "takedown/presentation/native_ui_text_draw.hpp"
#include "takedown/runtime/slus_21050_stream_environment.hpp"
#include "takedown/runtime/slus_21050_ui_draw.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace takedown::presentation {

namespace {

// Byte charge for count elements, or nullopt when the multiplication overflows.
std::optional<std::size_t> checked_scale(const std::size_t count, const std::size_t element_bytes) noexcept {
    if (count != 0U && count > std::numeric_limits<std::size_t>::max() / element_bytes) {
        return std::nullopt;
    }
    return count * element_bytes;
}

// Adds count to total when the sum stays within maximum.
bool add_within(std::size_t& total, const std::size_t count, const std::size_t maximum) noexcept {
    if (total > maximum || count > maximum - total) {
        return false;
    }
    total += count;
    return true;
}

// Checks whether the batch, the retained image and the rectangle and range
// storage fit under maximum. Overflow in any charge is treated as not fitting.
[[nodiscard]] bool retained_bytes_fit(const std::size_t image_bytes, const std::size_t rectangle_capacity,
                        const std::size_t range_capacity, const std::size_t maximum) {
    const std::optional<std::size_t> rectangle_bytes =
        checked_scale(rectangle_capacity, sizeof(NativeUiRectangle));
    const std::optional<std::size_t> range_bytes = checked_scale(range_capacity, sizeof(NativeUiRectangleRange));
    if (!rectangle_bytes || !range_bytes) {
        return false;
    }
    std::size_t total = 0U;
    return add_within(total, sizeof(NativeUiRectangleBatch), maximum) &&
           add_within(total, sizeof(guest::GsTextureImageSnapshot), maximum) &&
           add_within(total, image_bytes, maximum) && add_within(total, *rectangle_bytes, maximum) &&
           add_within(total, *range_bytes, maximum);
}

// Base level only, nearest or linear, with matching filters.
bool supported_sampling(const runtime::slus_21050::UiTextureSamplingState& sampling) noexcept {
    return sampling.maximum_mip_level == 0U && sampling.minification_filter <= 1U &&
           sampling.magnification_linear == static_cast<bool>(sampling.minification_filter);
}

// A, B and D select source, destination or zero; C source alpha, destination
// alpha or the fixed constant.
bool defined_blend_selectors(const runtime::slus_21050::UiAlphaEquationState& alpha) noexcept {
    return alpha.source_a <= 2U && alpha.source_b <= 2U && alpha.factor_c <= 2U && alpha.source_d <= 2U;
}

// The reviewed profile: canonical state, a bound raster generation, a 32-bit
// target with the fixed TEST/COLCLAMP words, blended Modulate text without
// depth, base-level sampling with matching filters, repeat/clamp addressing
// and defined blend selectors.
bool supported_profile(const NativeUiTextDraw& draw) noexcept {
    const runtime::slus_21050::UiDrawStateSnapshot& state = draw.draw.ui.state;
    if (runtime::slus_21050::is_canonical_ui_draw_state_snapshot(state) &&
           draw.draw.batch.raster.raster.value != 0U && draw.raster_generation.value != 0U &&
           state.target.identity.value != 0U && state.frame.pixel_storage_format == 0U &&
           state.frame.framebuffer_width != 0U && state.test_raw == 0x41001U &&
           state.colclamp_raw == 1U && draw.draw.primitive.alpha_blending_enabled &&
           draw.texture_function == NativeUiTextureFunction::Modulate &&
           draw.depth_mode == NativeUiDepthMode::NoReadOrWrite && draw.vertex_z == 0U &&
           supported_sampling(state.texture_sampling) && state.texture_wrap.horizontal_mode <= 1U &&
           state.texture_wrap.vertical_mode <= 1U && defined_blend_selectors(state.alpha)) {
        return true;
    }
    return false;
}

// Every emitted UV must be finite and, scaled by the image extent with Q=1,
// representable as a signed 32-bit 1/16 texel coordinate.
bool texture_coordinates_representable(const runtime::slus_21050::TextGlyphRectangleBits& rectangle,
                                       const std::uint32_t width, const std::uint32_t height) noexcept {
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        if ((rectangle.texture[lane] & 0x7F800000U) == 0x7F800000U) {
            return false;
        }
        const auto extent = (lane & 1U) == 0U ? width : height;
        const double texel_units = static_cast<double>(std::bit_cast<float>(rectangle.texture[lane])) *
                                   static_cast<double>(extent) * 16.0;
        if (texel_units < -2147483648.0 || texel_units > 2147483647.0) {
            return false;
        }
    }
    return true;
}

// A pass colour through the reviewed VU conversion: the packet header
// descriptor's four low bytes.
Rgba8 pass_color(const runtime::slus_21050::TextColorRgbaBits& color,
                 const NativeUiTextConversionInput& conversion) noexcept {
    const auto descriptor = runtime::slus_21050::make_stream_environment_header_descriptor(
        {color.lanes}, {{0U, 0U, 0U, conversion.color_scale_bits}});
    return Rgba8{static_cast<std::uint8_t>(descriptor), static_cast<std::uint8_t>(descriptor >> 8U),
                 static_cast<std::uint8_t>(descriptor >> 16U), static_cast<std::uint8_t>(descriptor >> 24U)};
}

NativeUiRectangleState rectangle_state(const runtime::slus_21050::UiDrawStateSnapshot& state) noexcept {
    const auto raw_scissor = state.scissor.raw;
    return NativeUiRectangleState{
        state.target.identity.value,
        state.target.width,
        state.target.height,
        state.target.origin_x,
        state.target.origin_y,
        static_cast<std::int32_t>(raw_scissor) & 0x7FF,
        static_cast<std::int32_t>(raw_scissor >> 32U) & 0x7FF,
        (static_cast<std::int32_t>(raw_scissor >> 16U) & 0x7FF) + 1,
        (static_cast<std::int32_t>(raw_scissor >> 48U) & 0x7FF) + 1,
        state.frame.write_mask,
        NativeUiRgbBlendEquation{static_cast<NativeUiBlendColor>(state.alpha.source_a),
                                 static_cast<NativeUiBlendColor>(state.alpha.source_b),
                                 static_cast<NativeUiBlendFactor>(state.alpha.factor_c),
                                 static_cast<NativeUiBlendColor>(state.alpha.source_d),
                                 state.alpha.fixed_alpha},
        state.texture_sampling.magnification_linear ? NativeUiTextureFilter::Linear
                                                    : NativeUiTextureFilter::Nearest,
        state.texture_sampling.minification_filter == 1U ? NativeUiTextureFilter::Linear
                                                         : NativeUiTextureFilter::Nearest,
        static_cast<NativeUiTextureAddressMode>(state.texture_wrap.horizontal_mode != 0U),
        static_cast<NativeUiTextureAddressMode>(state.texture_wrap.vertical_mode != 0U),
    };
}

} // namespace

// RECOVERY: matches the original apart from basic-block layout around the
// profile check and the indexed-image work-count rejection.
std::optional<NativeUiRectangleBatch>
compile_native_ui_text_draw(const NativeUiTextDraw& draw, const NativeUiTextConversionInput& conversion,
                            const NativeUiRectangleCompileLimits& limits) {
    if (!supported_profile(draw) || !draw.raster_image) {
        return std::nullopt;
    }
    std::size_t work_items = 0U;
    if (const auto* indexed = std::get_if<IndexedTexturePlane>(&draw.raster_image->plane)) {
        // Every indexed texel is examined.
        if (!add_within(work_items, indexed->indices.size(), limits.maximum_work_items)) {
            return std::nullopt;
        }
    }
    const std::optional<std::size_t> image_bytes =
        validated_gs_texture_image_payload_bytes(*draw.raster_image);
    if (!image_bytes) {
        return std::nullopt;
    }
    if (!add_within(work_items, draw.draw.batch.line_steps.size(), limits.maximum_work_items)) {
        return std::nullopt;
    }

    // Validate and count everything before materializing.
    std::size_t rectangle_count = 0U;
    std::size_t range_count = 0U;
    std::optional<Rgba8> preceding_color;
    for (const runtime::slus_21050::TextDrawLineStep& line : draw.draw.batch.line_steps) {
        const std::optional<std::size_t> line_rectangle_count =
            checked_scale(line.rectangles.size(), line.passes.size());
        if (!line_rectangle_count ||
            !add_within(work_items, line.passes.size(), limits.maximum_work_items) ||
            !add_within(work_items, *line_rectangle_count, limits.maximum_work_items) ||
            !add_within(rectangle_count, *line_rectangle_count, limits.maximum_output_rectangles)) {
            return std::nullopt;
        }
        if (line.rectangles.empty() || line.passes.empty()) {
            continue;
        }
        for (const runtime::slus_21050::TextGlyphRectangleBits& rectangle : line.rectangles) {
            if (!texture_coordinates_representable(rectangle, draw.raster_image->width,
                                                   draw.raster_image->height)) {
                return std::nullopt;
            }
        }
        for (const runtime::slus_21050::TextDrawPassBits& pass : line.passes) {
            const Rgba8 color = pass_color(pass.color, conversion);
            if (!preceding_color || !(*preceding_color == color)) {
                if (!add_within(range_count, 1U, limits.maximum_output_ranges)) {
                    return std::nullopt;
                }
                preceding_color = color;
            }
        }
    }
    if (!retained_bytes_fit(*image_bytes, rectangle_count, range_count, limits.maximum_retained_bytes)) {
        return std::nullopt;
    }

    NativeUiRectangleBatch batch{
        rectangle_state(draw.draw.ui.state),
        draw.raster_generation.value,
        TextureImageView{draw.raster_image->width, draw.raster_image->height,
                         std::shared_ptr<const TextureImagePlane>(draw.raster_image,
                                                                  &draw.raster_image->plane)},
        {},
        {},
    };
    batch.rectangles.reserve(rectangle_count);
    batch.ranges.reserve(range_count);
    if (!retained_bytes_fit(*image_bytes, batch.rectangles.capacity(), batch.ranges.capacity(),
                            limits.maximum_retained_bytes)) {
        return std::nullopt;
    }

    // XYOFFSET in 1/16 pixel units moves packet positions into the target.
    const auto raw_xyoffset = draw.draw.ui.state.xyoffset_raw;
    const int offset_x = static_cast<int>(raw_xyoffset) & 0xFFFF;
    const int offset_y = static_cast<int>(raw_xyoffset >> 32U) & 0xFFFF;
    for (const runtime::slus_21050::TextDrawLineStep& line : draw.draw.batch.line_steps) {
        if (line.rectangles.empty()) {
            continue;
        }
        for (const runtime::slus_21050::TextDrawPassBits& pass : line.passes) {
            const Rgba8 color = pass_color(pass.color, conversion);
            if (batch.ranges.empty() || !(batch.ranges.back().color == color)) {
                batch.ranges.push_back({batch.rectangles.size(), line.rectangles.size(), color});
            } else {
                batch.ranges.back().rectangle_count += line.rectangles.size();
            }
            const runtime::slus_21050::RawVector4Bits position_offset =
                runtime::slus_21050::make_stream_environment_position_offset(
                    {pass.translation.x_bits, pass.translation.y_bits},
                    {conversion.global_position_offset_bits});
            for (const runtime::slus_21050::TextGlyphRectangleBits& rectangle : line.rectangles) {
                const runtime::slus_21050::EncodedStreamEnvironmentElement encoded =
                    runtime::slus_21050::encode_stream_environment_element(
                        {{rectangle.position}}, {{rectangle.texture}}, position_offset, 0U);
                const auto first = encoded.vertices[0].packed_position_and_tag;
                const auto second = encoded.vertices[1].packed_position_and_tag;
                batch.rectangles.push_back({
                    (static_cast<int>(first) & 0xFFFF) - offset_x,
                    (static_cast<int>(first >> 16U) & 0xFFFF) - offset_y,
                    (static_cast<int>(second) & 0xFFFF) - offset_x,
                    (static_cast<int>(second >> 16U) & 0xFFFF) - offset_y,
                    rectangle.texture,
                });
            }
        }
    }
    return batch;
}

} // namespace takedown::presentation
