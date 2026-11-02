// 0x00399ec0 EmitScrapeSparks (a0 = effect object whose words +240/+244 are its random pair, a1/a2/a3 = three
// input quads: the first is added to every particle's position, the second and third weight its direction).
// Returns nothing.
//
// The function emits one burst of spark particles described by entry 0 of the effect table at 0x004a51c0 (72-byte
// entries; the original's outer loop starts at entry 0, tests `blez` on the entry number and then steps to 1, so it
// runs exactly once and only entry 0 is ever used: the code below is straight, not a loop over entries).
//
// It reseeds the VU0 random generator R from the object's random pair, builds a handful of base/range quads from
// the entry's floats, and then makes the entry's count of particles. Each particle draws from R:
//   - a random direction (a unit vector: base + range * jitter-residue, normalised with VRSQRT),
//   - a random radius that scales the direction into a start offset,
//   - a random velocity offset, and one more four-lane draw V whose x and y weight the second and third input
//     quads and whose z and w go to the sample as floats.
// Every jitter residue is the quad at 0x01ea4ba0 times a draw, rounded against the grid quad at 0x004c9a00; the
// jitter quad is rotated one lane per use and written back after each particle (three rotations per particle).
// The position and direction quads are appended to the trail through AppendSampleRecord (0x00254a90).
//
// The vector arithmetic runs through the VU0 macro FMAC kernels on raw lanes. The w lanes of the intermediate
// quads reach the two quads AppendSampleRecord reads, so they are tracked; six frame words that the original reads
// before ever writing (they keep whatever the dead frame held) are read from the frame.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00254a90)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
using Vec = guest::Gpr128;

constexpr GuestAddress kEffectTable = 0x004a51c0U;      // lui 0x4a, addiu 20928; entry 0 is the only one used
constexpr GuestAddress kTrails = 0x01d9d200U;           // lui 0x1da, addiu -11776: trail objects
constexpr std::uint32_t kTrailBytes = 336U;
constexpr GuestAddress kTrailRecord = 368U;             // trail t = kTrails + 336 * t + 368
constexpr GuestAddress kFloorHeight = 0x01dbca48U;      // lui 0x1dc, lwc1 -13752: float
constexpr GuestAddress kJitter = 0x01ea4ba0U;           // quad, rotated three lanes per particle
constexpr GuestAddress kGrid = 0x004c9a00U;             // quad: the grid the jitter residue is taken against
constexpr std::int32_t kSpanGp = -29668;                // gp-relative float
constexpr GuestAddress kRandomFirst = 240U;             // object words: the random pair
constexpr GuestAddress kRandomSecond = 244U;

// Effect table entry fields.
constexpr GuestAddress kTrailIndex = 0U;
constexpr GuestAddress kCount = 4U;
constexpr GuestAddress kWidthLow = 8U;
constexpr GuestAddress kWidthHigh = 12U;
constexpr GuestAddress kWidthSlope = 16U;
constexpr GuestAddress kSideways = 20U;
constexpr GuestAddress kUpward = 24U;
constexpr GuestAddress kForward = 28U;
constexpr GuestAddress kLifeLow = 32U;
constexpr GuestAddress kLifeHigh = 36U;
constexpr GuestAddress kSpeedScale = 40U;               // raw word handed to the vector unit as a float
constexpr GuestAddress kAgeLow = 44U;
constexpr GuestAddress kAgeHigh = 48U;
constexpr GuestAddress kSizeLow = 52U;
constexpr GuestAddress kSizeExtra = 56U;
constexpr GuestAddress kSampleScale1 = 60U;             // floats passed on to AppendSampleRecord
constexpr GuestAddress kSampleScale2 = 64U;
constexpr GuestAddress kSampleKind = 68U;               // byte: which sub-record AppendSampleRecord uses

constexpr std::uint32_t kFrameBytes = 944U;
constexpr GuestAddress kPositionSlot = 752U;            // frame quads AppendSampleRecord reads through a1, a2
constexpr GuestAddress kDirectionSlot = 736U;
// Frame words the original reads before writing (the w lane of six quads it builds with three swc1 stores).
constexpr GuestAddress kStaleDirection = 284U;
constexpr GuestAddress kStaleRadius = 268U;
constexpr GuestAddress kStaleVelocity = 252U;
constexpr GuestAddress kStaleLife = 236U;
constexpr GuestAddress kStaleVelocityRange = 220U;
constexpr GuestAddress kStaleHalf = 300U;

