// 0x004256c0 UpdateRadialBlur (a0 = the radial blur record, a1 = player index): decides where the screen's radial blur
// centres, how it falls off and how strong it is, for one player's view. Returns 1 when the record was set (even to
// nothing), 0 when there is no blur.
//
// The record: +0 a point 10 ahead of the camera as the last call saw it (the word at +12 is stale stack data: the
// original copies it from an unwritten stack slot), +16/+20 the blur centre (u, v), +24/+28 the falloff, +32 the
// strength, +36 the camera clock of the last call. Every call first stores the new point ahead (camera position plus
// 10 times the camera's at row, built from the orientation quaternion), the new clock and a strength of 0, then:
//   - no crash record for the view: return 0;
//   - just after a crash (crash start > 0, the view's flags do not bar it, less than the wobble time since): the blur
//     wobbles around the screen centre, four cosines of the time since the crash (the VU0 polynomial below);
//   - otherwise, when the flags ask for blur always (or the camera shakes), the blur centres where the old point ahead
//     now is on screen and is as strong as the camera turned: (|swing / elapsed| - 1) / 49 clamped to 1, centred
//     with the swing's z used as a divisor.
// The camera-shake paths call CameraShakeEnvelope (0x0030a990), which has no n3 file, so they decline.
//
// Style note: the VU0 steps are written on raw bits with the same FMAC primitives the original's COP2 instructions
// use (one lane at a time: no instruction here mixes lanes except through the broadcast operand), the COP1 steps with
// n3::fpu. MAC/STATUS flags, ACC, Q, I and the vector registers are scratch for the callers, as for the old native.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00130740)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

// Memory layout.
constexpr GuestAddress kGameCamera = 0x00666270U;      // pointer to the game camera record
constexpr GuestAddress kCameraSource = 104U;           // the camera's source record
constexpr GuestAddress kSourceOrientation = 16U;       // scaled orientation quaternion
constexpr GuestAddress kSourcePosition = 32U;          // position
constexpr GuestAddress kCameraClock = 0x004f50a0U;
constexpr GuestAddress kCrashClock = 0x00522680U;
constexpr GuestAddress kViews = 0x00522660U;           // per player, kViewStride bytes
constexpr std::uint32_t kViewStride = 10192U;
constexpr GuestAddress kViewShake = 0x0012bedcU;       // the player's camera shake record
constexpr GuestAddress kViewCrash = 0x0012d180U;       // pointer to the crash record
constexpr GuestAddress kCrashStart = 13620U;           // float: when the crash began
constexpr GuestAddress kShakeActive = 82U;             // byte in the shake record
constexpr GuestAddress kPlayerFlags = 0x005179c0U;     // per player, kFlagsStride bytes (low word used)
constexpr std::uint32_t kFlagsStride = 2992U;
constexpr GuestAddress kWobbleRates = 0x004d1540U;     // four angular rates
constexpr GuestAddress kWobbleWeights = 0x004d1550U;   // x, x, v, v

// Blur record offsets.
constexpr GuestAddress kAhead = 0U;
constexpr GuestAddress kCentreU = 16U;
constexpr GuestAddress kCentreV = 20U;
constexpr GuestAddress kFalloffU = 24U;
constexpr GuestAddress kFalloffV = 28U;
constexpr GuestAddress kStrength = 32U;
constexpr GuestAddress kTime = 36U;

// Words read gp-relative.
constexpr std::int32_t kQuaternionScaleGp = -30620;    // square root of two
constexpr std::int32_t kWobbleTimeGp = -30484;         // 0.3
constexpr std::int32_t kWobbleRateGp = -30384;         // 1 / 0.3
constexpr std::int32_t kFalloffBaseGp = -30796;        // 0.1
constexpr std::int32_t kFalloffScaleGp = -30604;       // 0.9
constexpr std::int32_t kBlurFalloffGp = -30380;        // 0.012
constexpr std::int32_t kTurnScaleGp = -30372;          // 1 / 49
constexpr std::int32_t kTurnLeastGp = -30368;          // 2 / 255
constexpr std::int32_t kTurnNearestGp = -31236;        // 0.01
constexpr std::int32_t kTurnSpreadGp = -30364;         // 0.998

