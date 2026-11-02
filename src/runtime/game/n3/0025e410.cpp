// 0x0025e410 EmitTrailParticlesOne (a0 = effect system, a1 = start quad, a2 = end quad, a3 = spread quad,
// t0 = emission kind, t1 = float pair that shades the colours, t2 = carry byte (0 = none), t3 = spacing gate for
// AppendSampleRecord, the byte at the entry $sp = "feed both trails" flag, f12 = intensity, f13 = size scale).
// Returns nothing.
//
// Single-trail twin of EmitTrailParticles (0x0025f0c0). The emission kind is a 56-byte record at 0x004bc040; the
// effect (336 bytes at system + 368) has two trails whose rates are at +264 and +308. How many particles are made
// follows rate * intensity * a random wobble; the fraction left over is kept in the carry byte (1/256 on the way in,
// a global scale on the way out). With the flag set the two trails' counts are added (the second one's rate comes
// from the kind's second trail rate), otherwise only the first trail counts.
//
// Unlike the pass-based twin there is one loop over all the particles: each draws a length from the VU0 R generator
// (start + range * random); one that is not longer than the limit is skipped. The others get a jittered direction
// and a colour from R and the shared jitter quad at 0x01d9d190 (rotated one lane after every use) and are appended
// to the effect by AppendSampleRecord (0x00254a90).
//
// The vector arithmetic runs through the VU0 macro FMAC kernels on raw lanes. The stack copies of the original only
// feed xyz lanes of those operations, or w lanes nothing reads (AppendSampleRecord reads xyz of its two quads), so
// lanes that are dead are left zero.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00254a90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Vec = guest::Gpr128;

constexpr GuestAddress kEmissions = 0x004bc040U;   // 56-byte emission kinds
constexpr std::uint32_t kEmissionBytes = 56U;
constexpr GuestAddress kEffects = 368U;            // effect e at system + 368 + 336 * e
constexpr std::uint32_t kEffectBytes = 336U;
constexpr GuestAddress kRandomPair = 0x0001f828U;  // system + ...: the effect system's two random words
constexpr GuestAddress kRate = 0x0001f848U;        // float: rate handed to the sample record
constexpr GuestAddress kJitter = 0x01d9d190U;      // quad rotated a lane after every use
constexpr GuestAddress kJitterGrid = 0x004bbfb0U;  // quad: the grid the jitter residue is taken against
constexpr std::int32_t kSizeLimitGp = -29952;      // gp-relative: shortest length worth a particle
constexpr std::int32_t kCarryScaleGp = -29604;     // gp-relative: carry out scale

// Emission kind fields.
constexpr GuestAddress kEffectIndex = 0U;
constexpr GuestAddress kSpreadScale = 12U;
constexpr GuestAddress kSideways = 16U;
constexpr GuestAddress kUpward = 20U;
constexpr GuestAddress kColourLow = 24U;
constexpr GuestAddress kColourGreen = 32U;
constexpr GuestAddress kColourBlue = 36U;
constexpr GuestAddress kLengthLow = 40U;
constexpr GuestAddress kLengthHigh = 44U;
constexpr GuestAddress kFirstTrailRate = 48U;
constexpr GuestAddress kSecondTrailRate = 52U;
constexpr GuestAddress kFirstEffectRate = 264U;  // in the effect: rate of the first trail
constexpr GuestAddress kSecondEffectRate = 308U;

constexpr std::uint32_t kFrameBytes = 944U;
constexpr GuestAddress kStartSlot = 912U;      // the frame quads AppendSampleRecord reads through a1, a2
constexpr GuestAddress kDirectionSlot = 752U;