// Float constants (raw bits).
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kMinusHalf = 0xbf000000U;
constexpr std::uint32_t kWordScale = 0x2f800000U;       // 2^-32

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 40U;               // 0x399ec0..0x399f5c: frame, loads, the random pair
constexpr std::uint32_t kToFloatPositive = 3U;          // unsigned word to float, bit 31 clear
constexpr std::uint32_t kToFloatNegative = 7U;          // ... bit 31 set
constexpr std::uint32_t kSeedCost = 15U + 2U;           // 0x399f88..0x399fc0 (constants, R seed) and the blez
constexpr std::uint32_t kPreludeCost = 89U;             // 0x399fc4..0x3a0124: the entry's base/range quads
constexpr std::uint32_t kPassCost = 120U;               // 0x39a128..0x39a304 incl. jal, delay slot and loop test
constexpr std::uint32_t kExitCost = 1U + 2U + 16U;      // 0x39a308, the blez falling out, the epilogue
constexpr std::uint32_t kCeiling = 20000U;

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
constexpr Vec kVf0 = n3::kVf0;

Vec add(const Vec& dest, const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, l, r, mask, false).value;
}
Vec sub(const Vec& dest, const Vec& l, const Vec& r, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_vector_add_subtract(dest, l, r, mask, true).value;
}
Vec mul(const Vec& dest, const Vec& l, const Vec& r) {
    return guest::execute_vu0_macro_vector_multiply(dest, l, r, kXyz).value;
}
Vec mul_lane(const Vec& dest, const Vec& l, const Vec& r, const std::size_t lane, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_broadcast_multiply(dest, l, r, mask, lane).value;
}
// ACC = base + vf0.x (VADDAx with VF0): the accumulator the following VMADD starts from.
Vec accumulate_base(const Vec& base, const std::uint8_t mask = kXyz) {
    return guest::execute_vu0_macro_broadcast_add_subtract(base, base, kVf0, mask, 0U, false).value;
}
// The rounding residue of `scaled` on the grid: (scaled + grid - grid) - scaled, xyz (VADDA, VMSUBAw, VMSUBw).
Vec residue(const Vec& scaled, const Vec& grid) {
    const Vec acc = guest::execute_vu0_macro_vector_add_subtract(scaled, scaled, grid, kXyz, false).value;
    const Vec acc2 =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(grid, acc, grid, kVf0, kXyz, 3U, true).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(scaled, acc2, scaled, kVf0, kXyz, 3U, true)
        .value;
}
// The same for the x lane alone (VMULx.x, VADDAx.x, VMSUBAw.x, VMSUBw.x): the other lanes stay the jitter's.
Vec residue_x(const Vec& jitter, const Vec& grid, const std::uint32_t draw) {
    Vec draws = jitter;
    draws.words[0] = draw;
    const Vec scaled = guest::execute_vu0_macro_broadcast_multiply(draws, jitter, draws, kX, 0U).value;
    const Vec acc = guest::execute_vu0_macro_broadcast_add_subtract(scaled, grid, scaled, kX, 0U, false).value;
    const Vec acc2 =
        guest::execute_vu0_macro_broadcast_multiply_accumulate(grid, acc, grid, kVf0, kX, 3U, true).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(scaled, acc2, scaled, kVf0, kX, 3U, true).value;
}
// base + range * weight on xyz (VADDAx then VMADD); the w lane is the weight's.
Vec lerp(const Vec& base, const Vec& range, const Vec& weight) {
    return guest::execute_vu0_macro_vector_multiply_accumulate(weight, accumulate_base(base), range, weight, kXyz,
                                                               false)
        .value;
}

Vec rotate(const Vec& v) { return Vec{{v.words[1], v.words[2], v.words[3], v.words[0]}}; }  // vmr32

template <class Memory>
Vec load_vec(const Memory& m, const GuestAddress address) { return n3::load_quad_any(m, address); }

// Unsigned word to float the way the original forms it (cvt.s.w of a signed word, with a halved copy and an
// added-to-itself result for bit 31).
std::uint32_t unsigned_to_float(const std::uint32_t word) {
    if (static_cast<std::int32_t>(word) >= 0) {
        return fpu::from_int(static_cast<std::int32_t>(word));
    }
    const std::uint32_t half = fpu::from_int(static_cast<std::int32_t>((word >> 1U) | (word & 1U)));
    return fpu::add(half, half);
}

// One step of the object's random pair (0x399f14..0x399f54): first = (first << 16) + (first >> 16 arithmetic);
// first += second; second += first. Returns the new first word.
template <class Memory>
std::uint32_t draw_pair(Memory& m, const GuestAddress object) {
    std::uint32_t first = m.read_u32_le(object + kRandomFirst);
    first = (first << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(first) >> 16);
    m.write_u32_le(object + kRandomFirst, first);
    std::uint32_t second = m.read_u32_le(object + kRandomSecond);
    first += second;
    m.write_u32_le(object + kRandomFirst, first);
    second += first;
    m.write_u32_le(object + kRandomSecond, second);
    return first;
}

