#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace takedown::runtime::slus_21050 {
using ObjectSubmissionQuadword = std::array<std::uint32_t,4>;

// An oriented box and six inward-facing distances: w - dot(xyz, point).
// No guest registers, memory addresses or microprogram are needed to classify it.
struct ObjectVisibilityInput {
    std::array<float,3> origin{};
    std::array<std::array<float,3>,3> edges{};
    std::array<std::array<float,4>,6> planes{};
};
enum class ObjectVisibility : std::uint32_t { Outside=0, Intersecting=1, Inside=2 };
// Shared preparation for classification and the temporary completion adapter.
// Corner masks use one bit per corner, independently for each clipping plane.
struct ObjectVisibilityCalculation {
    ObjectVisibility classification{};
    std::array<std::array<float,3>,8> corners{};
    std::array<std::array<float,4>,6> planes{};
    std::array<float,6> last_corner_xy_distances{};
    std::array<std::uint8_t,6> outside_corner_masks{};
    bool rejected_first_four{};
};
[[nodiscard]] ObjectVisibilityCalculation prepare_object_visibility(const ObjectVisibilityInput&) noexcept;
[[nodiscard]] ObjectVisibility classify_object_visibility(const ObjectVisibilityInput&) noexcept;
// Classify corners already transformed in their original arithmetic order.
// Reconstructing them from transformed edges would change rounding boundaries.
[[nodiscard]] ObjectVisibilityCalculation prepare_object_corner_visibility(
    const std::array<std::array<float,3>,8>& corners,
    const std::array<std::array<float,4>,6>& planes) noexcept;

// Game-side decision and packet contents, independent of EE registers and
// instruction execution. Classification is supplied by the object's visibility
// operation: 0 rejects, 2 makes the object fully visible, other values retain
// the requested mode. Mode 1 already establishes visibility.
struct ObjectSubmissionInput {
    std::array<ObjectSubmissionQuadword,3> object_templates{};
    std::span<const ObjectSubmissionQuadword> queued_state;
    std::uint32_t geometry_reference{}, trailing_reference{};
    std::uint32_t requested_mode{}, classification{};
    std::uint8_t previous_mode{}, token{};
};
struct ObjectSubmissionResult {
    std::size_t quadwords{};
    std::uint32_t mode{}, state_change_bits{};
    bool submitted{};
};

// Temporary packet encoder for the reference rendering backend. A future
// native renderer consumes the typed submission without this console encoding.
// Output must be disjoint from the input templates and queued state.
// Insufficient capacity or an unsupported mode rejects without writes.
[[nodiscard]] std::optional<ObjectSubmissionResult> assemble_object_submission(
    const ObjectSubmissionInput&, std::span<ObjectSubmissionQuadword> output) noexcept;
}
