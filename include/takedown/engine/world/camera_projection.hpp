#pragma once

// A camera's orientation from its quaternion, and the matrix that takes a
// world point to the camera's screen: x and y scaled by the view window and
// moved by its offset so the visible area spans 0 to 1, z the distance
// along the view direction.

#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct CameraBasis {
    Vector3 right;
    Vector3 up;
    Vector3 at;
};

// The rotation of a quaternion already scaled by the square root of two, so
// its products are the matrix terms directly.
[[nodiscard]] inline CameraBasis basis_from_scaled_quaternion(const Vector4 q) noexcept {
    const float xx = q.x * q.x;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yy = q.y * q.y;
    const float yz = q.y * q.z;
    const float zz = q.z * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    return {{0.0F + ((1.0F - yy) - zz), 0.0F + (xy + wz), 0.0F + (xz - wy)},
            {0.0F + (xy - wz), 0.0F + ((1.0F - xx) - zz), 0.0F + (yz + wx)},
            {0.0F + (xz + wy), 0.0F + (yz - wx), 0.0F + ((1.0F - xx) - yy)}};
}

// Rows of a 4x3 matrix applied as point.x * rows[0] + point.y * rows[1] +
// point.z * rows[2] + rows[3].
struct ScreenProjection {
    Vector3 rows[4];
};

[[nodiscard]] inline ScreenProjection screen_projection(const CameraBasis& basis, const Vector3 position,
                                                        const float inverse_window_x, const float inverse_window_y,
                                                        const float offset_x, const float offset_y) noexcept {
    const float scale_x = 0.0F + inverse_window_x * -0.5F;
    const float shift_x = 0.0F + (0.5F - scale_x * offset_x);
    const Vector3 column_x = basis.right * scale_x + basis.at * shift_x;
    const float scale_y = 0.0F + inverse_window_y * -0.5F;
    const float shift_y = 0.0F + (0.5F + scale_y * offset_y);
    const Vector3 column_y = basis.up * scale_y + basis.at * shift_y;
    const auto along = [&](const Vector3 column) {
        return (position.x * column.x + position.y * column.y) + position.z * column.z;
    };
    ScreenProjection projection;
    projection.rows[0] = {0.0F + column_x.x, 0.0F + column_y.x, 0.0F + basis.at.x};
    projection.rows[1] = {0.0F + column_x.y, 0.0F + column_y.y, 0.0F + basis.at.y};
    projection.rows[2] = {0.0F + column_x.z, 0.0F + column_y.z, 0.0F + basis.at.z};
    projection.rows[3] = {0.0F + (0.5F - (shift_x + along(column_x))), 0.0F + (0.5F - (shift_y + along(column_y))),
                          0.0F + (0.0F - along(basis.at))};
    return projection;
}

} // namespace takedown::engine
