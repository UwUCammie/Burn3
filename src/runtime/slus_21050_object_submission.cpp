#include "takedown/runtime/slus_21050_object_submission.hpp"

#include "takedown/guest/ps2_float.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace takedown::runtime::slus_21050 {
namespace {

// The game's float unit has no denormals or infinities: denormal operands are
// signed zeros and infinities are signed Fmax.
[[nodiscard]] inline float normalize_operand(const float value) noexcept {
    auto bits = std::bit_cast<std::uint32_t>(value);
    const auto exponent = bits & 0x7f800000U;
    if (exponent == 0U) {
        bits &= 0x80000000U;
    } else if (exponent == 0x7f800000U) {
        bits = (bits & 0x80000000U) | 0x7f7fffffU;
    }
    return std::bit_cast<float>(bits);
}

// RECOVERY: the original's saturation branches carry 50/50 probabilities where these early
// returns are predicted 34/66; the code is otherwise identical.
// Round an exact host-double result toward zero, saturating at Fmax and
// flushing denormal results to a signed zero.
[[nodiscard]] inline float finish_result(const double exact) noexcept {
    constexpr float kMax = 3.4028234663852886e+38F;
    if (exact > static_cast<double>(kMax)) {
        return kMax;
    }
    if (exact < -static_cast<double>(kMax)) {
        return -kMax;
    }
    auto bits = guest::ps2_narrow_binary32_toward_zero_bits(exact);
    if ((bits & 0x7f800000U) == 0U) {
        bits = std::signbit(exact) ? 0x80000000U : 0U;
    }
    return std::bit_cast<float>(bits);
}

inline double exact_product(float a, float b) noexcept {
    return static_cast<double>(normalize_operand(a)) * static_cast<double>(normalize_operand(b));
}

float corner_sum(float a, float b) noexcept {
    return finish_result(static_cast<double>(normalize_operand(a)) +
                         static_cast<double>(normalize_operand(b)));
}

// Products are already finished values, so only the running distance needs normalizing.
float corner_difference(float a, float b) noexcept {
    return finish_result(static_cast<double>(normalize_operand(a)) - static_cast<double>(b));
}

float corner_product(float a, float b) noexcept {
    return finish_result(static_cast<double>(normalize_operand(a)) *
                         static_cast<double>(normalize_operand(b)));
}

} // namespace

ObjectVisibilityCalculation prepare_object_visibility(const ObjectVisibilityInput& input) noexcept {
    ObjectVisibilityCalculation result{};
    result.corners[0] = input.origin;
    for (unsigned int lane = 0; lane != 3U; ++lane) {
        result.corners[1][lane] = corner_sum(result.corners[0][lane], input.edges[0][lane]);
        result.corners[3][lane] = corner_sum(result.corners[0][lane], input.edges[1][lane]);
        result.corners[4][lane] = corner_sum(result.corners[0][lane], input.edges[2][lane]);
        result.corners[2][lane] = corner_sum(result.corners[1][lane], input.edges[1][lane]);
        result.corners[5][lane] = corner_sum(result.corners[1][lane], input.edges[2][lane]);
        result.corners[6][lane] = corner_sum(result.corners[2][lane], input.edges[2][lane]);
        result.corners[7][lane] = corner_sum(result.corners[3][lane], input.edges[2][lane]);
    }
    return prepare_object_corner_visibility(result.corners, input.planes);
}

ObjectVisibility classify_object_visibility(const ObjectVisibilityInput& input) noexcept {
    return prepare_object_visibility(input).classification;
}

ObjectVisibilityCalculation prepare_object_corner_visibility(
    const std::array<std::array<float, 3>, 8>& corners,
    const std::array<std::array<float, 4>, 6>& planes) noexcept {
    ObjectVisibilityCalculation result{};
    result.corners = corners;
    result.planes = planes;
    for (unsigned int i = 4U; i != 6U; ++i) {
        for (auto& value : result.planes[i]) {
            value = corner_sum(0.0F, value);
        }
    }
    bool crosses_plane = false;
    bool rejected = false;
    for (unsigned int plane_index = 0; plane_index != 6U; ++plane_index) {
        if (plane_index == 4U && rejected) {
            result.rejected_first_four = true;
            result.classification = ObjectVisibility::Outside;
            return result;
        }
        const auto& plane = result.planes[plane_index];
        std::uint8_t outside = 0;
        for (unsigned int index = 0; index != 8U; ++index) {
            const auto& corner = corners[index];
            auto distance = normalize_operand(plane[3]);
            for (unsigned int axis = 0; axis != 2U; ++axis) {
                distance = corner_difference(
                    distance, finish_result(exact_product(plane[axis], corner[axis])));
            }
            if (index == 7U) {
                result.last_corner_xy_distances[plane_index] = distance;
            }
            distance = corner_difference(
                distance, finish_result(exact_product(plane[2], corner[2])));
            if (std::signbit(distance)) {
                outside |= static_cast<std::uint8_t>(1U << index);
            }
        }
        result.outside_corner_masks[plane_index] = outside;
        rejected |= outside == 0xffU;
        crosses_plane |= outside != 0U;
    }
    result.classification = rejected      ? ObjectVisibility::Outside
                            : crosses_plane ? ObjectVisibility::Intersecting
                                            : ObjectVisibility::Inside;
    return result;
}

// RECOVERY: GCC's dump stores the first cursor through a type-punned MEM reference; the
// generated code is otherwise identical.
std::optional<ObjectSubmissionResult> assemble_object_submission(
    const ObjectSubmissionInput& input, std::span<ObjectSubmissionQuadword> output) noexcept {
    if (input.requested_mode > 3U || input.queued_state.size() > 1024U) {
        return std::nullopt;
    }
    if (input.requested_mode != 1U && input.classification == 0U) {
        return ObjectSubmissionResult{};
    }
    const auto mode =
        input.requested_mode == 1U || input.classification == 2U ? 1U : input.requested_mode;
    const auto changed = static_cast<std::uint32_t>(input.queued_state.size()) |
                         (~static_cast<std::uint32_t>(input.previous_mode) & mode);
    const auto count = (changed != 0U ? 2U : 0U) + input.queued_state.size() + 3U;
    if (count > output.size()) {
        return std::nullopt;
    }
    auto cursor = output.begin();
    *cursor++ = ObjectSubmissionQuadword{0x50000000U, input.geometry_reference, 0U, 0U};
    *cursor++ = input.object_templates[0];
    cursor = std::copy(input.queued_state.begin(), input.queued_state.end(), cursor);
    if (changed != 0U) {
        *cursor++ = input.object_templates[1];
        const auto operation = mode == 2U ? 2U : mode == 3U ? 10U : 0U;
        *cursor++ = ObjectSubmissionQuadword{operation, 0U, input.token, changed};
    }
    *cursor++ = input.trailing_reference != 0U
                    ? ObjectSubmissionQuadword{0x50000000U, input.trailing_reference, 0x17000000U, 0U}
                    : input.object_templates[2];
    return ObjectSubmissionResult{count, mode, changed, true};
}

} // namespace takedown::runtime::slus_21050
