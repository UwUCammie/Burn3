// P1 WP3: plan_draw_car, the selection half of a native DrawCar (0x0021e6f0). Template definition, included by
// native_car_plan.cpp (explicit instantiation for the guest memory view) and by tests/test_native_car_plan.cpp.
//
// Every statement mirrors an instruction group of the original (addresses in the comments are DrawCar's). Where the
// original computes with VU0 macro instructions or the FPU, the same operations run in the same order on raw
// IEEE-754 bit patterns through the project's own lane helpers (guest::vu0_detail::fmac_lane: clamp mode 3 operands,
// truncating multiply, chop-toward-zero add, denormal flush; guest::ee_cop1: the FPU rules), so the matrices carry the
// guest's bits. What is NOT reproduced: the unwritten W lanes of the body matrix rows 0..2 (they come out of DrawCar's
// uninitialised stack frame; the VU1 program does not read them), which are 0 here.
//
// Cases the plan declines (returns false, the caller runs the guest DrawCar):
//   * lod outside 0..4, or no LOD block at all (blk == 0: the original dereferences a null pointer);
//   * more than 6 panels or 6 wheels (the original's stack arrays are sized for 6/8; the BGV format has no more);
//   * a detached-part pool / extras entry whose list index is outside C0..C7, a null extras header, or a plan that
//     would not fit kMaxPartDraws.
// Everything else DrawCar does is planned (damage zones, hanging panels, detached wheels, glass states, wreck fade,
// spin variants, pool, extras); see tests/test_native_car_plan.cpp for how each path was verified.

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/guest/vu0_simd_arithmetic.hpp"
#include "takedown/runtime/native_car_draw.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace takedown::runtime::native_car::plan_detail {

namespace vu = guest::vu0_detail;

using V = std::array<std::uint32_t, 4>;            // one VU0 register / GPR quadword as raw words
constexpr std::uint32_t kOne = 0x3f800000U;        // 1.0f
constexpr std::uint32_t kSign = 0x80000000U;

// ---- VU0 macro lane operations (clamp-mode-3 FMAC, see vu0_simd_arithmetic.hpp) ------------------------------------
[[nodiscard]] inline std::uint32_t vadd(const std::uint32_t a, const std::uint32_t b) noexcept {
    bool u{}, o{};
    return vu::fmac_lane<vu::Vu0Fmac::Add>(a, b, 0U, u, o);
}
[[nodiscard]] inline std::uint32_t vsub(const std::uint32_t a, const std::uint32_t b) noexcept {
    bool u{}, o{};
    return vu::fmac_lane<vu::Vu0Fmac::Subtract>(a, b, 0U, u, o);
}
[[nodiscard]] inline std::uint32_t vmul(const std::uint32_t a, const std::uint32_t b) noexcept {
    bool u{}, o{};
    return vu::fmac_lane<vu::Vu0Fmac::Multiply>(a, b, 0U, u, o);
}
// acc + a * b (VMADD family: the product is chopped on its own first)
[[nodiscard]] inline std::uint32_t vmadd(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) noexcept {
    bool u{}, o{};
    return vu::fmac_lane<vu::Vu0Fmac::MultiplyAdd>(a, b, acc, u, o);
}
// acc - a * b
[[nodiscard]] inline std::uint32_t vmsub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) noexcept {
    bool u{}, o{};
    return vu::fmac_lane<vu::Vu0Fmac::MultiplySubtract>(a, b, acc, u, o);
}

