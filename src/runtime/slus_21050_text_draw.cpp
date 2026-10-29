#include "takedown/runtime/slus_21050_text_draw.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kMaxFiniteBits = 0x7f7fffffU;

constexpr std::array<std::uint32_t, 4U> kHorizontalAlignmentBits{0U, 0U, 0x3f800000U, 0x3f000000U};

// Operands were checked finite by the caller, so only denormals need the EE
// signed-zero flush here.
std::uint32_t ee_operand(const std::uint32_t bits) noexcept {
    if ((bits & kExponentMask) == 0U) {
        return bits & kSignBit;
    }
    return bits;
}

// EE rounding truncates toward zero, denormal results flush to a signed zero,
// and an overflow saturates to the largest finite magnitude.
std::uint32_t ee_finish(const double exact, const float rounded) noexcept {
    unsigned int bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0) {
        if (exact < static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    } else if (exact < 0.0) {
        if (exact > static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    }
    const unsigned int exponent = bits & kExponentMask;
    if (exponent == 0U) {
        return bits & kSignBit;
    }
    if (exponent == kExponentMask) {
        return (bits & kSignBit) | kMaxFiniteBits;
    }
    return bits;
}

std::uint32_t ee_add(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_operand(left));
    const float right_value = std::bit_cast<float>(ee_operand(right));
    const double exact = static_cast<double>(left_value) + static_cast<double>(right_value);
    return ee_finish(exact, left_value + right_value);
}

std::uint32_t ee_sub(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_operand(left));
    const float right_value = std::bit_cast<float>(ee_operand(right));
    const double exact = static_cast<double>(left_value) - static_cast<double>(right_value);
    return ee_finish(exact, left_value - right_value);
}

std::uint32_t ee_mul(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_operand(left));
    const float right_value = std::bit_cast<float>(ee_operand(right));
    const double exact = static_cast<double>(left_value) * static_cast<double>(right_value);
    return ee_finish(exact, left_value * right_value);
}

// Shared view of the caller's font, render, and descriptor records.
struct TextDrawCommonInputView {
    std::span<const char16_t> text;
    const TextGlyphFontView& font;
    const TextDrawRenderRecordBits& render;
    const TextDrawDescriptorBits& descriptor;
    std::uint32_t uniform_scale_bits;
};

// Loop-local stop codes. The top-level planners map them onto their public
// stop reasons.
enum class TextDrawLoopStop : std::uint8_t {
    TextConsumed,
    BuilderReturnedZero,
    FiniteRangeLookahead,
};

struct TextDrawLoopPlan {
    std::vector<TextDrawLineStep> lines;
    std::size_t final_cursor_code_unit{};
    std::size_t glyph_builder_call_count{};
    std::int32_t final_line_index{};
    std::uint32_t final_line_y_bits{};
    TextDrawLoopStop stop{TextDrawLoopStop::TextConsumed};
};

