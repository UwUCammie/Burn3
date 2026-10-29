#pragma once

#include "takedown/runtime/slus_21050_object_submission.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

// XYZ is geometric; W is an independent per-column parameter, not a
// homogeneous coordinate. Preserve its representation until arithmetic uses it.
struct InstanceBatchColumn {
    std::array<float, 3> xyz{};
    std::uint32_t w_bits{};
};
struct InstanceBatchTransform {
    std::array<InstanceBatchColumn, 3> basis{};
    InstanceBatchColumn translation{};
};

enum class InstanceBatchDistanceMode : std::uint8_t {
    Disabled = 0,
    KeepNear = 1,
    KeepFar = 2,
};
enum class InstanceBatchOutputMode : std::uint8_t {
    PackedScale = 0,
    NormalizedLighting = 1,
};
struct InstanceBatchSetup {
    std::array<float, 3> minimum{}, maximum{};
    InstanceBatchTransform retained_transform{};
    std::array<float, 4> output_parameters{};
    std::array<float, 3> distance_reference{};
    float distance_threshold{};
    InstanceBatchDistanceMode distance_mode{};
    InstanceBatchOutputMode output_mode{};
};
struct InstanceBatchView {
    // Six ordinary planes: distance = W - X*x - Y*y - Z*z.
    // View-specific far-plane adjustment has already been applied.
    std::array<std::array<float, 4>, 6> planes{};
};
struct InstanceBatchInstance {
    InstanceBatchTransform transform{};
    bool compose_retained_transform{};
};

struct InstanceBatchPreparedRecord {
    // The prepared output columns, including the game's depth and scale
    // fields. Translation XYZ remains geometric; visibility is a separate
    // value rather than an integer hidden in a floating-point coordinate.
    InstanceBatchTransform transform{};
    // PackedScale produces four signed integer representations;
    // NormalizedLighting produces four binary32 representations.
    std::array<std::uint32_t, 4> parameter_bits{};
    ObjectVisibility visibility{ObjectVisibility::Outside};
};
inline constexpr std::size_t kInstanceBatchViewLimit = 3;
inline constexpr std::size_t kInstanceBatchFlushSize = 14;
using InstanceBatchPending =
    std::array<std::vector<InstanceBatchPreparedRecord>, kInstanceBatchViewLimit>;

struct InstanceBatchDecision {
    bool distance_rejected{};
    std::array<ObjectVisibility, kInstanceBatchViewLimit> visibility{};
};
struct InstanceBatchFlush {
    std::size_t view_index{};
    // A full flush occurs immediately after this many input instances have
    // been processed. Final partial drains use instances.size().
    std::size_t after_instance_count{};
    bool final_drain{};
    std::vector<InstanceBatchPreparedRecord> records;
};
struct InstanceBatchResult {
    std::vector<InstanceBatchDecision> decisions;
    std::vector<InstanceBatchFlush> flushes;
    InstanceBatchPending pending;
};

// Complete cull/prepare/queue operation for the recovered instance-batch
// family. All inputs are values or immutable borrowed spans; the result owns
// its records. Full flushes retain instance-major/view-major order. An optional
// final drain emits nonempty remaining views in view order.
//
// The distance rule intentionally uses 1 + deltaY^2 + deltaZ^2, ignoring X.
// KeepNear rejects a negative (threshold - distance), KeepFar a negative
// (distance - threshold), including the arithmetic's signed-zero behavior.
// An adapter maps original nonzero/non-one distance modes to KeepFar and
// nonzero output modes to NormalizedLighting after establishing this family.
//
// Reject unsupported enums, zero or >3 views, pending lists of >=14 records,
// nonempty pending lists for absent views, and pending records whose visibility
// is not Inside or Intersecting, without modifying inputs.
// Zero views is deliberately rejected: it is not equivalent to the original
// operation's stale-view behavior. Allocation failures propagate, with no
// input mutation. No guest machine, scheduler or packet encoding is owned here.
[[nodiscard]] std::optional<InstanceBatchResult> cull_and_prepare_instance_batch(
    const InstanceBatchSetup&, std::span<const InstanceBatchView>,
    std::span<const InstanceBatchInstance>, const InstanceBatchPending& pending = {},
    bool final_drain = false);

} // namespace takedown::runtime::slus_21050
