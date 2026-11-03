#pragma once

// Fast path for the classification of prepare_object_corner_visibility (slus_21050_object_submission.hpp): host doubles where the
// game's chopped arithmetic provably gives the same signs, false where the exact routine must decide. Tested against it in
// tests/test_corner_visibility_fast.cpp.

#include "takedown/runtime/slus_21050_object_submission.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace takedown::runtime::slus_21050 {

// The classification of prepare_object_corner_visibility (exact game arithmetic: every product and difference chopped toward zero,
// denormals flushed, saturation at Fmax) decided with host doubles when that is safe. A corner's distance is p3 - p0 c0 - p1 c1 - p2 c2;
// the game's result differs from the exact value by at most 6 chops (3 products, 3 differences) of at most 2^-23 of a partial result each, all bounded by
// B = |p3| + |p0 c0| + |p1 c1| + |p2 c2|. When |exact| > 2^-18 B (at least 4x that bound) and 2^-100, and B < 2^120 (no saturation), the game's
// result has the sign of the exact value, which is all the classification reads. Otherwise (a corner on or very near a plane, zeros,
// huge values) the exact routine decides. Planes 4 and 5 go through corner_sum(0, v) there, which only turns -0 into +0: the
// products are equal and the zero cases fall back.
[[nodiscard]] inline float corner_normalized(const float value) noexcept {
    std::uint32_t b = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t e = b & 0x7f800000U;
    if (e == 0U) {
        b &= 0x80000000U;
    } else if (e == 0x7f800000U) {
        b = (b & 0x80000000U) | 0x7f7fffffU;
    }
    return std::bit_cast<float>(b);
}
[[nodiscard]] inline bool fast_corner_classification(const std::array<std::array<float, 3>, 8>& corners, const std::array<std::array<float, 4>, 6>& planes,
                                              ObjectVisibility& out) noexcept {
    double c[8][3];
    for (std::size_t k = 0; k != 8U; ++k) {
        for (std::size_t a = 0; a != 3U; ++a) {
            c[k][a] = static_cast<double>(corner_normalized(corners[k][a]));
        }
    }
    bool rejected = false, crosses = false;
    for (std::size_t pi = 0; pi != 6U; ++pi) {
        const double p0 = corner_normalized(planes[pi][0]), p1 = corner_normalized(planes[pi][1]), p2 = corner_normalized(planes[pi][2]), p3 = corner_normalized(planes[pi][3]);
        unsigned outside = 0U;
        for (std::size_t k = 0; k != 8U; ++k) {
            const double t0 = p0 * c[k][0], t1 = p1 * c[k][1], t2 = p2 * c[k][2];
            const double exact = p3 - t0 - t1 - t2;
            const double bound = std::fabs(p3) + std::fabs(t0) + std::fabs(t1) + std::fabs(t2);
            const double mag = std::fabs(exact);
            if (!(bound < 0x1p120) || !(mag > bound * 0x1p-18) || !(mag > 0x1p-100)) {
                return false;
            }
            if (exact < 0.0) {
                outside |= 1U << k;
            }
        }
        rejected |= outside == 0xffU;
        crosses |= outside != 0U;
    }
    out = rejected ? ObjectVisibility::Outside : crosses ? ObjectVisibility::Intersecting : ObjectVisibility::Inside;
    return true;
}

// Both steps at once for a prop instance: the corners are the instance transform of the 8 local box corners, which the game computes
// with VU clamp-mode-3 arithmetic (3 truncated products, 3 chopped sums per lane: corner = ((b0 x + b1 y) + b2 z) + t * 1.0), and they
// are only used for the classification. Each corner lane is within 6 * 2^-23 of CA = |b0 x| + |b1 y| + |b2 z| + |t| of its exact
// value, which moves a plane distance by at most sum |p_a| * CA_a; the distance arithmetic itself adds at most 6 * 2^-23 of B. The
// sign is safe when |exact| > 2^-16 (B + E) (more than 5x the bound), |exact| > 2^-60 (flushed denormals: each at most 2^-126,
// multiplied by a plane coefficient below 2^60) and nothing comes near saturation. Otherwise the caller computes the exact corners.
// basis[c][a]: column c (x, y, z), lane a, as the instance words store them.
[[nodiscard]] inline bool fast_instance_classification(const std::array<std::array<float, 3>, 3>& basis, const std::array<float, 3>& translation,
                                                       const std::array<std::array<float, 3>, 8>& local,
                                                       const std::array<std::array<float, 4>, 6>& planes, ObjectVisibility& out) noexcept {
    double b[3][3], t[3];
    for (std::size_t c = 0; c != 3U; ++c) {
        for (std::size_t a = 0; a != 3U; ++a) {
            b[c][a] = static_cast<double>(corner_normalized(basis[c][a]));
        }
        t[c] = static_cast<double>(corner_normalized(translation[c]));
    }
    double c[8][3], ca[8][3];
    for (std::size_t k = 0; k != 8U; ++k) {
        const double x = corner_normalized(local[k][0]), y = corner_normalized(local[k][1]), z = corner_normalized(local[k][2]);
        for (std::size_t a = 0; a != 3U; ++a) {
            const double u = b[0][a] * x, v = b[1][a] * y, w = b[2][a] * z;
            c[k][a] = u + v + w + t[a];
            ca[k][a] = std::fabs(u) + std::fabs(v) + std::fabs(w) + std::fabs(t[a]);
            if (!(ca[k][a] < 0x1p100)) {
                return false;
            }
        }
    }
    bool rejected = false, crosses = false;
    for (std::size_t pi = 0; pi != 6U; ++pi) {
        const double p0 = corner_normalized(planes[pi][0]), p1 = corner_normalized(planes[pi][1]), p2 = corner_normalized(planes[pi][2]),
                     p3 = corner_normalized(planes[pi][3]);
        if (!(std::fabs(p0) < 0x1p60 && std::fabs(p1) < 0x1p60 && std::fabs(p2) < 0x1p60 && std::fabs(p3) < 0x1p100)) {
            return false;
        }
        unsigned outside = 0U;
        for (std::size_t k = 0; k != 8U; ++k) {
            const double t0 = p0 * c[k][0], t1 = p1 * c[k][1], t2 = p2 * c[k][2];
            const double exact = p3 - t0 - t1 - t2;
            const double bound = std::fabs(p3) + std::fabs(t0) + std::fabs(t1) + std::fabs(t2) +
                                 std::fabs(p0) * ca[k][0] + std::fabs(p1) * ca[k][1] + std::fabs(p2) * ca[k][2];
            const double mag = std::fabs(exact);
            if (!(bound < 0x1p110) || !(mag > bound * 0x1p-16) || !(mag > 0x1p-60)) {
                return false;
            }
            if (exact < 0.0) {
                outside |= 1U << k;
            }
        }
        rejected |= outside == 0xffU;
        crosses |= outside != 0U;
    }
    out = rejected ? ObjectVisibility::Outside : crosses ? ObjectVisibility::Intersecting : ObjectVisibility::Inside;
    return true;
}

} // namespace takedown::runtime::slus_21050
