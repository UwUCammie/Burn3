#pragma once

// How close a transform is to orthonormal and to the identity (RenderWare's
// RwMatrixOptimize): the rows' squared length errors, their squared dot
// products, and the squared distance of every element from the identity's,
// each against its tolerance.

#include "takedown/engine/math/bounds.hpp"
#include "takedown/engine/math/vector.hpp"

namespace takedown::engine {

struct MatrixTolerance {
    float normal{};
    float orthogonal{};
    float identity{};
};

struct MatrixShape {
    bool normal{};
    bool orthogonal{};
    bool identity{};
};

[[nodiscard]] inline MatrixShape matrix_shape(const Transform& matrix, const MatrixTolerance& tolerance) noexcept {
    const Vector3 right = matrix.basis[0];
    const Vector3 up = matrix.basis[1];
    const Vector3 at = matrix.basis[2];
    const Vector3 position = matrix.translation;
    const float right_error = dot(right, right) - 1.0F;
    const float up_error = dot(up, up) - 1.0F;
    const float at_error = dot(at, at) - 1.0F;
    MatrixShape shape;
    shape.normal = ((right_error * right_error + up_error * up_error) + at_error * at_error) <= tolerance.normal;
    const float up_at = dot(up, at);
    const float at_right = dot(at, right);
    const float right_up = dot(right, up);
    shape.orthogonal = ((up_at * up_at + at_right * at_right) + right_up * right_up) <= tolerance.orthogonal;
    if (shape.normal && shape.orthogonal) {
        const float right_x = right.x - 1.0F;
        const float up_y = up.y - 1.0F;
        const float at_z = at.z - 1.0F;
        const float error = (((((right_x * right_x + right.y * right.y) + right.z * right.z) +
                               ((up.x * up.x + up_y * up_y) + up.z * up.z)) +
                              ((at.x * at.x + at.y * at.y) + at_z * at_z)) +
                             ((position.x * position.x + position.y * position.y) + position.z * position.z));
        shape.identity = error <= tolerance.identity;
    }
    return shape;
}

} // namespace takedown::engine
