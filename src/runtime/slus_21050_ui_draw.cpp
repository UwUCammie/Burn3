#include "takedown/runtime/slus_21050_ui_draw.hpp"

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
constexpr std::uint32_t kMagnitudeMask = 0x7fffffffU;
constexpr std::uint32_t kMaxFiniteBits = 0x7f7fffffU;

// EE canonical operand: a denormal reads as a signed zero and an infinity or
// NaN exponent saturates to the largest finite value.
std::uint32_t ee_canonical(const std::uint32_t bits) noexcept {
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
    const float left_value = std::bit_cast<float>(ee_canonical(left));
    const float right_value = std::bit_cast<float>(ee_canonical(right));
    const double exact = static_cast<double>(left_value) + static_cast<double>(right_value);
    const float rounded = right_value + left_value;
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

struct WorkingUiState {
    UiDrawContext1Bits live;
    UiScissorState scissor;
    std::uint64_t cached_tex1{};
};

[[nodiscard]] UiDrawStateSnapshot make_snapshot(const WorkingUiState& state, const UiRenderTargetView& target,
                                  const UiPacketPositionOffsetBits offset) {
    UiDrawStateSnapshot result;
    result.target = target;
    result.frame.raw = state.live.frame;
    result.frame.framebuffer_pointer = static_cast<std::uint16_t>(state.live.frame) & 511U;
    result.frame.framebuffer_width = static_cast<std::uint8_t>(state.live.frame >> 16) & 63U;
    result.frame.pixel_storage_format = static_cast<std::uint8_t>(state.live.frame >> 24) & 63U;
    result.frame.write_mask = static_cast<std::uint32_t>(state.live.frame >> 32);
    result.alpha.raw = state.live.alpha;
    result.alpha.source_a = static_cast<std::uint8_t>(state.live.alpha) & 3U;
    result.alpha.source_b = static_cast<std::uint8_t>(state.live.alpha >> 2) & 3U;
    result.alpha.factor_c = static_cast<std::uint8_t>(state.live.alpha >> 4) & 3U;
    result.alpha.source_d = static_cast<std::uint8_t>(state.live.alpha >> 6) & 3U;
    result.alpha.fixed_alpha = static_cast<std::uint8_t>(state.live.alpha >> 32);
    result.texture_sampling.raw = state.live.tex1;
    result.texture_sampling.maximum_mip_level = static_cast<std::uint8_t>(state.live.tex1 >> 2) & 7U;
    result.texture_sampling.magnification_linear = (state.live.tex1 & 32U) != 0U;
    result.texture_sampling.minification_filter = static_cast<std::uint8_t>(state.live.tex1 >> 6) & 7U;
    result.texture_wrap.raw = state.live.clamp;
    result.texture_wrap.horizontal_mode = static_cast<std::uint8_t>(state.live.clamp) & 3U;
    result.texture_wrap.vertical_mode = static_cast<std::uint8_t>(state.live.clamp >> 2) & 3U;
    result.texture_wrap.horizontal_minimum = static_cast<std::uint16_t>(state.live.clamp >> 4) & 1023U;
    result.texture_wrap.horizontal_maximum = static_cast<std::uint16_t>(state.live.clamp >> 14) & 1023U;
    result.texture_wrap.vertical_minimum = static_cast<std::uint16_t>(state.live.clamp >> 24) & 1023U;
    result.texture_wrap.vertical_maximum = static_cast<std::uint16_t>(state.live.clamp >> 34) & 1023U;
    result.scissor = state.scissor;
    result.test_raw = state.live.test;
    result.zbuf_raw = state.live.zbuf;
    result.xyoffset_raw = state.live.xyoffset;
    result.colclamp_raw = kUiDrawColclampRaw;
    result.packet_position_offset = offset;
    return result;
}

} // namespace

