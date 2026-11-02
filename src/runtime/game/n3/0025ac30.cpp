// 0x0025ac30 EmitCarSprayParticles (a0 = trail system S, a1 = car C, a2 unused). Returns nothing.
//
// Per-frame particle emission for one car:
//   A. When the car's wheel record R = C[3268] has its "scrape" flag (bit 2 of R[4117]) and the record's age value is
//      above a threshold, one burst of scrape sparks (EmitScrapeSparks) is made at the contact point and the flag is
//      cleared.
//   B. A visibility factor k = 1 - (scale * age)^2; when k <= 0 the "spray" flag (bit 1) is cleared and nothing else
//      happens.
//   C. The emission count: the car's speed times k times the system rate, weighed by two clamped terms from the
//      car's surface record, fills two fractional accumulators that persist between frames (two floats in global
//      memory); their whole parts add up to n particles.
//   D. n particles: each draws from the VU0 random generator (seeded from the system's own random pair) a point in a
//      box around the car, a velocity and the sample floats, and appends them to the trail ring (AppendSampleRecord).
//      The jitter quad at 0x01d9d190 is rotated three lanes per particle and written back.
//
// The vector arithmetic runs through the VU0 macro FMAC kernels on raw lanes. The destination register of each
// instruction decides which lanes keep their old value, so the quads that reach AppendSampleRecord carry those
// lanes (the w lanes: the position's is the stale w of VF4 at the loop start, the velocity's is the box range's).

