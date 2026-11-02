// 0x0025ea70 SpawnParticleBurst (a0 = effect system, a1 = start quad, a2 = end quad, a3 = spread quad, t0 = emission
// kind, t1 = float pair that shades the sample, t2 = optional carry byte (0 = none), t3 = spacing gate for
// AppendSampleRecord, the byte at the entry $sp = "feed both trails" flag, f12 = intensity, f13 = size scale).
// Returns nothing. The one-shot sibling of EmitTrailParticles (0x0025f0c0).
//
// An emission kind is a 56-byte record at 0x004bc040 (effect index, ranges, per-trail rates). The effect (336 bytes
// at system + 368) has two rates, at +264 and +308. How many particles are made follows the rate scaled by the
// intensity and a random wobble; the fraction left over is kept in the carry byte (scaled by 1/256 on the way in,
// by a global on the way out). With the flag set both rates count (the second count is added to the first), without
// it only the first rate does.
//
// Each particle draws a speed vector from the VU0 R generator (seeded from the effect system's random pair), every
// draw jittered by the quad at 0x01d9d190 that is rotated one lane after use. A particle whose speed.x is within
// the limit is skipped; the others draw a heading, a start offset and a spread, and are appended to the effect by
// AppendSampleRecord (0x00254a90), which reads the six floats f12..f17 and two frame quads.
//
// The vector arithmetic runs through the VU0 macro FMAC kernels on raw lanes (flags are not tracked; nothing reads
// them). The stack copies of the original only feed xyz lanes of those operations, except for one stale frame word
// that reaches the w lane of the position quad AppendSampleRecord reads; that word is read from the frame.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00254a90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Vec = guest::Gpr128;

constexpr GuestAddress kEmissions = 0x004bc040U;       // 56-byte emission kinds
constexpr std::uint32_t kEmissionBytes = 56U;
constexpr GuestAddress kEffects = 368U;                // effect e at system + 368 + 336 * e
constexpr std::uint32_t kEffectBytes = 336U;
constexpr GuestAddress kRandomPair = 0x0001f828U;      // the effect system's two random words
constexpr GuestAddress kClock = 0x0001f848U;           // float: effect time
constexpr GuestAddress kJitter = 0x01d9d190U;          // quad rotated a lane after every use
constexpr GuestAddress kJitterGrid = 0x004bbfb0U;      // quad: the grid the jitter residue is taken against
// gp-relative globals.
constexpr std::int32_t kSizeLimitGp = -29952;
constexpr std::int32_t kCarryScaleGp = -29604;
constexpr std::int32_t kTiltGp = -29936;

// Emission kind fields.
constexpr GuestAddress kEffectIndex = 0U;
constexpr GuestAddress kSpreadScale = 12U;
constexpr GuestAddress kSideways = 16U;
constexpr GuestAddress kUpward = 20U;
constexpr GuestAddress kSizeY = 24U;
constexpr GuestAddress kSizeZLow = 32U;
constexpr GuestAddress kSizeZHigh = 36U;
constexpr GuestAddress kSizeXLow = 40U;
constexpr GuestAddress kSizeXHigh = 44U;
constexpr GuestAddress kFirstTrailRate = 48U;
constexpr GuestAddress kSecondTrailRate = 52U;
constexpr GuestAddress kFirstEffectRate = 264U;        // in the effect: rate of the first trail
constexpr GuestAddress kSecondEffectRate = 308U;

constexpr std::uint32_t kFrameBytes = 992U;
constexpr GuestAddress kSampleSlot = 960U;             // frame quads AppendSampleRecord reads through a1, a2
constexpr GuestAddress kPositionSlot = 768U;
constexpr GuestAddress kStaleWord = 252U;              // never written by the original: whatever the frame held

