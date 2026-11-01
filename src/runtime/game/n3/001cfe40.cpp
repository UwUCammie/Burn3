// 0x001cfe40 (a0 = body, f12 = dt): IntegrateBodyOrientation, a leaf with no result. A rigid-body angular update
// followed by re-orthonormalisation of the body's basis. Body fields: 3x3 rows at +48/+64/+80, angular velocity at
// +112, angular state at +128 and +144, a derived vector at +160, and the pointer at +184 to the 16-byte rows of the
// basis being corrected (rows at +0/+16/+32 of that pointer).
//
// A (0x1cfe40..0x1d0078, 143 retired): +128 += velocity * dt, +144 += the new +128, +160 = the matrix at +48
// applied to the +144 vector. The basis rows are each reduced by their cross with (+160 * dt) and renormalised
// (vrsqrt for the scale, sqrt.s for the length that is gathered into VF3.xyz). A length of the first row that is not
// positive sends control to the last ladder case.
// B (0x1d007c..0x1d046c): a ladder on the lengths and then on the absolute values of the three pairwise dots decides
// which two rows are rebuilt from cross products of the others (each rebuilt row is normalised in place). Seven
// cases, one per exit of the ladder.
// C (0x1d046c..0x1d0928, 503 retired): the basis is permuted through the frame into the body's rows at +48/+64/+80,
// +112 and +128 are cleared (the w lane of the old values is kept).
//
// The function runs on the real VU0 state and the real frame: the original's scratch quadwords are written to the
// frame addresses it uses, and the reads of 96/112/128/1424/1440($sp) before any store (the stale stack) are read
// from guest memory, so the lanes the original leaks come out the same. The six byte-copy loops each run exactly 8
// times (a2 is 8 right before them), so they are 16-byte copies of 64 retired instructions each and the path length
// has no data dependence beyond the ladder.
//
// Retired counts: 143 (A), 6/6 for the first two ladder tests, 48 for the dot block, 6 or 9 for the two tests after
// it, 42..46 for the case, and 503 for C. The longest path is 143 + 6 + 6 + 48 + 9 + 45 + 503 = 760.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;

constexpr GuestAddress kFrameSize = 1920U;
constexpr GuestAddress kBasisPointer = 184U;

constexpr std::uint32_t kPrefixCost = 143U;
constexpr std::uint32_t kTailCost = 503U;   // 0x1d046c..0x1d0928 with the six loops at 8 passes of 8
constexpr std::uint32_t kTestCost = 6U;     // the two tests of the lengths, and the |x| < |y| and |x| < |z| tests
constexpr std::uint32_t kDotsCost = 48U;    // 0x1d00ac..0x1d0168
constexpr std::uint32_t kAxisTestCost = 9U; // 0x1d02ec..0x1d030c (the test of |y| and |z|)
constexpr std::uint32_t kCaseOneCost = 45U;   // 0x1d0184, 0x1d0238, 0x1d0310, 0x1d0700
constexpr std::uint32_t kCaseThreeCost = 42U; // 0x1d03c4
constexpr std::uint32_t kCaseLengthCost = 46U; // 0x1d07b4, 0x1d086c

template <class Base>
class Body {
public:
    Body(guest::R5900State& cpu, Base& memory, const GuestAddress frame) noexcept
        : cpu_(cpu), m_(memory), frame_(frame) {}

    [[nodiscard]] Gpr128 vf(const unsigned index) const { return cpu_.read_vu0_vector(index); }
    void set(const unsigned index, const Gpr128& value) { cpu_.write_vu0_vector(index, value); }
    void lqc2(const unsigned index, const GuestAddress address) { set(index, n3::load_quad_any(m_, address)); }
    void sqc2(const unsigned index, const GuestAddress address) { n3::store_quad_any(m_, address, vf(index)); }
    [[nodiscard]] Gpr128 lq(const GuestAddress address) const { return n3::load_quad_any(m_, address); }
    void sq(const GuestAddress address, const Gpr128& value) { n3::store_quad_any(m_, address, value); }
    [[nodiscard]] GuestAddress slot(const GuestAddress offset) const { return frame_ + offset; }