// What the prelude (0x399fc4..0x3a0124) leaves in the frame: a base quad and a range quad per random draw.
struct Ranges {
    Vec direction_base;      // sp+864: (0.5, 0.5, 0.5, stale)
    Vec direction_range;     // sp+880: base - (-0.5, span, -0.5, stale)
    Vec radius_base;         // sp+832: (width high, width high * slope, width high, stale)
    Vec radius_range;        // sp+848: radius_base - (width low, width low * slope, width low, stale)
    Vec velocity_base;       // sp+800: (sideways, forward, sideways, stale)
    Vec velocity_range;      // sp+816: velocity_base - (-sideways, upward, -sideways, stale)
    Vec draw_base;           // sp+768: 2 * low - high
    Vec draw_range;          // sp+784: high - low
};

template <class Memory>
Ranges make_ranges(Memory& m, const GuestAddress entry, const GuestAddress frame, const std::uint32_t span,
                   const std::uint32_t floor_height) {
    const auto field = [&](const GuestAddress offset) { return m.read_u32_le(entry + offset); };
    const auto stale = [&](const GuestAddress offset) { return m.read_u32_le(frame + offset); };
    Ranges r{};

    r.direction_base = Vec{{kHalf, kHalf, kHalf, stale(kStaleHalf)}};
    const Vec direction_low{{kMinusHalf, span, kMinusHalf, stale(kStaleDirection)}};
    r.direction_range = sub(direction_low, r.direction_base, direction_low);

    const std::uint32_t slope = field(kWidthSlope);
    const std::uint32_t high = field(kWidthHigh);
    const std::uint32_t low = field(kWidthLow);
    r.radius_base = Vec{{high, fpu::mul(high, slope), high, stale(kStaleRadius)}};
    const Vec radius_low{{low, fpu::mul(low, slope), low, stale(kStaleVelocity)}};
    r.radius_range = sub(radius_low, r.radius_base, radius_low);

    const std::uint32_t sideways = field(kSideways);
    r.velocity_base = Vec{{sideways, field(kForward), sideways, stale(kStaleLife)}};
    const Vec velocity_low{{fpu::neg(sideways), field(kUpward), fpu::neg(sideways), stale(kStaleVelocityRange)}};
    r.velocity_range = sub(velocity_low, r.velocity_base, velocity_low);

    const Vec low_draw{{field(kAgeLow), field(kLifeLow), fpu::neg(field(kSizeLow)), floor_height}};
    const Vec high_draw{{field(kAgeHigh), field(kLifeHigh), field(kSizeLow),
                         fpu::add(floor_height, field(kSizeExtra))}};
    r.draw_base = sub(low_draw, add(low_draw, low_draw, low_draw, kXyzw), high_draw, kXyzw);
    r.draw_range = sub(low_draw, high_draw, low_draw, kXyzw);
    return r;
}

