// RECOVERY: rebuilt from the original object's GIMPLE. CMake builds this file
// with -frounding-math -ffp-contract=off; tools/gimple.py compiles without them,
// so its data check reports extra folded constants even though every function
// matches. Compiled with those two flags the object matches completely.
#include "takedown/runtime/slus_21050_instance_batch.hpp"

#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace takedown::runtime::slus_21050 {
namespace {

// Every arithmetic step goes through the VU clamp-mode-3 helpers: operands are
// clamped, products truncated and sums chopped toward zero.
using Point = std::array<float, 3>;
using Corners = std::array<Point, 8>;

[[nodiscard]] std::uint32_t bits_of(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float multiply(const float left, const float right) {
    bool underflow = false;
    bool overflow = false;
    return std::bit_cast<float>(
        guest::vu0_detail::multiply_bits(bits_of(left), bits_of(right), underflow, overflow));
}

[[nodiscard]] float add(const float left, const float right, const bool subtract = false) {
    bool underflow = false;
    bool overflow = false;
    return std::bit_cast<float>(guest::vu0_detail::add_subtract_bits(
        bits_of(left), bits_of(right), subtract, underflow, overflow));
}

Point linear_transform(const InstanceBatchTransform& transform, const Point& point) {
    Point result;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto x = multiply(transform.basis[0].xyz[axis], point[0]);
        const auto y = multiply(transform.basis[1].xyz[axis], point[1]);
        const auto xy = add(x, y);
        const auto z = multiply(transform.basis[2].xyz[axis], point[2]);
        result[axis] = add(xy, z);
    }
    return result;
}

// sqrt and reciprocal results use the same directed narrowing as the adders.
[[nodiscard]] std::uint32_t chop(const double exact) {
    bool underflow = false;
    bool overflow = false;
    return guest::vu0_detail::chop_exact_bits(exact, underflow, overflow);
}

// 1 / sqrt(squared); a zero sum saturates to signed Fmax instead of dividing.
[[nodiscard]] float inverse_length(const float squared) {
    const auto bits = guest::vu0_detail::clamp3_operand_bits(bits_of(squared));
    if ((bits & 0x7fffffffU) == 0U) {
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f7fffffU);
    }
    const auto root =
        chop(std::sqrt(std::fabs(static_cast<double>(std::bit_cast<float>(bits)))));
    return std::bit_cast<float>(chop(1.0 / static_cast<double>(std::bit_cast<float>(root))));
}

// VU FTOI: magnitudes of at least 2^31 (and non-finite values) saturate by sign.
[[nodiscard]] std::uint32_t signed_saturated(const float value) noexcept {
    const auto bits = bits_of(value);
    if ((bits & 0x7f800000U) >= 0x4f000000U) {
        return (bits & 0x80000000U) != 0U ? 0x80000000U : 0x7fffffffU;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

// Depth and scale fields: the retained transform with its W column and Z depth
// rewritten, plus the four output parameters for the selected output mode.
InstanceBatchPreparedRecord prepare_record(const InstanceBatchSetup& setup,
                                           const InstanceBatchTransform& transform) {
    InstanceBatchPreparedRecord record{transform};
    const auto zero = add(1.0F, 1.0F, true);
    for (auto& column : record.transform.basis) {
        column.w_bits = bits_of(add(zero, column.xyz[2]));
    }
    const auto depth = add(zero, transform.translation.xyz[2]);
    record.transform.translation.xyz[2] = depth;
    record.transform.translation.w_bits = bits_of(depth);
    if (setup.output_mode == InstanceBatchOutputMode::PackedScale) {
        const auto scale = add(0.0F, 128.5F);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            record.parameter_bits[axis] = signed_saturated(
                multiply(scale, std::bit_cast<float>(transform.basis[axis].w_bits)));
        }
        record.parameter_bits[3] = signed_saturated(setup.output_parameters[3]);
        return record;
    }
    std::array<std::array<float, 3>, 3> normalized{};
    for (std::size_t column = 0; column < 3; ++column) {
        const auto& source = transform.basis[column].xyz;
        const float inverse = inverse_length(
            add(add(multiply(source[0], source[0]), multiply(source[1], source[1])),
                multiply(source[2], source[2])));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            normalized[column][axis] = add(0.0F, multiply(source[axis], inverse));
        }
    }
    for (std::size_t column = 0; column < 3; ++column) {
        const float parameter = setup.output_parameters[column];
        const auto x = multiply(parameter, normalized[column][0]);
        const auto y = multiply(parameter, normalized[column][1]);
        const auto xy = add(x, y);
        const auto z = multiply(parameter, normalized[column][2]);
        record.parameter_bits[column] = bits_of(add(xy, z));
    }
    record.parameter_bits[3] =
        bits_of(add(multiply(setup.output_parameters[3], 1.0F / 128.0F), 65536.0F));
    const auto x = multiply(std::bit_cast<float>(transform.basis[0].w_bits), 0.33333334F);
    const auto y = multiply(std::bit_cast<float>(transform.basis[1].w_bits), 0.33333334F);
    const auto xy = add(x, y);
    const auto z = multiply(std::bit_cast<float>(transform.basis[2].w_bits), 0.33333334F);
    const auto sum = add(xy, z);
    const auto biased = add(add(0.0F, sum), 65536.0F);
    record.transform.basis[0].xyz[2] = add(0.0F, biased);
    record.transform.basis[1].xyz[2] = add(0.0F, multiply(sum, 0.5F));
    return record;
}