    // vaddw.x vf1,vf0,vf0w: VF1.x = 1.0.
    void one_into_vf1() { ee_cop2::vadd_broadcast(cpu_, 1U, 0U, 0U, kX, 3U, false); }
    // vadd<lane>.<mask> fd,vf0,ft<lane>: copies one lane of ft into the selected lanes of fd.
    void lane_to(const unsigned fd, const std::uint8_t mask, const unsigned ft, const std::size_t lane) {
        ee_cop2::vadd_broadcast(cpu_, fd, 0U, ft, mask, lane, false);
    }
    // vopmula.xyz acc,a,b ; vopmsub.xyz vf1,b,a: VF1 = the cross of the two registers.
    void cross(const unsigned a, const unsigned b) {
        ee_cop2::vopmula(cpu_, a, b);
        ee_cop2::vopmsub(cpu_, 1U, b, a);
    }
    // vmulax.xyz / vmadday.xyz / vmaddz.xyz: a 3x3 row combination, the vector's x, y, z weights the rows.
    void mula_x(const unsigned row, const unsigned vector) { ee_cop2::vmula_broadcast(cpu_, row, vector, kXyz, false); }
    void madda_y(const unsigned row, const unsigned vector) {
        ee_cop2::vmadd_broadcast(cpu_, 0U, row, vector, kXyz, 1U, false, true, false);
    }
    void madd_z(const unsigned fd, const unsigned row, const unsigned vector) {
        ee_cop2::vmadd_broadcast(cpu_, fd, row, vector, kXyz, 2U, false, false, false);
    }

    // The squared length of the row at `row` in VF1.x (vmul.xyz, vadday.x, vmaddz.x), with VF1 = `ones` before
    // the vmaddz.x so that only x is replaced. Leaves VF2 = row * row.
    void squared_length(const GuestAddress row, const Gpr128& ones) {
        lqc2(1U, row);
        ee_cop2::vmul(cpu_, 2U, 1U, 1U, kXyz);
        ee_cop2::vadda_broadcast(cpu_, 2U, 2U, kX, 1U);
        set(1U, ones);
        ee_cop2::vmadd_broadcast(cpu_, 1U, 1U, 2U, kX, 2U, false, false, false);
    }

    // Normalise the row in place. Entry: VF1 is "ones" (the vaddw.x result). The row is scaled by Q = 1/sqrt(len^2)
    // and the result stored back. With `with_sqrt` the original also takes sqrt.s of the squared length through f0
    // and keeps the quadword (x = the length, y = the sign extension of the 32-bit move, z/w from VF1): returned
    // through `length_quad`.
    Gpr128 normalise(const GuestAddress row, Gpr128* const length_quad) {
        squared_length(row, vf(1U));
        Gpr128 length = vf(1U);
        const Gpr128 original = lq(row);
        ee_cop2::vdiv(cpu_, 0U, 3U, 1U, 0U, true);
        if (length_quad != nullptr) {
            const std::uint32_t root = fpu::sqrt(length.words[0]);
            cpu_.write_fpr_bits(0U, root);
            length.words[0] = root;
            length.words[1] = (root & 0x80000000U) != 0U ? 0xffffffffU : 0U;
            *length_quad = length;
        }
        set(1U, original);
        ee_cop2::vmul_control(cpu_, 1U, 1U, kXyz, ee_cop2::kControlQ);
        const Gpr128 result = vf(1U);
        sq(row, result);
        return result;
    }

    // After a cross product in VF1: store it as the row, then normalise the row. Returns the normalised row.
    Gpr128 store_normalised(const GuestAddress row) {
        const Gpr128 cross_result = vf(1U);
        one_into_vf1();
        sq(row, cross_result);
        return normalise(row, nullptr);
    }

    guest::R5900State& cpu() { return cpu_; }

private:
    guest::R5900State& cpu_;
    Base& m_;
    GuestAddress frame_;
};