#include <algorithm>

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00399ec0)
N3_CALLEE(00254a90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Vec = guest::Gpr128;

constexpr GuestAddress kEffectRow = 0x004bc238U;     // lui 0x4c, addiu -15816
constexpr GuestAddress kGrid = 0x004bbfb0U;          // lui 0x4c, -16464: quad the jitter residue is taken against
constexpr GuestAddress kJitter = 0x01d9d190U;        // lui 0x1da, -11888: quad rotated three lanes per particle
constexpr GuestAddress kSparkEffect = 0x01ea4bb0U;   // lui 0x1ea, addiu 19376: argument of EmitScrapeSparks

// Effect row fields.
constexpr GuestAddress kRowTrail = 0U;
constexpr GuestAddress kRowSpread = 24U;
constexpr GuestAddress kRowRate1 = 48U;
constexpr GuestAddress kRowRate2 = 52U;

// System S.
constexpr GuestAddress kTrailBytes = 336U;
constexpr GuestAddress kTrailRecord = 368U;          // trail t = S + 336 * t + 368
constexpr GuestAddress kTrailScale1 = 632U;          // floats at S + 336 * t + 632 / + 676
constexpr GuestAddress kTrailScale2 = 676U;
constexpr GuestAddress kSeedFirst = 0x1f828U;
constexpr GuestAddress kSeedSecond = 0x1f82cU;
constexpr GuestAddress kTime = 0x1f848U;
constexpr GuestAddress kRate = 0x1f84cU;

// Car C.
constexpr GuestAddress kCarVelocity = 176U;
constexpr GuestAddress kCarBoxLow = 464U;
constexpr GuestAddress kCarBoxHigh = 480U;
constexpr GuestAddress kCarMatrix = 516U;
constexpr GuestAddress kCarMode = 533U;
constexpr GuestAddress kCarSurface = 3264U;
constexpr GuestAddress kCarWheels = 3268U;

// Wheel record R.
constexpr GuestAddress kWheelHeight = 1168U;
constexpr GuestAddress kWheelState = 4080U;
constexpr GuestAddress kWheelFlags = 4117U;
constexpr std::uint32_t kScrapeFlag = 0x04U;
constexpr std::uint32_t kSprayFlag = 0x02U;

// gp-relative tuning floats.
constexpr std::int32_t kGateGp = -29596;
constexpr std::int32_t kModeScaleGp = -29592;
constexpr std::int32_t kSlopeAGp = -27844;
constexpr std::int32_t kBaseAGp = -23688;
constexpr std::int32_t kSlopeBGp = -27836;
constexpr std::int32_t kBaseBGp = -27840;
constexpr std::int32_t kWeightCGp = -27828;
constexpr std::int32_t kWeightDGp = -27832;
constexpr std::int32_t kCarryFirstGp = -23676;
constexpr std::int32_t kCarrySecondGp = -23672;
constexpr std::int32_t kScaleXGp = -29684;
constexpr std::int32_t kScaleYGp = -29688;
constexpr std::int32_t kRangeWGp = -30684;
constexpr std::int32_t kBoxXLowGp = -29588;
constexpr std::int32_t kBoxXHighGp = -29584;

constexpr std::uint32_t kFrameBytes = 2112U;
constexpr GuestAddress kSparkPosition = 1824U;       // frame quads the callees read through a1..a3
constexpr GuestAddress kSparkRow = 1808U;
constexpr GuestAddress kSparkVelocity = 1840U;
constexpr GuestAddress kSamplePosition = 1888U;
constexpr GuestAddress kSampleVelocity = 1872U;

// Float constants (raw bits).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kMinusHalf = 0xbf000000U;
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kThreeHalves2 = 0x40600000U;  // 3.5
constexpr std::uint32_t kTen = 0x41200000U;
constexpr std::uint32_t kWordScale = 0x2f800000U;     // 2^-32

// Retired instruction counts per stretch of the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 36U;              // 0x25ac30..0x25acbc
constexpr std::uint32_t kScrapeTestCost = 4U;         // 0x25acc0..0x25accc
constexpr std::uint32_t kScrapeBodyCost = 71U;        // 0x25acd0..0x25ade8 (jal, delay slot and the flag clear)
constexpr std::uint32_t kModeOtherCost = 6U;          // 0x25adec..0x25ae10
constexpr std::uint32_t kModeOneCost = 8U;
constexpr std::uint32_t kFactorCost = 9U;             // 0x25ae14..0x25ae34
constexpr std::uint32_t kHiddenCost = 5U + 1U + 14U;  // flag clear, reload of ra, epilogue
constexpr std::uint32_t kRateCost = 51U;              // 0x25ae38..0x25af00
constexpr std::uint32_t kRateLessCost = 2U;           // a < b: 0x25af04..0x25af08
constexpr std::uint32_t kRateOtherCost = 1U;          // 0x25af0c
constexpr std::uint32_t kCountCost = 49U;             // 0x25af10..0x25afd0
constexpr std::uint32_t kNoneCost = 1U + 14U;         // n <= 0: reload of ra, epilogue
constexpr std::uint32_t kSetupCost = 68U;             // 0x25afd4..0x25b0e0
constexpr std::uint32_t kToFloatPositive = 3U;
constexpr std::uint32_t kToFloatNegative = 7U;
constexpr std::uint32_t kBoxCost = 77U;               // 0x25b10c..0x25b23c
constexpr std::uint32_t kDrawCost = 79U;              // 0x25b240..0x25b378
constexpr std::uint32_t kInsideCost = 7U;             // 0x25b3e4..0x25b3fc (P0.z < 1)
constexpr std::uint32_t kInsideOverCost = 2U;         // sum > 1: 0x25b400..0x25b404
constexpr std::uint32_t kOutsideCost = 2U;            // 0x25b37c..0x25b380
constexpr std::uint32_t kAppendCost = 17U;            // 0x25b384..0x25b3c4 incl. jal, delay slot, loop test
constexpr std::uint32_t kExitCost = 2U + 14U;         // 0x25b3c8..0x25b3cc, epilogue
constexpr std::uint32_t kCeiling = 20000U;

// --- scalar helpers ----------------------------------------------------------------------------------------

// MIN.S and MAX.S on raw words: the sign-magnitude ordering of ee_cop1_arithmetic.hpp (min_s / max_s).
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

std::uint32_t clamp01(const std::uint32_t value) { return min_bits(max_bits(value, 0U), kOne); }

// Unsigned word to float the way the original forms it (cvt.s.w of a signed word, with a halved copy and an
// added-to-itself result for bit 31).
std::uint32_t unsigned_to_float(const std::uint32_t word) {
    if (static_cast<std::int32_t>(word) >= 0) {
        return fpu::from_int(static_cast<std::int32_t>(word));
    }
    const std::uint32_t half = fpu::from_int(static_cast<std::int32_t>((word >> 1U) | (word & 1U)));
    return fpu::add(half, half);
}

// --- VU0 R (23-bit LFSR presented as a float in [1, 2)) -----------------------------------------------------

struct VuRandom {
    std::uint32_t bits{};
    void seed(const std::uint32_t value) { bits = (value & 0x007fffffU) | 0x3f800000U; }  // vrinit
    std::uint32_t next() {                                                                  // vrnext
        const std::uint32_t feedback = ((bits >> 4U) ^ (bits >> 22U)) & 1U;
        bits = (((bits << 1U) ^ feedback) & 0x007fffffU) | 0x3f800000U;
        return bits;
    }
};

// --- VU0 FMAC operations on raw lanes (flags are not tracked; nothing reads them) ---------------------------
// `dest` is the register the instruction writes: lanes outside the mask keep its value.

constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kXyzw = n3::kMaskXyzw;
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr Vec kVf0 = n3::kVf0;

Vec scalar(const std::uint32_t x) { return Vec{{x, 0U, 0U, 0U}}; }

Vec vadd(const Vec& dest, const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, l, r, mask, false).value;
}
Vec vsub(const Vec& dest, const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, l, r, mask, true).value;
}
// l +/- r[lane] on the masked lanes.
Vec vadd_lane(const Vec& dest, const Vec& l, const Vec& r, const std::size_t lane, const std::uint8_t mask,
              const bool subtract = false) {
    return guest::execute_vu0_macro_broadcast_add_subtract(dest, l, r, mask, lane, subtract).value;
}
// l * r[lane] on the masked lanes.
Vec vmul_lane(const Vec& dest, const Vec& l, const Vec& r, const std::size_t lane, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_broadcast_multiply(dest, l, r, mask, lane).value;
}
// x * y on the x lane (VMULx.x / VMULq.x of two scalars).
std::uint32_t vmul_x(const std::uint32_t left, const std::uint32_t right) {
    return vmul_lane(Vec{}, scalar(left), scalar(right), 0U, kX).words[0];
}
// ACC = base + vf0.x (VADDAx with VF0): the accumulator the following VMADD starts from.
Vec accumulate_base(const Vec& base, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_broadcast_add_subtract(base, base, kVf0, mask, 0U, false).value;
}
// dest = acc + left * right[lane] on xyz (VMADDx.xyz).
Vec vmadd_lane(const Vec& dest, const Vec& acc, const Vec& left, const Vec& right, const std::size_t lane) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(dest, acc, left, right, kXyz, lane, false).value;
}
// The rounding residue of draw * jitter.x on the grid, x lane (VMULx.x, VADDAx.x, VMSUBAw.x, VMSUBw.x).
std::uint32_t residue_x(const Vec& jitter, const Vec& grid, const std::uint32_t draw) {
    Vec draws = jitter;
    draws.words[0] = draw;
    const Vec scaled = guest::execute_vu0_macro_broadcast_multiply(draws, jitter, draws, kX, 0U).value;
    const Vec acc = guest::execute_vu0_macro_broadcast_add_subtract(scaled, grid, scaled, kX, 0U, false).value;
    const Vec acc2 =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(grid, acc, grid, kVf0, kX, 3U, true).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(scaled, acc2, scaled, kVf0, kX, 3U, true)
        .value.words[0];
}

