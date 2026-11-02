// 0x0032ff80 PickPairTargetAndFlags: for a pair record P (a0) with object record B = *(P+68), picks a target record
// T (the closest candidate from the record table, or the slot lookup of B's slot word when the best is too far),
// stores it in P.target and fills P's distance floats and flag bytes. Returns 1 when a target was set, else 0.
//
// Retired-instruction counts below are per path (delay slots included, callee counts added at each call).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001d0cb0)
N3_CALLEE(00141ae0)
N3_CALLEE(001419f0)
N3_CALLEE(0032fd90)
N3_CALLEE(0032f0d0)
N3_CALLEE(0015ac00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 208U;
constexpr std::uint32_t kCeiling = 20000U;

// Globals.
constexpr GuestAddress kRecordCount = 0x0064d3a0U;
constexpr GuestAddress kRecordTable = 0x0064d3b0U;
constexpr GuestAddress kObjectTable = 0x0055efecU;  // 48-byte entries, word 0 = object
constexpr GuestAddress kMinRating = 0x01e755a4U;
constexpr GuestAddress kCurveDeltaMax = 0x01e755b4U;
constexpr GuestAddress kCurveDeltaMin = 0x01e755b0U;
constexpr GuestAddress kScaledFloor = 0x01e755b8U;
constexpr GuestAddress kWindowHigh = 0x01e755d8U;
constexpr std::int32_t kGpScale = -30980;
constexpr std::int32_t kGpStart = -31324;
constexpr std::int32_t kGpWindowLow = -30696;

constexpr std::uint32_t kRecordStride = 10192U;
constexpr std::uint32_t kFarLimit = 0x42480000U;   // 50.0f
constexpr std::uint32_t kAlignLimit = 0x41a00000U;  // 20.0f

// Pair record P.
constexpr GuestAddress kPairAlignedDistance = 56U;
constexpr GuestAddress kPairDistanceAhead = 60U;
constexpr GuestAddress kPairProximityFlag = 64U;
constexpr GuestAddress kPairWindowFlag = 65U;
constexpr GuestAddress kPairCurveFlag = 66U;
constexpr GuestAddress kPairObject = 68U;
constexpr GuestAddress kPairTarget = 72U;

// Object record R.
constexpr GuestAddress kCurveArea = 4288U;
constexpr GuestAddress kRatingByIndex = 6292U;
constexpr GuestAddress kCurveOwner = 6324U;
constexpr GuestAddress kCurveIndex = 6328U;
constexpr GuestAddress kBusyByte = 6378U;
constexpr GuestAddress kRecordState = 6420U;
constexpr GuestAddress kObjectIndex = 6572U;
constexpr GuestAddress kRating = 9168U;
constexpr GuestAddress kCurveMargin = 9272U;
constexpr GuestAddress kSlotWord = 9264U;
constexpr GuestAddress kMode = 9280U;
constexpr GuestAddress kHeldByte = 10184U;

// Retired counts of the straight-line pieces.
constexpr std::uint32_t kEntry = 30U;
constexpr std::uint32_t kReturnEpilogue = 8U;
constexpr std::uint32_t kReturnZeroBranch = 2U;       // beq/daddu stub in front of the epilogue
constexpr std::uint32_t kSelectSingle = 5U + 3U;      // one record: base address chosen
constexpr std::uint32_t kSelectLoopSetup = 5U + 6U;
constexpr std::uint32_t kLoopCheck = 3U;
constexpr std::uint32_t kSkipSelf = 8U;
constexpr std::uint32_t kSkipFar = 15U;
constexpr std::uint32_t kSkipBusy = 18U;
constexpr std::uint32_t kTakeCandidate = 20U;
constexpr std::uint32_t kFarTest = 6U;
constexpr std::uint32_t kSlotLookup = 3U;             // jal, delay slot, move
constexpr std::uint32_t kNullTest = 2U;
constexpr std::uint32_t kSelfTest = 3U;
constexpr std::uint32_t kStateTest = 3U;
constexpr std::uint32_t kHeldTest = 3U;
constexpr std::uint32_t kModeTest = 3U;
constexpr std::uint32_t kCurveDeltaFirst = 11U;
constexpr std::uint32_t kCurveDeltaLow = 6U;
constexpr std::uint32_t kCurveScaled = 18U;
constexpr std::uint32_t kStoreTarget = 4U;
constexpr std::uint32_t kFillFields = 13U;
constexpr std::uint32_t kAlignTest = 31U;
constexpr std::uint32_t kCurveMargins = 15U;
constexpr std::uint32_t kCurveHigh = 6U;
constexpr std::uint32_t kCurveLookup = 12U;
constexpr std::uint32_t kFlagStub = 2U;
constexpr std::uint32_t kStoreFlags = 4U;
constexpr std::uint32_t kReturnOne = 2U;

template <class Memory>
GuestAddress rec_obj(Memory& m, const GuestAddress rec) {
    const auto index = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(rec + kObjectIndex)));
    return m.read_u32_le(kObjectTable + static_cast<std::uint32_t>(index) * 48U);
}

