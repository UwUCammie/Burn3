// 0x0025f0c0 EmitTrailParticles (a0 = effect system, a1 = start quad, a2 = end quad, a3 = spread quad,
// t0 = emission kind, t1 = float pair that shades the colours, t2 = carry byte, t3 = spacing gate for
// AppendSampleRecord, the byte at the entry $sp = "feed both trails" flag, f12 = intensity, f13 = rate scale,
// f14 = size scale). Returns nothing.
//
// Emission kind t0 is a 56-byte record at 0x004bc040 (effect index, rate, ranges). The effect (336 bytes at
// system + 368) has two trails; their rates are at +264 and +308. How many particles are made follows the rate
// scaled by f12 * f13 and a random wobble; the fraction left over is kept in the carry byte (scaled by 1/256 on
// the way in, by a global on the way out). With the flag set the second trail is fed first (count 2), then the
// flag byte is cleared and the first trail follows. No particles: the function returns right there.
//
// A pass spreads up to 8 particles evenly from the start to the end quad, offset by a random fraction. Each
// particle draws random sizes, a colour, a start offset and a direction from the VU0 R generator (seeded from
// the effect system's random pair), every draw jittered by the quad at 0x01d9d190 that is rotated one lane
// after use. A particle whose size.x is within the limit is skipped; the others are appended to the trail by
// AppendSampleRecord (0x00254a90) and the pass ends early when that refuses.
//
// The vector arithmetic runs through the VU0 macro FMAC kernels on raw lanes (the stack copies of the original
// only ever feed xyz lanes of those operations, or dead w lanes).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00254a90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Vec = guest::Gpr128;

constexpr GuestAddress kEmissions = 0x004bc040U;       // lui 0x4c, addiu -16320: 56-byte emission kinds
constexpr std::uint32_t kEmissionBytes = 56U;
constexpr GuestAddress kEffects = 368U;                // effect e at system + 368 + 336 * e
constexpr std::uint32_t kEffectBytes = 336U;
constexpr GuestAddress kRandomPair = 0x0001f828U;      // system + ...: the effect system's two random words
constexpr GuestAddress kClock = 0x0001f848U;           // float: effect time
constexpr GuestAddress kClockStep = 0x0001f84cU;       // float: time step of the frame
constexpr GuestAddress kGravityScale = 0x0001f850U;    // float
constexpr GuestAddress kJitter = 0x01d9d190U;          // quad rotated a lane after every use
constexpr GuestAddress kJitterGrid = 0x004bbfb0U;      // quad: the grid the jitter residue is taken against
// gp-relative globals (read through $gp).
constexpr std::int32_t kSizeLimitGp = -29952;
constexpr std::int32_t kCarryScaleGp = -29604;
constexpr std::int32_t kColourGp[4] = {-27816, -27812, -27808, -27804};

// Emission kind fields.
constexpr GuestAddress kEffectIndex = 0U;
constexpr GuestAddress kRate = 4U;
constexpr GuestAddress kOffset = 8U;
constexpr GuestAddress kSpreadScale = 12U;
constexpr GuestAddress kSideways = 16U;
constexpr GuestAddress kUpward = 20U;
constexpr GuestAddress kSizeY = 24U;
constexpr GuestAddress kGravity = 28U;
constexpr GuestAddress kSizeZLow = 32U;
constexpr GuestAddress kSizeZHigh = 36U;
constexpr GuestAddress kSizeXLow = 40U;
constexpr GuestAddress kSizeXHigh = 44U;
constexpr GuestAddress kFirstTrailRate = 48U;
constexpr GuestAddress kSecondTrailRate = 52U;
constexpr GuestAddress kFirstEffectRate = 264U;        // in the effect: rate of the first trail
constexpr GuestAddress kSecondEffectRate = 308U;

constexpr std::uint32_t kFrameBytes = 1440U;
constexpr GuestAddress kPositionSlot = 816U;           // the frame quads AppendSampleRecord reads through a1, a2
constexpr GuestAddress kDirectionSlot = 1184U;
constexpr GuestAddress kStaleOffsetW = 652U;          // w of the start-offset quad (640..652); never written here
constexpr std::uint32_t kMostPerPass = 8U;

