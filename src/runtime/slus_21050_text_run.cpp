#include "takedown/runtime/slus_21050_text_run.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr std::uint32_t kSignBit = 0x80000000U;
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kMagnitudeMask = 0x7fffffffU;
constexpr std::uint32_t kMaxFiniteBits = 0x7f7fffffU;
constexpr std::uint32_t kFcr31CauseMask = 0x0000C000U;
constexpr std::uint32_t kFcr31CauseOverflow = 0x00008010U;
constexpr std::uint32_t kFcr31CauseUnderflow = 0x00004008U;
constexpr std::uint32_t kFcr31ConditionBit = 0x00800000U;

// Accumulator and FCR31 flags threaded through one text run plan.
struct TextCop1Trace {
    std::uint32_t accumulator_;
    std::uint32_t fcr31_;
};

// The planners verify that operands are finite, so only denormals need the
// EE signed-zero flush here.
std::uint32_t ee_operand(const std::uint32_t bits) noexcept {
    if ((bits & kExponentMask) == 0U) {
        return bits & kSignBit;
    }
    return bits;
}

// Records overflow or underflow in FCR31 and returns the result as the EE unit
// would: an infinity saturates and a denormal flushes to a signed zero.
std::uint32_t cop1_finish(TextCop1Trace& arithmetic, const std::uint32_t bits) noexcept {
    arithmetic.fcr31_ = arithmetic.fcr31_ & ~kFcr31CauseMask;
    if ((bits & kMagnitudeMask) == kExponentMask) {
        arithmetic.fcr31_ = arithmetic.fcr31_ | kFcr31CauseOverflow;
        return (bits & kSignBit) | kMaxFiniteBits;
    }
    if ((bits & kExponentMask) == 0U && (bits & 8388607U) != 0U) {
        arithmetic.fcr31_ = arithmetic.fcr31_ | kFcr31CauseUnderflow;
        return bits & kSignBit;
    }
    return bits;
}

[[gnu::always_inline]] inline std::uint32_t cop1_add(TextCop1Trace& arithmetic, const std::uint32_t left,
                       const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_operand(left));
    const float right_value = std::bit_cast<float>(ee_operand(right));
    const double exact = static_cast<double>(left_value) + static_cast<double>(right_value);
    unsigned int bits = std::bit_cast<std::uint32_t>(left_value + right_value);
    if (exact > 0.0) {
        if (exact < static_cast<double>(left_value + right_value)) {
            bits = bits - 1U;
        }
    } else if (exact < 0.0) {
        if (exact > static_cast<double>(left_value + right_value)) {
            bits = bits - 1U;
        }
    }
    return cop1_finish(arithmetic, bits);
}

[[gnu::always_inline]] inline std::uint32_t cop1_mul(TextCop1Trace& arithmetic, const std::uint32_t left,
                       const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_operand(left));
    const float right_value = std::bit_cast<float>(ee_operand(right));
    const double exact = static_cast<double>(left_value) * static_cast<double>(right_value);
    unsigned int bits = std::bit_cast<std::uint32_t>(left_value * right_value);
    if (exact > 0.0) {
        if (exact < static_cast<double>(left_value * right_value)) {
            bits = bits - 1U;
        }
    } else if (exact < 0.0) {
        if (exact > static_cast<double>(left_value * right_value)) {
            bits = bits - 1U;
        }
    }
    return cop1_finish(arithmetic, bits);
}

// Sets or clears the FCR31 condition bit from a comparison against zero.
void cop1_set_condition(TextCop1Trace& arithmetic, const bool less_than_zero) noexcept {
    if (less_than_zero) {
        arithmetic.fcr31_ = arithmetic.fcr31_ | kFcr31ConditionBit;
    } else {
        arithmetic.fcr31_ = arithmetic.fcr31_ & ~kFcr31ConditionBit;
    }
}

// Exponent field all ones: infinity or NaN, rejected by the planners.
bool has_nonfinite_exponent(const std::uint32_t bits) noexcept {
    return (bits & kExponentMask) == kExponentMask;
}

bool is_negative_float(const std::uint32_t bits) noexcept {
    return std::bit_cast<float>(ee_operand(bits)) < 0.0f;
}