// w component of the object's quadword at +176.
template <class Memory>
std::uint32_t rec_obj_w(Memory& m, const GuestAddress rec) {
    return n3::load_quad_any(m, rec_obj(m, rec) + 176U).words[3];
}

template <class Context>
n3::Run pick_pair_target_and_flags(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pair = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint32_t g0 = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpScale));
    const GuestAddress obj = m.read_u32_le(pair + kPairObject);

    m.write_u32_le(pair + kPairTarget, 0U);
    std::uint32_t retired = kEntry;
    auto done = [&](const std::uint32_t result, const std::uint32_t exit_path) {
        call.return_value(result);
        return call.finish(retired + exit_path + kReturnEpilogue);
    };

    std::uint32_t best = m.read_u32_le(obj + kRating);
    if (n3::fpu::lt(best, m.read_u32_le(kMinRating))) {
        return done(0U, kReturnZeroBranch);
    }

    // Choose the target.
    GuestAddress target = 0U;
    bool single_record = false;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(kRecordCount));
    if (count == 1) {
        // A lone record is taken as is: the original jumps past the far test.
        target = kRecordTable;
        single_record = true;
        retired += kSelectSingle;
    } else {
        retired += kSelectLoopSetup;
        best = m.read_u32_le(gp + static_cast<std::uint32_t>(kGpStart));
        for (std::int32_t i = 0; i < count; ++i) {
            retired += kLoopCheck;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            const GuestAddress rec = kRecordTable + static_cast<std::uint32_t>(i) * kRecordStride;
            if (rec == obj) {
                retired += kSkipSelf;
                continue;
            }
            const auto slot = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(rec + kObjectIndex)));
            const std::uint32_t rating = n3::fpu::abs(
                m.read_u32_le(obj + kRatingByIndex + (static_cast<std::uint32_t>(slot) << 2)));
            if (!n3::fpu::le(rating, best)) {
                retired += kSkipFar;
                continue;
            }
            if (m.read_u8(rec + kBusyByte) != 0U) {
                retired += kSkipBusy;
                continue;
            }
            target = rec;
            best = rating;
            retired += kTakeCandidate;
        }
        retired += kLoopCheck;
    }

    if (!single_record) {
        retired += kFarTest;
    }
    if (!single_record && !n3::fpu::le(best, kFarLimit)) {
        call.set_arg(0, m.read_u32_le(obj + kSlotWord));
        const n3::Run lookup = call.invoke([](auto& mm) { return n3::fn_001d0cb0(mm); }, kFrameBytes);
        if (lookup.declined) {
            return call.decline();
        }
        target = call.result();
        retired += kSlotLookup + lookup.retired;
    }

    // Reject unusable targets.
    retired += kNullTest;
    if (target == 0U) {
        return done(0U, kReturnZeroBranch);
    }
    const GuestAddress owner = m.read_u32_le(pair + kPairObject);
    retired += kSelfTest;
    if (target == owner) {
        return done(0U, kReturnZeroBranch);
    }
    retired += kStateTest;
    if (m.read_u32_le(target + kRecordState) == 0U) {
        retired += kHeldTest;
        if (m.read_u8(target + kHeldByte) != 0U) {
            return done(0U, kReturnZeroBranch);
        }
    }

    retired += kModeTest;
    if (m.read_u32_le(owner + kMode) == 0U) {
        // The two curve values must be close, and the target's scaled w large enough.
        call.set_arg(0, target + kCurveArea);
        const n3::Run first = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
        if (first.declined) {
            return call.decline();
        }
        const std::uint32_t target_curve = call.float_result();
        call.set_arg(0, owner + kCurveArea);
        const n3::Run second = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
        if (second.declined) {
            return call.decline();
        }
        const std::uint32_t delta = n3::fpu::sub(target_curve, call.float_result());
        retired += kCurveDeltaFirst + first.retired + second.retired;
        if (!n3::fpu::le(delta, m.read_u32_le(kCurveDeltaMax))) {
            return done(0U, kReturnZeroBranch);
        }
        retired += kCurveDeltaLow;
        if (n3::fpu::lt(delta, n3::fpu::neg(m.read_u32_le(kCurveDeltaMin)))) {
            return done(0U, kReturnZeroBranch);
        }
        retired += kCurveScaled;
        if (n3::fpu::lt(n3::fpu::mul(g0, rec_obj_w(m, target)), m.read_u32_le(kScaledFloor))) {
            return done(0U, kReturnZeroBranch);
        }
    }

    // Target accepted: fill the pair record. (The target is never null here, so the original's
    // "target went dead" reset of the fields is unreachable.)
    m.write_u32_le(pair + kPairTarget, target);
    retired += kStoreTarget;

    call.set_arg(0, m.read_u32_le(pair + kPairObject));
    call.set_arg(1, m.read_u32_le(pair + kPairTarget));
    const n3::Run aligned = call.invoke([](auto& mm) { return n3::fn_00141ae0(mm); }, kFrameBytes);
    if (aligned.declined) {
        return call.decline();
    }
    m.write_u32_le(pair + kPairAlignedDistance, call.float_result());

    call.set_arg(0, m.read_u32_le(pair + kPairObject));
    call.set_arg(1, m.read_u32_le(pair + kPairTarget));
    const n3::Run ahead = call.invoke([](auto& mm) { return n3::fn_001419f0(mm); }, kFrameBytes);
    if (ahead.declined) {
        return call.decline();
    }
    m.write_u32_le(pair + kPairDistanceAhead, call.float_result());

    call.set_arg(0, pair);
    const n3::Run proximity = call.invoke([](auto& mm) { return n3::fn_0032fd90(mm); }, kFrameBytes);
    if (proximity.declined) {
        return call.decline();
    }
    m.write_u8(pair + kPairProximityFlag, static_cast<std::uint8_t>(call.result()));
    retired += kFillFields + aligned.retired + ahead.retired + proximity.retired;

    // Window flag: only set when the two w components are nearly equal and the curve value is in range.
    std::uint32_t window_flag = 0U;
    const GuestAddress current = m.read_u32_le(pair + kPairTarget);
    if (current == 0U) {
        retired += kFlagStub;
    } else {
        const GuestAddress pair_obj = m.read_u32_le(pair + kPairObject);
        const std::uint32_t wa = rec_obj_w(m, current);
        const std::uint32_t wb = rec_obj_w(m, pair_obj);
        const std::uint32_t spread = n3::fpu::abs(n3::fpu::msub(n3::fpu::mul(g0, wa), g0, wb));
        retired += kAlignTest;
        if (n3::fpu::le(spread, kAlignLimit)) {
            call.set_arg(0, current + kCurveArea);
            const n3::Run c0 = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
            if (c0.declined) {
                return call.decline();
            }
            const std::uint32_t curve_target = call.float_result();
            const GuestAddress listed = m.read_u32_le(pair + kPairObject);
            call.set_arg(0, listed + kCurveArea);
            const n3::Run c1 = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
            if (c1.declined) {
                return call.decline();
            }
            const std::uint32_t behind = n3::fpu::neg(n3::fpu::sub(curve_target, call.float_result()));
            const GuestAddress margin_obj = m.read_u32_le(pair + kPairObject);
            const std::uint32_t margin = m.read_u32_le(margin_obj + kCurveMargin);
            retired += kCurveMargins + c0.retired + c1.retired;
            if (n3::fpu::lt(behind, n3::fpu::add(m.read_u32_le(gp + static_cast<std::uint32_t>(kGpWindowLow)),
                                                  margin))) {
                retired += kFlagStub;
            } else {
                retired += kCurveHigh;
                if (!n3::fpu::le(behind, n3::fpu::add(margin, m.read_u32_le(kWindowHigh)))) {
                    retired += kFlagStub;
                } else {
                    const GuestAddress owner_curve = m.read_u32_le(margin_obj + kCurveOwner);
                    const std::uint32_t h = m.read_u16_le(margin_obj + kCurveIndex);
                    const GuestAddress entries = m.read_u32_le(m.read_u32_le(owner_curve + 4U) + 8U);
                    window_flag = m.read_u8(entries + 10U * h + 2U) == 255U ? 1U : 0U;
                    retired += kCurveLookup + kFlagStub;
                }
            }
        } else {
            retired += 1U;
        }
    }

    m.write_u8(pair + kPairWindowFlag, static_cast<std::uint8_t>(window_flag));
    call.set_arg(0, pair);
    const n3::Run curve = call.invoke([](auto& mm) { return n3::fn_0032f0d0(mm); }, kFrameBytes);
    if (curve.declined) {
        return call.decline();
    }
    m.write_u8(pair + kPairCurveFlag, static_cast<std::uint8_t>(call.result()));
    retired += kStoreFlags + curve.retired;
    call.return_value(1U);
    return call.finish(retired + kReturnOne + kReturnEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0032ff80, 980U, 0x9c2d2e3378946d53ULL, "PickPairTargetAndFlags", kCeiling, pick_pair_target_and_flags,
         EngineReturn::Integer)