// RECOVERY: the loop body is not yet identical to the original GIMPLE. The
// eligibility inputs load in a different order, the cursor and span arithmetic
// are sequenced differently, and some anonymous temporaries differ. Behaviour
// follows the GIMPLE: stop codes, passes, and cursor advancement all match.
// Walks the text one wrapped line at a time. The eligibility predicate and the
// stop-after predicate decide which lines carry passes and whether scanning
// ends early.
template <typename Eligible, typename StopAfter, typename PopulatePasses>
[[nodiscard]] std::optional<TextDrawLoopPlan>
plan_text_draw_lines(const TextDrawCommonInputView& input,
                     const TextGlyphAxisScaleBits axis_scale,
                     const std::uint32_t line_step_bits,
                     Eligible&& eligible,
                     StopAfter&& stop_after,
                     PopulatePasses&& populate_passes) {
    TextDrawLoopPlan result{};
    const auto alignment_bits = kHorizontalAlignmentBits[input.descriptor.layout_flags & 3U];
    std::size_t cursor = 0U;
    std::int32_t line_index = 0;
    std::uint32_t line_y_bits = 0U;
    while (cursor < input.text.size()) {
        if (result.glyph_builder_call_count == std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        ++result.glyph_builder_call_count;
        const auto remaining = input.text.subspan(cursor);
        const auto glyph_plan = plan_text_glyph_rectangles(TextGlyphBuildInput{
            .text = remaining,
            .font = input.font,
            .box = input.render.layout_box,
            .axis_scale = axis_scale,
            .uniform_scale_bits = input.uniform_scale_bits,
            .maximum_line_width_bits = input.descriptor.maximum_line_width_bits,
            .horizontal_alignment_bits = alignment_bits,
            .layout_flags = input.descriptor.layout_flags,
        });
        if (!glyph_plan) {
            if ((input.descriptor.layout_flags & 100U) == 100U) {
                return std::nullopt;
            }
            const auto empty_selection = select_fitting_text_span(TextSpanSelectionInput{
                .text = remaining,
                .font = input.font,
                .maximum_line_width_bits = input.descriptor.maximum_line_width_bits,
                .word_wrap_mode = TextWordWrapMode::PreserveWords,
            });
            if (!empty_selection ||
                empty_selection->begin_code_unit != empty_selection->end_code_unit ||
                empty_selection->end_code_unit != remaining.size()) {
                return std::nullopt;
            }
            cursor += empty_selection->end_code_unit;
            result.final_cursor_code_unit = cursor;
            result.final_line_index = line_index;
            result.final_line_y_bits = line_y_bits;
            result.stop = TextDrawLoopStop::BuilderReturnedZero;
            return result;
        }
        if (glyph_plan->selected_span.begin_code_unit >= glyph_plan->selected_span.end_code_unit ||
            glyph_plan->selected_span.end_code_unit > remaining.size()) {
            return std::nullopt;
        }
        SelectedTextSpan absolute_span = glyph_plan->selected_span;
        absolute_span.begin_code_unit += cursor;
        absolute_span.end_code_unit += cursor;
        const bool eligible_line = eligible(line_index);
        const bool stop_line = stop_after(line_index);

        TextDrawLineStep step{};
        step.line_index = line_index;
        step.cursor_before_code_unit = cursor;
        cursor = absolute_span.end_code_unit;
        step.cursor_after_code_unit = cursor;
        step.selected_span.begin_code_unit = absolute_span.begin_code_unit;
        step.selected_span.end_code_unit = absolute_span.end_code_unit;
        step.selected_span.width_bits = absolute_span.width_bits;
        step.glyph_builder_cost = glyph_plan->cost;
        step.baseline_y_bits = line_y_bits;
        step.eligible_for_submission = eligible_line;
        step.advances_line_state = !stop_line;
        step.rectangles.assign(glyph_plan->rectangles.begin(),
                               glyph_plan->rectangles.begin() + glyph_plan->rectangle_count);
        if (eligible_line && glyph_plan->rectangle_count != 0U) {
            populate_passes(step);
        }
        result.lines.push_back(std::move(step));
        if (stop_line) {
            result.final_cursor_code_unit = cursor;
            result.final_line_index = line_index;
            result.final_line_y_bits = line_y_bits;
            result.stop = TextDrawLoopStop::FiniteRangeLookahead;
            return result;
        }
        line_y_bits = ee_add(line_y_bits, line_step_bits);
        ++line_index;
    }
    result.final_cursor_code_unit = cursor;
    result.final_line_index = line_index;
    result.final_line_y_bits = line_y_bits;
    return result;
}

} // namespace

