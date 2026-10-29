#include "takedown/engine/math/bounds.hpp"

#include <algorithm>

namespace takedown::engine {

namespace {

[[nodiscard]] float component(const Vector3& v, const int axis) noexcept {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

} // namespace

Bounds transform_bounds(const Transform& transform, const Bounds& local) noexcept {
    float low[3];
    float high[3];
    for (int axis = 0; axis < 3; ++axis) {
        // Each basis row scales one local axis; whichever box face gives the
        // smaller (larger) product contributes to the low (high) extent.
        float terms_low[3];
        float terms_high[3];
        for (int row = 0; row < 3; ++row) {
            const float scale = component(transform.basis[row], axis);
            const float from_minimum = component(local.minimum, row) * scale;
            const float from_maximum = component(local.maximum, row) * scale;
            terms_low[row] = std::min(from_minimum, from_maximum);
            terms_high[row] = std::max(from_minimum, from_maximum);
        }
        low[axis] = terms_low[0] + terms_low[1] + terms_low[2];
        high[axis] = terms_high[0] + terms_high[1] + terms_high[2];
    }
    const Vector3& translation = transform.translation;
    Bounds result;
    result.minimum.x = low[0] + translation.x;
    result.minimum.y = low[1] + translation.y;
    result.minimum.z = low[2] + translation.z;
    result.maximum.x = high[0] + translation.x;
    result.maximum.y = high[1] + translation.y;
    result.maximum.z = high[2] + translation.z;
    return result;
}

} // namespace takedown::engine