// The seven ladder cases. Each rebuilds two of the three basis rows at `base` (the pointer at +184) and returns
// with VU0 and memory as the original leaves them before 0x1d046c.

// 0x1d0184: |x| < |y| and |x| < |z|: rows 0 and 2 are rebuilt.
void case_first(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(1U, r2);
    b.lqc2(2U, r1);
    b.cross(2U, 1U);
    b.store_normalised(base);
    b.lqc2(1U, r1);
    b.lqc2(2U, base);
    b.cross(2U, 1U);
    b.store_normalised(r2);
}

// 0x1d0238: |x| < |y| and |x| >= |z|: rows 2 and 1.
void case_second(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(1U, r1);
    b.lqc2(2U, base);
    b.cross(2U, 1U);
    b.store_normalised(r2);
    b.lqc2(2U, r2);
    b.lqc2(1U, base);
    b.cross(2U, 1U);
    b.store_normalised(r1);
}

// 0x1d0310: |x| >= |y| and |y| < |z|: rows 1 and 0.
void case_third(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(2U, r2);
    b.lqc2(1U, base);
    b.cross(2U, 1U);
    b.store_normalised(r1);
    b.lqc2(2U, r1);
    b.lqc2(1U, r2);
    b.cross(2U, 1U);
    b.store_normalised(base);
}

// 0x1d03c4: |x| >= |y| and |y| >= |z|: rows 2 and 1, the first from the cross of VF3 and VF4 left by the dot block.
void case_fourth(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.cross(3U, 4U);
    b.store_normalised(r2);
    b.lqc2(2U, r2);
    b.lqc2(1U, base);
    b.cross(2U, 1U);
    b.store_normalised(r1);
}

// 0x1d0700: the third length is not positive: rows 2 and 1, the second from the normalised first.
void case_fifth(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(2U, base);
    b.lqc2(1U, r1);
    b.cross(2U, 1U);
    const Gpr128 row2 = b.store_normalised(r2);
    b.lqc2(1U, base);
    b.set(2U, row2);
    b.cross(2U, 1U);
    b.store_normalised(r1);
}

// 0x1d07b4: the second length is not positive: rows 1 and 0.
void case_sixth(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(2U, r2);
    b.lqc2(1U, base);
    b.cross(2U, 1U);
    const Gpr128 row1 = b.store_normalised(r1);
    b.lqc2(1U, r2);
    b.set(2U, row1);
    b.cross(2U, 1U);
    b.store_normalised(base);
}

// 0x1d086c: the first length is not positive: rows 0 and 2.
void case_seventh(auto& b, const GuestAddress base) {
    const GuestAddress r1 = base + 16U;
    const GuestAddress r2 = base + 32U;
    b.lqc2(2U, r1);
    b.lqc2(1U, r2);
    b.cross(2U, 1U);
    b.store_normalised(base);
    b.lqc2(1U, r1);
    b.lqc2(2U, base);
    b.cross(2U, 1U);
    b.store_normalised(r2);
}

