// 0x001ba4c0 (a0 = A, a1 = B, a2 = C, a3 = D: pointers to {x, y} floats; t0..t3 = outputs): SegmentSegmentClosestPoints2D.
// Closest points between segment AB and segment CD. t0 receives the point on AB, t1 the point on CD (each {x, y}),
// t2 the parameter on AB and t3 the parameter on CD (both 0..1). Returns f0 = the distance, or 1000.0 when the
// segments are (nearly) parallel: the dot product of the direction vectors d = B - A and e = D - C has a magnitude
// below 2^-16 (exponent test on the raw bits, so zero and denormals take it and NaN/Inf do not).
// Four candidates are tried in order (C onto AB, D onto AB, A onto CD, B onto CD; a point's parameter is
// ((P - origin) . dir) / |dir|^2 clamped to [0, 1] with MAX.S then MIN.S), each replacing the best squared
// distance only when strictly smaller. The first candidate's results are stored unconditionally; later ones
// overwrite them. The original copies vectors through stack slots with ld/sd (bit-exact pairs of floats) and reuses
// slots 288/292 for the two squared lengths: the second one sees the later pair, as traced below. Floats are raw
// bit patterns; the accumulator starts at +0.0 (adda f0 = zero).

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kOne = 0x3f800000U;           // 1.0
constexpr std::uint32_t kFar = 0x447a0000U;           // 1000.0
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kParallelLimit = 0x37800001U; // lui 0x3780 + ori 1: a threshold, not an address

// MAX.S and MIN.S on the raw sign/magnitude patterns.
std::uint32_t ee_max(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}
std::uint32_t ee_min(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}

// clamp(raw): MAX.S with +0.0, then MIN.S with 1.0.
std::uint32_t clamp01(const std::uint32_t raw) { return ee_min(ee_max(raw, 0U), kOne); }

// ADDA.S f0, x: the accumulator becomes +0.0 + x (it matters for -0).
std::uint32_t adda(const std::uint32_t x) { return fpu::add(0U, x); }

// |v|^2 as the original forms it: the accumulator = y*y (MULA.S, or MUL.S then ADDA.S f0), then MADD x*x.
std::uint32_t squared_length_mula(const std::uint32_t x, const std::uint32_t y) {
    return fpu::madd(fpu::mul(y, y), x, x);
}
std::uint32_t squared_length_adda(const std::uint32_t x, const std::uint32_t y) {
    return fpu::madd(adda(fpu::mul(y, y)), x, x);
}