// Float constants (raw bits).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kCarryIn = 0x3b800000U;        // 1/256
constexpr std::uint32_t kWordScale = 0x2f800000U;      // 2^-32
constexpr std::uint32_t kWobbleBase = 0x3f600000U;     // 0.875
constexpr std::uint32_t kWobbleRange = 0x3e800000U;    // 0.25

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 46U;              // 0x25f0c0..0x25f194: frame, loads, carry as a float
constexpr std::uint32_t kRandomCost = 41U;             // 0x25f194..0x25f234: first draw and the rate
constexpr std::uint32_t kToFloatPositive = 3U;         // unsigned word to float, bit 31 clear
constexpr std::uint32_t kToFloatNegative = 7U;         // ... bit 31 set
constexpr std::uint32_t kWobbleCost = 11U;             // 0x25f260..0x25f288
constexpr std::uint32_t kBothCountsCost = 19U;         // 0x25f28c..0x25f2d4
constexpr std::uint32_t kOneCountCost = 8U;            // 0x25f2d8..0x25f2f4
constexpr std::uint32_t kCarryStoreCost = 7U;          // 0x25f2f8..0x25f310
constexpr std::uint32_t kReturnCost = 19U;             // 0x25f8f8..0x25f940
constexpr std::uint32_t kSetupCost = 61U;              // 0x25f314..0x25f404
constexpr std::uint32_t kSetupTailCost = 101U;         // 0x25f430..0x25f5c0
constexpr std::uint32_t kPassHeadBoth = 9U;            // 0x25f5c4.. with the flag set (second trail first)
constexpr std::uint32_t kPassHeadFirst = 8U;
constexpr std::uint32_t kEmptyPassCost = 1U;           // 0x25f8e4
constexpr std::uint32_t kPassSetupCost = 29U;          // 0x25f5ec..0x25f65c
constexpr std::uint32_t kPassSetupTailCost = 15U;      // 0x25f688..0x25f6c0
constexpr std::uint32_t kParticleCost = 33U;           // 0x25f6c4..0x25f744
constexpr std::uint32_t kSkipCost = 7U;                // 0x25f748..0x25f760: step to the next particle
constexpr std::uint32_t kPassEndCost = 2U;             // 0x25f764: the loop ran out
constexpr std::uint32_t kEmitCost = 70U;               // 0x25f76c..0x25f880
constexpr std::uint32_t kColourBelowOne = 7U;          // 0x25f890..0x25f8a8
constexpr std::uint32_t kColourClamped = 1U;           // 0x25f8ac
constexpr std::uint32_t kColourOne = 3U;               // 0x25f884..0x25f88c
constexpr std::uint32_t kCallCost = 13U;               // 0x25f8b0..0x25f8e0: arguments, jal, result test
constexpr std::uint32_t kJoinCost = 2U;                // 0x25f8e8
constexpr std::uint32_t kNextPassCost = 2U;            // 0x25f8f0

constexpr std::uint32_t kCeiling = 20000U;

// --- The effect system's random pair ------------------------------------------------------------------------

// One draw (0x25f194..0x25f22c): first = (first << 16) + (first >> 16 arithmetic); first += second;
// second += first. Returns the new first word.
template <class Memory>
std::uint32_t draw(Memory& m, const GuestAddress system) {
    std::uint32_t first = m.read_u32_le(system + kRandomPair);
    std::uint32_t second = m.read_u32_le(system + kRandomPair + 4U);
    first = (first << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(first) >> 16);
    first += second;
    second += first;
    m.write_u32_le(system + kRandomPair, first);
    m.write_u32_le(system + kRandomPair + 4U, second);
    return first;
}

// Unsigned word to float the way the original forms it (cvt.s.w of a signed word, with a halved copy and an
// added-to-itself result for bit 31).
std::uint32_t unsigned_to_float(const std::uint32_t word) {
    if (static_cast<std::int32_t>(word) >= 0) {
        return fpu::from_int(static_cast<std::int32_t>(word));
    }
    const std::uint32_t half = fpu::from_int(static_cast<std::int32_t>((word >> 1U) | (word & 1U)));
    return fpu::add(half, half);
}