// RECOVERY: not yet identical to the original GIMPLE. The finite and NUL scans
// lower to different loop shapes, and the split text span names differ. The
// stop mapping, field copies, and result shape follow the original.
std::optional<PlainTextDrawPlan>
plan_plain_text_draw_batch(const PlainTextDrawInput& input) {
    const std::array<std::uint32_t, 11U> values{
        input.font.horizontal_scale_bits,
        input.font.vertical_scale_bits,
        input.render.layout_box.base_x_bits,
        input.render.layout_box.base_y_bits,
        input.render.layout_box.horizontal_extent_bits,
        input.render.vertical_extent_bits,
        input.descriptor.inverse_reference_width_bits,
        input.descriptor.inverse_reference_height_bits,
        input.descriptor.maximum_line_width_bits,
        input.style.uniform_scale_bits,
        input.style.line_gap_bits,
    };
    if (std::find_if(values.begin(), values.end(), [](const std::uint32_t bits) {
            return (bits & kExponentMask) == kExponentMask;
        }) != values.end()) {
        return std::nullopt;
    }
    const std::span<const char16_t> text = input.text;
    if (std::find(text.begin(), text.end(), u'\0') != text.end()) {
        return std::nullopt;
    }

    PlainTextDrawPlan plan{};
    plan.axis_scale.x_bits = ee_mul(input.render.layout_box.horizontal_extent_bits,
                                    input.descriptor.inverse_reference_width_bits);
    plan.axis_scale.y_bits = ee_mul(input.render.vertical_extent_bits,
                                    input.descriptor.inverse_reference_height_bits);
    const std::uint32_t line_height_bits = ee_add(input.style.uniform_scale_bits,
                                                  input.style.line_gap_bits);
    const std::uint32_t line_step_bits = ee_mul(line_height_bits, plan.axis_scale.y_bits);
    const int range_end = input.style.line_range_extent == -1
                                       ? -1
                                       : input.style.first_line + input.style.line_range_extent;
    plan.line_step_bits = line_step_bits;
    plan.inclusive_end_line = range_end;
    plan.batch.raster.raster = input.font_raster;
    plan.stop_reason = text.empty() ? PlainTextStopReason::TextAlreadyEmpty
                                          : PlainTextStopReason::TextConsumed;
    if (text.empty()) {
        return plan;
    }

    const TextDrawCommonInputView common{
        .text = text,
        .font = input.font,
        .render = input.render,
        .descriptor = input.descriptor,
        .uniform_scale_bits = input.style.uniform_scale_bits,
    };
    const TextGlyphAxisScaleBits axis_scale = plan.axis_scale;
    std::optional<TextDrawLoopPlan> lines = plan_text_draw_lines(
        common,
        axis_scale,
        line_step_bits,
        [&input, &range_end](int line_index) {
            return range_end == -1 ||
                   ((line_index >= input.style.first_line) & (line_index <= range_end));
        },
        [&range_end](int line_index) {
            return range_end != -1 && line_index > range_end;
        },
        [&input](TextDrawLineStep& step) {
            step.passes.push_back(TextDrawPassBits{
                .color = input.render.color,
                .translation = {.x_bits = 0U, .y_bits = step.baseline_y_bits},
            });
        });
    if (!lines) {
        return std::nullopt;
    }
    plan.batch.line_steps = std::move(lines->lines);
    plan.final_cursor_code_unit = lines->final_cursor_code_unit;
    plan.glyph_builder_call_count = lines->glyph_builder_call_count;
    plan.final_line_index = lines->final_line_index;
    plan.final_line_y_bits = lines->final_line_y_bits;
    switch (lines->stop) {
    case TextDrawLoopStop::TextConsumed:
        plan.stop_reason = PlainTextStopReason::TextConsumed;
        break;
    case TextDrawLoopStop::BuilderReturnedZero:
        plan.stop_reason = PlainTextStopReason::BuilderReturnedZero;
        break;
    case TextDrawLoopStop::FiniteRangeLookahead:
        plan.stop_reason = PlainTextStopReason::FiniteRangeLookahead;
        break;
    }
    return plan;
}