// Player flag bits.
constexpr std::uint32_t kFlagNoCrashWobble = 0x2000U;
constexpr std::uint32_t kFlagAlwaysBlur = 0x0400U;
constexpr std::uint32_t kFlagShakeBlur = 0x0004U | 0x0020U;

// Float constants (raw bits).
constexpr std::uint32_t kZero = 0x00000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kAheadDistance = 0x41200000U;  // 10.0
constexpr std::uint32_t kTwoPi = 0x40c90fdaU;
constexpr std::uint32_t kInverseTwoPi = 0xbe22f983U;   // -1 / (2 pi)
constexpr std::uint32_t kRoundingMagic = 0x4b400000U;  // 1.5 * 2^23
constexpr std::uint32_t kSeries1 = 0xc2992661U;
constexpr std::uint32_t kSeries2 = 0xc2255de0U;
constexpr std::uint32_t kSeries3 = 0x42a33457U;
constexpr std::uint32_t kSeries4 = 0x421ed7b7U;
constexpr std::uint32_t kSignMask = 0x7fffffffU;

// The original's frame: addiu $sp, $sp, -2576. Its stack slots are scratch below the entry $sp; the one slot read
// before it is written is the stale fourth word of the "at" row (frame + 1120 + 12).
constexpr std::uint32_t kFrameBytes = 2576U;
constexpr std::uint32_t kQuaternionSlot = 144U;
constexpr std::uint32_t kStaleAtW = 1120U + 12U;

// Retired instruction counts from the listing (brief.py's block table), delay slots included. The first block, the
// six fixed 8-pass byte-copy loops (8 instructions a pass) with the code between them, and the crash test.
constexpr std::uint32_t kPrologueCost = 116U;          // 0x4256c0..0x42588c, without StoreQuadword's 3
constexpr std::uint32_t kCopyLoopsCost = 6U * 8U * 8U + 5U + 6U + 8U + 6U + 6U;  // 0x425890..0x4259c8
constexpr std::uint32_t kCrashTestCost = 25U;          // 0x4259cc..0x425a2c
constexpr std::uint32_t kThroughCrashTest = kPrologueCost + kCopyLoopsCost + kCrashTestCost;
constexpr std::uint32_t kNoCrashCost = 2U;             // 0x425cd4
constexpr std::uint32_t kStartTestCost = 4U;           // 0x425a30: crash start <= 0
constexpr std::uint32_t kFlagTestCost = 9U;            // 0x425a40: the no-wobble flag
constexpr std::uint32_t kTimeTestCost = 7U;            // 0x425a64: time since the crash
constexpr std::uint32_t kWobbleCost = 133U;            // 0x425a80..0x425c90
constexpr std::uint32_t kFlagsCost = 11U;              // 0x425c94: always-blur flag
constexpr std::uint32_t kNoShakeCost = 3U + 2U;        // 0x425cc0, 0x425ccc: shake byte clear, result 0
constexpr std::uint32_t kSwingCost = 44U;              // 0x425cf8..0x425da4: basis, swing, elapsed test
constexpr std::uint32_t kStaleExitCost = 2U;           // 0x425fd8 / 0x425fe0: result 1
constexpr std::uint32_t kTurnCost = 37U;               // 0x425da8..0x425e38: rate, turned, strength test
constexpr std::uint32_t kClampTestCost = 3U;           // 0x425e3c
constexpr std::uint32_t kClampCost = 1U;               // 0x425e48
constexpr std::uint32_t kSignTestCost = 8U;            // 0x425e4c
constexpr std::uint32_t kNegateCost = 2U;              // 0x425e6c
constexpr std::uint32_t kScreenCost = 20U;             // 0x425e74
constexpr std::uint32_t kCornerAboveCost = 2U;         // 0x425ec4: screen x > 0
constexpr std::uint32_t kCornerBelowCost = 1U;         // 0x425ecc: screen x <= 0
constexpr std::uint32_t kScreenYCost = 8U;             // 0x425ed0
constexpr std::uint32_t kFalloffCost = 40U;            // 0x425efc..0x425f98
constexpr std::uint32_t kShakeFlagsCost = 10U + 3U;    // 0x425f9c, 0x425fc4: shaking, neither flag bit
constexpr std::uint32_t kResultOneCost = 1U;           // 0x426000
constexpr std::uint32_t kEpilogueCost = 11U;           // 0x426004..0x42602c
constexpr std::uint32_t kCeiling = 20000U;