// Linear part first, then the translation scaled by 1.0 as the VU does.
[[nodiscard]] Point transform_point(const InstanceBatchTransform& transform, const Point& point) {
    auto result = linear_transform(transform, point);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result[axis] = add(result[axis], multiply(transform.translation.xyz[axis], 1.0F));
    }
    return result;
}

// Distance ignores X: 1 + dy^2 + dz^2 against the threshold, KeepNear/KeepFar.
[[nodiscard]] bool distance_rejected(const InstanceBatchSetup& setup,
                                     const InstanceBatchTransform& transform) {
    if (setup.distance_mode == InstanceBatchDistanceMode::Disabled) {
        return false;
    }
    const auto delta_y =
        add(setup.distance_reference[1], transform.translation.xyz[1], true);
    const auto delta_z =
        add(setup.distance_reference[2], transform.translation.xyz[2], true);
    const auto distance = add(add(1.0F, multiply(delta_y, delta_y)),
                              multiply(1.0F, multiply(delta_z, delta_z)));
    return (bits_of(setup.distance_mode == InstanceBatchDistanceMode::KeepNear
                        ? add(setup.distance_threshold, distance, true)
                        : add(distance, setup.distance_threshold, true)) &
            0x80000000U) != 0U;
}

[[nodiscard]] bool supported(const InstanceBatchDistanceMode mode) noexcept {
    switch (mode) {
    case InstanceBatchDistanceMode::Disabled:
    case InstanceBatchDistanceMode::KeepNear:
    case InstanceBatchDistanceMode::KeepFar:
        return true;
    }
    return false;
}