// 0x1d046c..0x1d0928: the (corrected) basis is combined with the body's old rows through the frame and written to
// the body's rows at +48/+64/+80; +112 and +128 are cleared (w kept).
void rewrite_rows(auto& b, auto& m, const GuestAddress body) {
    // t3/t2/t1 weight the three vectors in VF1; `vector` is the register holding the weights.
    const auto combine = [&b](const Gpr128& r0, const Gpr128& r1, const Gpr128& r2, const unsigned vector) {
        b.set(1U, r0);
        b.mula_x(1U, vector);
        b.set(1U, r1);
        b.madda_y(1U, vector);
        b.set(1U, r2);
        b.madd_z(1U, 1U, vector);
        return b.vf(1U);
    };
    // The six 8-pass byte loops: 16 bytes copied between frame slots.
    const auto copy16 = [&b](const GuestAddress from, const GuestAddress to) { b.sq(to, b.lq(from)); };

    const GuestAddress base = m.read_u32_le(body + kBasisPointer);
    b.lqc2(1U, b.slot(128U));
    b.lqc2(6U, b.slot(96U));
    const Gpr128 basis0 = b.lq(base);
    const Gpr128 basis1 = b.lq(base + 16U);
    b.lqc2(2U, b.slot(112U));
    b.set(5U, basis0);
    const Gpr128 basis2 = b.lq(base + 32U);
    b.lane_to(1U, kX, 5U, 2U);
    b.sq(b.slot(1872U), basis0);
    const Gpr128 old0 = b.lq(body);
    b.sq(b.slot(1888U), basis1);
    b.set(4U, basis1);
    b.lane_to(6U, kX, 5U, 0U);
    const Gpr128 old1 = b.lq(body + 16U);
    const Gpr128 old2 = b.lq(body + 32U);
    b.sq(b.slot(1904U), basis2);
    b.lane_to(2U, kX, 5U, 1U);
    b.set(3U, basis2);
    b.lane_to(1U, kY, 4U, 2U);
    b.lane_to(6U, kY, 4U, 0U);
    b.lane_to(6U, kZ, 3U, 0U);
    b.lane_to(2U, kY, 4U, 1U);
    b.lane_to(1U, kZ, 3U, 2U);
    b.lane_to(2U, kZ, 3U, 1U);
    b.sqc2(1U, b.slot(128U));
    b.sqc2(6U, b.slot(96U));
    b.sqc2(6U, b.slot(1584U));
    b.sqc2(2U, b.slot(112U));

    b.sq(b.slot(48U), combine(old0, old1, old2, 6U));
    copy16(b.slot(112U), b.slot(1600U));
    b.lqc2(2U, b.slot(1600U));
    b.sq(b.slot(64U), combine(old0, old1, old2, 2U));
    copy16(b.slot(128U), b.slot(1616U));
    b.lqc2(2U, b.slot(1616U));
    b.sq(b.slot(80U), combine(old0, old1, old2, 2U));

    const Gpr128 first = b.lq(b.slot(48U));
    b.sq(b.slot(1632U), first);
    b.sq(body + 48U, first);
    copy16(b.slot(64U), b.slot(1648U));
    b.sq(body + 64U, b.lq(b.slot(1648U)));
    copy16(b.slot(80U), b.slot(1664U));
    const Gpr128 third_old = b.lq(b.slot(1664U));
    b.lqc2(4U, b.slot(1872U));
    const Gpr128 second_basis = b.lq(b.slot(1888U));
    b.sq(body + 80U, third_old);
    const Gpr128 third_basis = b.lq(b.slot(1904U));

    // The new rows against the corrected basis, with VF4/VF3/VF2 as the basis rows.
    const auto against_basis = [&b]() {
        b.mula_x(4U, 1U);
        b.madda_y(3U, 1U);
        b.madd_z(1U, 2U, 1U);
        return b.vf(1U);
    };
    b.lqc2(1U, body + 48U);
    b.mula_x(4U, 1U);
    b.set(3U, second_basis);
    b.madda_y(3U, 1U);
    b.set(2U, third_basis);
    b.madd_z(1U, 2U, 1U);
    const Gpr128 row0 = b.vf(1U);
    b.lqc2(1U, body + 64U);
    b.sq(b.slot(0U), row0);
    const Gpr128 row1 = against_basis();
    b.lqc2(1U, body + 80U);
    b.sq(b.slot(16U), row1);
    const Gpr128 row2 = against_basis();
    b.sq(b.slot(32U), row2);
    b.sq(b.slot(1536U), row0);
    b.sq(body + 48U, row0);
    copy16(b.slot(16U), b.slot(1552U));
    b.sq(body + 64U, b.lq(b.slot(1552U)));
    copy16(b.slot(32U), b.slot(1568U));
    const Gpr128 last = b.lq(b.slot(1568U));
    ee_cop2::vadd_vector(b.cpu(), 2U, 0U, 0U, kXyz, false);
    ee_cop2::vadd_vector(b.cpu(), 1U, 0U, 0U, kXyz, false);
    b.sq(body + 80U, last);
    b.sqc2(2U, body + 112U);
    b.sqc2(1U, body + 128U);
}