// Float constants (raw bits).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kCarryIn = 0x3b800000U;    // 1/256
constexpr std::uint32_t kWordScale = 0x2f800000U;  // 2^-32
constexpr std::uint32_t kWobbleBase = 0x3f600000U; // 0.875
constexpr std::uint32_t kWobbleRange = 0x3e800000U;// 0.25

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 37U;          // 0x25e410..0x25e4a0: frame, quads, record, effect
constexpr std::uint32_t kNoCarryCost = 4U;         // 0x25ea20: carry in = 0.5
constexpr std::uint32_t kCarryInCost = 11U;        // 0x25e4a4..0x25e4e8: lbu and 1/256
constexpr std::uint32_t kFirstDrawCost = 35U;      // 0x25e4ec..0x25e574: the first random step
constexpr std::uint32_t kToFloatPositive = 3U;     // unsigned word to float, bit 31 clear
constexpr std::uint32_t kToFloatNegative = 7U;     // ... bit 31 set
constexpr std::uint32_t kWobbleCost = 11U;         // 0x25e5a0..0x25e5c8
constexpr std::uint32_t kBothCountsCost = 19U;     // 0x25e5cc..0x25e614
constexpr std::uint32_t kOneCountCost = 8U;        // 0x25e618..0x25e634
constexpr std::uint32_t kCarryJoinCost = 2U;       // 0x25e638
constexpr std::uint32_t kCarryStoreCost = 6U;      // 0x25e640..0x25e654 (with a carry byte)
constexpr std::uint32_t kSecondDrawCost = 36U;     // 0x25e658..0x25e6e4: the second random step
constexpr std::uint32_t kSetupCost = 75U;          // 0x25e710..0x25e838: the vector setup
constexpr std::uint32_t kHeadCost = 3U;            // 0x25ea0c: loop head (count test, decrement)
constexpr std::uint32_t kLengthCost = 12U;         // 0x25e83c..0x25e868: the length test
constexpr std::uint32_t kDirectionCost = 38U;      // 0x25e86c..0x25e900
constexpr std::uint32_t kFlipCost = 3U;            // 0x25ea30: the direction is negated
constexpr std::uint32_t kEmitCost = 66U;           // 0x25e904..0x25ea08, jal and delay slot included
constexpr std::uint32_t kExitCost = 2U + 13U;      // 0x25ea18 and the epilogue 0x25ea3c..0x25ea6c

constexpr std::uint32_t kCeiling = 20000U;

constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kXyzw = n3::kMaskXyzw;
constexpr Vec kVf0 = n3::kVf0;

// --- The effect system's random pair ------------------------------------------------------------------------