// Float constants (raw bits).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kThreeQuarters = 0x3f400000U;
constexpr std::uint32_t kCarryIn = 0x3b800000U;        // 1/256
constexpr std::uint32_t kWordScale = 0x2f800000U;      // 2^-32
constexpr std::uint32_t kWobbleBase = 0x3f600000U;     // 0.875
constexpr std::uint32_t kWobbleRange = 0x3e800000U;    // 0.25

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 36U;              // 0x25ea70..0x25eafc
constexpr std::uint32_t kNoCarryCost = 4U;             // 0x25f080..0x25f08c
constexpr std::uint32_t kCarryReadCost = 11U;          // lbu, conversion of a byte, 1/256 scale
constexpr std::uint32_t kToFloatPositive = 3U;         // unsigned word to float, bit 31 clear
constexpr std::uint32_t kToFloatNegative = 7U;         // ... bit 31 set
constexpr std::uint32_t kFirstDrawCost = 35U;          // 0x25eb48..0x25ebd0
constexpr std::uint32_t kWobbleCost = 11U;             // 0x25ebfc..0x25ec24
constexpr std::uint32_t kBothCountsCost = 19U;         // 0x25ec28..0x25ec70
constexpr std::uint32_t kOneCountCost = 8U;            // 0x25ec74..0x25ec90
constexpr std::uint32_t kCarryTestCost = 2U;           // 0x25ec94
constexpr std::uint32_t kCarryStoreCost = 6U;          // 0x25ec9c..0x25ecb0
constexpr std::uint32_t kSecondDrawCost = 36U;         // 0x25ecb4..0x25ed40
constexpr std::uint32_t kSetupCost = 79U;              // 0x25ed6c..0x25eea4
constexpr std::uint32_t kTestCost = 3U;                // 0x25f06c..0x25f074: the loop test
constexpr std::uint32_t kSpeedCost = 32U;              // 0x25eea8..0x25ef24: speed vector, jitter store, accept test
constexpr std::uint32_t kEmitCost = 81U;               // 0x25ef28..0x25f068: the rest of the particle and the call
constexpr std::uint32_t kExitCost = 14U;               // 0x25f078..0x25f0bc after the last test

constexpr std::uint32_t kCeiling = 20000U;

// --- The effect system's random pair ------------------------------------------------------------------------

// One draw: first = (first << 16) + (first >> 16 arithmetic); first += second; second += first.
// Returns the new first word.
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

// --- VU0 FMAC operations on raw lanes -----------------------------------------------------------------------

constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr Vec kVf0 = n3::kVf0;

// dest.xyz = left - right (VSUB.xyz): lanes outside the mask stay those of `dest`.
Vec vsub(const Vec& dest, const Vec& left, const Vec& right) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, left, right, kXyz, true).value;
}
Vec vadd(const Vec& dest, const Vec& left, const Vec& right) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, left, right, kXyz, false).value;
}
Vec vmul(const Vec& l, const Vec& r) {
    return guest::execute_vu0_macro_vector_multiply(l, l, r, kXyz).value;
}
// ACC = base + vf0.x (VADDAx with VF0): the accumulator the following VMADD starts from.
Vec accumulate_base(const Vec& base) {
    return guest::execute_vu0_macro_broadcast_add_subtract(base, base, kVf0, kXyz, 0U, false).value;
}
// base + range * weight on xyz (VADDAx, then VMADD); the w lane is the weight's.
Vec lerp(const Vec& base, const Vec& range, const Vec& weight) {
    const Vec acc = accumulate_base(base);
    return guest::execute_vu0_macro_vector_multiply_accumulate(weight, acc, range, weight, kXyz, false).value;
}
// VMSUBAw / VMSUBw against VF0.w = 1.0: acc - left * 1.0.
Vec msub_one(const Vec& acc, const Vec& left) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(left, acc, left, kVf0, kXyz, 3U, true).value;
}

// The rounding residue of jitter * random on the grid, (jitter*r + grid - grid) - jitter*r, with three draws
// from R.
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

// The same residue for the x lane alone, with one draw (VMULx.x, VADDAx.x, VMSUBAw.x, VMSUBw.x).
std::uint32_t jittered_x(const Vec& jitter, const Vec& grid, VuRandom& random) {
    Vec draws{};
    draws.words[0] = random.next();
    const Vec scaled = guest::execute_vu0_macro_broadcast_multiply(draws, jitter, draws, kX, 0U).value;
    const Vec acc = guest::execute_vu0_macro_broadcast_add_subtract(scaled, grid, scaled, kX, 0U, false).value;
    const Vec acc2 = guest::execute_vu0_macro_broadcast_multiply_accumulate(grid, acc, grid, kVf0, kX, 3U, true)
                         .value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(scaled, acc2, scaled, kVf0, kX, 3U, true)
        .value.words[0];
}

Vec rotate(const Vec& v) { return Vec{{v.words[1], v.words[2], v.words[3], v.words[0]}}; }  // vmr32

template <class Memory>
Vec load_vec(const Memory& m, const GuestAddress address) { return n3::load_quad_any(m, address); }