Vec rotate(const Vec& v) { return Vec{{v.words[1], v.words[2], v.words[3], v.words[0]}}; }  // vmr32

// The loop's constant quads, built once from the car and the tuning floats.
struct Box {
    Vec start;        // E1 = 2 * X - Y: the box corner the random quad is added to
    Vec range;        // F = Y - X
    Vec anchor;       // A: the car's position blended with its box (w lane is VF4's)
    Vec x_base;       // sp+2048
    Vec x_range;      // sp+2064
    Vec y_base;       // sp+2080
    Vec y_range;      // sp+2096
    Vec z_base;       // sp+2016
    Vec z_range;      // sp+2032
};

struct Tuning {
    std::uint32_t scale_x;    // G(-29684)
    std::uint32_t scale_y;    // G(-29688)
    std::uint32_t range_w;    // G(-30684)
    std::uint32_t box_x_low;  // G(-29588)
    std::uint32_t box_x_high; // G(-29584)
};

// The setup of 0x25b03c..0x25b23c: the rows of the car's matrix (M0, M2, M3), its two box quads, the velocity V
// and the factors k, c, d and the effect row's spread turn into the quads the loop reads from the frame.
Box make_box(const Vec& m0, const Vec& m2, const Vec& m3, const Vec& low, const Vec& high, const Vec& velocity,
             const std::uint32_t k, const std::uint32_t c, const std::uint32_t d, const std::uint32_t spread,
             const std::uint32_t vf4_w, const Tuning& tune) {
    const Vec half = scalar(kHalf);
    const Vec minus_half = scalar(kMinusHalf);
    Box box{};

    Vec vf4 = vmul_lane(Vec{{0U, 0U, 0U, vf4_w}}, m0, high, 0U);   // M0 * high.x
    const Vec vf7 = vmul_lane(m2, m2, high, 2U);                    // M2 * high.z
    const Vec vf2 = vadd(m3, m3, vf4);
    vf4 = vadd(vf4, vf2, vf7);
    const Vec dx = vadd_lane(vf2, low, high, 0U, kX, true);         // low.x - high.x
    const Vec vf3 = vmul_lane(m0, m0, dx, 0U);                      // M0 * dx
    Vec vf1 = vadd_lane(low, low, high, 2U, kZ, true);              // low.z - high.z
    const Vec vf2b = vmul_lane(dx, m2, vf1, 2U);                    // M2 * dz
    vf1 = vadd(vf1, vf3, vf2b);
    vf1 = vmul_lane(vf1, vf1, half, 0U);
    box.anchor = vadd(vf4, vf4, vf1);

    const Vec wa = vmul_lane(vf3, vf3, scalar(k), 0U);              // M0 * dx * k
    const Vec wb = vmul_lane(vf2b, vf2b, scalar(k), 0U);            // M2 * dz * k
    const Vec wa_neg = vmul_lane(vf1, wa, minus_half, 0U);
    box.y_base = vmul_lane(wa, wa, half, 0U);
    box.y_range = vsub(wa_neg, box.y_base, wa_neg);
    const Vec wb_neg = vmul_lane(wb, wb, minus_half, 0U);
    box.x_base = vmul_lane(wb, wb, half, 0U);
    box.x_range = vsub(box.y_range, box.x_base, wb_neg);

    box.z_base = vmul_lane(high, velocity, scalar(tune.scale_x), 0U);
    const Vec z_neg = vmul_lane(wb, velocity, scalar(tune.scale_y), 0U);
    box.z_range = vsub(z_neg, box.z_base, z_neg);

    const std::uint32_t two_spread = fpu::mul(kTwo, spread);
    const Vec x{{tune.box_x_low, fpu::neg(two_spread), fpu::mul(kHalf, c), fpu::mul(tune.scale_y, d)}};
    const Vec y{{tune.box_x_high, two_spread, fpu::mul(kThreeHalves2, c), fpu::mul(tune.range_w, d)}};
    const Vec twice = vadd(x, x, x, kXyzw);
    box.start = vsub(twice, twice, y, kXyzw);
    box.range = vsub(y, y, x, kXyzw);
    return box;
}