std::uint32_t to_float_cost(const std::uint32_t word) {
    return static_cast<std::int32_t>(word) >= 0 ? kToFloatPositive : kToFloatNegative;
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

constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kXyzw = n3::kMaskXyzw;
constexpr Vec kVf0 = n3::kVf0;

Vec vadd(const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(l, l, r, mask, false).value;
}
Vec vsub(const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(l, l, r, mask, true).value;
}
Vec vmul(const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_multiply(l, l, r, mask).value;
}
Vec vmul_lane(const Vec& l, const Vec& r, const std::size_t lane, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_broadcast_multiply(l, l, r, mask, lane).value;
}
// ACC = base + vf0.x (VADDAx with VF0): the accumulator the following VMADD starts from.
Vec accumulate_base(const Vec& base, const std::uint8_t mask) {
    return guest::execute_vu0_macro_broadcast_add_subtract(base, base, kVf0, mask, 0U, false).value;
}
// base + range * weight on the lanes of `mask` (VADDAx, then VMADD).
Vec lerp(const Vec& base, const Vec& range, const Vec& weight, const std::uint8_t mask = kXyz) {
    const Vec acc = accumulate_base(base, mask);
    return guest::execute_vu0_macro_vector_multiply_accumulate(weight, acc, range, weight, mask, false).value;
}
// VMSUBAw / VMSUBw against VF0.w = 1.0: acc - left * 1.0.
Vec msub_one(const Vec& acc, const Vec& left) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(left, acc, left, kVf0, kXyz, 3U, true).value;
}

// The rounding residue of jitter * random on the grid, (jitter*r + grid - grid) - jitter*r, with three draws
// from R (0x25f6c4..0x25f6f0).
Vec jittered(const Vec& jitter, const Vec& grid, VuRandom& random) {
    Vec draws{};
    draws.words[0] = random.next();
    draws.words[1] = random.next();
    draws.words[2] = random.next();
    const Vec scaled = vmul(jitter, draws);
    const Vec acc = guest::execute_vu0_macro_vector_add_subtract(scaled, scaled, grid, kXyz, false).value;
    const Vec acc2 = guest::execute_vu0_macro_broadcast_multiply_accumulate(grid, acc, grid, kVf0, kXyz, 3U, true)
                         .value;
    return msub_one(acc2, scaled);
}

Vec rotate(const Vec& v) { return Vec{{v.words[1], v.words[2], v.words[3], v.words[0]}}; }  // vmr32

template <class Memory>
Vec load_vec(const Memory& m, const GuestAddress address) { return n3::load_quad_any(m, address); }

// What the pass setup (0x25f314..0x25f5c0) leaves in the frame for the particle loops.
struct Ranges {
    Vec size_base;        // x/y/z size at the top of its range
    Vec size_range;
    Vec offset_base;      // start offset: (offset, offset, offset) .. (-offset)
    Vec offset_range;
    Vec direction_high;
    Vec direction_range;
    Vec colour_base;
    Vec colour_range;
    Vec quarter_spread;
    Vec fall;             // gravity pulling on y
    std::uint32_t size_limit{};
};