template <class Context>
n3::Run segment_closest_points(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress a_ptr = call.arg(0);
    const GuestAddress b_ptr = call.arg(1);
    const GuestAddress c_ptr = call.arg(2);
    const GuestAddress d_ptr = call.arg(3);
    const GuestAddress out_ab = call.arg(4);
    const GuestAddress out_cd = call.arg(5);
    const GuestAddress out_tab = call.arg(6);
    const GuestAddress out_tcd = call.arg(7);

    // Entry loads (0x4c8..0x508) and the two direction vectors.
    const std::uint32_t ax = m.read_u32_le(a_ptr);
    const std::uint32_t bx = m.read_u32_le(b_ptr);
    const std::uint32_t ay = m.read_u32_le(a_ptr + 4U);
    const std::uint32_t by = m.read_u32_le(b_ptr + 4U);
    const std::uint32_t dxc = m.read_u32_le(d_ptr);
    const std::uint32_t dyc = m.read_u32_le(d_ptr + 4U);
    const std::uint32_t cy = m.read_u32_le(c_ptr + 4U);
    const std::uint32_t cx = m.read_u32_le(c_ptr);
    const std::uint32_t dx = fpu::sub(bx, ax);   // d = B - A
    const std::uint32_t dy = fpu::sub(by, ay);
    const std::uint32_t ex = fpu::sub(dxc, cx);  // e = D - C
    const std::uint32_t ey = fpu::sub(dyc, cy);

    // dot = dy*ey (MULA) + dx*ex (MADD)
    const std::uint32_t dot = fpu::madd(fpu::mul(dy, ey), dx, ex);
    if ((dot & kExponentMask) < kParallelLimit) {
        call.cpu().write_fpr_bits(0U, kFar);
        return call.finish(45U);  // 39 + 4 + 2
    }
    std::uint32_t retired = 39U + 106U;

    // Candidate 1 and 2: C and D onto AB.
    const std::uint32_t len_d = fpu::madd(adda(fpu::mul(dy, dy)), dx, dx);  // |d|^2
    const std::uint32_t cax = fpu::sub(cx, ax);
    const std::uint32_t cay = fpu::sub(cy, ay);
    const std::uint32_t dax = fpu::sub(dxc, ax);
    const std::uint32_t day = fpu::sub(dyc, ay);
    const std::uint32_t t_c_raw = fpu::div(fpu::madd(adda(fpu::mul(cay, dy)), cax, dx), len_d);
    const std::uint32_t t_d_raw = fpu::div(fpu::madd(adda(fpu::mul(day, dy)), dax, dx), len_d);
    const std::uint32_t t_c = clamp01(t_c_raw);
    const std::uint32_t t_d = clamp01(t_d_raw);
    const std::uint32_t p1x = fpu::add(ax, fpu::mul(dx, t_c));
    const std::uint32_t p1y = fpu::add(ay, fpu::mul(dy, t_c));
    const std::uint32_t p2x = fpu::add(ax, fpu::mul(dx, t_d));
    const std::uint32_t p2y = fpu::add(ay, fpu::mul(dy, t_d));
    const std::uint32_t cx2 = m.read_u32_le(c_ptr);
    const std::uint32_t cy2 = m.read_u32_le(c_ptr + 4U);
    const std::uint32_t cp1x = fpu::sub(cx, p1x);
    const std::uint32_t cp1y = fpu::sub(cy, p1y);
    const std::uint32_t dp2x = fpu::sub(dxc, p2x);
    const std::uint32_t dp2y = fpu::sub(dyc, p2y);
    // Unconditional stores: P1, C, tC, and 0.0 (f0 is still the zero from mtc1 $zero).
    m.write_u32_le(out_ab, p1x);
    m.write_u32_le(out_ab + 4U, p1y);
    m.write_u32_le(out_cd, cx2);
    m.write_u32_le(out_cd + 4U, cy2);
    m.write_u32_le(out_tab, t_c);
    m.write_u32_le(out_tcd, 0U);
    // |C - P1|^2 uses the pair read at 288 before the overwrite; |D - P2|^2 the pair after it.
    std::uint32_t best = squared_length_adda(cp1x, cp1y);
    const std::uint32_t dist_d = squared_length_adda(dp2x, dp2y);
    if (fpu::lt(dist_d, best)) {
        retired += 12U;
        best = dist_d;
        const std::uint32_t qdx = m.read_u32_le(d_ptr);
        const std::uint32_t qdy = m.read_u32_le(d_ptr + 4U);
        m.write_u32_le(out_ab, p2x);
        m.write_u32_le(out_ab + 4U, p2y);
        m.write_u32_le(out_cd, qdx);
        m.write_u32_le(out_cd + 4U, qdy);
        m.write_u32_le(out_tab, t_d);
        m.write_u32_le(out_tcd, kOne);
    }

    // Candidates 3 and 4: A and B onto CD. The inputs are re-read from guest memory (the stores above may alias).
    retired += 79U;
    const std::uint32_t r_cy = m.read_u32_le(c_ptr + 4U);
    const std::uint32_t r_by = m.read_u32_le(b_ptr + 4U);
    const std::uint32_t r_cx = m.read_u32_le(c_ptr);
    const std::uint32_t r_ax = m.read_u32_le(a_ptr);
    const std::uint32_t r_bx = m.read_u32_le(b_ptr);
    const std::uint32_t r_ay = m.read_u32_le(a_ptr + 4U);
    const std::uint32_t bcy = fpu::sub(r_by, r_cy);
    const std::uint32_t acx = fpu::sub(r_ax, r_cx);
    const std::uint32_t acy = fpu::sub(r_ay, r_cy);
    const std::uint32_t bcx = fpu::sub(r_bx, r_cx);
    const std::uint32_t len_e = fpu::madd(fpu::mul(ey, ey), ex, ex);  // |e|^2 (MULA f9, f9)
    const std::uint32_t t_a = clamp01(fpu::div(fpu::madd(fpu::mul(acy, ey), acx, ex), len_e));
    const std::uint32_t t_b = clamp01(fpu::div(fpu::madd(fpu::mul(bcy, ey), bcx, ex), len_e));
    const std::uint32_t q1x = fpu::add(r_cx, fpu::mul(ex, t_a));
    const std::uint32_t q1y = fpu::add(r_cy, fpu::mul(ey, t_a));
    const std::uint32_t q2x = fpu::add(r_cx, fpu::mul(ex, t_b));
    const std::uint32_t q2y = fpu::add(r_cy, fpu::mul(ey, t_b));
    const std::uint32_t aq1x = fpu::sub(r_ax, q1x);
    const std::uint32_t aq1y = fpu::sub(r_ay, q1y);
    const std::uint32_t dist_a = squared_length_mula(aq1x, aq1y);
    if (fpu::lt(dist_a, best)) {
        retired += 12U;
        best = dist_a;
        const std::uint32_t sax = m.read_u32_le(a_ptr);
        const std::uint32_t say = m.read_u32_le(a_ptr + 4U);
        m.write_u32_le(out_ab, sax);
        m.write_u32_le(out_ab + 4U, say);
        m.write_u32_le(out_cd, q1x);
        m.write_u32_le(out_cd + 4U, q1y);
        m.write_u32_le(out_tab, 0U);
        m.write_u32_le(out_tcd, t_a);
    }

    // Candidate 4: B onto CD.
    retired += 17U;
    const std::uint32_t s_by = m.read_u32_le(b_ptr + 4U);
    const std::uint32_t s_bx = m.read_u32_le(b_ptr);
    const std::uint32_t bq2y = fpu::sub(s_by, q2y);
    const std::uint32_t bq2x = fpu::sub(s_bx, q2x);
    const std::uint32_t dist_b = squared_length_mula(bq2x, bq2y);
    if (fpu::lt(dist_b, best)) {
        retired += 13U;
        best = dist_b;
        const std::uint32_t sbx = m.read_u32_le(b_ptr);
        const std::uint32_t sby = m.read_u32_le(b_ptr + 4U);
        m.write_u32_le(out_ab, sbx);
        m.write_u32_le(out_ab + 4U, sby);
        m.write_u32_le(out_cd, q2x);
        m.write_u32_le(out_cd + 4U, q2y);
        m.write_u32_le(out_tab, kOne);
        m.write_u32_le(out_tcd, t_b);
    }

    // 0x884..0x89c (7), 0x944 (1), 0x948 jr and its delay slot (2).
    retired += 7U + 1U + 2U;
    call.cpu().write_fpr_bits(0U, fpu::sqrt(best));
    return call.finish(retired);
}

} // namespace

// Declared cost 288 = the longest control-flow path (bin/brief.py).
extern constinit const EngineFunction n3_001ba4c0{0x001ba4c0U, 1168U, 0x66307ab35f712b54ULL,
                                                  "SegmentSegmentClosestPoints2D", 288U, n3::live<&segment_closest_points<NativeFunctionContext>>,
                                                  EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(001ba4c0, segment_closest_points)