// ---- EE FPU (add.s/sub.s/mul.s/msub.s/c.ole.s ...) -------------------------------------------------------------------
namespace fpu {
[[nodiscard]] inline std::uint32_t finish(const std::uint32_t bits) noexcept {
    if ((bits & 0x7fffffffU) == 0x7f800000U) {
        return (bits & kSign) | 0x7f7fffffU;
    }
    return guest::ee_cop1::is_cop1_denormal(bits) ? bits & kSign : bits;
}
[[nodiscard]] inline std::uint32_t norm(const std::uint32_t a) noexcept { return guest::ee_cop1::normalize_cop1_operand(a); }
[[nodiscard]] inline std::uint32_t add(const std::uint32_t a, const std::uint32_t b) noexcept {
    return finish(guest::ee_cop1::cop1_add_normal_finite(norm(a), norm(b)));
}
[[nodiscard]] inline std::uint32_t sub(const std::uint32_t a, const std::uint32_t b) noexcept {
    return finish(guest::ee_cop1::cop1_sub_normal_finite(norm(a), norm(b)));
}
[[nodiscard]] inline std::uint32_t mul(const std::uint32_t a, const std::uint32_t b) noexcept {
    return finish(guest::ee_cop1::cop1_mul_normal_finite(norm(a), norm(b)));
}
// MSUB.S: acc - a*b with the product rounded to single first
[[nodiscard]] inline std::uint32_t msub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) noexcept {
    const auto product = norm(guest::ee_cop1::cop1_mul_normal_finite(norm(a), norm(b)));
    return finish(guest::ee_cop1::cop1_sub_normal_finite(norm(acc), product));
}
[[nodiscard]] inline std::uint32_t abs(const std::uint32_t a) noexcept { return a & 0x7fffffffU; }
[[nodiscard]] inline float value(const std::uint32_t bits) noexcept { return std::bit_cast<float>(norm(bits)); }
[[nodiscard]] inline bool lt(const std::uint32_t a, const std::uint32_t b) noexcept { return value(a) < value(b); }
[[nodiscard]] inline bool le(const std::uint32_t a, const std::uint32_t b) noexcept { return value(a) <= value(b); }
} // namespace fpu

// Rodrigues rotation rows about `axis` (xyz of the quad), `sin`/`cos` raw words: 0x21e98c..0x21ea4c (axis Z, F+68/+72)
// and 0x21eb28..0x21ebe4 (axis X, F+76/+80) are the same computation. Row-vector convention. The W lanes are not
// meaningful (stack leftovers in the original).
[[nodiscard]] inline std::array<V, 3> axis_rotation_rows(const std::uint32_t sin_bits, const std::uint32_t cos_bits,
                                                         const V& u) noexcept {
    const std::uint32_t one_minus_cos = fpu::sub(kOne, cos_bits);               // sub.s $f0, 1.0, cos
    const std::uint32_t s = vadd(0U, sin_bits);                                  // vf6 = (sin, cos, 1-cos, 1.0)
    const std::uint32_t omc = vadd(0U, one_minus_cos);
    const std::uint32_t w1 = vmul(kOne, kOne);
    V a{};                                                                       // vf5.xyz = (1 - u) * (1 - cos)
    V us{};                                                                      // vf2.xyz = u * sin
    for (std::size_t i = 0; i < 3; ++i) {
        a[i] = vmul(vadd(vsub(0U, u[i]), w1), omc);
        us[i] = vmul(u[i], s);
    }
    const std::uint32_t ax = vsub(w1, a[0]);                                     // vsubx.w / vsuby.w / vsubz.w: 1 - a
    const std::uint32_t ay = vsub(w1, a[1]);
    const std::uint32_t az = vsub(w1, a[2]);
    std::array<V, 3> rows{};
    rows[0] = {vadd(0U, ax), vadd(0U, us[2]), vadd(0U, vsub(0U, us[1])), 0U};
    rows[1] = {vadd(0U, vsub(0U, us[2])), vadd(0U, ay), vadd(0U, us[0]), 0U};
    rows[2] = {vadd(0U, us[1]), vadd(0U, vsub(0U, us[0])), vadd(0U, az), 0U};
    return rows;
}

// VMULAx.xyz / VMADDAy.xyz / VMADDz.xyz: r.x * m0 + r.y * m1 + r.z * m2 (xyz lanes); `w` is the destination's W lane.
[[nodiscard]] inline V xform_vector(const V& r, const V& m0, const V& m1, const V& m2, const std::uint32_t w) noexcept {
    V o{};
    for (std::size_t i = 0; i < 3; ++i) {
        std::uint32_t acc = vmul(m0[i], r[0]);
        acc = vmadd(acc, m1[i], r[1]);
        o[i] = vmadd(acc, m2[i], r[2]);
    }
    o[3] = w;
    return o;
}
// ... then VMADDAz + VMADDw.xyz with the translation row: r.x * m0 + r.y * m1 + r.z * m2 + t * 1.0
[[nodiscard]] inline V xform_point(const V& r, const V& m0, const V& m1, const V& m2, const V& t,
                                   const std::uint32_t w) noexcept {
    V o{};
    for (std::size_t i = 0; i < 3; ++i) {
        std::uint32_t acc = vmul(m0[i], r[0]);
        acc = vmadd(acc, m1[i], r[1]);
        acc = vmadd(acc, m2[i], r[2]);
        o[i] = vmadd(acc, t[i], kOne);
    }
    o[3] = w;
    return o;
}