template <class Memory>
Ranges make_ranges(Memory& m, const GuestAddress kind, const Vec& spread, const std::uint32_t gravity_scale,
                   const std::uint32_t intensity, const std::uint32_t size_scale, const GuestAddress shade,
                   const std::uint32_t gp) {
    const auto field = [&](const GuestAddress offset) { return m.read_u32_le(kind + offset); };
    Ranges r{};
    // quarter_spread = ((spread * scale) * 0.5) * 0.5 on xyz.
    Vec scale{};
    scale.words[0] = field(kSpreadScale);
    Vec half{};
    half.words[0] = kHalf;
    const Vec spread_scaled = vmul_lane(spread, scale, 0U);
    const Vec spread_half = vmul_lane(spread_scaled, half, 0U);
    r.quarter_spread = vmul_lane(spread_half, half, 0U);
    // fall = (0, 0 + gravity, 0).
    const std::uint32_t gravity = fpu::mul(field(kGravity), fpu::mul(intensity, gravity_scale));
    Vec fall = vadd(kVf0, kVf0);
    Vec gravity_lane{};
    gravity_lane.words[0] = gravity;
    r.fall = guest::execute_vu0_macro_broadcast_add_subtract(fall, kVf0, gravity_lane, 4U, 0U, false).value;

    // Sizes: base = (x_high, y, z_high); low = (x_low, -y, z_low); range = base - low.
    const std::uint32_t size_x_high = fpu::mul(size_scale, field(kSizeXHigh));
    const std::uint32_t size_x_low = fpu::mul(size_scale, field(kSizeXLow));
    const std::uint32_t size_y = field(kSizeY);
    r.size_base = Vec{{size_x_high, size_y, field(kSizeZHigh), 0U}};
    const Vec size_low{{size_x_low, fpu::neg(size_y), field(kSizeZLow), 0U}};
    r.size_range = vsub(r.size_base, size_low);

    // Directions: high = (sideways, up, sideways) + quarter, low = (-sideways, 0, -sideways) + quarter.
    const std::uint32_t sideways = field(kSideways);
    const Vec high{{sideways, field(kUpward), sideways, 0U}};
    const Vec low{{fpu::neg(sideways), 0U, fpu::neg(sideways), 0U}};
    r.direction_high = vadd(high, r.quarter_spread);
    const Vec direction_low = vadd(low, r.quarter_spread);
    r.direction_range = vsub(r.direction_high, direction_low);

    // Start offset: (offset) .. (0 - offset).
    const std::uint32_t offset = field(kOffset);
    r.offset_base = Vec{{offset, offset, offset, 0U}};
    const Vec offset_low = vsub(kVf0, r.offset_base);
    r.offset_range = vsub(r.offset_base, offset_low);

    // Colour: red/green low = shade * term, high = shade * (term + term); z and w run -1 .. 1.
    const auto term = [&](const unsigned index) {
        return m.read_u32_le(gp + static_cast<std::uint32_t>(kColourGp[index]));
    };
    const std::uint32_t red_low = fpu::mul(m.read_u32_le(shade), term(0U));
    const std::uint32_t red_high = fpu::add(term(1U), red_low);
    const std::uint32_t green_high = fpu::mul(m.read_u32_le(shade + 4U), fpu::add(term(2U), term(3U)));
    const std::uint32_t green_low = fpu::mul(m.read_u32_le(shade + 4U), term(2U));
    const Vec low_colour{{red_low, green_low, 0U, 0U}};
    const Vec high_colour{{red_high, green_high, kOne, kOne}};
    r.colour_base = vsub(vadd(low_colour, low_colour, kXyzw), high_colour, kXyzw);
    r.colour_range = vsub(high_colour, low_colour, kXyzw);
    r.size_limit = m.read_u32_le(gp + static_cast<std::uint32_t>(kSizeLimitGp));
    return r;
}