// RECOVERY: not yet identical to the original GIMPLE. The finite and NUL scans
// lower to different loop shapes, and the split text span names differ. The
// stop mapping, field copies, and result shape follow the original.
std::optional<DropShadowTextDrawPlan>
plan_drop_shadow_text_draw_batch(const DropShadowTextDrawInput& input) {
    const std::array<std::uint32_t, 13U> values{
        input.font.horizontal_scale_bits,
        input.font.vertical_scale_bits,
        input.render.layout_box.base_x_bits,
        input.render.layout_box.base_y_bits,
        input.render.layout_box.horizontal_extent_bits,
        input.render.vertical_extent_bits,
        input.descriptor.inverse_reference_width_bits,
        input.descriptor.inverse_reference_height_bits,
        input.descriptor.maximum_line_width_bits,
        input.style.effect_offset_x_bits,
        input.style.effect_offset_y_bits,
        input.style.uniform_scale_bits,
        input.style.line_gap_bits,
    };
    if (std::find_if(values.begin(), values.end(), [](const std::uint32_t bits) {
            return (bits & kExponentMask) == kExponentMask;
        }) != values.end()) {
        return std::nullopt;
    }
    const std::span<const char16_t> text = input.text;
    if (std::find(text.begin(), text.end(), u'\0') != text.end()) {
        return std::nullopt;
    }

    DropShadowTextDrawPlan plan{};
    plan.axis_scale.x_bits = ee_mul(input.render.layout_box.horizontal_extent_bits,
                                    input.descriptor.inverse_reference_width_bits);
    plan.axis_scale.y_bits = ee_mul(input.render.vertical_extent_bits,
                                    input.descriptor.inverse_reference_height_bits);
    const TextDrawTranslationBits shadow_offset{
        .x_bits = ee_mul(input.style.effect_offset_x_bits, plan.axis_scale.x_bits),
        .y_bits = ee_mul(input.style.effect_offset_y_bits, plan.axis_scale.y_bits),
    };
    const std::uint32_t line_height_bits = ee_add(input.style.uniform_scale_bits,
                                                  input.style.line_gap_bits);
    const std::uint32_t line_step_bits = ee_mul(line_height_bits, plan.axis_scale.y_bits);
    const int range_end = input.style.line_range_extent == -1
                                       ? -1
                                       : input.style.first_line + input.style.line_range_extent;
    plan.shadow_offset = shadow_offset;
    plan.line_step_bits = line_step_bits;
    plan.inclusive_end_line = range_end;
    plan.batch.raster.raster = input.font_raster;
    plan.stop_reason = text.empty() ? DropShadowTextStopReason::TextAlreadyEmpty
                                          : DropShadowTextStopReason::TextConsumed;
    if (text.empty()) {
        return plan;
    }

    const TextColorRgbaBits shadow_color{
        .lanes = {0U, 0U, 0U, input.render.color.lanes[3]},
    };
    const TextDrawCommonInputView common{
        .text = text,
        .font = input.font,
        .render = input.render,
        .descriptor = input.descriptor,
        .uniform_scale_bits = input.style.uniform_scale_bits,
    };
    const TextGlyphAxisScaleBits axis_scale = plan.axis_scale;
    std::optional<TextDrawLoopPlan> lines = plan_text_draw_lines(
        common,
        axis_scale,
        line_step_bits,
        [&input, &range_end](int line_index) {
            return input.style.first_line == -1 ||
                   ((line_index >= input.style.first_line) & (line_index <= range_end));
        },
        [](int) {
            return false;
        },
        [&input, &shadow_color, &shadow_offset](TextDrawLineStep& step) {
            step.passes.reserve(2U);
            step.passes.push_back(TextDrawPassBits{
                .color = shadow_color,
                .translation = {
                    .x_bits = ee_add(0U, shadow_offset.x_bits),
                    .y_bits = ee_add(step.baseline_y_bits, shadow_offset.y_bits),
                },
            });
            step.passes.push_back(TextDrawPassBits{
                .color = input.render.color,
                .translation = {.x_bits = 0U, .y_bits = step.baseline_y_bits},
            });
        });
    if (!lines) {
        return std::nullopt;
    }
    if (lines->stop == TextDrawLoopStop::FiniteRangeLookahead) {
        return std::nullopt;
    }
    plan.batch.line_steps = std::move(lines->lines);
    plan.final_cursor_code_unit = lines->final_cursor_code_unit;
    plan.glyph_builder_call_count = lines->glyph_builder_call_count;
    plan.final_line_index = lines->final_line_index;
    plan.final_line_y_bits = lines->final_line_y_bits;
    plan.stop_reason = lines->stop == TextDrawLoopStop::BuilderReturnedZero
                           ? DropShadowTextStopReason::BuilderReturnedZero
                           : DropShadowTextStopReason::TextConsumed;
    return plan;
}