using Mat = std::array<V, 4>;

template <class Memory>
[[nodiscard]] V load_quad(const Memory& mem, const std::uint32_t a) {
    return V{mem.u32(a), mem.u32(a + 4U), mem.u32(a + 8U), mem.u32(a + 12U)};
}
template <class Memory>
[[nodiscard]] Mat load_mat(const Memory& mem, const std::uint32_t a) {
    return Mat{load_quad(mem, a), load_quad(mem, a + 16U), load_quad(mem, a + 32U), load_quad(mem, a + 48U)};
}

// ClassifySphereInCullingFrustum (0x002202d0, n3 source 002202d0.cpp): 0 outside, 1 straddles, 2 inside.
// The frustum lives at 0x0067d5a0 (S + 272).
template <class Memory>
[[nodiscard]] std::uint32_t classify_sphere(const Memory& mem, const V& sphere) {
    constexpr std::uint32_t kFrustum = 0x0067d5a0U;
    const std::uint32_t radius = sphere[3];
    const auto side_negative = [&](const std::uint32_t set, const std::uint32_t r) {
        const V d = load_quad(mem, set + 48U), x = load_quad(mem, set), y = load_quad(mem, set + 16U),
                z = load_quad(mem, set + 32U);
        const V vf1{r, vadd(0U, r), vadd(0U, r), vmul(kOne, r)};                // vf1 = (r, r, r, r)
        std::uint32_t any = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            std::uint32_t acc = vadd(d[i], 0U);                                  // vaddax.xyzw $acc, vf2, vf0x
            acc = vmsub(acc, x[i], sphere[0]);
            acc = vmsub(acc, y[i], sphere[1]);
            acc = vmsub(acc, z[i], sphere[2]);
            any |= vmadd(acc, vf1[i], kOne);                                     // vmaddw.xyzw vf1, vf1, vf0w
        }
        return (any & kSign) != 0U;
    };
    if (side_negative(kFrustum + 32U, radius)) {
        return 0;
    }
    const std::uint32_t negative_radius = radius ^ kSign;
    const std::uint32_t one = vadd(0U, kOne);                                    // vaddw.x vf2, vf0, vf0w
    std::uint32_t result = 2;
    for (std::uint32_t plane = kFrustum; plane < kFrustum + 32U; plane += 16U) {
        const V q = load_quad(mem, plane);
        const std::uint32_t t0 = vmul(sphere[0], q[0]);                          // vmul.xyz vf1, vf4, vf3
        const std::uint32_t t1 = vmul(sphere[1], q[1]);
        const std::uint32_t t2 = vmul(sphere[2], q[2]);
        const std::uint32_t acc = vadd(t0, t1);                                  // vadday.x $acc, vf1, vf1y
        const std::uint32_t dot = vmadd(acc, one, t2);                           // vmaddz.x vf1, vf2, vf1z
        const std::uint32_t distance = fpu::sub(dot, q[3]);
        if (!fpu::le(distance, radius)) {
            return 0;
        }
        if (!fpu::le(distance, negative_radius)) {
            result = 1;
        }
    }
    if (result == 1) {
        return 1;
    }
    return side_negative(kFrustum + 96U, negative_radius) ? 1U : 2U;
}

} // namespace takedown::runtime::native_car::plan_detail