// What the setup (0x25ed6c..0x25eea4) leaves in the frame for the particle loop. Each pair is a base quad and the
// range that a jittered random residue scales (base + range * residue).
struct Ranges {
    Vec speed_base;       // sp+832: (size.x high, y, size.z high), the x scaled by f13
    Vec speed_range;      // sp+848: base - (size.x low, -y, size.z low)
    Vec heading_base;     // sp+800: (carried second float, 1, tilt)
    Vec heading_range;    // sp+816: base - (half the second float, 0, 3/4 spread scale); w = the stale frame word
    Vec offset_base;      // sp+896: (sideways, up, sideways)
    Vec offset_range;     // sp+912: base - (-sideways, 0, -sideways)
    Vec path_base;        // sp+864: the third input quad
    Vec path_range;       // sp+880: third - second quad
    std::uint32_t speed_limit{};
};

template <class Memory>
Ranges make_ranges(Memory& m, const GuestAddress kind, const Vec& second, const Vec& third, const std::uint32_t scale,
                   const std::uint32_t pair_b, const std::uint32_t stale, const std::uint32_t gp) {
    const auto field = [&](const GuestAddress offset) { return m.read_u32_le(kind + offset); };
    Ranges r{};
    const std::uint32_t size_y = field(kSizeY);
    const std::uint32_t sideways = field(kSideways);
    const std::uint32_t spread_scale = field(kSpreadScale);

    r.speed_base = Vec{{fpu::mul(scale, field(kSizeXHigh)), size_y, field(kSizeZHigh), 0U}};
    const Vec speed_low{{fpu::mul(scale, field(kSizeXLow)), fpu::neg(size_y), field(kSizeZLow), 0U}};
    r.speed_range = vsub(r.speed_base, r.speed_base, speed_low);

    r.path_base = third;
    r.path_range = vsub(second, third, second);

    r.offset_base = Vec{{sideways, field(kUpward), sideways, 0U}};
    const Vec offset_low{{fpu::neg(sideways), 0U, fpu::neg(sideways), 0U}};
    r.offset_range = vsub(r.offset_base, r.offset_base, offset_low);

    const std::uint32_t tilt = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kTiltGp)), spread_scale);
    r.heading_base = Vec{{pair_b, kOne, tilt, 0U}};
    const Vec heading_low{{fpu::mul(kHalf, pair_b), 0U, fpu::mul(kThreeQuarters, spread_scale), stale}};
    r.heading_range = vsub(heading_low, r.heading_base, heading_low);

    r.speed_limit = m.read_u32_le(gp + static_cast<std::uint32_t>(kSizeLimitGp));
    return r;
}

