// fast_corner_classification against prepare_object_corner_visibility: wherever the fast path answers, the classification must equal
// the exact routine's. Random planes/corners at game-like scales, corners placed on or near a plane, and extreme bit patterns.
#include "takedown/runtime/slus_21050_corner_visibility_fast.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace takedown::runtime::slus_21050;

namespace {
// The props core's transform_point (VU clamp-mode-3 helpers), reproduced as the reference.
float vmul(float a, float b) { bool u, o; return std::bit_cast<float>(takedown::guest::vu0_detail::multiply_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b), u, o)); }
float vadd(float a, float b) { bool u, o; return std::bit_cast<float>(takedown::guest::vu0_detail::add_subtract_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b), false, u, o)); }
std::array<float, 3> vu_transform(const std::array<std::array<float, 3>, 3>& b, const std::array<float, 3>& t, const std::array<float, 3>& p) {
    std::array<float, 3> out{};
    for (std::size_t a = 0; a != 3U; ++a) {
        const float xy = vadd(vmul(b[0][a], p[0]), vmul(b[1][a], p[1]));
        out[a] = vadd(vadd(xy, vmul(b[2][a], p[2])), vmul(t[a], 1.0F));
    }
    return out;
}
} // namespace

int main() {
    std::mt19937_64 rng(0xC0FFEEULL);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::uniform_int_distribution<int> pick(0, 99);
    auto scaled = [&](double lo, double hi) { return static_cast<float>(unit(rng) * std::exp(std::log(lo) + (std::log(hi) - std::log(lo)) * (unit(rng) * 0.5 + 0.5))); };
    auto weird = [&]() -> float {
        static const std::uint32_t pats[] = {0U, 0x80000000U, 1U, 0x807fffffU, 0x7f800000U, 0xff800000U, 0x7fc00000U, 0x7f7fffffU, 0xff7fffffU, 0x00800000U};
        return std::bit_cast<float>(pats[static_cast<std::size_t>(pick(rng)) % 10U]);
    };
    std::uint64_t cases = 0, fast = 0, bad = 0;
    for (int it = 0; it < 4000000; ++it) {
        std::array<std::array<float, 3>, 8> corners{};
        std::array<std::array<float, 4>, 6> planes{};
        const int mode = pick(rng);
        const double cs = mode < 50 ? 2000.0 : mode < 80 ? 50.0 : 1e6;
        for (auto& c : corners) for (auto& v : c) v = pick(rng) < 2 ? weird() : scaled(1e-3, cs);
        for (std::size_t p = 0; p != 6U; ++p) {
            for (std::size_t a = 0; a != 3U; ++a) planes[p][a] = pick(rng) < 2 ? weird() : static_cast<float>(unit(rng));
            planes[p][3] = pick(rng) < 2 ? weird() : scaled(1e-3, cs);
            if (mode >= 60 && mode < 90) {   // put a corner (nearly) on this plane
                const auto& c = corners[static_cast<std::size_t>(pick(rng)) % 8U];
                double d = 0.0;
                for (std::size_t a = 0; a != 3U; ++a) d += static_cast<double>(planes[p][a]) * static_cast<double>(c[a]);
                const double nudge = mode < 70 ? 0.0 : d * std::ldexp(unit(rng), -(pick(rng) % 30));
                planes[p][3] = static_cast<float>(d + nudge);
            }
        }
        ++cases;
        ObjectVisibility f{};
        if (fast_corner_classification(corners, planes, f)) {
            ++fast;
            const ObjectVisibility e = prepare_object_corner_visibility(corners, planes).classification;
            if (e != f) {
                if (++bad <= 10) std::printf("MISMATCH case %llu mode %d fast %d exact %d\n", static_cast<unsigned long long>(cases), mode, int(f), int(e));
            }
        }
    }

    std::uint64_t icases = 0, ifast = 0, ibad = 0;
    for (int it = 0; it < 3000000; ++it) {
        std::array<std::array<float, 3>, 3> basis{};
        std::array<float, 3> trans{};
        std::array<std::array<float, 3>, 8> local{};
        std::array<std::array<float, 4>, 6> planes{};
        const int mode = pick(rng);
        for (auto& col : basis) for (auto& v : col) v = pick(rng) < 1 ? weird() : static_cast<float>(unit(rng) * (mode < 50 ? 1.0 : 20.0));
        for (auto& v : trans) v = pick(rng) < 1 ? weird() : scaled(1e-2, mode < 70 ? 5000.0 : 1e6);
        const float hx = scaled(0.1, 60.0), hy = scaled(0.1, 60.0), hz = scaled(0.1, 60.0);
        for (std::size_t k = 0; k != 8U; ++k) local[k] = {(k & 1U) ? hx : -hx, (k & 2U) ? hy : -hy, (k & 4U) ? hz : -hz};
        if (pick(rng) < 2) local[static_cast<std::size_t>(pick(rng)) % 8U][static_cast<std::size_t>(pick(rng)) % 3U] = weird();
        std::array<std::array<float, 3>, 8> vc{};
        for (std::size_t k = 0; k != 8U; ++k) vc[k] = vu_transform(basis, trans, local[k]);
        for (std::size_t p = 0; p != 6U; ++p) {
            for (std::size_t a = 0; a != 3U; ++a) planes[p][a] = pick(rng) < 1 ? weird() : static_cast<float>(unit(rng));
            planes[p][3] = pick(rng) < 1 ? weird() : scaled(1e-2, 1e5);
            if (mode >= 40 && mode < 85) {   // a game-computed corner (nearly) on the plane
                const auto& c = vc[static_cast<std::size_t>(pick(rng)) % 8U];
                double d = 0.0;
                for (std::size_t a = 0; a != 3U; ++a) d += static_cast<double>(planes[p][a]) * static_cast<double>(c[a]);
                const double nudge = mode < 55 ? 0.0 : d * std::ldexp(unit(rng), -(pick(rng) % 30));
                planes[p][3] = static_cast<float>(d + nudge);
            }
        }
        ++icases;
        ObjectVisibility f{};
        if (fast_instance_classification(basis, trans, local, planes, f)) {
            ++ifast;
            const ObjectVisibility e = prepare_object_corner_visibility(vc, planes).classification;
            if (e != f && ++ibad <= 10) std::printf("INSTANCE MISMATCH case %llu mode %d fast %d exact %d\n", static_cast<unsigned long long>(icases), mode, int(f), int(e));
        }
    }
    std::printf("instances %llu fast-path %llu (%.1f%%) mismatches %llu\n", static_cast<unsigned long long>(icases), static_cast<unsigned long long>(ifast),
                100.0 * double(ifast) / double(icases), static_cast<unsigned long long>(ibad));
    bad += ibad;
    std::printf("cases %llu fast-path %llu (%.1f%%) mismatches %llu\n", static_cast<unsigned long long>(cases), static_cast<unsigned long long>(fast),
                100.0 * double(fast) / double(cases), static_cast<unsigned long long>(bad));
    return bad == 0 ? 0 : 1;
}