// One step: first = (first << 16) + (first >> 16 arithmetic); first += second; second += first. Returns first.
template <class Memory>
std::uint32_t step_random(Memory& m, const GuestAddress system) {
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
Vec msub_one(const Vec& acc, const Vec& left, const std::uint8_t mask) {
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(left, acc, left, kVf0, mask, 3U, true).value;
}

Vec lane_x(const std::uint32_t bits) { return Vec{{bits, 0U, 0U, 0U}}; }

// The rounding residue of jitter * random on the grid, (jitter*r + grid - grid) - jitter*r, with three draws from R.
Vec jittered(const Vec& jitter, const Vec& grid, VuRandom& random) {
    Vec draws{};
    draws.words[0] = random.next();
    draws.words[1] = random.next();
    draws.words[2] = random.next();
    const Vec scaled = vmul(jitter, draws);
    const Vec acc = vadd(scaled, grid);
    return msub_one(msub_one(acc, grid, kXyz), scaled, kXyz);
}

// The same on the x lane alone with one draw.
std::uint32_t jittered_x(const Vec& jitter, const Vec& grid, const std::uint32_t draw) {
    const Vec scaled = vmul_lane(jitter, lane_x(draw), 0U, kMaskX);
    const Vec acc = guest::execute_vu0_macro_broadcast_add_subtract(grid, grid, scaled, kMaskX, 0U, false).value;
    return msub_one(msub_one(acc, grid, kMaskX), scaled, kMaskX).words[0];
}

Vec rotate(const Vec& v) { return Vec{{v.words[1], v.words[2], v.words[3], v.words[0]}}; }  // vmr32

// 0 + bits on the x lane through the VU adder (VADDAx/VADDx against VF0.x = 0).
std::uint32_t zero_plus(const std::uint32_t bits) {
    return guest::execute_vu0_macro_broadcast_add_subtract(kVf0, kVf0, lane_x(bits), kMaskX, 0U, false).value.words[0];
}

template <class Context>
n3::Run emit_trail_particles_one(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress system = call.arg(0);
    const Vec start = n3::load_quad_any(m, call.arg(1));
    const Vec end = n3::load_quad_any(m, call.arg(2));
    const Vec spread = n3::load_quad_any(m, call.arg(3));
    const GuestAddress kind = kEmissions + call.arg(4) * kEmissionBytes;
    const GuestAddress pair = call.arg(5);  // float pair that shades the colours
    const GuestAddress carry_at = call.arg(6);
    const bool has_carry = cpu.read_gpr_u64(10U) != 0U;
    const std::uint64_t spacing_gate = call.arg64(7);
    const GuestAddress entry_sp = call.stack_pointer();
    const GuestAddress frame = entry_sp - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t intensity = cpu.read_fpr_bits(12U);
    const std::uint32_t size_scale = cpu.read_fpr_bits(13U);
    const auto field = [&](const GuestAddress offset) { return m.read_u32_le(kind + offset); };
    const GuestAddress effect = system + kEffects + field(kEffectIndex) * kEffectBytes;
    const bool both = m.read_u8(entry_sp) != 0U;  // lbu 944($sp): the flag byte lives in the caller's frame

    std::uint32_t retired = kEntryCost;

    // The carried fraction and the first draw give how many particles are made.
    std::uint32_t carried = kHalf;
    if (has_carry) {
        carried = fpu::mul(kCarryIn, fpu::from_int(m.read_u8(carry_at)));
        retired += kCarryInCost;
    } else {
        retired += kNoCarryCost;
    }
    const std::uint32_t first_draw = step_random(m, system);
    retired += kFirstDrawCost + to_float_cost(first_draw) + kWobbleCost;
    const std::uint32_t wobble_unit = fpu::mul(kWordScale, unsigned_to_float(first_draw));
    const std::uint32_t wobble = fpu::madd(fpu::add(0U, kWobbleBase), kWobbleRange, wobble_unit);
    const std::uint32_t amount = fpu::mul(intensity, wobble);

    std::int32_t first_count = 0;
    std::int32_t second_count = 0;
    std::uint32_t fraction = 0U;
    if (both) {
        const std::uint32_t first =
            fpu::mul(fpu::mul(amount, field(kFirstTrailRate)), m.read_u32_le(effect + kFirstEffectRate));
        const std::uint32_t second =
            fpu::mul(fpu::mul(amount, field(kSecondTrailRate)), m.read_u32_le(effect + kSecondEffectRate));
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
    retired += kCarryJoinCost;
    if (has_carry) {
        const std::uint32_t scaled = fpu::mul(m.read_u32_le(gp + static_cast<std::uint32_t>(kCarryScaleGp)), fraction);
        m.write_u8(carry_at, static_cast<std::uint8_t>(static_cast<std::uint32_t>(fpu::to_int(scaled))));
        retired += kCarryStoreCost;
    }

    // The second draw seeds R.
    const std::uint32_t second_draw = step_random(m, system);
    retired += kSecondDrawCost + to_float_cost(second_draw) + kSetupCost;
    VuRandom random;
    random.seed(fpu::mul(kWordScale, unsigned_to_float(second_draw)));

    // --- Vector setup (0x25e728..0x25e838) -----------------------------------------------------------------
    // Direction range: (sideways, upward, sideways) and the same doubled.
    const std::uint32_t sideways = field(kSideways);
    const Vec direction{{sideways, field(kUpward), sideways, 0U}};
    const Vec direction_twice = vsub(direction, vsub(kVf0, direction));
    // Spread: spread * scale, and the half that is left of it after taking half away.
    const Vec spread_scaled = vmul_lane(spread, lane_x(field(kSpreadScale)), 0U);
    const Vec spread_half = vsub(spread_scaled, vmul_lane(spread_scaled, lane_x(kHalf), 0U));
    // Length of a particle: offset + span * random, tested against the limit.
    const std::uint32_t length_low = fpu::mul(size_scale, field(kLengthLow));
    const std::uint32_t length_high = fpu::mul(size_scale, field(kLengthHigh));
    const std::uint32_t length_span = zero_plus(fpu::sub(length_high, length_low));
    const std::uint32_t length_offset = zero_plus(fpu::sub(fpu::add(length_low, length_low), length_high));
    const Vec length_start = lane_x(zero_plus(length_offset));
    const Vec length_range = lane_x(length_span);
    const std::uint32_t length_limit = m.read_u32_le(gp + static_cast<std::uint32_t>(kSizeLimitGp));
    // Colour of a particle: low .. high, base = 2 * low - high, range = high - low.
    const std::uint32_t colour_a = field(kColourLow);
    const std::uint32_t pair_y_setup = m.read_u32_le(pair + 4U);
    const Vec colour_low{{fpu::neg(colour_a), field(kColourGreen), fpu::mul(kHalf, pair_y_setup), 0U}};
    const Vec colour_high{{colour_a, field(kColourBlue), pair_y_setup, kOne}};
    const Vec colour_base = vsub(vadd(colour_low, colour_low, kXyzw), colour_high, kXyzw);
    const Vec colour_range = vsub(colour_high, colour_low, kXyzw);
    const Vec grid = n3::load_quad_any(m, kJitterGrid);

    // Passes: second count + first count (32-bit); a negative count is far above kCeiling.
    std::int32_t remaining = static_cast<std::int32_t>(static_cast<std::uint32_t>(second_count) +
                                                       static_cast<std::uint32_t>(first_count));
    for (;;) {
        retired += kHeadCost;
        if (remaining == 0) {
            break;
        }
        --remaining;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kLengthCost;
        const std::uint32_t length_random = random.next();
        const std::uint32_t length =
            guest::execute_vu0_macro_broadcast_multiply_accumulate(lane_x(length_random), length_start, length_range,
                                                                   lane_x(length_random), kMaskX, 0U, false)
                .value.words[0];
        if (fpu::le(length, length_limit)) {
            continue;  // not long enough to be worth a particle
        }

        // Direction: the jitter quad's residue on the grid (rotated one lane after use), flipped to face the end.
        retired += kDirectionCost;
        const Vec jitter = n3::load_quad_any(m, kJitter);
        Vec heading = lerp(direction, direction_twice, jittered(jitter, grid, random));
        const Vec jitter_next = rotate(jitter);
        n3::store_quad_any(m, kJitter, jitter_next);
        const Vec along_end = vmul(heading, end);
        const Vec dot_acc =
            guest::execute_vu0_macro_broadcast_add_subtract(along_end, along_end, along_end, kMaskX, 1U, false).value;
        const std::uint32_t dot = guest::execute_vu0_macro_broadcast_multiply_accumulate(
                                      lane_x(kOne), dot_acc, lane_x(kOne), along_end, kMaskX, 2U, false)
                                      .value.words[0];
        if (fpu::lt(dot, 0U)) {
            retired += kFlipCost;
            heading = vsub(kVf0, heading);
        }

        // Heading plus a spread offset, and the colour (all four lanes random).
        retired += kEmitCost;
        const std::uint32_t residue_x = jittered_x(jitter_next, grid, random.next());
        const Vec spread_offset = guest::execute_vu0_macro_broadcast_multiply_accumulate(
                                      spread_scaled, accumulate_base(spread_scaled, kXyz), spread_half,
                                      lane_x(residue_x), kXyz, 0U, false)
                                      .value;
        n3::store_quad_any(m, kJitter, rotate(jitter_next));
        const Vec direction_out = vadd(heading, spread_offset);
        Vec draws{};
        draws.words[0] = random.next();
        draws.words[1] = random.next();
        draws.words[2] = random.next();
        draws.words[3] = random.next();
        const Vec colour = lerp(colour_base, colour_range, draws, kXyzw);

        // f13 = pair.x + colour.w * (pair.y - colour.z), formed in the w lane.
        const std::uint32_t pair_x = m.read_u32_le(pair);
        const std::uint32_t pair_y = m.read_u32_le(pair + 4U);
        const Vec weighted = guest::execute_vu0_macro_broadcast_multiply(
                                 colour, colour, lane_x(fpu::sub(pair_y, colour.words[2])), n3::kMaskW, 0U)
                                 .value;
        const std::uint32_t shade_w = guest::execute_vu0_macro_broadcast_add_subtract(
                                          weighted, weighted, lane_x(pair_x), n3::kMaskW, 0U, false)
                                          .value.words[3];

        n3::store_quad_any(m, frame + kStartSlot, start);
        n3::store_quad_any(m, frame + kDirectionSlot, direction_out);
        call.set_arg(0U, effect);
        call.set_arg(1U, frame + kStartSlot);
        call.set_arg(2U, frame + kDirectionSlot);
        call.set_arg64(3U, spacing_gate);
        call.set_arg(4U, remaining >= first_count ? 1U : 0U);
        call.set_float_arg(0U, m.read_u32_le(system + kRate));
        call.set_float_arg(1U, shade_w);
        call.set_float_arg(2U, colour.words[2]);
        call.set_float_arg(3U, colour.words[1]);
        call.set_float_arg(4U, length);
        call.set_float_arg(5U, colour.words[0]);
        const n3::Run appended = call.invoke([](auto& machine) { return n3::fn_00254a90(machine); }, kFrameBytes);
        if (appended.declined) {
            return call.decline();
        }
        retired += appended.retired;
    }
    call.set_vector_random(random.bits);
    return call.finish(retired + kExitCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0025e410, 1632U, 0xddfb2ea75cd54cb0ULL, "EmitTrailParticlesOne", kCeiling, emit_trail_particles_one,
         EngineReturn::Nothing)