// ---- VU0 FMAC lane primitives (one lane at a time; the same kernels the COP2 instructions run) ----------------

constexpr std::uint8_t kLaneX = 8U;

Gpr128 scalar(const std::uint32_t bits) { return Gpr128{{bits, 0U, 0U, 0U}}; }

std::uint32_t vu_mul(const std::uint32_t a, const std::uint32_t b) {
    return guest::execute_vu0_macro_vector_multiply({}, scalar(a), scalar(b), kLaneX).value.words[0];
}
std::uint32_t vu_add(const std::uint32_t a, const std::uint32_t b) {
    return guest::execute_vu0_macro_vector_add_subtract({}, scalar(a), scalar(b), kLaneX, false).value.words[0];
}
std::uint32_t vu_sub(const std::uint32_t a, const std::uint32_t b) {
    return guest::execute_vu0_macro_vector_add_subtract({}, scalar(a), scalar(b), kLaneX, true).value.words[0];
}
// ACC + a * b and ACC - a * b, the product chopped first (VMADD / VMSUB and their ACC forms).
std::uint32_t vu_mac(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    return guest::execute_vu0_macro_vector_multiply_accumulate({}, scalar(acc), scalar(a), scalar(b), kLaneX, false)
        .value.words[0];
}
std::uint32_t vu_msub(const std::uint32_t acc, const std::uint32_t a, const std::uint32_t b) {
    return guest::execute_vu0_macro_vector_multiply_accumulate({}, scalar(acc), scalar(a), scalar(b), kLaneX, true)
        .value.words[0];
}

// The cosine of one wobble angle, as the original's VU0 sequence computes it: reduce |angle| / (2 pi) to its
// distance from a quarter turn with the 1.5 * 2^23 rounding trick, then an odd series in that distance.
std::uint32_t wave_cosine(const std::uint32_t angle) {
    const std::uint32_t turns = angle & kSignMask;
    std::uint32_t acc = vu_mul(turns, kInverseTwoPi);
    acc = vu_msub(acc, kOne, kRoundingMagic);
    acc = vu_mac(acc, kOne, kRoundingMagic);
    acc = vu_msub(acc, turns, kInverseTwoPi);
    const std::uint32_t distance = vu_sub(vu_msub(acc, kOne, kHalf) & kSignMask, kQuarter);
    const std::uint32_t d2 = vu_mul(distance, distance);
    const std::uint32_t d4 = vu_mul(d2, d2);
    const std::uint32_t d8 = vu_mul(d4, d4);
    const std::uint32_t term3 = vu_mul(vu_mul(distance, kSeries1), d2);
    const std::uint32_t term2 = vu_mul(distance, kSeries2);
    const std::uint32_t term5 = vu_mul(distance, kSeries3);
    const std::uint32_t term9 = vu_mul(distance, kSeries4);
    acc = vu_mul(term2, d2);
    acc = vu_mac(acc, term3, d4);
    acc = vu_mac(acc, term5, d4);
    acc = vu_mac(acc, distance, kTwoPi);
    return vu_mac(acc, term9, d8);
}

struct Row {
    std::uint32_t x, y, z;
};

// ---- the function -------------------------------------------------------------------------------------------------

// The values the blur record's centre, falloff and strength are set to.
struct Blur {
    std::uint32_t u, v, falloff, strength;
};

// lw / lwc1 fault unless the word is 4-byte aligned and inside guest memory (the call is then left to the original).
bool word_readable(const auto& memory, const GuestAddress address) {
    return (address & 3U) == 0U && memory.contains(address, 4U);
}

