#include "takedown/runtime/slus_21050_ui_text_draw.hpp"

#include "takedown/guest/types.hpp"
#include "takedown/guest/vu0_macro_fmac.hpp"

#include <algorithm>
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

// EE floating point operands: a denormal reads as a signed zero and an
// infinity or NaN exponent saturates to the largest finite value.
std::uint32_t ee_canonical(const std::uint32_t bits) noexcept {
    if ((bits & kExponentMask) == 0U) {
        return bits & kSignBit;
    }
    if ((bits & kExponentMask) == kExponentMask) {
        return (bits & kSignBit) | kMaxFiniteBits;
    }
    return bits;
}

std::uint32_t ee_multiply(const std::uint32_t left, const std::uint32_t right) noexcept {
    const float left_value = std::bit_cast<float>(ee_canonical(left));
    const float right_value = std::bit_cast<float>(ee_canonical(right));
    const double exact = static_cast<double>(left_value) * static_cast<double>(right_value);
    const float rounded = right_value * left_value;
    std::uint32_t bits = std::bit_cast<std::uint32_t>(rounded);
    if (exact > 0.0) {
        if (exact < static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    } else if (exact < 0.0) {
        if (exact > static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    }
    if ((bits & kMagnitudeMask) == kExponentMask) {
        return (bits & kSignBit) | kMaxFiniteBits;
    }
    if ((bits & kExponentMask) == 0U) {
        if ((bits & 8388607U) != 0U) {
            return bits & kSignBit;
        }
    }
    return bits;
}

std::uint32_t ee_add(const std::uint32_t left, const std::uint32_t right) noexcept {
    const std::uint32_t left_bits = ee_canonical(left);
    const std::uint32_t right_bits = ee_canonical(right);
    const std::uint32_t left_magnitude = left_bits & kMagnitudeMask;
    const std::uint32_t right_magnitude = right_bits & kMagnitudeMask;
    const std::uint32_t left_sign = left_bits & kSignBit;
    const std::uint32_t right_sign = right_bits & kSignBit;
    if (((right_bits | left_bits) & kMagnitudeMask) == 0U) {
        return left_sign == right_sign ? left_sign : 0U;
    }
    if (left_magnitude == 0U) {
        return right_bits;
    }
    if (right_magnitude == 0U) {
        return left_bits;
    }
    const bool left_larger = left_magnitude >= right_magnitude;
    const std::uint32_t larger = left_larger ? left_bits : right_bits;
    const std::uint32_t smaller = left_larger ? right_bits : left_bits;
    const std::uint32_t larger_magnitude = larger & kMagnitudeMask;
    const std::uint32_t exponent_distance = (larger_magnitude >> 23U) - ((smaller >> 23U) & 255U);
    const std::uint64_t larger_significand = (larger & 8388607U) | 8388608U;
    const std::uint64_t smaller_significand = (smaller & 8388607U) | 8388608U;
    const std::uint32_t smaller_exponent = (smaller >> 23U) & 255U;
    if (left_sign == right_sign) {
        if (exponent_distance > 24U) {
            return larger;
        }
        const std::uint64_t exact_significand = (larger_significand << exponent_distance) + smaller_significand;
        const auto clz = static_cast<std::uint32_t>(__builtin_clzll(exact_significand));
        const auto highest_bit = static_cast<std::int32_t>(63U - clz);
        const std::int32_t exponent = highest_bit + static_cast<std::int32_t>(smaller_exponent) - 23;
        std::uint32_t bits = 0U;
        if (exponent > 254) {
            bits = 2139095039U;
        } else if (exponent > 0) {
            const std::int32_t shift = 40 - static_cast<std::int32_t>(clz);
            const std::uint64_t normalized = shift >= 0 ? exact_significand >> shift
                                                        : exact_significand << (clz - 40U);
            bits = (static_cast<std::uint32_t>(exponent) << 23U) |
                   (static_cast<std::uint32_t>(normalized) & 8388607U);
        }
        return left_sign | bits;
    }
    if (left_magnitude == right_magnitude) {
        return 0U;
    }
    if (exponent_distance > 24U) {
        return (larger & kSignBit) | (larger_magnitude - 1U);
    }
    const std::uint64_t exact_significand = (larger_significand << exponent_distance) - smaller_significand;
    const std::uint32_t result_sign = larger & kSignBit;
    if (exact_significand == 0U) {
        return result_sign;
    }
    const auto clz = static_cast<std::uint32_t>(__builtin_clzll(exact_significand));
    const auto highest_bit = static_cast<std::int32_t>(63U - clz);
    const std::int32_t exponent = highest_bit + static_cast<std::int32_t>(smaller_exponent) - 23;
    std::uint32_t bits = 0U;
    if (exponent > 254) {
        bits = 2139095039U;
    } else if (exponent > 0) {
        const std::int32_t shift = 40 - static_cast<std::int32_t>(clz);
        const std::uint64_t normalized = shift >= 0 ? exact_significand >> shift : exact_significand << (clz - 40U);
        bits = (static_cast<std::uint32_t>(exponent) << 23U) | (static_cast<std::uint32_t>(normalized) & 8388607U);
    }
    return result_sign | bits;
}

} // namespace

std::optional<UiTextEffectKind> decode_ui_text_effect_callback(const guest::GuestAddress callback_address) noexcept {
    switch (callback_address) {
    case 3116112U:
        return UiTextEffectKind::Outline;
    case 3116752U:
        return UiTextEffectKind::DropShadow;
    case 3117376U:
        return UiTextEffectKind::Plain;
    default:
        return std::nullopt;
    }
}

TextDrawRenderRecordBits transform_ui_text_render_record(const TextDrawRenderRecordBits& local,
                                                         const UiTextParentTransformBits& parent) noexcept {
    guest::Gpr128 local_color{};
    local_color.words = local.color.lanes;
    guest::Gpr128 parent_color{};
    parent_color.words = parent.color.lanes;
    const guest::Gpr128 multiplied =
        guest::execute_vu0_macro_vector_multiply(guest::Gpr128{}, local_color, parent_color, 15).value;
    TextColorRgbaBits color{};
    for (std::size_t lane = 0U; lane != 4U; ++lane) {
        const auto value = static_cast<std::int32_t>(multiplied.words[lane]);
        color.lanes[lane] = static_cast<std::uint32_t>(std::min(value, 1065353216));
    }
    TextDrawRenderRecordBits result{};
    const std::uint32_t scaled_x = ee_multiply(parent.scale_x_bits, local.layout_box.base_x_bits);
    const std::uint32_t scaled_y = ee_multiply(parent.scale_y_bits, local.layout_box.base_y_bits);
    result.layout_box.base_x_bits = ee_add(parent.translation_x_bits, scaled_x);
    result.layout_box.base_y_bits = ee_add(parent.translation_y_bits, scaled_y);
    result.layout_box.horizontal_extent_bits = ee_multiply(local.layout_box.horizontal_extent_bits, parent.scale_x_bits);
    result.vertical_extent_bits = ee_multiply(local.vertical_extent_bits, parent.scale_y_bits);
    result.color = color;
    return result;
}

std::optional<std::vector<UiTextDrawSubmission>>
associate_ui_text_draw_submissions(const UiDrawStatePlan& state_plan,
                                   const std::span<const UiTextNodeSubmissionBits> nodes) {
    if (nodes.size() != state_plan.submissions.size()) {
        return std::nullopt;
    }
    if (nodes.size() > 65536U) {
        return std::nullopt;
    }
    std::vector<UiTextDrawSubmission> result;
    result.reserve(nodes.size());
    for (std::size_t index = 0U; index != nodes.size(); ++index) {
        const UiTextNodeSubmissionBits& node = nodes[index];
        const UiDrawSubmissionState& planned = state_plan.submissions[index];
        const std::optional<UiTextEffectKind> effect = decode_ui_text_effect_callback(node.callback_address);
        if (!effect.has_value()) {
            return std::nullopt;
        }
        if (node.key.value != planned.key.value || node.bucket_index != planned.bucket_index ||
            node.node_index != planned.node_index) {
            return std::nullopt;
        }
        result.push_back(UiTextDrawSubmission{
            .key = node.key,
            .bucket_index = node.bucket_index,
            .node_index = node.node_index,
            .effect = *effect,
            .render = transform_ui_text_render_record(node.local_render, node.parent),
            .state = planned.state,
        });
    }
    return result;
}

} // namespace takedown::runtime::slus_21050