// One font lookup for a single UTF-16 code unit. Each examined record in the
// collision bucket consumes one probe; an exhausted probe budget fails the
// lookup, as does a zero code unit.
const TextGlyphMetricsBits* find_text_run_glyph(const TextGlyphFontView& font, const char16_t unit,
                                                std::size_t& remaining_probes) noexcept {
    if (unit == u'\0') {
        return nullptr;
    }
    for (const TextGlyphMetricsBits& record : font.buckets[static_cast<std::size_t>(unit & 127U)]) {
        if (remaining_probes == 0U) {
            return nullptr;
        }
        --remaining_probes;
        if (record.code_unit == unit) {
            return &record;
        }
    }
    return nullptr;
}

// Emits one visible glyph at the current pen. Negative-U glyphs are handled by
// the caller and advance without emitting.
[[gnu::always_inline]] inline void emit_text_run_rectangle(TextRunRectanglePlan& plan, TextCop1Trace& arithmetic,
                             const TextGlyphMetricsBits& glyph, const std::uint32_t pen_x,
                             const std::uint32_t box_base_y, const std::uint32_t horizontal_scale,
                             const std::uint32_t vertical_scale) {
    const unsigned int bearing_x = cop1_mul(arithmetic, glyph.bearing_x_bits, horizontal_scale);
    const unsigned int bearing_y = cop1_mul(arithmetic, glyph.bearing_y_bits, vertical_scale);
    const unsigned int x0 = cop1_add(arithmetic, pen_x, bearing_x);
    const unsigned int y0 = cop1_add(arithmetic, box_base_y, bearing_y);
    const unsigned int width = cop1_mul(arithmetic, glyph.texture_width_bits, horizontal_scale);
    const unsigned int height = cop1_mul(arithmetic, glyph.texture_height_bits, vertical_scale);
    const unsigned int x1 = cop1_add(arithmetic, x0, width);
    const unsigned int y1 = cop1_add(arithmetic, y0, height);
    const unsigned int u1 = cop1_add(arithmetic, glyph.texture_u_bits, glyph.texture_width_bits);
    const unsigned int v1 = cop1_add(arithmetic, glyph.texture_v_bits, glyph.texture_height_bits);
    plan.rectangles.push_back(TextGlyphRectangleBits{
        .position = {x0, y0, x1, y1},
        .texture = {glyph.texture_u_bits, glyph.texture_v_bits, u1, v1},
    });
}

} // namespace

// RECOVERY: rebuilt from the GIMPLE behaviour, not yet byte-identical. The COP1
// trace (accumulator and FCR31) is reconstructed from its uses, and the statement
// order and temporaries differ from the original.
std::optional<TextRunRectanglePlan>
plan_resolved_text_run_rectangles(const ResolvedTextRunBuildInput& input,
                                  const ResolvedTextRunBuildBudget& budget) {
    if (input.glyphs.size() > budget.maximum_glyphs) {
        return std::nullopt;
    }
    TextRunRectanglePlan plan{};
    const std::size_t glyph_count = input.glyphs.size();
    const std::size_t maximum_rectangles = budget.maximum_rectangles;
    if (glyph_count == 0U) {
        return plan;
    }
    const std::array<std::uint32_t, 7U> fields{
        input.font_scale.x_bits,
        input.font_scale.y_bits,
        input.box.base_x_bits,
        input.box.base_y_bits,
        input.box.horizontal_extent_bits,
        input.vertical_extent_bits,
        input.horizontal_stretch_bits,
    };
    for (const std::uint32_t bits : fields) {
        if (has_nonfinite_exponent(bits)) {
            return std::nullopt;
        }
    }

    TextCop1Trace arithmetic;
    arithmetic.accumulator_ = 0U;
    arithmetic.fcr31_ = 0U;
    const unsigned int axis_x = cop1_mul(arithmetic, input.box.horizontal_extent_bits, input.font_scale.x_bits);
    const unsigned int vertical_scale = cop1_mul(arithmetic, input.vertical_extent_bits, input.font_scale.y_bits);
    const unsigned int horizontal_scale = cop1_mul(arithmetic, axis_x, input.horizontal_stretch_bits);
    unsigned int pen_x = cop1_add(arithmetic, 0U, input.box.base_x_bits);

    plan.rectangles.reserve(std::min(glyph_count, maximum_rectangles));
    for (std::size_t index = 0U; index != glyph_count; ++index) {
        const TextGlyphMetricsBits* glyph = &input.glyphs[index];
        const std::array<std::uint32_t, 7U> fields{
            glyph->texture_u_bits,
            glyph->texture_v_bits,
            glyph->texture_width_bits,
            glyph->texture_height_bits,
            glyph->bearing_x_bits,
            glyph->bearing_y_bits,
            glyph->advance_x_bits,
        };
        for (const std::uint32_t bits : fields) {
            if (has_nonfinite_exponent(bits)) {
                return std::nullopt;
            }
        }
        const bool negative_u = is_negative_float(glyph->texture_u_bits);
        cop1_set_condition(arithmetic, negative_u);
        if (!negative_u) {
            if (plan.rectangles.size() == maximum_rectangles) {
                return std::nullopt;
            }
            emit_text_run_rectangle(plan, arithmetic, *glyph, pen_x, input.box.base_y_bits,
                                    horizontal_scale, vertical_scale);
        }
        arithmetic.accumulator_ = pen_x;
        pen_x = cop1_add(arithmetic, arithmetic.accumulator_,
                         cop1_mul(arithmetic, glyph->advance_x_bits, horizontal_scale));
    }
    return plan;
}

