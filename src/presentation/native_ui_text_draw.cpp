#include "takedown/presentation/native_ui_text_draw.hpp"

#include "takedown/guest/gs_primitive_assembler.hpp"
#include "takedown/presentation/gs_texture_residency.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace takedown::presentation {

namespace {

// Adds bytes to total unless the sum overflows.
bool add_bytes(std::size_t& total, const std::size_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += bytes;
    return true;
}

// The request's charge inside an owning value of owner_bytes: the value plus
// every line-step, rectangle and pass allocation capacity.
std::optional<std::size_t> request_retained_bytes(const NativeUiTextDrawRequest& request,
                                                  const std::size_t owner_bytes) noexcept {
    if (request.raster_generation.value == 0U || request.draw.batch.raster.raster.value == 0U ||
        request.texture_function != NativeUiTextureFunction::Modulate ||
        request.depth_mode != NativeUiDepthMode::NoReadOrWrite ||
        !runtime::slus_21050::is_canonical_ui_draw_state_snapshot(request.draw.ui.state)) {
        return std::nullopt;
    }
    std::size_t total = owner_bytes;
    if (!add_bytes(total, request.draw.batch.line_steps.capacity() *
                              sizeof(runtime::slus_21050::TextDrawLineStep))) {
        return std::nullopt;
    }
    for (const runtime::slus_21050::TextDrawLineStep& step : request.draw.batch.line_steps) {
        if (!add_bytes(total, step.rectangles.capacity() * sizeof(runtime::slus_21050::TextGlyphRectangleBits)) ||
            !add_bytes(total, step.passes.capacity() * sizeof(runtime::slus_21050::TextDrawPassBits))) {
            return std::nullopt;
        }
    }
    return total;
}

} // namespace

NativeUiTextDrawEffectiveStateStatus
classify_native_ui_text_draw_effective_state(const guest::GsPrimitiveAssemblerState& actual) noexcept {
    const auto controls = guest::decode_gs_primitive_controls(actual);
    if (!controls.known || !actual.fba[0].known || !actual.pabe.known || !actual.scanmsk.known ||
        !actual.dthe.known) {
        return NativeUiTextDrawEffectiveStateStatus::Incomplete;
    }
    if (controls.topology != guest::GsPrimitiveTopology::Sprite ||
        (controls.effective_attributes_raw & 2040U) != 80U || (actual.fba[0].raw & 1U) != 0U || (actual.pabe.raw & 1U) != 0U ||
        (actual.scanmsk.raw & 3U) != 0U || (actual.dthe.raw & 1U) != 0U) {
        return NativeUiTextDrawEffectiveStateStatus::Unsupported;
    }
    return NativeUiTextDrawEffectiveStateStatus::Supported;
}

NativeUiTextDrawAmbientStateStatus classify_native_ui_text_draw_ambient_state(
    const guest::GsDrawingContextState& actual, const std::optional<std::uint64_t> actual_colclamp,
    const runtime::slus_21050::UiDrawStateSnapshot& expected) noexcept {
    const std::array<const guest::GsTrackedRegister*, 8> actual_registers{
        &actual.frame, &actual.tex1, &actual.clamp, &actual.scissor,
        &actual.alpha, &actual.test, &actual.zbuf,  &actual.xyoffset,
    };
    for (const auto* const reg : actual_registers) {
        if (!reg->known) {
            return NativeUiTextDrawAmbientStateStatus::Incomplete;
        }
    }
    if (!actual_colclamp) {
        return NativeUiTextDrawAmbientStateStatus::Incomplete;
    }
    if (!runtime::slus_21050::is_canonical_ui_draw_state_snapshot(expected)) {
        return NativeUiTextDrawAmbientStateStatus::Mismatch;
    }
    const std::array<std::uint64_t, 8> expected_values{
        expected.frame.raw,         expected.texture_sampling.raw, expected.texture_wrap.raw,
        expected.scissor.raw,       expected.alpha.raw,            expected.test_raw,
        expected.zbuf_raw,          expected.xyoffset_raw,
    };
    for (std::size_t index = 0; index != 8; ++index) {
        if (actual_registers[index]->raw != expected_values[index]) {
            return NativeUiTextDrawAmbientStateStatus::Mismatch;
        }
    }
    if (*actual_colclamp != expected.colclamp_raw) {
        return NativeUiTextDrawAmbientStateStatus::Mismatch;
    }
    return NativeUiTextDrawAmbientStateStatus::Matches;
}

std::optional<std::size_t>
native_ui_text_draw_request_retained_bytes(const NativeUiTextDrawRequest& request) noexcept {
    return request_retained_bytes(request, sizeof(NativeUiTextDrawRequest));
}

std::optional<std::size_t> native_ui_text_draw_retained_bytes(const NativeUiTextDraw& draw) noexcept {
    const std::optional<std::size_t> image_payload_bytes =
        !draw.raster_image ? std::nullopt : validated_gs_texture_image_payload_bytes(*draw.raster_image);
    std::optional<std::size_t> total = request_retained_bytes(draw, sizeof(NativeUiTextDraw));
    if (!total || !image_payload_bytes) {
        return std::nullopt;
    }
    // The referenced image is charged once per queued draw.
    if (!add_bytes(*total, sizeof(guest::GsTextureImageSnapshot)) || !add_bytes(*total, *image_payload_bytes)) {
        return std::nullopt;
    }
    return total;
}

} // namespace takedown::presentation