// The blur this call asks for after a crash: four weighted cosines of the time since the crash give the centre's
// offset from the screen middle, which also sets how far the blur falls off.
Blur crash_wobble(const auto& m, const std::uint32_t gp, const std::uint32_t since) {
    const auto global = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };
    const Gpr128 rates = n3::load_quad_any(m, kWobbleRates);
    const Gpr128 weights = n3::load_quad_any(m, kWobbleWeights);
    std::uint32_t wave[4];
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        wave[lane] = vu_mul(wave_cosine(vu_mul(rates.words[lane], since)), weights.words[lane]);
    }
    const std::uint32_t u = vu_add(wave[0], wave[1]);
    const std::uint32_t v = vu_add(wave[2], wave[3]);
    const std::uint32_t fade = fpu::mul(global(kWobbleRateGp), since);
    const std::uint32_t strength = fpu::sub(kOne, fpu::mul(fade, fade));
    const std::uint32_t offset = fpu::sqrt(fpu::madd(fpu::add(kZero, fpu::mul(v, v)), u, u));
    const std::uint32_t spread = fpu::mul(
        global(kBlurFalloffGp),
        fpu::madd(fpu::add(kZero, global(kFalloffBaseGp)), global(kFalloffScaleGp), strength));
    const std::uint32_t falloff = fpu::sub(kOne, fpu::div(spread, fpu::add(kHalf, offset)));
    return {fpu::add(u, kHalf), fpu::add(v, kHalf), falloff, strength};
}