// RECOVERY: rebuilt from the GIMPLE behaviour, not yet byte-identical. The COP1
// trace (accumulator and FCR31) is reconstructed from its uses, and the statement
// order and temporaries differ from the original.
std::optional<TextRunRectanglePlan>
plan_text_run_rectangles(const TextRunBuildInput& input, const TextRunBuildBudget& budget) {
    if (input.text.size() > budget.maximum_code_units) {
        return std::nullopt;
    }
    TextRunRectanglePlan plan{};
    std::size_t remaining_probes = budget.maximum_glyph_record_probes;
    const std::size_t glyph_count = input.text.size();
    const std::size_t maximum_rectangles = budget.maximum_rectangles;
    if (glyph_count == 0U) {
        return plan;
    }
    const std::array<std::uint32_t, 7U> fields{
        input.font.horizontal_scale_bits,
        input.font.vertical_scale_bits,
        input.box.base_x_bits,
        input.box.base_y_bits,
        input.box.horizontal_extent_bits,
        input.vertical_extent_bits,
        input.horizontal_stretch_bits,
    };
    for (const std::uint32_t bits : fields) {
        if (has_nonfinite_exponent(bits)) {
            return std::nullopt;
        }
    }

    TextCop1Trace arithmetic;
    arithmetic.accumulator_ = 0U;
    arithmetic.fcr31_ = 0U;
    const unsigned int axis_x = cop1_mul(arithmetic, input.box.horizontal_extent_bits, input.font.horizontal_scale_bits);
    const unsigned int vertical_scale = cop1_mul(arithmetic, input.vertical_extent_bits, input.font.vertical_scale_bits);
    const unsigned int horizontal_scale = cop1_mul(arithmetic, axis_x, input.horizontal_stretch_bits);
    unsigned int pen_x = cop1_add(arithmetic, 0U, input.box.base_x_bits);

    plan.rectangles.reserve(std::min(glyph_count, maximum_rectangles));
    for (std::size_t index = 0U; index != glyph_count; ++index) {
        const TextGlyphMetricsBits* glyph = find_text_run_glyph(input.font, input.text[index], remaining_probes);
        if (glyph == nullptr) {
            return std::nullopt;
        }
        const std::array<std::uint32_t, 7U> fields{
            glyph->texture_u_bits,
            glyph->texture_v_bits,
            glyph->texture_width_bits,
            glyph->texture_height_bits,
            glyph->bearing_x_bits,
            glyph->bearing_y_bits,
            glyph->advance_x_bits,
        };
        for (const std::uint32_t bits : fields) {
            if (has_nonfinite_exponent(bits)) {
                return std::nullopt;
            }
        }
        const bool negative_u = is_negative_float(glyph->texture_u_bits);
        cop1_set_condition(arithmetic, negative_u);
        if (!negative_u) {
            if (plan.rectangles.size() == maximum_rectangles) {
                return std::nullopt;
            }
            emit_text_run_rectangle(plan, arithmetic, *glyph, pen_x, input.box.base_y_bits,
                                    horizontal_scale, vertical_scale);
        }
        arithmetic.accumulator_ = pen_x;
        pen_x = cop1_add(arithmetic, arithmetic.accumulator_,
                         cop1_mul(arithmetic, glyph->advance_x_bits, horizontal_scale));
    }
    return plan;
}

} // namespace takedown::runtime::slus_21050