// RECOVERY: not yet identical to the original GIMPLE. The finite and NUL scans
// lower to different loop shapes, and the split text span names differ. The
// stop mapping, field copies, and result shape follow the original.
std::optional<OutlinedTextDrawPlan>
plan_outlined_text_draw_batch(const OutlinedTextDrawInput& input) {
    const std::array<std::uint32_t, 13U> values{
        input.font.horizontal_scale_bits,
        input.font.vertical_scale_bits,
        input.render.layout_box.base_x_bits,
        input.render.layout_box.base_y_bits,
        input.render.layout_box.horizontal_extent_bits,
        input.render.vertical_extent_bits,
        input.descriptor.inverse_reference_width_bits,
        input.descriptor.inverse_reference_height_bits,
        input.descriptor.maximum_line_width_bits,
        input.style.effect_offset_x_bits,
        input.style.effect_offset_y_bits,
        input.style.uniform_scale_bits,
        input.style.line_gap_bits,
    };
    if (std::find_if(values.begin(), values.end(), [](const std::uint32_t bits) {
            return (bits & kExponentMask) == kExponentMask;
        }) != values.end()) {
        return std::nullopt;
    }
    const std::span<const char16_t> text = input.text;
    if (std::find(text.begin(), text.end(), u'\0') != text.end()) {
        return std::nullopt;
    }

    OutlinedTextDrawPlan plan{};
    plan.axis_scale.x_bits = ee_mul(input.render.layout_box.horizontal_extent_bits,
                                    input.descriptor.inverse_reference_width_bits);
    plan.axis_scale.y_bits = ee_mul(input.render.vertical_extent_bits,
                                    input.descriptor.inverse_reference_height_bits);
    const TextDrawTranslationBits outline_offset{
        .x_bits = ee_mul(input.style.effect_offset_x_bits, plan.axis_scale.x_bits),
        .y_bits = ee_mul(input.style.effect_offset_y_bits, plan.axis_scale.y_bits),
    };
    const std::uint32_t line_height_bits = ee_add(input.style.uniform_scale_bits,
                                                  input.style.line_gap_bits);
    const std::uint32_t line_step_bits = ee_mul(line_height_bits, plan.axis_scale.y_bits);
    plan.outline_offset = outline_offset;
    plan.line_step_bits = line_step_bits;
    plan.batch.raster.raster = input.font_raster;
    plan.stop_reason = text.empty() ? OutlinedTextStopReason::TextAlreadyEmpty
                                          : OutlinedTextStopReason::TextConsumed;
    if (text.empty()) {
        return plan;
    }

    const TextColorRgbaBits outline_color{
        .lanes = {0U, 0U, 0U, input.render.color.lanes[3]},
    };
    const TextDrawCommonInputView common{
        .text = text,
        .font = input.font,
        .render = input.render,
        .descriptor = input.descriptor,
        .uniform_scale_bits = input.style.uniform_scale_bits,
    };
    const TextGlyphAxisScaleBits axis_scale = plan.axis_scale;
    std::optional<TextDrawLoopPlan> lines = plan_text_draw_lines(
        common,
        axis_scale,
        line_step_bits,
        [](int) {
            return true;
        },
        [](int) {
            return false;
        },
        [&input, &outline_color, &outline_offset](TextDrawLineStep& step) {
            step.passes.reserve(5U);
            unsigned int horizontal_offset_bits = outline_offset.x_bits;
            for (std::size_t pair = 0U; pair != 2U; ++pair) {
                step.passes.push_back(TextDrawPassBits{
                    .color = outline_color,
                    .translation = {
                        .x_bits = ee_add(0U, horizontal_offset_bits),
                        .y_bits = ee_add(step.baseline_y_bits, outline_offset.y_bits),
                    },
                });
                step.passes.push_back(TextDrawPassBits{
                    .color = outline_color,
                    .translation = {
                        .x_bits = ee_sub(0U, horizontal_offset_bits),
                        .y_bits = ee_sub(step.baseline_y_bits, outline_offset.y_bits),
                    },
                });
                horizontal_offset_bits = horizontal_offset_bits ^ kSignBit;
            }
            step.passes.push_back(TextDrawPassBits{
                .color = input.render.color,
                .translation = {.x_bits = 0U, .y_bits = step.baseline_y_bits},
            });
        });
    if (!lines) {
        return std::nullopt;
    }
    if (lines->stop == TextDrawLoopStop::FiniteRangeLookahead) {
        return std::nullopt;
    }
    plan.batch.line_steps = std::move(lines->lines);
    plan.final_cursor_code_unit = lines->final_cursor_code_unit;
    plan.glyph_builder_call_count = lines->glyph_builder_call_count;
    plan.final_line_y_bits = lines->final_line_y_bits;
    plan.stop_reason = lines->stop == TextDrawLoopStop::BuilderReturnedZero
                           ? OutlinedTextStopReason::BuilderReturnedZero
                           : OutlinedTextStopReason::TextConsumed;
    return plan;
}

} // namespace takedown::runtime::slus_21050