namespace takedown::runtime::native_car {

template <class Memory>
bool plan_draw_car(const Memory& mem, const std::uint32_t s, const std::uint32_t f, const std::uint32_t b,
                   const std::int32_t lod, CarDrawPlan& plan) {
    using namespace plan_detail;
    constexpr std::uint32_t kGp = 0x004e8670U;                  // $gp of the executable
    constexpr std::uint32_t kGameTime = 0x00522680U;            // lui 0x52 / lw 9856
    constexpr std::uint32_t kCameraGlobal = 0x00666270U;        // lui 0x66 / lw 25200: render camera object pointer
    constexpr std::uint32_t kAxisZ = 0x004b9d90U;               // R1 axis (0, 0, 1) set up by 0x4d5a20
    constexpr std::uint32_t kAxisX = 0x004b9d80U;               // R2 axis (1, 0, 0)
    constexpr std::uint32_t kPoolMask = 0x00648188U;            // u64 active mask of the detached-part pool
    constexpr std::uint32_t kPoolBase = 0x00631870U;            // 64 records of 1248 bytes
    constexpr std::size_t kMaxPanels = 6;
    constexpr std::size_t kMaxWheels = 6;

    plan.s = s;
    plan.f = f;
    plan.b = b;
    plan.blk = 0;
    plan.lod = lod;
    plan.light_bits = mem.u8(f + 88U) & 0xFCU;                                   // 0x21e6f4: andi 0xfc
    plan.frustum_class = mem.s8(f + 91U);
    plan.f12 = 1.0F;
    plan.f13 = 1.0F;
    plan.drawn = false;
    plan.s248 = mem.u32(b + 1184U);                                              // 0x21e748, stored even on early return
    plan.s252 = mem.s8(f + 89U);                                                 // 0x21e750
    plan.texture = 0;
    plan.glass_patch_count = 0;
    plan.part_count = 0;
    const std::uint32_t m = mem.u32(f + 64U);
    plan.m = m;
    const std::int32_t cls = plan.frustum_class;

    if (cls == 0 && mem.u8(b + 4130U) == 0) {                                    // 0x21e774 / 0x21e7b0: nothing visible
        return true;
    }

    // ---- LOD block: search up from `lod`, then down (0x21e77c..0x21e7e4) ----------------------------------------------
    if (lod < 0 || lod > 4) {
        return false;
    }
    std::int32_t use_lod = lod;
    std::uint32_t blk = mem.u32(m + 76U + 4U * static_cast<std::uint32_t>(use_lod));
    while (blk == 0 && use_lod < 4) {
        ++use_lod;
        blk = mem.u32(m + 76U + 4U * static_cast<std::uint32_t>(use_lod));
    }
    while (blk == 0 && use_lod > 0) {
        --use_lod;
        blk = mem.u32(m + 76U + 4U * static_cast<std::uint32_t>(use_lod));
    }
    if (blk == 0) {
        return false;
    }
    plan.blk = blk;
    plan.lod = use_lod;
    const std::int32_t panel_count = mem.s8(m + 12U);
    const std::int32_t wheel_count = mem.s8(m + 13U);
    if (panel_count > static_cast<std::int32_t>(kMaxPanels) || wheel_count > static_cast<std::int32_t>(kMaxWheels)) {
        return false;
    }

    // ---- StartEffect scales (0x21e7e8..0x21e8a8) -----------------------------------------------------------------------
    std::uint32_t f12 = kOne;
    std::uint32_t f13 = kOne;
    if (mem.u8(b + 4123U) != 0U) {
        // adda.s ACC = 0 + f12 ; msub.s f12 = ACC - gp[-30684] * B.f[880]
        const std::uint32_t acc = fpu::add(0U, f12);
        f12 = fpu::msub(acc, mem.u32(kGp - 30684U), mem.u32(b + 880U));
    }
    if ((mem.u8(b + 4117U) & 0x10U) != 0U) {
        // qmtc2 time, vf2 ; lqc2 vf1, 4096(B) ; vsubw.x vf1, vf2, vf1w
        const std::uint32_t t = vsub(mem.u32(kGameTime), mem.u32(b + 4108U));
        if (fpu::le(t, mem.u32(kGp - 31264U))) {
            const std::uint32_t acc = fpu::add(0U, kOne);
            const std::uint32_t k13 = fpu::msub(acc, 0x3ff00000U, t);           // 1 - 1.875 t
            const std::uint32_t k12 = fpu::msub(acc, 0x40100000U, t);           // 1 - 2.25 t
            f13 = fpu::mul(f13, k13);
            f12 = fpu::mul(f12, k12);
        } else {
            f13 = fpu::mul(f13, 0x3e800000U);                                    // * 0.25
            f12 = fpu::mul(f12, mem.u32(kGp - 30796U));
        }
    }
    plan.f12 = std::bit_cast<float>(f12);
    plan.f13 = std::bit_cast<float>(f13);
    plan.texture = mem.u32(m + 96U);                                             // 0x21ff60 a1
    plan.drawn = true;

    // ---- glass patch (0x21e8cc..0x21e984, repeated for panels) ---------------------------------------------------------
    const std::uint8_t glass_state = mem.u8(b + 4116U);
    const auto patch_glass = [&](const std::uint32_t list) {
        const std::int32_t g = mem.s8(list + 1U);
        if (g < 0) {
            return;
        }
        const std::uint32_t entry = mem.u32(list + 4U) + 16U * static_cast<std::uint32_t>(g);
        auto& p = plan.glass_patches[plan.glass_patch_count++];
        p.entry = entry;
        if (glass_state == 2U) {
            p.b11 = 4;
            p.b13 = 128;
        } else if (glass_state == 1U) {
            p.b11 = 3;
            p.b13 = 72;
        } else {
            p.b11 = 2;
            p.b13 = mem.u8(entry + 12U);
        }
    };
    patch_glass(mem.u32(blk));

    const std::uint32_t value = cls == 1 ? 2U : 0U;
    const bool body_skinned = mem.u8(b + 4123U) != 0U;
    const auto add_part = [&](const PartKind kind, const std::size_t index, const bool skinned, const bool lighting,
                              const std::uint32_t list, const std::uint32_t mask, const std::uint32_t v,
                              const std::uint32_t zones, const std::uint32_t zone_count, const Mat& matrix) -> PartDraw& {
        PartDraw& p = plan.parts[plan.part_count++];
        p = PartDraw{};
        p.kind = kind;
        p.index = static_cast<std::uint8_t>(index);
        p.skinned = skinned;
        p.lighting = lighting;
        p.list = list;
        p.mask = mask;
        p.value = v;
        p.zones = zones;
        p.zone_count = zone_count;
        p.f12 = 1.0F;
        p.matrix = matrix;
        return p;
    };

    // ---- body matrix R2 * R1 * F (0x21e98c..0x21ecb8) ------------------------------------------------------------------
    const Mat fm = load_mat(mem, f);
    const auto r1 = axis_rotation_rows(mem.u32(f + 68U), mem.u32(f + 72U), load_quad(mem, kAxisZ));
    Mat t1{};
    for (std::size_t i = 0; i < 3; ++i) {
        t1[i] = xform_vector(r1[i], fm[0], fm[1], fm[2], 0U);
    }
    // the translation row of both rotations is (+0, +0, +0) (vadd.xyz vf1, vf0, vf0), so only F's translation survives
    t1[3] = xform_point(V{vadd(0U, 0U), vadd(0U, 0U), vadd(0U, 0U), 0U}, fm[0], fm[1], fm[2], fm[3], fm[3][3]);
    const auto r2 = axis_rotation_rows(mem.u32(f + 76U), mem.u32(f + 80U), load_quad(mem, kAxisX));
    Mat body{};
    for (std::size_t i = 0; i < 3; ++i) {
        body[i] = xform_vector(r2[i], t1[0], t1[1], t1[2], 0U);
    }
    body[3] = xform_point(V{vadd(0U, 0U), vadd(0U, 0U), vadd(0U, 0U), 0U}, t1[0], t1[1], t1[2], t1[3], t1[3][3]);

    // ---- pass 1: body ---------------------------------------------------------------------------------------------------
    if (cls != 0) {
        add_part(PartKind::Body, 0, body_skinned, true, mem.u32(blk), 0x00FFU, value, body_skinned ? b + 1280U : 0U,
                 body_skinned ? 8U : 0U, body);
    }

    // ---- pass 2: panels, far to near (0x21ed5c..0x21f17c) --------------------------------------------------------------
    std::array<Mat, kMaxPanels> panel{};
    for (std::int32_t j = 0; j < panel_count; ++j) {
        const auto uj = static_cast<std::uint32_t>(j);
        const std::uint32_t rest = b + 384U + 64U * uj;
        if (mem.s8(b + 1202U + uj) < 3) {
            Mat p{};
            for (std::size_t i = 0; i < 3; ++i) {
                const V row = load_quad(mem, rest + 16U * static_cast<std::uint32_t>(i));
                p[i] = xform_vector(row, body[0], body[1], body[2], row[3]);
            }
            const V tr = load_quad(mem, rest + 48U);
            p[3] = xform_point(tr, body[0], body[1], body[2], body[3], tr[3]);
            panel[static_cast<std::size_t>(j)] = p;
        } else {
            panel[static_cast<std::size_t>(j)] = load_mat(mem, rest);
        }
    }
    std::array<std::uint8_t, kMaxPanels> order{};
    if (panel_count > 0) {
        // |P.translation - camera|^2, the camera position from the render camera object
        const std::uint32_t camera_object = mem.u32(kCameraGlobal);
        const V cam = load_quad(mem, mem.u32(camera_object + 104U) + 32U);
        std::array<std::uint32_t, kMaxPanels> key{};
        for (std::int32_t j = 0; j < panel_count; ++j) {
            const V& tr = panel[static_cast<std::size_t>(j)][3];
            std::uint32_t d[3];
            for (std::size_t i = 0; i < 3; ++i) {
                d[i] = vsub(tr[i], cam[i]);
                d[i] = vmul(d[i], d[i]);
            }
            const std::uint32_t acc = vadd(d[0], d[1]);                          // vadday.x
            key[static_cast<std::size_t>(j)] = vmadd(acc, vadd(0U, kOne), d[2]); // vmaddz.x vf1, vf2(=1.0), vf1z
            order[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(j);
        }
        // selection sort, descending, strict "<" swap (0x21ef00..0x21f1ac): not stable
        for (std::size_t a0 = 0; a0 < static_cast<std::size_t>(panel_count); ++a0) {
            for (std::size_t a1 = a0 + 1; a1 < static_cast<std::size_t>(panel_count); ++a1) {
                if (fpu::lt(key[a0], key[a1])) {
                    const std::uint32_t tk = key[a0];
                    key[a0] = key[a1];
                    key[a1] = tk;
                    const std::uint8_t ti = order[a0];
                    order[a0] = order[a1];
                    order[a1] = ti;
                }
            }
        }
    }
    for (std::int32_t i = 0; i < panel_count; ++i) {
        const std::size_t j = order[static_cast<std::size_t>(i)];
        const auto uj = static_cast<std::uint32_t>(j);
        const std::int32_t state = mem.s8(b + 1202U + uj);
        // a1 of the original: the frustum class, except that a hanging panel (state 3) carries its own sphere class
        // into the draw (0x21efec skips the reload of F.s8[91] at 0x21f000)
        std::int32_t panel_class = cls;
        if (state == 3) {
            const V& tr = panel[j][3];
            const V sphere{vadd(0U, tr[0]), vadd(0U, tr[1]), vadd(0U, tr[2]), vmul(kOne, mem.u32(m + 52U + 4U * uj))};
            panel_class = static_cast<std::int32_t>(classify_sphere(mem, sphere));
            if (panel_class == 0) {
                continue;
            }
        } else if (state == 4) {
            continue;
        }
        const std::uint32_t list = mem.u32(blk + 4U + 4U * uj);
        patch_glass(list);
        if (panel_class == 0) {
            continue;
        }
        const bool skinned = mem.u8(b + 4124U + uj) != 0U;
        add_part(PartKind::Panel, j, skinned, false, list, 0x03FFU, panel_class == 1 ? 2U : 0U,
                 skinned ? b + 2304U + 128U * uj : 0U, skinned ? 2U : 0U, panel[j]);
    }

    // ---- pass 3: body glass ---------------------------------------------------------------------------------------------
    if (cls != 0) {
        add_part(PartKind::Glass, 0, body_skinned, true, mem.u32(blk), 0x0300U, value, body_skinned ? b + 1280U : 0U,
                 body_skinned ? 8U : 0U, body);
    }

    // ---- pass 4: wheels (0x21f26c..0x21f5e8) ----------------------------------------------------------------------------
    const std::uint32_t spin2 = mem.u32(blk + 36U);
    const std::uint32_t spin1 = mem.u32(blk + 32U);
    for (std::int32_t k = 0; k < wheel_count; ++k) {
        const auto uk = static_cast<std::uint32_t>(k);
        const Mat raw = load_mat(mem, b + 64U * uk);
        Mat matrix{};
        std::uint32_t wheel_class{};
        if (mem.s8(b + 1196U + uk) == 3) {
            // detached: the block holds a world matrix; culled by its own sphere
            matrix = raw;
            const std::uint32_t radius = fpu::mul(mem.u32(m + 24U), mem.u32(m + 28U + 4U * uk));
            const V sphere{vadd(0U, raw[3][0]), vadd(0U, raw[3][1]), vadd(0U, raw[3][2]), vmul(kOne, radius)};
            wheel_class = classify_sphere(mem, sphere);
        } else {
            for (std::size_t i = 0; i < 3; ++i) {
                matrix[i] = xform_vector(raw[i], fm[0], fm[1], fm[2], raw[i][3]);
            }
            matrix[3] = xform_point(raw[3], fm[0], fm[1], fm[2], fm[3], fm[3][3]);
            wheel_class = static_cast<std::uint32_t>(cls);
        }
        std::uint32_t variant = 0;
        const std::uint32_t rate = fpu::abs(mem.u32(b + 1256U + 4U * uk));
        if (!fpu::le(rate, 0x42480000U) && spin2 != 0U) {                        // |rate| > 50 and B2 exists
            variant = 2;
        } else if (!fpu::le(rate, 0x41c80000U) && spin1 != 0U) {                 // |rate| > 25 and B1 exists
            variant = 1;
        }
        if (wheel_class == 0U) {
            continue;
        }
        add_part(PartKind::Wheel, uk, false, true, mem.u32(blk + 28U + 4U * variant), 0x03FFU,
                 wheel_class == 1U ? 2U : 0U, 0U, 0U, matrix);
    }

    // ---- passes 5/6: detached-part pool or extras (0x21f2ac..0x21f750) -----------------------------------------------
    if (m == 0 || mem.u32(m + 5824U) == 0U) {
        return true;
    }
    const std::uint32_t hdr = mem.u32(b + 4132U);
    if (mem.u8(b + 4131U) == 1U || mem.u8(b + 4130U) != 0U) {
        const std::uint64_t mask = static_cast<std::uint64_t>(mem.u32(kPoolMask)) |
                                   (static_cast<std::uint64_t>(mem.u32(kPoolMask + 4U)) << 32U);
        for (std::uint32_t i = 0; i < 64U; ++i) {
            if (((mask >> i) & 1U) == 0U) {
                continue;
            }
            const std::uint32_t rec = kPoolBase + 1248U * i;
            if (mem.u32(mem.u32(rec + 688U) + 3268U) != b || mem.u8(rec + 698U) != 2U) {
                continue;
            }
            if (hdr == 0U) {
                return false;
            }
            const std::int32_t slot = static_cast<std::int16_t>(mem.u16(rec + 696U));
            const std::uint32_t entry = mem.u32(hdr + 8U) + 80U * static_cast<std::uint32_t>(slot);
            const std::int32_t list_index = mem.s8(entry + 12U);
            if (list_index < 0 || list_index > 7 || plan.part_count >= kMaxPartDraws) {
                return false;
            }
            add_part(PartKind::Pool, i, false, true, mem.u32(blk + 40U + 4U * static_cast<std::uint32_t>(list_index)),
                     0x03FFU, 2U, 0U, 0U, load_mat(mem, mem.u32(rec + 516U)));
        }
        return true;
    }
    if (cls == 0) {
        return true;
    }
    if (hdr == 0U) {
        return false;
    }
    const std::int32_t count = mem.s8(hdr);
    const std::uint32_t entries = mem.u32(hdr + 8U);
    if (count > 0 && static_cast<std::size_t>(count) + plan.part_count > kMaxPartDraws) {
        return false;
    }
    for (std::int32_t e = 0; e < count; ++e) {
        const std::uint32_t entry = entries + 80U * static_cast<std::uint32_t>(e);
        const std::int32_t list_index = mem.s8(entry + 12U);
        if (list_index < 0 || list_index > 7) {
            return false;
        }
        const Mat raw = load_mat(mem, entry + 16U);
        Mat matrix{};
        for (std::size_t i = 0; i < 3; ++i) {
            matrix[i] = xform_vector(raw[i], fm[0], fm[1], fm[2], raw[i][3]);
        }
        matrix[3] = xform_point(raw[3], fm[0], fm[1], fm[2], fm[3], fm[3][3]);
        add_part(PartKind::Extra, static_cast<std::size_t>(e), false, true,
                 mem.u32(blk + 40U + 4U * static_cast<std::uint32_t>(list_index)), 0x03FFU, value, 0U, 0U, matrix);
    }
    return true;
}

} // namespace takedown::runtime::native_car