[[nodiscard]] bool supported(const InstanceBatchOutputMode mode) noexcept {
    switch (mode) {
    case InstanceBatchOutputMode::PackedScale:
    case InstanceBatchOutputMode::NormalizedLighting:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_request(const InstanceBatchSetup& setup,
                                 std::span<const InstanceBatchView> views,
                                 const InstanceBatchPending& pending) {
    if (views.empty() || views.size() > kInstanceBatchViewLimit) {
        return false;
    }
    if (!supported(setup.distance_mode) || !supported(setup.output_mode)) {
        return false;
    }
    for (std::size_t view = 0; view < kInstanceBatchViewLimit; ++view) {
        if (pending[view].size() >= kInstanceBatchFlushSize) {
            return false;
        }
        if (views.size() <= view && !pending[view].empty()) {
            return false;
        }
        for (const auto& record : pending[view]) {
            if (record.visibility != ObjectVisibility::Inside &&
                record.visibility != ObjectVisibility::Intersecting) {
                return false;
            }
        }
    }
    return true;
}

// The eight corners of the bounding box in their original arithmetic order.
[[nodiscard]] std::array<Point, 8> make_local_corners(const InstanceBatchSetup& setup) {
    Point extent;
    Point origin{};
    std::array<Point, 3> edges{};
    const float zero = add(0.0F, 0.0F, true);
    for (auto& edge : edges) {
        edge.fill(zero);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto size = add(setup.maximum[axis], setup.minimum[axis], true);
        extent[axis] = size;
        edges[axis][axis] = add(0.0F, size);
        origin[axis] = add(0.0F, setup.minimum[axis]);
    }
    std::array<Point, 8> corners;
    corners[0] = setup.minimum;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        corners[1][axis] = add(origin[axis], multiply(edges[0][axis], 1.0F));
        corners[2][axis] = add(origin[axis], multiply(edges[2][axis], 1.0F));
        corners[4][axis] = add(origin[axis], multiply(edges[1][axis], 1.0F));
        corners[3][axis] = add(corners[1][axis], edges[2][axis]);
        corners[5][axis] = add(corners[1][axis], edges[1][axis]);
        corners[6][axis] = add(corners[2][axis], edges[1][axis]);
        corners[7][axis] = add(corners[3][axis], edges[1][axis]);
    }
    return corners;
}

// An instance either uses its own transform or is composed with the retained one.
[[nodiscard]] InstanceBatchTransform instance_transform(const InstanceBatchSetup& setup,
                                                        const InstanceBatchInstance& instance) {
    InstanceBatchTransform transform = instance.transform;
    InstanceBatchTransform source;
    if (instance.compose_retained_transform) {
        source = instance.transform;
        for (auto& column : source.basis) {
            for (auto& value : column.xyz) {
                value = add(0.0F, value);
            }
        }
        for (auto& value : source.translation.xyz) {
            value = add(0.0F, value);
        }
        for (std::size_t column = 0; column < 3; ++column) {
            transform.basis[column].xyz =
                linear_transform(source, setup.retained_transform.basis[column].xyz);
        }
        transform.translation.xyz =
            transform_point(source, setup.retained_transform.translation.xyz);
    }
    return transform;
}

} // namespace

std::optional<InstanceBatchResult> cull_and_prepare_instance_batch(
    const InstanceBatchSetup& setup, std::span<const InstanceBatchView> views,
    std::span<const InstanceBatchInstance> instances, const InstanceBatchPending& pending,
    bool final_drain) {
    if (!valid_request(setup, views, pending)) {
        return std::nullopt;
    }

    InstanceBatchResult result{};
    result.pending = pending;
    result.decisions.reserve(instances.size());
    for (std::size_t view = 0; view != views.size(); ++view) {
        result.pending[view].reserve(kInstanceBatchFlushSize);
    }

    const std::array<Point, 8> local_corners = make_local_corners(setup);

    for (std::size_t index = 0; index != instances.size(); ++index) {
        InstanceBatchDecision decision{};
        const InstanceBatchTransform transform = instance_transform(setup, instances[index]);
        decision.distance_rejected = distance_rejected(setup, transform);
        if (!decision.distance_rejected) {
            Corners corners;
            for (std::size_t corner = 0; corner < 8; ++corner) {
                corners[corner] = transform_point(transform, local_corners[corner]);
            }
            std::optional<InstanceBatchPreparedRecord> prepared;
            for (std::size_t view = 0; view != views.size(); ++view) {
                const auto visibility =
                    prepare_object_corner_visibility(corners, views[view].planes).classification;
                decision.visibility[view] = visibility;
                if (visibility == ObjectVisibility::Outside) {
                    continue;
                }
                if (!prepared) {
                    prepared = prepare_record(setup, transform);
                }
                prepared->visibility = visibility;
                result.pending[view].push_back(*prepared);
            }
        }
        result.decisions.push_back(decision);
        for (std::size_t view = 0; view != views.size(); ++view) {
            auto& records = result.pending[view];
            if (records.size() == kInstanceBatchFlushSize) {
                result.flushes.push_back({view, index + 1, false, std::move(records)});
                records = {};
                records.reserve(kInstanceBatchFlushSize);
            }
        }
    }
    if (final_drain) {
        for (std::size_t view = 0; view != views.size(); ++view) {
            auto& records = result.pending[view];
            if (!records.empty()) {
                result.flushes.push_back({view, instances.size(), true, std::move(records)});
                records = {};
            }
        }
    }
    return result;
}

} // namespace takedown::runtime::slus_21050