template <class Context>
n3::Run emit_car_spray_particles(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress system = call.arg(0);
    const GuestAddress car = call.arg(1);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const auto tune_word = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };

    GuestAddress wheels = m.read_u32_le(car + kCarWheels);
    const GuestAddress matrix = m.read_u32_le(car + kCarMatrix);
    // lqc2 faults on a misaligned address (lq masks it); such a call is left to the original.
    if (((wheels | call.stack_pointer()) & 15U) != 0U) {
        return call.decline();
    }
    const Vec velocity = n3::load_quad_any(m, car + kCarVelocity);
    const Vec wheel_state = n3::load_quad_any(m, wheels + kWheelState);
    const std::uint32_t time = m.read_u32_le(system + kTime);
    // age = TIME - state.w (VSUBw.x)
    const std::uint32_t age = vadd_lane(Vec{}, scalar(time), wheel_state, 3U, kX, true).words[0];
    std::uint32_t retired = kHeadCost;

    // A. The scrape-sparks burst.
    if ((m.read_u8(wheels + kWheelFlags) & kScrapeFlag) != 0U) {
        retired += kScrapeTestCost;
        if (!fpu::le(age, tune_word(kGateGp))) {
            if ((matrix & 15U) != 0U) {
                return call.decline();
            }
            const Vec row0 = n3::load_quad_any(m, matrix);
            const Vec row1 = n3::load_quad_any(m, matrix + 16U);
            const Vec row2 = n3::load_quad_any(m, matrix + 32U);
            const Vec row3 = n3::load_quad_any(m, matrix + 48U);
            const std::uint32_t height = min_bits(n3::load_quad_any(m, wheels + kWheelHeight).words[3], kOne);
            // The contact point (x, 0, max(0, z + 0.25)) from the wheel state through the matrix, its y pulled
            // under the matrix origin by the wheel height.
            const std::uint32_t lifted = vadd_lane(Vec{}, wheel_state, scalar(kQuarter), 0U, kZ).words[2];
            const std::uint32_t offset_x = vmul_lane(Vec{}, wheel_state, scalar(kHalf), 0U, kX).words[0];
            Vec local = vadd_lane(wheel_state, kVf0, scalar(offset_x), 0U, kX);
            local = vadd_lane(local, kVf0, scalar(0U), 0U, kY);
            local = vadd_lane(local, kVf0, scalar(max_bits(0U, lifted)), 0U, kZ);
            Vec acc = guest::execute_vu0_macro_broadcast_multiply(row0, row0, local, kXyz, 0U).value;
            acc = guest::execute_vu0_macro_broadcast_multiply_accumulate(row1, acc, row1, local, kXyz, 1U, false)
                      .value;
            acc = guest::execute_vu0_macro_broadcast_multiply_accumulate(row2, acc, row2, local, kXyz, 2U, false)
                      .value;
            const Vec point = guest::execute_vu0_macro_broadcast_multiply_accumulate(row3, acc, row3, kVf0, kXyz, 3U,
                                                                                     false)
                                  .value;
            const std::uint32_t under = vadd_lane(row3, row3, scalar(height), 0U, kY, true).words[1];
            const Vec position = vadd_lane(point, kVf0, scalar(min_bits(point.words[1], under)), 0U, kY);

            n3::store_quad_any(m, frame + kSparkPosition, position);
            n3::store_quad_any(m, frame + kSparkRow, row1);
            n3::store_quad_any(m, frame + kSparkVelocity, velocity);
            call.set_arg(0U, kSparkEffect);
            call.set_arg(1U, frame + kSparkPosition);
            call.set_arg(2U, frame + kSparkRow);
            call.set_arg(3U, frame + kSparkVelocity);
            const n3::Run burst = call.invoke([](auto& machine) { return n3::fn_00399ec0(machine); }, kFrameBytes);
            if (burst.declined) {
                return call.decline();
            }
            retired += kScrapeBodyCost + burst.retired;
            wheels = m.read_u32_le(car + kCarWheels);
            m.write_u8(wheels + kWheelFlags, static_cast<std::uint8_t>(m.read_u8(wheels + kWheelFlags) & 0xfbU));
        }
    }

    // B. The visibility factor: k = 1 - (scale * age)^2.
    const bool mode_one = static_cast<std::int8_t>(m.read_u8(car + kCarMode)) == 1;
    const std::uint32_t scaled = fpu::mul(mode_one ? kHalf : tune_word(kModeScaleGp), age);
    retired += mode_one ? kModeOneCost : kModeOtherCost;
    const std::uint32_t k = fpu::msub(fpu::add(0U, kOne), scaled, scaled);
    retired += kFactorCost;
    if (fpu::le(k, 0U)) {
        wheels = m.read_u32_le(car + kCarWheels);
        m.write_u8(wheels + kWheelFlags, static_cast<std::uint8_t>(m.read_u8(wheels + kWheelFlags) & ~kSprayFlag));
        return call.finish(retired + kHiddenCost);
    }

    // C. The emission count.
    const Vec square = guest::execute_vu0_macro_vector_multiply(velocity, velocity, velocity, kXyz).value;
    const Vec one = scalar(kOne);
    const Vec lane_sum = guest::execute_vu0_macro_broadcast_add_subtract(square, square, square, kX, 1U, false).value;
    const std::uint32_t radicand =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(one, lane_sum, one, square, kX, 2U, false)
            .value.words[0];
    const std::uint32_t root = guest::ee_cop2::square_root_bits(radicand & 0x7fffffffU);
    cpu.write_vu0_control(guest::ee_cop2::kControlQ, root);
    const std::uint32_t speed = vmul_x(kOne, root);
    const std::uint32_t rate = vmul_x(vmul_x(vmul_x(speed, kTen), k), m.read_u32_le(system + kRate));

    const GuestAddress surface = m.read_u32_le(car + kCarSurface);
    const std::uint32_t surface_a = m.read_u32_le(surface + 92U);
    const std::uint32_t surface_b = m.read_u32_le(surface + 96U);
    const std::uint32_t lowest = min_bits(surface_b, surface_a);
    const std::uint32_t a = clamp01(fpu::mul(tune_word(kSlopeAGp), fpu::sub(surface_a, tune_word(kBaseAGp))));
    const std::uint32_t b = clamp01(fpu::mul(tune_word(kSlopeBGp), fpu::sub(surface_b, tune_word(kBaseBGp))));
    const std::uint32_t both = fpu::add(a, b);
    const std::uint32_t both_squared = fpu::mul(both, both);
    const bool a_less = fpu::lt(a, b);
    const std::uint32_t blend = fpu::add(both_squared, a_less ? a : b);
    retired += kRateCost + (a_less ? kRateLessCost : kRateOtherCost);

    const std::uint32_t c = clamp01(fpu::mul(blend, tune_word(kWeightCGp)));
    const std::uint32_t d = min_bits(max_bits(fpu::mul(lowest, tune_word(kWeightDGp)), 0U), fpu::sub(kOne, c));

    const GuestAddress row = kEffectRow;
    const GuestAddress trail_object = system + kTrailBytes * m.read_u32_le(row + kRowTrail);
    const GuestAddress trail = trail_object + kTrailRecord;
    const std::uint32_t carry_first = tune_word(kCarryFirstGp);
    const std::uint32_t carry_second = tune_word(kCarrySecondGp);
    const std::uint32_t first = fpu::madd(fpu::add(0U, carry_first), fpu::mul(rate, m.read_u32_le(row + kRowRate1)),
                                          m.read_u32_le(trail_object + kTrailScale1));
    const std::uint32_t second = fpu::madd(fpu::add(0U, carry_second), fpu::mul(rate, m.read_u32_le(row + kRowRate2)),
                                           m.read_u32_le(trail_object + kTrailScale2));
    const std::int32_t whole_first = fpu::to_int(first);
    m.write_u32_le(gp + static_cast<std::uint32_t>(kCarryFirstGp), fpu::sub(first, fpu::from_int(whole_first)));
    const std::int32_t whole_second = fpu::to_int(second);
    m.write_u32_le(gp + static_cast<std::uint32_t>(kCarrySecondGp), fpu::sub(second, fpu::from_int(whole_second)));
    std::uint32_t remaining = static_cast<std::uint32_t>(whole_first) + static_cast<std::uint32_t>(whole_second);
    retired += kCountCost;
    if (static_cast<std::int32_t>(remaining) <= 0) {
        return call.finish(retired + kNoneCost);
    }

    // D. The particles. The system's random pair seeds R.
    if ((matrix & 15U) != 0U) {
        return call.decline();
    }
    const Vec low = n3::load_quad_any(m, car + kCarBoxLow);
    const Vec high = n3::load_quad_any(m, car + kCarBoxHigh);
    std::uint32_t seed_first = m.read_u32_le(system + kSeedFirst);
    seed_first = (seed_first << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(seed_first) >> 16);
    const std::uint32_t seed_second = m.read_u32_le(system + kSeedSecond);
    seed_first += seed_second;
    m.write_u32_le(system + kSeedFirst, seed_first);
    m.write_u32_le(system + kSeedSecond, seed_second + seed_first);
    VuRandom random;
    random.seed(fpu::mul(kWordScale, unsigned_to_float(seed_first)));
    retired += kSetupCost + (static_cast<std::int32_t>(seed_first) >= 0 ? kToFloatPositive : kToFloatNegative) + kBoxCost;

    const Tuning tune{tune_word(kScaleXGp), tune_word(kScaleYGp), tune_word(kRangeWGp), tune_word(kBoxXLowGp),
                      tune_word(kBoxXHighGp)};
    const Box box = make_box(n3::load_quad_any(m, matrix), n3::load_quad_any(m, matrix + 32U),
                             n3::load_quad_any(m, matrix + 48U), low, high, velocity, k, c, d,
                             m.read_u32_le(row + kRowSpread), cpu.read_vu0_vector(4U).words[3],
                             tune);

    const Vec grid = n3::load_quad_any(m, kGrid);
    while (true) {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        // P0 = E1 + F * (four draws)
        Vec draws{};
        draws.words[0] = random.next();
        draws.words[1] = random.next();
        draws.words[2] = random.next();
        draws.words[3] = random.next();
        const Vec p0 = guest::execute_vu0_macro_vector_multiply_accumulate(draws, accumulate_base(box.start, kXyzw),
                                                                           box.range, draws, kXyzw, false)
                           .value;

        // The three residues take the jitter lane by lane; X1, Y1, Z1 weigh the box quads with them.
        const Vec jitter = n3::load_quad_any(m, kJitter);
        const Vec jitter1 = rotate(jitter);
        const Vec jitter2 = rotate(jitter1);
        Vec first_residue = box.range;
        first_residue.words[0] = residue_x(jitter, grid, random.next());
        const Vec x1 = vmadd_lane(first_residue, accumulate_base(box.x_base), box.x_range, first_residue, 0U);
        Vec second_residue = box.x_range;
        second_residue.words[0] = residue_x(jitter1, grid, random.next());
        const Vec y1 = vmadd_lane(second_residue, accumulate_base(box.y_base), box.y_range, second_residue, 0U);
        const Vec offset = vadd(x1, y1, x1);
        const Vec position = vadd(box.anchor, box.anchor, offset);
        const Vec scaled_offset = vmul_lane(offset, offset, p0, 0U);
        Vec third_residue = position;
        third_residue.words[0] = residue_x(jitter2, grid, random.next());
        const Vec z1 = vmadd_lane(third_residue, accumulate_base(box.z_base), box.z_range, third_residue, 0U);
        const Vec velocity_out = vadd(scaled_offset, scaled_offset, z1);
        n3::store_quad_any(m, kJitter, rotate(jitter2));

        retired += kDrawCost;
        std::uint32_t sample_z = kOne;
        std::uint32_t sample_w = 0U;
        if (fpu::lt(p0.words[2], kOne)) {
            sample_z = p0.words[2];
            sample_w = p0.words[3];
            retired += kInsideCost;
            if (!fpu::le(fpu::add(p0.words[2], p0.words[3]), kOne)) {
                sample_w = fpu::sub(kOne, p0.words[2]);
                retired += kInsideOverCost;
            }
        } else {
            retired += kOutsideCost;
        }

        n3::store_quad_any(m, frame + kSamplePosition, position);
        n3::store_quad_any(m, frame + kSampleVelocity, velocity_out);
        call.set_arg(0U, trail);
        call.set_arg(1U, frame + kSamplePosition);
        call.set_arg(2U, frame + kSampleVelocity);
        call.set_arg(3U, 0U);
        call.set_arg(4U, static_cast<std::int32_t>(whole_first) < static_cast<std::int32_t>(remaining) ? 1U : 0U);
        call.set_float_arg(0U, m.read_u32_le(system + kTime));
        call.set_float_arg(1U, sample_w);
        call.set_float_arg(2U, sample_z);
        call.set_float_arg(3U, kOne);
        call.set_float_arg(4U, k);
        call.set_float_arg(5U, p0.words[1]);
        const n3::Run appended = call.invoke([](auto& machine) { return n3::fn_00254a90(machine); }, kFrameBytes);
        if (appended.declined) {
            return call.decline();
        }
        retired += kAppendCost + appended.retired;
        if (--remaining == 0U) {
            break;
        }
    }
    call.set_vector_random(random.bits);
    return call.finish(retired + kExitCost);
}

}  // namespace
}  // namespace takedown::runtime

N3_ENTRY(0025ac30, 2068U, 0x9cb4248fbc9794e3ULL, "EmitCarSprayParticles", kCeiling, emit_car_spray_particles,
         EngineReturn::Nothing)
