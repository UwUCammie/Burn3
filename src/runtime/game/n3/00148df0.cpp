// 0x00148df0 (a0 = object, a1 = quad Q, a2 = quad index in its low 16 bits): InverseBilinearQuadFraction.
// The object's table (the u32 at object+4 points to P; the u32 at P+0 is the base of 4-halfword entries, 4 bytes
// per index) names four vertices i0..i3 of a road quad; the u32 at 0x0064d354 points to the 16-byte vertices.
// In the XZ plane
// (Y is never read) the result is returned in f0: a triangle A, B, C when i2 == i3, a triangle with origin C when
// i0 == i1, and otherwise the bilinear quad, where the fraction is the root of K*v*v + Bq*v + Cq = 0 (or -Cq/Bq when
// |K| is below the epsilon at gp-31236). The original's stack traffic only moves bit patterns and it makes no guest
// stores, so the values are used directly.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kVertexBase = 0x0064d354U;   // lui 0x65, lw -11436: pointer to the 16-byte vertices
constexpr std::uint32_t kFourBits = 0x40800000U;    // lui 0x4080: 4.0f
constexpr GuestAddress kEpsilonOffset = 31236U;     // lwc1 -31236($gp): epsilon at gp - 31236

// Retired instruction counts of the exits, delay slots included.
constexpr std::uint32_t kTriangleCost = 98U;        // i2 == i3
constexpr std::uint32_t kOriginCCost = 100U;        // i0 == i1
constexpr std::uint32_t kRootCost = 139U;           // full quad, |K| not below epsilon (or NaN)
constexpr std::uint32_t kLinearCost = 142U;         // full quad, |K| below epsilon

// The x and z components of a 16-byte record: word 0 and word 2.
struct Xz {
    std::uint32_t x;
    std::uint32_t z;
};

Xz xz_of(const guest::Gpr128& value) {
    return Xz{value.words[0], value.words[2]};
}

template <class Context>
n3::Run inverse_bilinear_quad_fraction(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const Xz q = xz_of(n3::load_quad_any(m, call.arg(1U)));
    const std::uint32_t index = call.arg(2U) & 0xffffU;
    const GuestAddress table = m.read_u32_le(m.read_u32_le(call.arg(0U) + 4U));
    const GuestAddress entry = table + (index << 2U);
    const std::uint32_t i0 = m.read_u16_le(entry);
    const std::uint32_t i1 = m.read_u16_le(entry + 2U);
    const std::uint32_t i2 = m.read_u16_le(entry + 4U);
    const std::uint32_t i3 = m.read_u16_le(entry + 6U);
    const GuestAddress vertices = m.read_u32_le(kVertexBase);
    const Xz a = xz_of(n3::load_quad_any(m, vertices + (i0 << 4U)));
    const Xz b = xz_of(n3::load_quad_any(m, vertices + (i1 << 4U)));
    const Xz c = xz_of(n3::load_quad_any(m, vertices + (i2 << 4U)));
    const Xz d = xz_of(n3::load_quad_any(m, vertices + (i3 << 4U)));
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t epsilon = m.read_u32_le(gp - kEpsilonOffset);

    if (i2 == i3) {
        // Triangle A, B, C: q, b and c relative to A.
        const std::uint32_t qx = fpu::sub(q.x, a.x);
        const std::uint32_t qz = fpu::sub(q.z, a.z);
        const std::uint32_t bx = fpu::sub(b.x, a.x);
        const std::uint32_t bz = fpu::sub(b.z, a.z);
        const std::uint32_t cx = fpu::sub(c.x, a.x);
        const std::uint32_t cz = fpu::sub(c.z, a.z);
        const std::uint32_t numerator = fpu::msub(fpu::mul(qz, bx), qx, bz);
        const std::uint32_t denominator = fpu::msub(fpu::mul(cz, bx), cx, bz);
        cpu.write_fpr_bits(0U, fpu::div(numerator, denominator));
        return call.finish(kTriangleCost);
    }
    if (i0 == i1) {
        // Triangle with origin C: q, d and a relative to C.
        const std::uint32_t qx = fpu::sub(q.x, c.x);
        const std::uint32_t qz = fpu::sub(q.z, c.z);
        const std::uint32_t ax = fpu::sub(a.x, c.x);
        const std::uint32_t az = fpu::sub(a.z, c.z);
        const std::uint32_t dx = fpu::sub(d.x, c.x);
        const std::uint32_t dz = fpu::sub(d.z, c.z);
        const std::uint32_t numerator = fpu::msub(fpu::mul(qz, dx), qx, dz);
        const std::uint32_t denominator = fpu::msub(fpu::mul(az, dx), ax, dz);
        cpu.write_fpr_bits(0U, fpu::div(numerator, denominator));
        return call.finish(kOriginCCost);
    }

    // Full quad: p = Q - A, b = B - A, c = C - A and e = ((A - B) - C) + D.
    const std::uint32_t px = fpu::sub(q.x, a.x);
    const std::uint32_t pz = fpu::sub(q.z, a.z);
    const std::uint32_t bx = fpu::sub(b.x, a.x);
    const std::uint32_t bz = fpu::sub(b.z, a.z);
    const std::uint32_t cx = fpu::sub(c.x, a.x);
    const std::uint32_t cz = fpu::sub(c.z, a.z);
    const std::uint32_t ex = fpu::add(fpu::sub(fpu::sub(a.x, b.x), c.x), d.x);
    const std::uint32_t ez = fpu::add(fpu::sub(fpu::sub(a.z, b.z), c.z), d.z);
    // K = ez*cx - ex*cz
    const std::uint32_t k = fpu::msub(fpu::mul(ez, cx), ex, cz);
    // Bq = ex*pz - ez*px - bx*cz + bz*cx, accumulated in the original's order.
    const std::uint32_t bq_acc = fpu::msub(fpu::msub(fpu::mul(ex, pz), ez, px), bx, cz);
    const std::uint32_t bq = fpu::madd(bq_acc, bz, cx);
    // Cq = pz*bx - px*bz
    const std::uint32_t cq = fpu::msub(fpu::mul(pz, bx), px, bz);
    // disc = Bq*Bq - (4*K)*Cq
    const std::uint32_t four_k = fpu::mul(kFourBits, k);
    const std::uint32_t disc = fpu::msub(fpu::mul(bq, bq), four_k, cq);
    const std::uint32_t root = fpu::sqrt(disc);
    if (fpu::lt(fpu::abs(k), epsilon)) {
        cpu.write_fpr_bits(0U, fpu::div(fpu::neg(cq), bq));
        return call.finish(kLinearCost);
    }
    cpu.write_fpr_bits(0U, fpu::div(fpu::add(fpu::neg(bq), root), fpu::add(k, k)));
    return call.finish(kRootCost);
}

} // namespace

extern constinit const EngineFunction n3_00148df0{0x00148df0U, 848U, 0x902fb90bbec273e6ULL,
                                                  "InverseBilinearQuadFraction", 142U,
                                                  n3::live<&inverse_bilinear_quad_fraction<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(00148df0, inverse_bilinear_quad_fraction)