// RECOVERY: rebuilt from the GIMPLE behaviour, not yet byte-identical. Statement
// order, temporaries, and zero-initialisation style differ from the original.
bool is_canonical_ui_draw_state_snapshot(const UiDrawStateSnapshot& state) noexcept {
    const auto& scissor = state.scissor;
    const bool bounds_ordered = scissor.left <= scissor.right && scissor.top <= scissor.bottom;
    const UiFrameState frame_decoded = [&state] {
        UiFrameState decoded{};
        decoded.raw = state.frame.raw;
        decoded.framebuffer_pointer = static_cast<std::uint16_t>(state.frame.raw) & 511U;
        decoded.framebuffer_width = static_cast<std::uint8_t>(state.frame.raw >> 16) & 63U;
        decoded.pixel_storage_format = static_cast<std::uint8_t>(state.frame.raw >> 24) & 63U;
        decoded.write_mask = static_cast<std::uint32_t>(state.frame.raw >> 32);
        return decoded;
    }();
    if (state.frame.raw != frame_decoded.raw || state.frame.framebuffer_pointer != frame_decoded.framebuffer_pointer ||
        state.frame.framebuffer_width != frame_decoded.framebuffer_width ||
        state.frame.pixel_storage_format != frame_decoded.pixel_storage_format ||
        state.frame.write_mask != frame_decoded.write_mask) {
        return false;
    }
    const UiAlphaEquationState alpha_decoded = [&state] {
        UiAlphaEquationState decoded{};
        decoded.raw = state.alpha.raw;
        decoded.source_a = static_cast<std::uint8_t>(state.alpha.raw) & 3U;
        decoded.source_b = static_cast<std::uint8_t>(state.alpha.raw >> 2) & 3U;
        decoded.factor_c = static_cast<std::uint8_t>(state.alpha.raw >> 4) & 3U;
        decoded.source_d = static_cast<std::uint8_t>(state.alpha.raw >> 6) & 3U;
        decoded.fixed_alpha = static_cast<std::uint8_t>(state.alpha.raw >> 32);
        return decoded;
    }();
    if (state.alpha.raw != alpha_decoded.raw || state.alpha.source_a != alpha_decoded.source_a ||
        state.alpha.source_b != alpha_decoded.source_b || state.alpha.factor_c != alpha_decoded.factor_c ||
        state.alpha.source_d != alpha_decoded.source_d || state.alpha.fixed_alpha != alpha_decoded.fixed_alpha) {
        return false;
    }
    const UiTextureSamplingState sampling_decoded = [&state] {
        UiTextureSamplingState decoded{};
        decoded.raw = state.texture_sampling.raw;
        decoded.maximum_mip_level = static_cast<std::uint8_t>(state.texture_sampling.raw >> 2) & 7U;
        decoded.magnification_linear = (state.texture_sampling.raw & 32U) != 0U;
        decoded.minification_filter = static_cast<std::uint8_t>(state.texture_sampling.raw >> 6) & 7U;
        return decoded;
    }();
    if (state.texture_sampling.maximum_mip_level != sampling_decoded.maximum_mip_level ||
        state.texture_sampling.magnification_linear != sampling_decoded.magnification_linear ||
        state.texture_sampling.minification_filter != sampling_decoded.minification_filter) {
        return false;
    }
    const UiTextureWrapState wrap_decoded = [&state] {
        UiTextureWrapState decoded{};
        decoded.raw = state.texture_wrap.raw;
        decoded.horizontal_mode = static_cast<std::uint8_t>(state.texture_wrap.raw) & 3U;
        decoded.vertical_mode = static_cast<std::uint8_t>(state.texture_wrap.raw >> 2) & 3U;
        decoded.horizontal_minimum = static_cast<std::uint16_t>(state.texture_wrap.raw >> 4) & 1023U;
        decoded.horizontal_maximum = static_cast<std::uint16_t>(state.texture_wrap.raw >> 14) & 1023U;
        decoded.vertical_minimum = static_cast<std::uint16_t>(state.texture_wrap.raw >> 24) & 1023U;
        decoded.vertical_maximum = static_cast<std::uint16_t>(state.texture_wrap.raw >> 34) & 1023U;
        return decoded;
    }();
    if (state.texture_wrap.raw != wrap_decoded.raw || state.texture_wrap.horizontal_mode != wrap_decoded.horizontal_mode ||
        state.texture_wrap.vertical_mode != wrap_decoded.vertical_mode ||
        state.texture_wrap.horizontal_minimum != wrap_decoded.horizontal_minimum ||
        state.texture_wrap.horizontal_maximum != wrap_decoded.horizontal_maximum ||
        state.texture_wrap.vertical_minimum != wrap_decoded.vertical_minimum ||
        state.texture_wrap.vertical_maximum != wrap_decoded.vertical_maximum) {
        return false;
    }
    const auto horizontal = static_cast<std::uint64_t>(scissor.left) |
                            (static_cast<std::uint64_t>(scissor.right) << 16);
    const auto vertical_top = static_cast<std::uint64_t>(static_cast<std::uint32_t>(scissor.top)) << 32;
    const auto vertical_bottom = static_cast<std::uint64_t>(scissor.bottom) << 48;
    if (scissor.raw != (vertical_bottom | (vertical_top | horizontal))) {
        return false;
    }
    if (scissor.has_pixels != bounds_ordered) {
        return false;
    }
    if (state.target.width <= 0 || state.target.height <= 0) {
        return false;
    }
    if (state.target.width > kUiDrawStateMaximumTargetExtent || state.target.height > kUiDrawStateMaximumTargetExtent) {
        return false;
    }
    return state.colclamp_raw == kUiDrawColclampRaw;
}

