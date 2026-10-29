#pragma once

#include "takedown/runtime/slus_21050_text_glyph.hpp"
#include "takedown/runtime/slus_21050_text_plain_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kSubmitPlainTextLinesAddress = 0x002f9140U;
inline constexpr guest::GuestAddress kSubmitDropShadowTextLinesAddress = 0x002f8ed0U;
inline constexpr guest::GuestAddress kSubmitOutlinedTextLinesAddress = 0x002f8c50U;
inline constexpr std::size_t kSubmitPlainTextNativeMaximumLineCount = 1U;
inline constexpr NativeFunctionLimits kSubmitPlainTextNativeMaximumCost{38175U, 38175U};
inline constexpr std::size_t kSubmitDropShadowNativeMaximumLineCount = 1U;
inline constexpr NativeFunctionLimits kSubmitDropShadowNativeMaximumCost{45937U, 45937U};
inline constexpr std::size_t kSubmitOutlinedTextNativeMaximumLineCount = 50U;
inline constexpr NativeFunctionLimits kSubmitOutlinedTextNativeMaximumCost{1551558U, 1551558U};

// Stable, renderer-facing identity for the font atlas selected by the game.
// A guest adapter may derive this from a validated raster resource while a
// fully native content path can assign its own identity.
struct TextRasterIdentity {
    std::uint64_t value{};

    bool operator==(const TextRasterIdentity&) const = default;
};

// Opaque, process-local identity for one immutable sampled-image generation:
// exact texels plus the effective CLUT palette for indexed content. A content
// or effective-palette change requires a new nonzero token. Logical asset
// identity, sampling/draw state, guest/GS values, and backend handles stay out
// of this renderer-neutral value; zero means unresolved. This is a semantic
// identifier rather than an unforgeable capability. Draw admission must resolve
// it through the owning residency store and retain the returned strong pin.
struct TextRasterResidencyToken {
    std::uint64_t value{};

    bool operator==(const TextRasterResidencyToken&) const = default;
};

struct TextRasterBinding {
    TextRasterIdentity raster;
    std::uint32_t palette_variant_index{};

    bool operator==(const TextRasterBinding&) const = default;
};

struct TextColorRgbaBits {
    std::array<std::uint32_t, 4U> lanes{};

    bool operator==(const TextColorRgbaBits&) const = default;
};

struct TextDrawTranslationBits {
    std::uint32_t x_bits{};
    std::uint32_t y_bits{};

    bool operator==(const TextDrawTranslationBits&) const = default;
};

struct TextDrawPassBits {
    TextColorRgbaBits color;
    TextDrawTranslationBits translation;

    bool operator==(const TextDrawPassBits&) const = default;
};

// One nonzero BuildTextGlyphRectangles invocation. The source span uses
// absolute indices in the original input and rectangles own their compact
// output. Filtered lines and the finite-range lookahead remain present with no
// passes, preserving retail planning semantics without exposing fixed guest
// buffers to a renderer.
struct TextDrawLineStep {
    std::int32_t line_index{};
    std::size_t cursor_before_code_unit{};
    std::size_t cursor_after_code_unit{};
    SelectedTextSpan selected_span;
    std::vector<TextGlyphRectangleBits> rectangles;
    NativeFunctionLimits glyph_builder_cost{};
    std::uint32_t baseline_y_bits{};
    bool eligible_for_submission{};
    bool advances_line_state{};
    std::vector<TextDrawPassBits> passes;

    bool operator==(const TextDrawLineStep&) const = default;
};

// Backends bind raster once, even when line_steps is empty, then consume each
// step and its passes in stored order. All three recovered text producers bind
// base palette variant zero. No member borrows the input text, font buckets,
// or retail fixed rectangle arrays.
struct TextDrawBatch {
    TextRasterBinding raster;
    std::vector<TextDrawLineStep> line_steps;

    bool operator==(const TextDrawBatch&) const = default;
};

struct TextDrawRenderRecordBits {
    TextLayoutBoxBits layout_box;
    std::uint32_t vertical_extent_bits{};
    TextColorRgbaBits color;

    bool operator==(const TextDrawRenderRecordBits&) const = default;
};

