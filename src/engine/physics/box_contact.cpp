#include "takedown/engine/physics/box_contact.hpp"

#include "takedown/engine/math/chopped.hpp"
#include "takedown/engine/math/chopped_vector.hpp"
#include "takedown/engine/math/scalar.hpp"

#include <bit>
#include <cmath>

namespace takedown::engine {
namespace {

constexpr std::size_t kAxisCount = 15U;
constexpr float kTwo = 2.0F;
constexpr float kMinusTwo = -2.0F;
constexpr float kHalf = 0.5F;
constexpr float kMinimumLengthSquared = std::bit_cast<float>(0x2f800000U);  // 2^-32

Vector3 multiply_lanes(const Vector3 a, const Vector3 b) noexcept {
    return {chop_mul(a.x, b.x), chop_mul(a.y, b.y), chop_mul(a.z, b.z)};
}

Vector3 absolute(const Vector3 v) noexcept { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

// A sum the way the original adds a vector's lanes: x and y, then z.
float add_lanes(const Vector3 v) noexcept { return chop_add(chop_add(v.x, v.y), v.z); }

// The vector unit's add of zero, which turns a negative zero into zero.
float without_negative_zero(const float value) noexcept { return chop_add(0.0F, value); }

Vector3 lanes_without_negative_zero(const Vector3 v) noexcept {
    return {without_negative_zero(v.x), without_negative_zero(v.y), without_negative_zero(v.z)};
}

float lane(const Vector3 v, const int index) noexcept { return index == 0 ? v.x : (index == 1 ? v.y : v.z); }

// The box axes and extents the contact code works with (a stack record in
// the original, in the order its supporting-point routines read it).
struct BoxFrame {
    std::array<Vector3, 3> axes;
    Vector4 center;
    Vector3 half;
};

struct Support {
    Vector3 point;
    std::array<float, 6> range;  // per axis: the lowest and highest step along it that stays on the face
};

// The corner of `box` farthest in the direction opposite `direction`, with
// the range of steps along each axis over which that face stays the extreme.
Support support_point(const BoxFrame& box, const Vector3 direction, const BoxContactConstants& constants) noexcept {
    Support result;
    result.point = box.center.xyz();
    for (int axis = 0; axis < 3; ++axis) {
        const float dot_value = chop_dot(direction, box.axes[static_cast<std::size_t>(axis)]);
        const float half = lane(box.half, axis);
        const Vector3 step = chop_mul(box.axes[static_cast<std::size_t>(axis)], half);
        float low;
        float high;
        if (game_less(dot_value, constants.face_low)) {
            result.point = chop_add(result.point, step);
            low = chop_mul(half, kMinusTwo);
            high = 0.0F;
        } else if (game_less_equal(dot_value, constants.face_high)) {
            low = chop_sub(0.0F, half);
            high = half;
        } else {
            result.point = chop_sub(result.point, step);
            low = 0.0F;
            high = chop_mul(half, kTwo);
        }
        result.range[static_cast<std::size_t>(axis) * 2U] = low;
        result.range[static_cast<std::size_t>(axis) * 2U + 1U] = high;
    }
    return result;
}


// Clips a convex polygon to the slab low <= p[axis] <= high, the way the
// original's clipper (0x002beeb0) does it: each crossing is
// inverse * (plane - previous) of the way from the previous vertex, with the
// scalar unit's rounding for that step and the vector unit's for blending.
std::size_t clip_to_slab(const Vector4* input, const std::size_t count, const int axis, const float low,
                         const float high, Vector4* output) noexcept {
    const auto axis_value = [axis](const Vector4& v) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); };
    const auto side_of = [&](const float value) {
        if (game_less(value, low)) {
            return 1;
        }
        return game_less_equal(value, high) ? 0 : 2;
    };
    std::size_t written = 0U;
    int previous_side = side_of(axis_value(input[count - 1U]));
    for (std::size_t index = 0U; index < count; ++index) {
        const Vector4& current = input[index];
        const float current_value = axis_value(current);
        const int side = side_of(current_value);
        if (side != previous_side) {
            const Vector4& previous = input[index == 0U ? count - 1U : index - 1U];
            const float previous_value = axis_value(previous);
            const float inverse = game_div(1.0F, chop_sub(current_value, previous_value));
            const auto crossing = [&](const float plane) {
                const float along = chop_mul(inverse, chop_sub(plane, previous_value));
                const float rest = chop_sub(1.0F, along);
                const Vector3 blended = chop_add(chop_mul(current.xyz(), along), chop_mul(previous.xyz(), rest));
                output[written++] = {blended.x, blended.y, blended.z, current.w};
            };
            if (previous_side == 1) {
                crossing(low);
            } else if (previous_side == 2) {
                crossing(high);
            }
            if (side == 1) {
                crossing(low);
            } else if (side == 2) {
                crossing(high);
            }
        }
        if (side == 0) {
            output[written++] = current;
        }
        previous_side = side;
    }
    return written;
}

} // namespace