template <class Context>
n3::Run update_radial_blur(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress blur = call.arg(0);
    const std::uint32_t player = call.arg(1);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const auto global = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress view = kViews + player * kViewStride;
    const GuestAddress shake = view + kViewShake;
    const GuestAddress flags_address = kPlayerFlags + player * kFlagsStride;

    // The camera's source record and every guest access the original makes that could fault.
    const GuestAddress camera = m.read_u32_le(kGameCamera);
    if (!word_readable(m, camera + kCameraSource)) {
        return call.decline();
    }
    const GuestAddress source = m.read_u32_le(camera + kCameraSource);
    if ((blur & 15U) != 0U || !m.contains(blur, 48U) || (source & 15U) != 0U ||
        !m.contains(source + kSourceOrientation, 32U) || !word_readable(m, flags_address) ||
        !word_readable(m, view + kViewCrash) || !m.contains(shake + kShakeActive, 1U)) {
        return call.decline();
    }
    const Gpr128 last_ahead = n3::load_quad_any(m, blur + kAhead);
    const std::uint32_t then = m.read_u32_le(blur + kTime);
    const std::uint32_t now = m.read_u32_le(kCameraClock);
    const Gpr128 orientation = n3::load_quad_any(m, source + kSourceOrientation);
    const Gpr128 position = n3::load_quad_any(m, source + kSourcePosition);

    // The orientation times the root-two scale, handed to StoreQuadword(frame + 144) as the original does.
    const std::uint32_t scale = global(kQuaternionScaleGp);
    Gpr128 quaternion{};
    for (std::size_t lane = 0U; lane < 4U; ++lane) {
        quaternion.words[lane] = vu_mul(orientation.words[lane], scale);
    }
    call.set_arg(0, frame + kQuaternionSlot);
    call.set_arg128(1, quaternion);
    const n3::Run stored = call.invoke([](auto& machine) { return n3::fn_00130740(machine); }, kFrameBytes);
    if (stored.declined) {
        return call.decline();
    }

    // The camera's axes from the quaternion (each element is moved through VADDx with VF0, i.e. 0 + element).
    const std::uint32_t xx = vu_mul(quaternion.words[0], quaternion.words[0]);
    const std::uint32_t xy = vu_mul(quaternion.words[0], quaternion.words[1]);
    const std::uint32_t xz = vu_mul(quaternion.words[0], quaternion.words[2]);
    const std::uint32_t yy = vu_mul(quaternion.words[1], quaternion.words[1]);
    const std::uint32_t yz = vu_mul(quaternion.words[1], quaternion.words[2]);
    const std::uint32_t zz = vu_mul(quaternion.words[2], quaternion.words[2]);
    const std::uint32_t wx = vu_mul(quaternion.words[3], quaternion.words[0]);
    const std::uint32_t wy = vu_mul(quaternion.words[3], quaternion.words[1]);
    const std::uint32_t wz = vu_mul(quaternion.words[3], quaternion.words[2]);
    const std::uint32_t one_less_xx = fpu::sub(kOne, xx);
    const std::uint32_t one_less_yy = fpu::sub(kOne, yy);
    const auto moved = [](const std::uint32_t element) { return vu_add(kZero, element); };
    const Row right{moved(fpu::sub(one_less_yy, zz)), moved(fpu::add(xy, wz)), moved(fpu::sub(xz, wy))};
    const Row up{moved(fpu::sub(xy, wz)), moved(fpu::sub(one_less_xx, zz)), moved(fpu::add(yz, wx))};
    const Row at{moved(fpu::add(xz, wy)), moved(fpu::sub(yz, wx)), moved(fpu::sub(one_less_xx, yy))};
    // The at row's fourth word is never set: it is what the stack held.
    const std::uint32_t at_w = m.read_u32_le(frame + kStaleAtW);

    // The point 10 ahead of the camera now.
    const Gpr128 ahead{{vu_add(position.words[0], vu_mul(at.x, kAheadDistance)),
                        vu_add(position.words[1], vu_mul(at.y, kAheadDistance)),
                        vu_add(position.words[2], vu_mul(at.z, kAheadDistance)), at_w}};

    std::uint32_t retired = kThroughCrashTest + stored.retired;
    // Every exit stores the new point ahead, the clock and a strength of 0; the blur values only on a blur.
    const auto done = [&](const std::uint32_t result, const std::uint32_t cost, const Blur* blur_values) {
        n3::store_quad_any(m, blur + kAhead, ahead);
        m.write_u32_le(blur + kTime, now);
        m.write_u32_le(blur + kStrength, kZero);
        if (blur_values != nullptr) {
            m.write_u32_le(blur + kCentreU, blur_values->u);
            m.write_u32_le(blur + kCentreV, blur_values->v);
            m.write_u32_le(blur + kStrength, blur_values->strength);
            m.write_u32_le(blur + kFalloffU, blur_values->falloff);
            m.write_u32_le(blur + kFalloffV, blur_values->falloff);
        }
        call.return_value(result);
        return call.finish(cost);
    };

    const GuestAddress crash = m.read_u32_le(view + kViewCrash);
    if (crash == 0U) {
        return done(0U, retired + kNoCrashCost + kEpilogueCost, nullptr);
    }
    if (!word_readable(m, crash + kCrashStart)) {
        return call.decline();
    }
    const std::uint32_t crash_start = m.read_u32_le(crash + kCrashStart);
    const std::uint32_t flags = m.read_u32_le(flags_address);

    // Just after a crash: the wobble.
    retired += kStartTestCost;
    bool wobbling = !fpu::le(crash_start, kZero);
    if (wobbling) {
        retired += kFlagTestCost;
        wobbling = (flags & kFlagNoCrashWobble) == 0U;
    }
    std::uint32_t since = kZero;
    if (wobbling) {
        retired += kTimeTestCost;
        since = fpu::sub(m.read_u32_le(kCrashClock), crash_start);
        wobbling = fpu::lt(since, global(kWobbleTimeGp));
    }
    if (wobbling) {
        const Blur wobble = crash_wobble(m, gp, since);
        return done(1U, retired + kWobbleCost + kEpilogueCost, &wobble);
    }

    // Otherwise only with the always-blur flag; a shaking camera without it asks CameraShakeEnvelope (0x0030a990),
    // which has no n3 file.
    retired += kFlagsCost;
    if ((flags & kFlagAlwaysBlur) == 0U) {
        if (m.read_u8(shake + kShakeActive) == 0U) {
            return done(0U, retired + kNoShakeCost + kEpilogueCost, nullptr);
        }
        return call.decline();
    }

    // How far the view swung since the last call: the old point ahead in the camera's axes, less the 10 it started at.
    retired += kSwingCost;
    const std::uint32_t elapsed = fpu::sub(now, then);
    if (fpu::le(elapsed, kZero)) {
        return done(1U, retired + kStaleExitCost + kEpilogueCost, nullptr);
    }
    const auto offset_of = [&](const Row& row) {
        const std::uint32_t along = vu_add(vu_add(vu_mul(row.x, position.words[0]), vu_mul(row.y, position.words[1])),
                                           vu_mul(row.z, position.words[2]));
        return vu_sub(kZero, along);
    };
    const auto swing_of = [&](const Row& row) {
        std::uint32_t acc = vu_mul(row.x, last_ahead.words[0]);
        acc = vu_mac(acc, row.y, last_ahead.words[1]);
        acc = vu_mac(acc, row.z, last_ahead.words[2]);
        return vu_mac(acc, offset_of(row), kOne);
    };
    const std::uint32_t swing_x = swing_of(right);
    const std::uint32_t swing_y = swing_of(up);
    const std::uint32_t swing_z = swing_of(at);

    // Its rate per second, and how much faster than 1 the point moved.
    retired += kTurnCost;
    const std::uint32_t per_second = fpu::div(kOne, elapsed);
    Row rate{vu_mul(swing_x, per_second), vu_mul(swing_y, per_second),
             vu_mul(moved(vu_sub(swing_z, kAheadDistance)), per_second)};
    const std::uint32_t squares_xy = vu_add(vu_mul(rate.x, rate.x), vu_mul(rate.y, rate.y));
    const std::uint32_t radicand = vu_mac(squares_xy, kOne, vu_mul(rate.z, rate.z));
    const std::uint32_t turned = vu_mul(kOne, guest::ee_cop2::square_root_bits(radicand & kSignMask));
    std::uint32_t strength = fpu::mul(global(kTurnScaleGp), fpu::sub(turned, kOne));
    if (fpu::lt(strength, global(kTurnLeastGp))) {
        return done(1U, retired + kStaleExitCost + kEpilogueCost, nullptr);
    }
    retired += kClampTestCost;
    if (!fpu::le(strength, kOne)) {
        strength = kOne;
        retired += kClampCost;
    }

    // Behind the camera the rate flips; the centre is the rate scaled by 0.998 / max(z, 0.01) around the middle.
    retired += kSignTestCost;
    if (fpu::lt(rate.z, kZero)) {
        rate = {vu_sub(kZero, rate.x), vu_sub(kZero, rate.y), vu_sub(kZero, rate.z)};
        retired += kNegateCost;
    }
    retired += kScreenCost;
    // MAX.S orders raw sign/magnitude exactly as the VU0 MAX does.
    const std::uint32_t spread = fpu::div(global(kTurnSpreadGp), guest::ee_cop2::maximum_bits(rate.z, global(kTurnNearestGp)));
    const std::uint32_t screen_x = vu_mul(rate.x, spread);
    const std::uint32_t screen_y = vu_mul(rate.y, spread);
    const bool x_low = fpu::le(screen_x, kZero);
    retired += x_low ? kCornerBelowCost : kCornerAboveCost;
    retired += kScreenYCost;
    const bool y_low = fpu::le(screen_y, kZero);
    retired += y_low ? kCornerBelowCost : kCornerAboveCost;
    retired += kFalloffCost;
    const std::uint32_t corner_u = x_low ? kOne : kZero;
    const std::uint32_t corner_v = y_low ? kOne : kZero;
    const std::uint32_t u = vu_add(screen_x, kHalf);
    const std::uint32_t v = vu_add(screen_y, kHalf);
    const std::uint32_t du = fpu::sub(u, corner_u);
    const std::uint32_t dv = fpu::sub(v, corner_v);
    const std::uint32_t reach = fpu::sqrt(fpu::madd(fpu::mul(dv, dv), du, du));
    const std::uint32_t inner = fpu::madd(fpu::add(kZero, kHalf), kHalf, strength);
    const std::uint32_t falloff = fpu::sub(kOne, fpu::div(fpu::mul(global(kBlurFalloffGp), inner), reach));
    const Blur turn{u, v, falloff, strength};

    if (m.read_u8(shake + kShakeActive) == 0U) {
        return done(1U, retired + kResultOneCost + kEpilogueCost, &turn);
    }
    if ((flags & kFlagShakeBlur) != 0U) {
        return call.decline();  // the strength is scaled by CameraShakeEnvelope (0x0030a990)
    }
    return done(1U, retired + kShakeFlagsCost + kResultOneCost + kEpilogueCost, &turn);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(004256c0, 2416U, 0x897f2c0dc09abbb8ULL, "UpdateRadialBlur", kCeiling, update_radial_blur,
         EngineReturn::Integer)