struct TextDrawDescriptorBits {
    std::uint32_t inverse_reference_width_bits{};
    std::uint32_t inverse_reference_height_bits{};
    std::uint32_t layout_flags{};
    std::uint32_t maximum_line_width_bits{};

    bool operator==(const TextDrawDescriptorBits&) const = default;
};

// These are exactly the plain caller's used style fields. Effect offsets are
// introduced by the shadow and outline callers and intentionally do not enter
// this one-pass API.
struct PlainTextStyleBits {
    std::uint32_t uniform_scale_bits{};
    std::uint32_t line_gap_bits{};
    std::int16_t first_line{};
    std::int16_t line_range_extent{-1};

    bool operator==(const PlainTextStyleBits&) const = default;
};

struct PlainTextDrawInput {
    // Content only; span::end() represents the terminating UTF-16 zero.
    std::span<const char16_t> text;
    TextGlyphFontView font;
    TextRasterIdentity font_raster;
    TextDrawRenderRecordBits render;
    TextDrawDescriptorBits descriptor;
    PlainTextStyleBits style;
};

enum class PlainTextStopReason : std::uint8_t {
    TextAlreadyEmpty,
    TextConsumed,
    BuilderReturnedZero,
    FiniteRangeLookahead,
};

// Register-independent result for SubmitPlainTextLines. Axis and line-step
// values retain exact EE scalar bits. final_line_index/final_line_y describe
// the caller state after all advancing lines; a lookahead step deliberately
// leaves both unchanged even though final_cursor has advanced through it.
struct PlainTextDrawPlan {
    TextGlyphAxisScaleBits axis_scale;
    std::uint32_t line_step_bits{};
    std::int32_t inclusive_end_line{-1};
    TextDrawBatch batch;
    std::size_t final_cursor_code_unit{};
    std::size_t glyph_builder_call_count{};
    std::int32_t final_line_index{};
    std::uint32_t final_line_y_bits{};
    PlainTextStopReason stop_reason{PlainTextStopReason::TextAlreadyEmpty};

    bool operator==(const PlainTextDrawPlan&) const = default;
};

// Builds the complete one-pass, game-level draw plan with retail line
// filtering, cumulative Y advancement, and finite-range lookahead. Raster
// selection and packet submission are represented as meaning rather than EE
// calls. Embedded zeroes, selected CR/LF content, unsupported glyph paths, and
// non-finite inputs return nullopt without retaining borrowed data.
[[nodiscard]] std::optional<PlainTextDrawPlan>
plan_plain_text_draw_batch(const PlainTextDrawInput& input);

// Exact style values consumed by SubmitDropShadowTextLines. The effect offsets
// are scaled independently on X/Y before any text is examined, while the line
// fields retain the shadow caller's distinct first-line sentinel semantics.
struct DropShadowTextStyleBits {
    std::uint32_t effect_offset_x_bits{};
    std::uint32_t effect_offset_y_bits{};
    std::uint32_t uniform_scale_bits{};
    std::uint32_t line_gap_bits{};
    std::int16_t first_line{-1};
    std::int16_t line_range_extent{-1};

    bool operator==(const DropShadowTextStyleBits&) const = default;
};

struct DropShadowTextDrawInput {
    // Content only; span::end() represents the terminating UTF-16 zero.
    std::span<const char16_t> text;
    TextGlyphFontView font;
    TextRasterIdentity font_raster;
    TextDrawRenderRecordBits render;
    TextDrawDescriptorBits descriptor;
    DropShadowTextStyleBits style;
};

enum class DropShadowTextStopReason : std::uint8_t {
    TextAlreadyEmpty,
    TextConsumed,
    BuilderReturnedZero,
};

// Renderer-facing two-pass result for SubmitDropShadowTextLines. shadow_offset
// retains the computed {dx,dy} even when no line is submitted. Each eligible
// visible line stores the black-alpha pass followed by the original-color
// pass; filtered lines retain owned geometry with no passes.
struct DropShadowTextDrawPlan {
    TextGlyphAxisScaleBits axis_scale;
    TextDrawTranslationBits shadow_offset;
    std::uint32_t line_step_bits{};
    std::int32_t inclusive_end_line{-1};
    TextDrawBatch batch;
    std::size_t final_cursor_code_unit{};
    std::size_t glyph_builder_call_count{};
    std::int32_t final_line_index{};
    std::uint32_t final_line_y_bits{};
    DropShadowTextStopReason stop_reason{DropShadowTextStopReason::TextAlreadyEmpty};