template <class Context>
n3::Run emit_trail_particles(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress system = call.arg(0);
    const Vec start = load_vec(m, call.arg(1));
    const Vec end = load_vec(m, call.arg(2));
    const Vec spread = load_vec(m, call.arg(3));
    const GuestAddress kind = kEmissions + call.arg(4) * kEmissionBytes;
    const GuestAddress shade = call.arg(5);
    const GuestAddress carry_at = call.arg(6);
    const std::uint64_t spacing_gate = call.arg64(7);
    const GuestAddress entry_sp = call.stack_pointer();
    const GuestAddress frame = entry_sp - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t intensity = cpu.read_fpr_bits(12U);
    const std::uint32_t rate_scale = cpu.read_fpr_bits(13U);
    const std::uint32_t size_scale = cpu.read_fpr_bits(14U);
    const GuestAddress effect = system + kEffects + m.read_u32_le(kind + kEffectIndex) * kEffectBytes;
    const bool both = m.read_u8(entry_sp) != 0U;  // lbu 1440($sp): the flag byte lives in the caller's frame

    // The rate, the carried fraction and the first draw give how many particles each trail gets.
    const std::uint32_t carried = fpu::mul(kCarryIn, fpu::from_int(m.read_u8(carry_at)));
    const std::uint32_t rate = fpu::mul(m.read_u32_le(kind + kRate), fpu::mul(intensity, rate_scale));
    std::uint32_t retired = kEntryCost + kRandomCost;
    const std::uint32_t first_draw = draw(m, system);
    retired += to_float_cost(first_draw) + kWobbleCost;
    const std::uint32_t wobble_unit = fpu::mul(kWordScale, unsigned_to_float(first_draw));
    const std::uint32_t wobble = fpu::madd(fpu::add(0U, kWobbleBase), kWobbleRange, wobble_unit);
    const std::uint32_t amount = fpu::mul(rate, wobble);

    std::int32_t first_count = 0;
    std::int32_t second_count = 0;
    std::uint32_t fraction = 0U;
    if (both) {
        const std::uint32_t first = fpu::mul(fpu::mul(amount, m.read_u32_le(kind + kFirstTrailRate)),
                                             m.read_u32_le(effect + kFirstEffectRate));
        const std::uint32_t second = fpu::mul(fpu::mul(amount, m.read_u32_le(kind + kSecondTrailRate)),
                                              m.read_u32_le(effect + kSecondEffectRate));
        const std::uint32_t first_total = fpu::add(first, carried);
        first_count = fpu::to_int(first_total);
        const std::uint32_t second_total = fpu::add(second, fpu::sub(first_total, fpu::from_int(first_count)));
        second_count = fpu::to_int(second_total);
        fraction = fpu::sub(second_total, fpu::from_int(second_count));
        retired += kBothCountsCost;
    } else {
        const std::uint32_t total = fpu::add(fpu::mul(amount, m.read_u32_le(effect + kFirstEffectRate)), carried);
        first_count = fpu::to_int(total);
        fraction = fpu::sub(total, fpu::from_int(first_count));
        retired += kOneCountCost;
    }
    m.write_u8(carry_at, static_cast<std::uint8_t>(
                             static_cast<std::uint32_t>(fpu::to_int(fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kCarryScaleGp)),
                                                                             fraction)))));
    retired += kCarryStoreCost;
    if (static_cast<std::uint32_t>(first_count) + static_cast<std::uint32_t>(second_count) == 0U) {
        return call.finish(retired + kReturnCost);
    }

    // Counts become unsigned words (the original's packed signed minimum with 8).
    const auto limit_count = [](const std::int32_t count) {
        return static_cast<std::uint32_t>(count < static_cast<std::int32_t>(kMostPerPass)
                                              ? count
                                              : static_cast<std::int32_t>(kMostPerPass));
    };
    const std::uint32_t first_pass_count = limit_count(first_count);
    const std::uint32_t second_pass_count = limit_count(second_count);

    // The second draw seeds R; the frame quads follow from the kind's ranges.
    retired += kSetupCost + kSetupTailCost;
    const std::uint32_t second_draw = draw(m, system);
    retired += to_float_cost(second_draw);
    VuRandom random;
    random.seed(fpu::mul(kWordScale, unsigned_to_float(second_draw)));
    const Ranges ranges = make_ranges(m, kind, spread, m.read_u32_le(system + kGravityScale), intensity,
                                      size_scale, shade, gp);

    std::uint32_t flag = both ? m.read_u8(entry_sp) : 0U;
    for (;;) {
        retired += flag != 0U ? kPassHeadBoth : kPassHeadFirst;
        const std::uint32_t count = flag != 0U ? second_pass_count : first_pass_count;
        std::uint64_t remaining = count;
        if (remaining == 0U) {
            retired += kEmptyPassCost;
        } else {
            // One pass: spread `remaining` particles from start to end.
            retired += kPassSetupCost + kPassSetupTailCost;
            const std::uint32_t inverse = fpu::div(kOne, fpu::from_int(static_cast<std::int32_t>(remaining)));
            const std::uint32_t pass_draw = draw(m, system);
            retired += to_float_cost(pass_draw);
            Vec inverse_lane{};
            inverse_lane.words[0] = inverse;
            const Vec step = vmul_lane(vsub(end, start), inverse_lane, 0U);
            const std::uint32_t along = fpu::mul(kWordScale, unsigned_to_float(pass_draw));
            Vec along_lane{};
            along_lane.words[0] = along;
            const std::uint32_t time_step = m.read_u32_le(system + kClockStep);
            const std::uint32_t age_step = fpu::mul(time_step, inverse);
            std::uint32_t age = fpu::sub(m.read_u32_le(system + kClock), time_step);
            Vec at = vadd(start, vmul_lane(step, along_lane, 0U));

            for (;;) {
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
                retired += kParticleCost;
                Vec jitter = load_vec(m, kJitter);
                const Vec grid = load_vec(m, kJitterGrid);
                const Vec size = lerp(ranges.size_base, ranges.size_range, jittered(jitter, grid, random));
                jitter = rotate(jitter);
                n3::store_quad_any(m, kJitter, jitter);
                bool accepted = true;
                if (!fpu::le(size.words[0], ranges.size_limit)) {
                    // A particle worth keeping: colour, start offset, heading and direction.
                    Vec draws{};
                    draws.words[0] = random.next();
                    draws.words[1] = random.next();
                    draws.words[2] = random.next();
                    draws.words[3] = random.next();
                    const Vec colour = lerp(ranges.colour_base, ranges.colour_range, draws, kXyzw);
                    const Vec shift = lerp(ranges.offset_base, ranges.offset_range, jittered(jitter, grid, random));
                    jitter = rotate(jitter);
                    const Vec heading =
                        lerp(ranges.direction_high, ranges.direction_range, jittered(jitter, grid, random));
                    jitter = rotate(jitter);
                    n3::store_quad_any(m, kJitter, jitter);

                    Vec half{};
                    half.words[0] = kHalf;
                    Vec direction = vadd(heading, vmul_lane(ranges.quarter_spread, colour, 2U));
                    direction = vadd(direction, vmul_lane(ranges.fall, colour, 3U));
                    direction = vsub(direction, vmul_lane(shift, half, 0U));
                    Vec position = vadd(at, shift);
                    // The w lanes AppendSampleRecord copies into the record. The original builds both quads with
                    // .xyz operations only, so each keeps the w of the register it was last loaded into: the
                    // position the colour range's (lq 1264($sp), 1.0 here), the direction the start-offset range's
                    // (sp+1360, whose w is the stale frame word at 652($sp): 640..648 are the only words written).
                    position.words[3] = ranges.colour_range.words[3];
                    direction.words[3] = m.read_u32_le(frame + kStaleOffsetW);

                    // Colour for the sample: y clamped to 1, x boosted as far as keeps the sum within 1.
                    std::uint32_t base_colour = colour.words[1];
                    std::uint32_t boost = 0U;
                    if (fpu::lt(base_colour, kOne)) {
                        retired += kColourBelowOne;
                        boost = colour.words[0];
                        if (!fpu::le(fpu::add(base_colour, boost), kOne)) {
                            boost = fpu::sub(kOne, base_colour);
                            retired += kColourClamped;
                        }
                    } else {
                        retired += kColourOne;
                        base_colour = kOne;
                    }
                    retired += kEmitCost + kCallCost;

                    n3::store_quad_any(m, frame + kPositionSlot, position);
                    n3::store_quad_any(m, frame + kDirectionSlot, direction);
                    call.set_arg(0U, effect);
                    call.set_arg(1U, frame + kPositionSlot);
                    call.set_arg(2U, frame + kDirectionSlot);
                    call.set_arg64(3U, spacing_gate);
                    call.set_arg(4U, flag);
                    call.set_float_arg(0U, age);
                    call.set_float_arg(1U, boost);
                    call.set_float_arg(2U, base_colour);
                    call.set_float_arg(3U, size.words[2]);
                    call.set_float_arg(4U, size.words[0]);
                    call.set_float_arg(5U, size.words[1]);
                    const n3::Run appended = call.invoke([](auto& machine) { return n3::fn_00254a90(machine); },
                                                         kFrameBytes);
                    if (appended.declined) {
                        return call.decline();
                    }
                    retired += appended.retired;
                    accepted = call.result64() != 0U;
                }
                if (!accepted) {
                    retired += kEmptyPassCost;
                    break;
                }
                retired += kSkipCost;
                age = fpu::add(age, age_step);
                at = vadd(at, step);
                remaining = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(remaining) - 1U)));
                if (remaining == 0U) {
                    retired += kPassEndCost;
                    break;
                }
            }
        }
        retired += kJoinCost;
        if (flag == 0U) {
            break;
        }
        retired += kNextPassCost;
        m.write_u8(entry_sp, 0U);
        flag = 0U;
    }
    call.set_vector_random(random.bits);
    return call.finish(retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025f0c0, 2180U, 0x1353b62b6b01a86aULL, "EmitTrailParticles", kCeiling, emit_trail_particles,
         EngineReturn::Nothing)