BoxContact test_box_pair(const BoxPair& pair, const bool want_contact, const BoxContactConstants& constants,
                         const BoxContactStaleLanes& stale) noexcept {
    BoxContact result;
    const std::array<Vector3, 3> rows{pair.frame[0].xyz(), pair.frame[1].xyz(), pair.frame[2].xyz()};
    const std::array<Vector3, 3> world{constants.world_axes[0].xyz(), constants.world_axes[1].xyz(),
                                       constants.world_axes[2].xyz()};

    // The first box's centre placed in the second box's frame, and the
    // half extents of both.
    const Vector3 first_center = chop_mul(chop_add(pair.first_lower.xyz(), pair.first_upper.xyz()), kHalf);
    Vector3 placed = chop_add(pair.frame[3].xyz(), chop_mul(rows[0], first_center.x));
    placed = chop_add(placed, chop_mul(rows[1], first_center.y));
    placed = chop_add(placed, chop_mul(rows[2], first_center.z));
    const Vector3 first_half = chop_mul(chop_sub(pair.first_upper.xyz(), pair.first_lower.xyz()), kHalf);
    const Vector3 second_half = chop_mul(chop_sub(pair.second_upper.xyz(), pair.second_lower.xyz()), kHalf);
    const Vector3 second_center = chop_mul(chop_add(pair.second_lower.xyz(), pair.second_upper.xyz()), kHalf);

    // The 15 axes, in the order the original tests them: the first box's
    // (x, z, y), the second box's (x, z, y), then each of the first box's
    // against each of the second's.
    std::array<Vector4, kAxisCount> axes;
    axes[0] = pair.frame[0];
    axes[1] = pair.frame[2];
    axes[2] = pair.frame[1];
    axes[3] = constants.world_axes[0];
    axes[4] = constants.world_axes[2];
    axes[5] = constants.world_axes[1];
    constexpr std::array<std::size_t, 3> kOrder{0U, 2U, 1U};
    for (std::size_t first = 0U; first < 3U; ++first) {
        for (std::size_t second = 0U; second < 3U; ++second) {
            const std::size_t index = 6U + first * 3U + second;
            const Vector3 cross_value = chop_cross(rows[kOrder[first]], world[kOrder[second]]);
            axes[index] = {cross_value.x, cross_value.y, cross_value.z, stale.edge_normal_w[first * 3U + second]};
        }
    }

    std::array<float, kAxisCount> overlap{};
    for (std::size_t index = 0U; index < kAxisCount; ++index) {
        const Vector3 axis = axes[index].xyz();
        const Vector3 along_first = lanes_without_negative_zero(
            {chop_dot(axis, rows[0]), chop_dot(axis, rows[1]), chop_dot(axis, rows[2])});
        const float first_radius = add_lanes(multiply_lanes(absolute(along_first), first_half));
        const float first_position = chop_dot(placed, axis);
        const Vector3 along_second = lanes_without_negative_zero(
            {chop_dot(axis, world[0]), chop_dot(axis, world[1]), chop_dot(axis, world[2])});
        const float second_radius = add_lanes(multiply_lanes(absolute(along_second), second_half));
        const float second_position = chop_dot(second_center, axis);
        const float first_low = chop_sub(first_position, first_radius);
        const float first_high = chop_add(first_position, first_radius);
        const float second_low = chop_sub(second_position, second_radius);
        const float second_high = chop_add(second_position, second_radius);
        result.axes_tested = static_cast<std::uint32_t>(index) + 1U;
        if (!game_less_equal(first_low, second_high) || !game_less_equal(second_low, first_high)) {
            return result;
        }
        overlap[index] = chop_sub(minimum(first_high, second_high), maximum(first_low, second_low));
    }
    result.hit = true;
    if (!want_contact) {
        return result;
    }

    // The axis along which the boxes overlap least, as a distance (the
    // overlap over the axis's length); an axis too short to have a
    // direction is skipped.
    float best_depth = constants.depth_start;
    int best = -1;
    for (std::size_t index = 0U; index < kAxisCount; ++index) {
        const Vector3 axis = axes[index].xyz();
        const float length_squared = chop_dot(axis, axis);
        if (game_less(length_squared, kMinimumLengthSquared)) {
            continue;
        }
        const float scale = chop_reciprocal_sqrt(length_squared);
        axes[index] = {chop_mul(axis.x, scale), chop_mul(axis.y, scale), chop_mul(axis.z, scale), axes[index].w};
        const float depth = game_div(overlap[index], game_sqrt(length_squared));
        if (game_less(depth, best_depth)) {
            best_depth = depth;
            best = static_cast<int>(index);
        }
    }
    if (best < 0) {
        result.hit = false;
        return result;
    }

    // The normal points from the first box towards the second.
    Vector4 normal = axes[static_cast<std::size_t>(best)];
    if (!game_less_equal(chop_dot(chop_sub(second_center, placed), normal.xyz()), 0.0F)) {
        normal = {chop_mul(normal.x, -1.0F), chop_mul(normal.y, -1.0F), chop_mul(normal.z, -1.0F), normal.w};
    }
    const Vector3 n = normal.xyz();
    const Vector3 away{chop_sub(0.0F, n.x), chop_sub(0.0F, n.y), chop_sub(0.0F, n.z)};

    const BoxFrame first_frame{rows, {placed.x, placed.y, placed.z, pair.frame[1].w}, first_half};
    const BoxFrame second_frame{world, {second_center.x, second_center.y, second_center.z, pair.second_upper.w},
                                second_half};

    result.has_contact = true;
    result.axis = best;
    result.depth = best_depth;
    result.normal = normal;
    if (best < 3) {
        const Support corner = support_point(second_frame, away, constants);
        const Vector3 shift = chop_mul(n, best_depth);
        const Vector3 point = chop_sub(corner.point, shift);
        result.point = {point.x, point.y, point.z, normal.w};
    } else if (best < 6) {
        const Support corner = support_point(first_frame, n, constants);
        const Vector3 shift = chop_mul(n, best_depth);
        const Vector3 point = chop_add(corner.point, shift);
        result.point = {point.x, point.y, point.z, normal.w};
    } else {
        const std::size_t group = static_cast<std::size_t>(best - 6);
        const std::size_t first_axis = static_cast<std::size_t>(constants.edge_order[group / 3U]);
        const std::size_t second_axis = static_cast<std::size_t>(constants.edge_order[group % 3U]);
        const Support first_corner = support_point(first_frame, n, constants);
        const Support second_corner = support_point(second_frame, away, constants);
        const Vector3 across = chop_cross(n, world[second_axis]);
        const float second_along = chop_dot(across, second_corner.point);
        const float parallel = chop_dot(rows[first_axis], across);
        if (game_less(std::fabs(parallel), constants.axis_epsilon)) {
            result.hit = false;
            result.has_contact = false;
            return result;
        }
        const float first_along = chop_dot(first_corner.point, across);
        float step = game_div(chop_sub(second_along, first_along), parallel);
        step = maximum(step, first_corner.range[first_axis * 2U]);
        step = minimum(step, first_corner.range[first_axis * 2U + 1U]);
        const Vector3 along = chop_mul(rows[first_axis], step);
        const Vector3 edge_point = chop_add(first_corner.point, along);
        const Vector3 point = chop_add(edge_point, chop_mul(n, best_depth));
        result.point = {point.x, point.y, point.z, stale.edge_point_w};
    }
    return result;
}