    bool operator==(const DropShadowTextDrawPlan&) const = default;
};

// Builds the complete two-pass draw plan. A first_line of -1 disables
// filtering. An extent of -1 only makes inclusive_end_line -1; with any other
// first_line that combination filters every nonnegative line. Unlike the
// plain caller, a finite range never stops suffix scanning.
[[nodiscard]] std::optional<DropShadowTextDrawPlan>
plan_drop_shadow_text_draw_batch(const DropShadowTextDrawInput& input);

// SubmitOutlinedTextLines consumes the same effect and line-height scalars as
// the shadow caller, but it has no line-range fields or filtering behavior.
struct OutlinedTextStyleBits {
    std::uint32_t effect_offset_x_bits{};
    std::uint32_t effect_offset_y_bits{};
    std::uint32_t uniform_scale_bits{};
    std::uint32_t line_gap_bits{};

    bool operator==(const OutlinedTextStyleBits&) const = default;
};

struct OutlinedTextDrawInput {
    // Content only; span::end() represents the terminating UTF-16 zero.
    std::span<const char16_t> text;
    TextGlyphFontView font;
    TextRasterIdentity font_raster;
    TextDrawRenderRecordBits render;
    TextDrawDescriptorBits descriptor;
    OutlinedTextStyleBits style;
};

enum class OutlinedTextStopReason : std::uint8_t {
    TextAlreadyEmpty,
    TextConsumed,
    BuilderReturnedZero,
};

// Renderer-facing five-pass result for SubmitOutlinedTextLines. outline_offset
// retains the caller's restored {dx,dy}; the first four passes use its exact
// ADD.S/SUB.S/NEG.S sequence and the fifth uses the original color/baseline.
// Every represented nonzero builder result advances the baseline.
struct OutlinedTextDrawPlan {
    TextGlyphAxisScaleBits axis_scale;
    TextDrawTranslationBits outline_offset;
    std::uint32_t line_step_bits{};
    TextDrawBatch batch;
    std::size_t final_cursor_code_unit{};
    std::size_t glyph_builder_call_count{};
    std::uint32_t final_line_y_bits{};
    OutlinedTextStopReason stop_reason{OutlinedTextStopReason::TextAlreadyEmpty};

    bool operator==(const OutlinedTextDrawPlan&) const = default;
};

// Builds the complete outline plan without range filtering. Embedded zeroes,
// selected CR/LF content, unsupported glyph paths, and non-finite arithmetic
// inputs remain on the compatibility path.
[[nodiscard]] std::optional<OutlinedTextDrawPlan>
plan_outlined_text_draw_batch(const OutlinedTextDrawInput& input);

// Installs the optional whole-function ABI composition for one ordinary,
// positive, unfiltered plain-text line when the caller and every nested seam
// match the US retail ELF. Calls outside that narrow, no-flush profile retain
// their generated path.
[[nodiscard]] bool
register_slus_21050_text_plain_native_function(NativeFunctionDispatcher& dispatcher,
                                                const guest::EeMemory& memory,
                                                SubmitPlainTextLinesTransportClaimBinding
                                                    transport_binding = {});

// Installs the optional whole-function ABI composition for the recovered
// outline producer when the caller and all three nested seams match the US
// retail ELF. Live admission remains invocation-specific and falls back for
// unproved stack, alias, glyph, or packet-ring shapes.
[[nodiscard]] bool
register_slus_21050_text_outline_native_function(NativeFunctionDispatcher& dispatcher,
                                                 const guest::EeMemory& memory);

// Installs the optional whole-function ABI composition for one positive
// `first_line == -1`, unlimited-range, one-line, no-flush drop-shadow draw
// when the caller and every nested seam match the US retail ELF. Multiline,
// finite-range, zero-result, and flush shapes retain generated execution.
[[nodiscard]] bool
register_slus_21050_text_drop_shadow_native_function(NativeFunctionDispatcher& dispatcher,
                                                     const guest::EeMemory& memory);

} // namespace takedown::runtime::slus_21050