namespace {

constexpr std::uint64_t kFrameCompareMask = 72057590804054527ULL;
constexpr std::uint64_t kFrameTopByteBits = 0xFF00000000000000ULL;
constexpr std::uint64_t kFilterClearMask = 0x1E0ULL;
constexpr std::uint64_t kFilterLinearBits = 0x120ULL;
constexpr std::uint32_t kPacketOffsetScaleBits = 0x3D800000U;
constexpr std::uint32_t kHalfBits = 0x3F000000U;
constexpr std::uint32_t kSaturatingExponentLimit = 0x4E800000U;

constexpr std::array<std::uint64_t, 4U> kAlphaRows{68U, 72U, 66U, 42U};

// Exact int32 to EE binary32, rounded toward zero, with denormals flushed.
std::uint32_t ee_from_int(const std::int32_t value) noexcept {
    const double exact = static_cast<double>(value);
    const float rounded = static_cast<float>(exact);
    std::uint32_t bits = std::bit_cast<std::uint32_t>(rounded);
    if (value > 0) {
        if (exact < static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    } else if (value != 0) {
        if (exact > static_cast<double>(rounded)) {
            bits = bits - 1U;
        }
    }
    return ee_canonical(bits);
}

std::uint32_t ee_mul(const std::uint32_t left, const std::uint32_t right) noexcept {
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

// Truncating EE float to int32 with saturation: a magnitude above 2^30 clamps
// to the int32 limit selected by the sign bit.
std::int32_t ee_saturating_int(const std::uint32_t bits) noexcept {
    if ((bits & kExponentMask) <= kSaturatingExponentLimit) {
        return static_cast<std::int32_t>(std::bit_cast<float>(bits));
    }
    return static_cast<std::int32_t>(bits) >= 0 ? std::numeric_limits<std::int32_t>::max()
                                                : std::numeric_limits<std::int32_t>::min();
}

// Orders two raw binary32 bit patterns the way the retail rectangle code does:
// signed bit comparisons, with both-negative pairs swapped.
std::pair<std::uint32_t, std::uint32_t> ordered_bit_pair(const std::uint32_t first,
                                                         const std::uint32_t second) noexcept {
    const auto a = static_cast<std::int32_t>(first);
    const auto b = static_cast<std::int32_t>(second);
    const bool both_negative = a < 0 && b < 0;
    const std::int32_t low = both_negative ? std::max(a, b) : std::min(a, b);
    const std::int32_t high = both_negative ? std::min(a, b) : std::max(a, b);
    return {static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high)};
}

bool has_exponent_255(const std::uint32_t bits) noexcept {
    return (bits & kExponentMask) == kExponentMask;
}

std::uint64_t encode_scissor_raw(const std::int32_t left, const std::int32_t right,
                                 const std::int32_t top, const std::int32_t bottom) noexcept {
    const auto horizontal = static_cast<std::uint64_t>(left) |
                            (static_cast<std::uint64_t>(right) << 16);
    const auto vertical_top = static_cast<std::uint64_t>(static_cast<std::uint32_t>(top)) << 32;
    const auto vertical_bottom = static_cast<std::uint64_t>(bottom) << 48;
    return horizontal | vertical_top | vertical_bottom;
}

} // namespace

// RECOVERY: rebuilt from the GIMPLE behaviour, not yet byte-identical. Statement
// order, temporaries, and zero-initialisation style differ from the original.
std::optional<UiDrawStatePlan>
plan_ui_draw_bucket_states(const UiDrawStateInput& input) {
    if (input.target.width <= 0 || input.target.height <= 0) {
        return std::nullopt;
    }
    if (input.target.width > kUiDrawStateMaximumTargetExtent ||
        input.target.height > kUiDrawStateMaximumTargetExtent) {
        return std::nullopt;
    }
    if (input.buckets.size() > kUiDrawStateMaximumBucketCount) {
        return std::nullopt;
    }
    if (((input.live.frame ^ input.cached.frame) & kFrameCompareMask) != 0U) {
        return std::nullopt;
    }

    // First pass: validate the retained submission budget over active buckets.
    std::size_t submission_count = 0U;
    for (const UiDrawBucketView& bucket : input.buckets) {
        if ((bucket.flags & 1U) != 0U) {
            continue;
        }
        const std::size_t size = bucket.submissions.size();
        if (size == 0U) {
            continue;
        }
        if (kUiDrawStateMaximumSubmissionCount - submission_count < size) {
            return std::nullopt;
        }
        submission_count += size;
    }

    UiDrawStatePlan plan{};
    WorkingUiState state{};
    state.live = input.live;
    state.scissor.raw = input.live.scissor;
    state.scissor.left = static_cast<std::int32_t>(static_cast<std::uint32_t>(input.live.scissor)) & 2047;
    state.scissor.right = static_cast<std::int32_t>(static_cast<std::uint32_t>(input.live.scissor >> 16)) & 2047;
    state.scissor.top = static_cast<std::int32_t>(static_cast<std::uint32_t>(input.live.scissor >> 32)) & 2047;
    state.scissor.bottom = static_cast<std::int32_t>(static_cast<std::uint32_t>(input.live.scissor >> 48)) & 2047;
    state.scissor.has_pixels = state.scissor.left <= state.scissor.right &&
                               state.scissor.top <= state.scissor.bottom;
    state.cached_tex1 = input.cached.tex1;

    // Packet position offset: integer target origin plus the cached XY offset
    // in 1/16 units, converted to binary32 and scaled by 1/16 on each axis.
    const auto x_local = static_cast<std::uint32_t>(input.cached.xyoffset) & 65535U;
    const auto y_local = static_cast<std::uint32_t>(input.cached.xyoffset >> 32) & 65535U;
    const auto x_position = static_cast<std::int32_t>(
        x_local + static_cast<std::uint32_t>(input.target.origin_x * 16));
    const auto y_position = static_cast<std::int32_t>(
        y_local + static_cast<std::uint32_t>(input.target.origin_y * 16));
    const UiPacketPositionOffsetBits packet_position_offset{
        .x = ee_mul(ee_from_int(x_position), kPacketOffsetScaleBits),
        .y = ee_mul(ee_from_int(y_position), kPacketOffsetScaleBits),
    };

    plan.entry_state = make_snapshot(state, input.target, packet_position_offset);
    plan.final_bucket_state.colclamp_raw = 1U;
    plan.transitions.reserve(input.buckets.size() * 4U);
    plan.submissions.reserve(submission_count);

    std::uint32_t previous_flags = 64U;
    std::uint64_t previous_scissor_key = 0U;
    for (std::size_t bucket_index = 0U; bucket_index != input.buckets.size(); ++bucket_index) {
        const UiDrawBucketView& bucket = input.buckets[bucket_index];
        if ((bucket.flags & 1U) != 0U || bucket.submissions.empty()) {
            continue;
        }
        const std::uint32_t changed_flags = bucket.flags ^ previous_flags;

        if ((changed_flags & 102U) != 0U) {
            const std::uint32_t alpha_flags = bucket.flags & 38U;
            std::uint8_t alpha_preset = 0U;
            if (alpha_flags != 0U && (alpha_flags & (alpha_flags - 1U)) == 0U) {
                const auto index = static_cast<std::size_t>(std::countr_zero(alpha_flags)) - 1U;
                alpha_preset = std::array<std::uint8_t, 5U>{1U, 2U, 0U, 0U, 3U}[index];
            }
            state.live.alpha = kAlphaRows[alpha_preset];
            state.live.frame = input.cached.frame | kFrameTopByteBits;
            plan.transitions.push_back(UiDrawStateTransition{
                .bucket_index = bucket_index,
                .change = UiAlphaAndFrameTransition{
                    .preset = static_cast<UiAlphaPreset>(alpha_preset),
                },
                .state_after = make_snapshot(state, input.target, packet_position_offset),
            });
        }

        if ((changed_flags & 72U) != 0U) {
            const auto filter_preset = static_cast<UiTextureFilterPreset>((bucket.flags >> 3) & 1U);
            state.cached_tex1 &= ~kFilterClearMask;
            if (filter_preset == UiTextureFilterPreset::Linear) {
                state.cached_tex1 |= kFilterLinearBits;
            }
            state.live.tex1 = state.cached_tex1;
            plan.transitions.push_back(UiDrawStateTransition{
                .bucket_index = bucket_index,
                .change = UiTextureFilterTransition{.preset = filter_preset},
                .state_after = make_snapshot(state, input.target, packet_position_offset),
            });
        }

        if ((changed_flags & 80U) != 0U) {
            const auto wrap_preset = static_cast<UiTextureWrapPreset>((bucket.flags >> 4) & 1U);
            state.live.clamp = wrap_preset == UiTextureWrapPreset::Clamp ? 5U : 0U;
            plan.transitions.push_back(UiDrawStateTransition{
                .bucket_index = bucket_index,
                .change = UiTextureWrapTransition{.preset = wrap_preset},
                .state_after = make_snapshot(state, input.target, packet_position_offset),
            });
        }

        if (bucket.scissor.transition_key != previous_scissor_key) {
            UiScissorState scissor{};
            UiScissorSource source = UiScissorSource::FullTarget;
            if (bucket.scissor.transition_key == 0U) {
                if (bucket.scissor.rectangle.has_value()) {
                    return std::nullopt;
                }
                scissor.left = input.target.origin_x;
                scissor.right = input.target.origin_x + (input.target.width - 1);
                scissor.top = input.target.origin_y;
                scissor.bottom = input.target.origin_y + (input.target.height - 1);
                scissor.has_pixels = true;
            } else {
                if (!bucket.scissor.rectangle.has_value()) {
                    return std::nullopt;
                }
                const UiClipRectangleBits& rectangle = *bucket.scissor.rectangle;
                if (has_exponent_255(rectangle.x) || has_exponent_255(rectangle.y) ||
                    has_exponent_255(rectangle.width) || has_exponent_255(rectangle.height)) {
                    return std::nullopt;
                }
                source = UiScissorSource::ClipRectangle;
                const std::uint32_t x_end = ee_add(rectangle.x, rectangle.width);
                const std::uint32_t y_end = ee_add(rectangle.y, rectangle.height);
                const auto [x_low, x_high] = ordered_bit_pair(rectangle.x, x_end);
                const auto [y_low, y_high] = ordered_bit_pair(rectangle.y, y_end);
                std::int32_t left = ee_saturating_int(ee_mul(x_low, kHalfBits));
                std::int32_t right = ee_saturating_int(ee_mul(x_high, kHalfBits));
                std::int32_t top = ee_saturating_int(ee_mul(y_low, kHalfBits));
                std::int32_t bottom = ee_saturating_int(ee_mul(y_high, kHalfBits));
                const bool has_pixels = input.target.width > left && right > 0 &&
                                        input.target.height > top && bottom > 0;
                if (has_pixels) {
                    left = std::max(left, 0);
                    right = std::min(right, input.target.width - 1);
                    top = std::max(top, 0);
                    bottom = std::min(bottom, input.target.height - 1);
                } else {
                    left = 0;
                    right = -1;
                    top = -1;
                    bottom = 0;
                }
                scissor.left = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(left) + static_cast<std::uint32_t>(input.target.origin_x));
                scissor.right = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(right) + static_cast<std::uint32_t>(input.target.origin_x));
                scissor.top = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(top) + static_cast<std::uint32_t>(input.target.origin_y));
                scissor.bottom = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(bottom) + static_cast<std::uint32_t>(input.target.origin_y));
                scissor.has_pixels = has_pixels;
            }
            scissor.raw = encode_scissor_raw(scissor.left, scissor.right, scissor.top, scissor.bottom);
            state.scissor = scissor;
            plan.transitions.push_back(UiDrawStateTransition{
                .bucket_index = bucket_index,
                .change = UiScissorTransition{
                    .source = source,
                    .transition_key = bucket.scissor.transition_key,
                },
                .state_after = make_snapshot(state, input.target, packet_position_offset),
            });
        }

        for (std::size_t node_index = 0U; node_index != bucket.submissions.size(); ++node_index) {
            plan.submissions.push_back(UiDrawSubmissionState{
                .key = bucket.submissions[node_index],
                .bucket_index = bucket_index,
                .node_index = node_index,
                .state = make_snapshot(state, input.target, packet_position_offset),
            });
        }
        previous_flags = bucket.flags;
        previous_scissor_key = bucket.scissor.transition_key;
    }

    plan.final_bucket_state = make_snapshot(state, input.target, packet_position_offset);
    plan.final_cached_tex1 = state.cached_tex1;
    return plan;
}

} // namespace takedown::runtime::slus_21050