template <class Context>
n3::Run integrate_body_orientation(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress body = call.arg(0);
    Body b{cpu, m, call.stack_pointer() - kFrameSize};
    const std::uint32_t dt = cpu.read_fpr_bits(12U);

    // MFC1 writes the low 64 bits of the register sign-extended; QMTC2 then moves all 128 bits to the VU.
    const auto with_dt = [dt](Gpr128 quad) {
        quad.words[0] = dt;
        quad.words[1] = (dt & 0x80000000U) != 0U ? 0xffffffffU : 0U;
        return quad;
    };

    // --- A: integrate -------------------------------------------------------------------------------------
    b.lqc2(1U, body + 112U);
    b.set(2U, with_dt(cpu.read_gpr(3U)));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kXyz, 0U);
    b.lqc2(2U, body + 128U);
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, false);
    b.sqc2(2U, body + 128U);
    b.lqc2(1U, body + 144U);
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, kXyz, false);
    b.sqc2(1U, body + 144U);
    const Gpr128 state = b.lq(body + 144U);
    const Gpr128 matrix0 = b.lq(body + 48U);
    const Gpr128 matrix1 = b.lq(body + 64U);
    const Gpr128 matrix2 = b.lq(body + 80U);
    b.set(1U, matrix0);
    b.set(2U, state);
    b.mula_x(1U, 2U);
    b.set(1U, matrix1);
    b.madda_y(1U, 2U);
    b.set(1U, matrix2);
    b.madd_z(1U, 1U, 2U);
    const Gpr128 derived = b.vf(1U);
    b.sq(body + 160U, derived);
    b.lqc2(1U, body + 160U);
    b.set(2U, with_dt(derived));
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, kXyz, 0U);   // VF2 = derived * dt

    // Each basis row loses its cross with VF2. The pointer at +184 is read again for each row.
    for (const GuestAddress offset : {32U, 16U}) {
        const GuestAddress row = m.read_u32_le(body + kBasisPointer) + offset;
        b.lqc2(1U, row);
        b.cross(1U, 2U);
        b.lqc2(3U, row);
        ee_cop2::vadd_vector(cpu, 3U, 3U, 1U, kXyz, true);
        b.sqc2(3U, row);
    }
    const GuestAddress base = m.read_u32_le(body + kBasisPointer);
    b.lqc2(1U, base);
    b.cross(1U, 2U);
    const Gpr128 cross_result = b.vf(1U);
    b.lqc2(3U, base);
    b.set(2U, cross_result);
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, true);
    b.sqc2(3U, base);

    // Normalise the three rows; VF3 gathers the three lengths in x, y, z.
    b.one_into_vf1();
    Gpr128 length_quad;
    b.normalise(base, &length_quad);
    b.one_into_vf1();
    b.lqc2(3U, b.slot(1440U));
    b.set(2U, length_quad);
    b.lane_to(3U, kX, 2U, 0U);
    b.normalise(base + 16U, &length_quad);
    b.one_into_vf1();
    b.set(2U, length_quad);
    b.lane_to(3U, kY, 2U, 0U);
    const Gpr128 gathered = b.vf(3U);
    b.normalise(base + 32U, &length_quad);
    cpu.write_fpr_bits(1U, 0U);
    b.set(2U, gathered);
    b.set(1U, length_quad);
    b.lane_to(2U, kZ, 1U, 0U);
    const Gpr128 lengths = b.vf(2U);
    cpu.write_fpr_bits(0U, lengths.words[0]);
    b.sqc2(2U, b.slot(1440U));
    b.set(1U, lengths);
    std::uint32_t retired = kPrefixCost;

    // --- B: the ladder -------------------------------------------------------------------------------------
    if (fpu::le(lengths.words[0], 0U)) {
        case_seventh(b, base);
        retired += kCaseLengthCost;
    } else {
        retired += kTestCost;
        b.sqc2(1U, b.slot(944U));
        cpu.write_fpr_bits(0U, lengths.words[1]);
        if (fpu::le(lengths.words[1], 0U)) {
            case_sixth(b, base);
            retired += kCaseLengthCost;
        } else {
            retired += kTestCost;
            b.sqc2(1U, b.slot(928U));
            cpu.write_fpr_bits(0U, lengths.words[2]);
            if (fpu::le(lengths.words[2], 0U)) {
                case_fifth(b, base);
                retired += kCaseOneCost;
            } else {
                retired += kDotsCost;
                const GuestAddress r1 = base + 16U;
                const GuestAddress r2 = base + 32U;
                b.lqc2(4U, r1);
                b.lqc2(2U, r2);
                b.one_into_vf1();
                ee_cop2::vmul(cpu, 3U, 4U, 2U, kXyz);
                ee_cop2::vadda_broadcast(cpu, 3U, 3U, kX, 1U);
                b.set(1U, b.vf(1U));
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 3U, kX, 2U, false, false, false);   // dot(row1, row2)
                Gpr128 a1 = b.vf(1U);
                b.lqc2(5U, b.slot(1424U));
                b.set(3U, a1);
                b.lane_to(5U, kX, 3U, 0U);
                b.lqc2(3U, base);
                b.one_into_vf1();
                a1 = b.vf(1U);
                const Gpr128 first_dot = b.vf(5U);
                ee_cop2::vmul(cpu, 2U, 2U, 3U, kXyz);
                ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
                b.set(1U, a1);
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);   // dot(row2, row0)
                b.set(2U, first_dot);
                b.lane_to(2U, kY, 1U, 0U);
                const Gpr128 two_dots = b.vf(2U);
                b.one_into_vf1();
                ee_cop2::vmul(cpu, 2U, 3U, 4U, kXyz);
                a1 = b.vf(1U);
                ee_cop2::vadda_broadcast(cpu, 2U, 2U, kX, 1U);
                b.set(1U, a1);
                ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kX, 2U, false, false, false);   // dot(row0, row1)
                b.set(2U, two_dots);
                b.lane_to(2U, kZ, 1U, 0U);
                ee_cop2::vabs(cpu, 1U, 2U, kXyz);
                const Gpr128 magnitudes = b.vf(1U);
                b.sqc2(1U, b.slot(864U));
                cpu.write_fpr_bits(0U, magnitudes.words[1]);
                b.sqc2(1U, b.slot(1424U));
                cpu.write_fpr_bits(1U, magnitudes.words[0]);
                if (fpu::lt(magnitudes.words[0], magnitudes.words[1])) {
                    retired += kTestCost;
                    b.sqc2(1U, b.slot(848U));
                    cpu.write_fpr_bits(0U, magnitudes.words[2]);
                    if (fpu::lt(magnitudes.words[0], magnitudes.words[2])) {
                        case_first(b, base);
                    } else {
                        case_second(b, base);
                    }
                    retired += kCaseOneCost;
                } else {
                    retired += kAxisTestCost;
                    b.sqc2(1U, b.slot(832U));
                    b.sqc2(1U, b.slot(816U));
                    cpu.write_fpr_bits(1U, magnitudes.words[1]);
                    cpu.write_fpr_bits(0U, magnitudes.words[2]);
                    if (fpu::lt(magnitudes.words[1], magnitudes.words[2])) {
                        case_third(b, base);
                        retired += kCaseOneCost;
                    } else {
                        case_fourth(b, base);
                        retired += kCaseThreeCost;
                    }
                }
            }
        }
    }

    // --- C ----------------------------------------------------------------------------------------------------
    rewrite_rows(b, m, body);
    return call.finish(retired + kTailCost);
}

} // namespace

extern constinit const EngineFunction n3_001cfe40{0x001cfe40U, 2796U, 0xd80db09f4fba2ef9ULL, "IntegrateBodyOrientation",
                                                  760U, n3::live<&integrate_body_orientation<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001cfe40, integrate_body_orientation)