template <class Context>
n3::Run emit_scrape_sparks(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();

    const GuestAddress object = call.arg(0);
    const Vec first_input = load_vec(m, call.arg(1));
    const Vec second_input = load_vec(m, call.arg(2));
    const Vec third_input = load_vec(m, call.arg(3));
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t floor_height = m.read_u32_le(kFloorHeight);

    // The object's random pair gives the word that seeds R.
    std::uint32_t retired = kEntryCost;
    const std::uint32_t word = draw_pair(m, object);
    retired += static_cast<std::int32_t>(word) >= 0 ? kToFloatPositive : kToFloatNegative;
    VuRandom random;
    random.seed(fpu::mul(kWordScale, unsigned_to_float(word)));
    retired += kSeedCost;

    // Entry 0 of the effect table (the original's entry loop runs once).
    const GuestAddress entry = kEffectTable;
    const GuestAddress trail = kTrails + kTrailBytes * m.read_u32_le(entry + kTrailIndex) + kTrailRecord;
    std::uint32_t remaining = m.read_u32_le(entry + kCount);  // do-while: a count of 0 wraps and hits the ceiling
    const std::uint32_t speed_scale = m.read_u32_le(entry + kSpeedScale);
    const Ranges ranges = make_ranges(m, entry, frame, m.read_u32_le(gp + static_cast<std::uint32_t>(kSpanGp)),
                                      floor_height);
    retired += kPreludeCost;

    do {
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kPassCost;

        // V = base + range * (four draws): x and y weight the second and third inputs, z and w go to the sample.
        Vec draws{};
        draws.words[0] = random.next();
        draws.words[1] = random.next();
        draws.words[2] = random.next();
        draws.words[3] = random.next();
        const Vec v = guest::execute_vu0_macro_vector_multiply_accumulate(
                          draws, accumulate_base(ranges.draw_base, kXyzw), ranges.draw_range, draws, kXyzw, false)
                          .value;

        // Direction: base + range * residue, scaled to unit length (VRSQRT of its squared length).
        Vec rand3{};
        rand3.words[0] = random.next();
        rand3.words[1] = random.next();
        rand3.words[2] = random.next();
        rand3.words[3] = ranges.draw_range.words[3];
        const Vec jitter = load_vec(m, kJitter);
        const Vec grid = load_vec(m, kGrid);
        const Vec jitter1 = rotate(jitter);
        const Vec direction = lerp(ranges.direction_base, ranges.direction_range, residue(mul(rand3, jitter, rand3), grid));
        const Vec square = mul(direction, direction, direction);
        Vec lane_sum = guest::execute_vu0_macro_broadcast_add_subtract(square, square, square, kX, 1U, false).value;
        Vec one = jitter1;
        one.words[0] = kOne;
        const std::uint32_t squared_length =
            guest::execute_vu0_macro_broadcast_multiply_accumulate(one, lane_sum, one, square, kX, 2U, false)
                .value.words[0];
        const std::uint32_t inverse_length = guest::ee_cop2::reciprocal_square_root_bits(kOne, squared_length);
        Vec inverse{};
        inverse.words[0] = inverse_length;
        const Vec unit = mul_lane(direction, direction, inverse, 0U);

        // Radius: base + range * x-lane residue (the lane-x weight scales all three lanes), applied to the unit
        // direction.
        const Vec weight = residue_x(jitter1, grid, random.next());
        const Vec radius = guest::execute_vu0_macro_broadcast_multiply_accumulate(
                               weight, accumulate_base(ranges.radius_base), ranges.radius_range, weight, kXyz, 0U,
                               false)
                               .value;
        const Vec offset = mul(unit, unit, radius);

        // Velocity: base + range * residue with the next jitter rotation.
        const Vec jitter2 = rotate(jitter1);
        Vec rand_v{};
        rand_v.words[0] = random.next();
        rand_v.words[1] = random.next();
        rand_v.words[2] = random.next();
        rand_v.words[3] = radius.words[3];
        const Vec velocity = lerp(ranges.velocity_base, ranges.velocity_range, residue(mul(rand_v, jitter2, rand_v), grid));
        const Vec jitter3 = rotate(jitter2);
        n3::store_quad_any(m, kJitter, jitter3);

        // Position = offset + first input; direction = velocity + offset * speed + second * V.x + third * V.y.
        Vec speed{};
        speed.words[0] = speed_scale;
        const Vec scaled_offset = mul_lane(velocity, offset, speed, 0U);
        Vec heading = add(velocity, velocity, scaled_offset);
        heading = add(heading, heading, mul_lane(second_input, second_input, v, 0U));
        heading = add(heading, heading, mul_lane(third_input, third_input, v, 1U));
        const Vec position = add(offset, offset, first_input);

        n3::store_quad_any(m, frame + kPositionSlot, position);
        n3::store_quad_any(m, frame + kDirectionSlot, heading);
        call.set_arg(0U, trail);
        call.set_arg(1U, frame + kPositionSlot);
        call.set_arg(2U, frame + kDirectionSlot);
        call.set_arg(3U, 1U);
        call.set_arg(4U, m.read_u8(entry + kSampleKind));
        call.set_float_arg(0U, v.words[3]);
        call.set_float_arg(1U, m.read_u32_le(entry + kSampleScale1));
        call.set_float_arg(2U, m.read_u32_le(entry + kSampleScale2));
        call.set_float_arg(3U, kOne);
        call.set_float_arg(4U, kOne);
        call.set_float_arg(5U, v.words[2]);
        const n3::Run appended = call.invoke([](auto& machine) { return n3::fn_00254a90(machine); }, kFrameBytes);
        if (appended.declined) {
            return call.decline();
        }
        retired += appended.retired;
        --remaining;
    } while (remaining != 0U);

    retired += kExitCost;
    call.set_vector_random(random.bits);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00399ec0, 1172U, 0x8cde26b2fb2292cbULL, "EmitScrapeSparks", kCeiling, emit_scrape_sparks,
         EngineReturn::Nothing)