MeshBoxResult test_mesh_against_box(const std::size_t polygon_count,
                                    const std::function<MeshPolygon(std::size_t)>& polygon,
                                    const std::array<Vector4, 4>& to_box, const std::array<Vector4, 4>& to_world,
                                    const Vector4& box_upper, const Vector4& box_lower, const float leftover_w,
                                    const MeshBoxConstants& constants) noexcept {
    MeshBoxResult result;
    Vector4 minimum_corner = constants.minimum_start;
    Vector4 maximum_corner = constants.maximum_start;
    Vector3 normal_sum{};
    Vector3 center_sum{};
    int touching = 0;

    const auto place = [](const std::array<Vector4, 4>& matrix, const Vector3 v) {
        Vector3 acc = chop_mul(matrix[0].xyz(), v.x);
        acc = chop_add(acc, chop_mul(matrix[1].xyz(), v.y));
        acc = chop_add(acc, chop_mul(matrix[2].xyz(), v.z));
        return chop_add(acc, chop_mul(matrix[3].xyz(), 1.0F));
    };

    for (std::size_t index = 0U; index < polygon_count; ++index) {
        const MeshPolygon source = polygon(index);
        std::array<Vector4, 32> buffer_a;
        std::array<Vector4, 32> buffer_b;
        std::array<Vector4, 3> placed_vertices;
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const Vector3 p = place(to_box, source.vertices[corner].xyz());
            placed_vertices[corner] = {p.x, p.y, p.z, source.vertices[corner].w};
        }
        // The box's slabs in the order x, z, y; the first corner is the
        // larger.
        std::size_t count = clip_to_slab(placed_vertices.data(), 3U, 0, box_lower.x, box_upper.x, buffer_a.data());
        if (count < 3U) {
            continue;
        }
        count = clip_to_slab(buffer_a.data(), count, 2, box_lower.z, box_upper.z, buffer_b.data());
        if (count < 3U) {
            continue;
        }
        count = clip_to_slab(buffer_b.data(), count, 1, box_lower.y, box_upper.y, buffer_a.data());
        if (count < 3U) {
            continue;
        }

        result.hit = true;
        result.last_id = source.id;
        const Vector3 plane = source.plane.xyz();
        normal_sum = chop_add(normal_sum, plane);
        Vector3 vertex_sum = buffer_a[0].xyz();
        // The first vertex does not enter the bounds.
        for (std::size_t corner = 1U; corner < count; ++corner) {
            const Vector3 v = buffer_a[corner].xyz();
            minimum_corner = {minimum(minimum_corner.x, v.x), minimum(minimum_corner.y, v.y),
                              minimum(minimum_corner.z, v.z), minimum_corner.w};
            maximum_corner = {maximum(maximum_corner.x, v.x), maximum(maximum_corner.y, v.y),
                              maximum(maximum_corner.z, v.z), maximum_corner.w};
            vertex_sum = chop_add(vertex_sum, v);
        }
        const float inverse = game_div(1.0F, static_cast<float>(count));
        const Vector3 centroid = chop_mul(vertex_sum, inverse);
        center_sum = chop_add(center_sum, place(to_world, centroid));
        ++touching;
    }
    if (!result.hit) {
        return result;
    }
    result.has_bounds = true;
    result.bounds_minimum = minimum_corner;
    result.bounds_maximum = maximum_corner;

    const float length_squared = chop_dot(normal_sum, normal_sum);
    if (game_less(length_squared, constants.normal_epsilon)) {
        return result;
    }
    const float scale = chop_reciprocal_sqrt(length_squared);
    const Vector3 unit = chop_mul(normal_sum, scale);
    result.normal = {unit.x, unit.y, unit.z, leftover_w};
    const float mean_scale = game_div(1.0F, static_cast<float>(touching));
    const Vector3 mean = chop_mul(center_sum, mean_scale);
    result.point = {mean.x, mean.y, mean.z, leftover_w};

    // How far the mean point is from the box's face in the direction of the
    // normal, as the least of the distances along each axis.
    Vector3 rotated = chop_mul(to_box[0].xyz(), unit.x);
    rotated = chop_add(rotated, chop_mul(to_box[1].xyz(), unit.y));
    rotated = chop_add(rotated, chop_mul(to_box[2].xyz(), unit.z));
    const Vector3 position = place(to_box, mean);

    Vector3 distance;
    Vector3 speed;
    const auto axis_terms = [](const float rotated_lane, const float position_lane, const float upper,
                               const float lower, float& distance_lane, float& speed_lane) {
        if (game_less(rotated_lane, 0.0F)) {
            speed_lane = without_negative_zero(chop_sub(0.0F, rotated_lane));
            distance_lane = without_negative_zero(chop_sub(upper, position_lane));
        } else {
            distance_lane = without_negative_zero(chop_sub(position_lane, lower));
            speed_lane = without_negative_zero(rotated_lane);
        }
    };
    axis_terms(rotated.x, position.x, box_upper.x, box_lower.x, distance.x, speed.x);
    axis_terms(rotated.y, position.y, box_upper.y, box_lower.y, distance.y, speed.y);
    axis_terms(rotated.z, position.z, box_upper.z, box_lower.z, distance.z, speed.z);

    float time;
    if (game_less(chop_mul(distance.x, speed.y), chop_mul(distance.y, speed.x))) {
        if (game_less(chop_mul(distance.x, speed.z), chop_mul(distance.z, speed.x))) {
            time = game_div(distance.x, speed.x);
        } else {
            time = game_div(distance.z, speed.z);
        }
    } else if (game_less(chop_mul(distance.y, speed.z), chop_mul(distance.z, speed.y))) {
        time = game_div(distance.y, speed.y);
    } else {
        time = game_div(distance.z, speed.z);
    }
    if (game_less(time, constants.distance_floor)) {
        time = constants.distance_floor;
    }
    const Vector3 push = chop_mul(unit, time);
    result.push = {push.x, push.y, push.z, leftover_w};
    result.has_contact = true;
    return result;
}

} // namespace takedown::engine