template <class Context>
n3::Run spawn_particle_burst(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress system = call.arg(0);
    const Vec first_quad = load_vec(m, call.arg(1));   // handed on to AppendSampleRecord through a1
    const Vec second_quad = load_vec(m, call.arg(2));
    const Vec third_quad = load_vec(m, call.arg(3));
    const GuestAddress kind = kEmissions + call.arg(4) * kEmissionBytes;
    const GuestAddress pair = call.arg(5);
    const bool has_carry = call.arg64(6) != 0U;
    const GuestAddress carry_at = call.arg(6);
    const std::uint64_t spacing_gate = call.arg64(7);
    const GuestAddress entry_sp = call.stack_pointer();
    const GuestAddress frame = entry_sp - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t intensity = cpu.read_fpr_bits(12U);
    const std::uint32_t scale = cpu.read_fpr_bits(13U);
    const GuestAddress effect = system + kEffects + m.read_u32_le(kind + kEffectIndex) * kEffectBytes;
    const bool both = m.read_u8(entry_sp) != 0U;  // lbu 992($sp): the flag byte lives in the caller's frame

    std::uint32_t retired = kEntryCost;
    // The carried fraction of the last call as a float (1/256 per unit); one half when there is none.
    std::uint32_t carried = kHalf;
    if (has_carry) {
        carried = fpu::mul(kCarryIn, fpu::from_int(m.read_u8(carry_at)));
        retired += kCarryReadCost;
    } else {
        retired += kNoCarryCost;
    }

    // The first draw wobbles the intensity.
    const std::uint32_t first_draw = draw(m, system);
    retired += kFirstDrawCost + to_float_cost(first_draw) + kWobbleCost;
    const std::uint32_t wobble_unit = fpu::mul(kWordScale, unsigned_to_float(first_draw));
    const std::uint32_t wobble = fpu::madd(fpu::add(0U, kWobbleBase), kWobbleRange, wobble_unit);
    const std::uint32_t amount = fpu::mul(intensity, wobble);

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
    retired += kCarryTestCost;
    if (has_carry) {
        const std::uint32_t scaled = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kCarryScaleGp)), fraction);
        m.write_u8(carry_at, static_cast<std::uint8_t>(static_cast<std::uint32_t>(fpu::to_int(scaled))));
        retired += kCarryStoreCost;
    }

    // The second draw seeds R; the ranges follow from the kind. Frame word 252 is never written by the original.
    const std::uint32_t second_draw = draw(m, system);
    retired += kSecondDrawCost + to_float_cost(second_draw) + kSetupCost;
    VuRandom random;
    random.seed(fpu::mul(kWordScale, unsigned_to_float(second_draw)));
    const std::uint32_t stale = m.read_u32_le(frame + kStaleWord);
    const Ranges ranges = make_ranges(m, kind, second_quad, third_quad, scale, m.read_u32_le(pair + 4U), stale, gp);

    // Particles: the first trail's count plus the second's, counted down; the callee's gate argument says whether
    // this was not the last one.
    std::int32_t remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(first_count) +
                                                       static_cast<std::uint32_t>(second_count));
    for (;;) {
        retired += kTestCost;
        if (remaining <= 0) {
            break;
        }
        --remaining;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kSpeedCost;
        const Vec jitter = load_vec(m, kJitter);
        const Vec grid = load_vec(m, kJitterGrid);
        const Vec speed = lerp(ranges.speed_base, ranges.speed_range, jittered(jitter, grid, random));
        const Vec jitter1 = rotate(jitter);
        n3::store_quad_any(m, kJitter, jitter1);
        if (fpu::le(speed.words[0], ranges.speed_limit)) {
            continue;
        }

        // A particle worth keeping: heading, start offset and spread, each jittered by the next lane rotation.
        retired += kEmitCost;
        const Vec heading = lerp(ranges.heading_base, ranges.heading_range, jittered(jitter1, grid, random));
        const Vec jitter2 = rotate(jitter1);
        Vec shifted = ranges.heading_range;  // vf1 after VMULx.x: lanes y, z, w of sp+816, x the residue
        shifted.words[0] = jittered_x(jitter2, grid, random);
        // B0 = path_base + path_range * x (w from `shifted`), C0 = B0 * heading.z
        const Vec acc = accumulate_base(ranges.path_base);
        const Vec path = guest::execute_vu0_macro_broadcast_multiply_accumulate(shifted, acc, ranges.path_range,
                                                                                 shifted, kXyz, 0U, false)
                             .value;
        const Vec along = guest::execute_vu0_macro_broadcast_multiply(path, path, heading, kXyz, 2U).value;
        const Vec jitter3 = rotate(jitter2);
        const Vec offset = lerp(ranges.offset_base, ranges.offset_range, jittered(jitter3, grid, random));
        n3::store_quad_any(m, kJitter, rotate(jitter3));

        const std::uint32_t pair_a = m.read_u32_le(pair);
        const std::uint32_t pair_b = m.read_u32_le(pair + 4U);
        const Vec position = vadd(along, along, offset);
        // f13 = pair_a + heading.y * (pair_b - heading.x), formed on the vector unit's y lane.
        const std::uint32_t gap = fpu::sub(pair_b, heading.words[0]);
        Vec gap_vec{};
        gap_vec.words[0] = gap;
        const Vec scaled_y = guest::execute_vu0_macro_broadcast_multiply(position, heading, gap_vec, kY, 0U).value;
        Vec pair_vec{};
        pair_vec.words[0] = pair_a;
        const Vec lateral = guest::execute_vu0_macro_broadcast_add_subtract(pair_vec, scaled_y, pair_vec, kY, 0U, false)
                                .value;

        n3::store_quad_any(m, frame + kSampleSlot, first_quad);
        n3::store_quad_any(m, frame + kPositionSlot, position);
        call.set_arg(0U, effect);
        call.set_arg(1U, frame + kSampleSlot);
        call.set_arg(2U, frame + kPositionSlot);
        call.set_arg64(3U, spacing_gate);
        call.set_arg(4U, static_cast<std::int32_t>(first_count) < remaining ? 1U : 0U);
        call.set_float_arg(0U, m.read_u32_le(system + kClock));
        call.set_float_arg(1U, lateral.words[1]);
        call.set_float_arg(2U, heading.words[0]);
        call.set_float_arg(3U, speed.words[2]);
        call.set_float_arg(4U, speed.words[0]);
        call.set_float_arg(5U, speed.words[1]);
        const n3::Run appended = call.invoke([](auto& machine) { return n3::fn_00254a90(machine); }, kFrameBytes);
        if (appended.declined) {
            return call.decline();
        }
        retired += appended.retired;
    }
    retired += kExitCost;
    call.set_vector_random(random.bits);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025ea70, 1616U, 0x531f28073010237eULL, "SpawnParticleBurst", kCeiling, spawn_particle_burst,
         EngineReturn::Nothing)
