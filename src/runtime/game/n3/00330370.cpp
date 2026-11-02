// 0x00330370 UpdatePairLink (a0 = pair record P): the per-frame state machine of a car-pair link. P links an owner
// car O (P+68) to a target car T (P+72, may be 0) while the two are in a certain relation along the track curve.
// It is the sibling of PickPairTargetAndFlags (0x0032ff80), LinkedPairCurveStep (0x0032f840), UpdateTimedObjectState
// (0x0032fb30), PairCurveWindowCheck (0x0032f0d0) and CheckPairProximityVisible (0x0032fd90) and uses the same
// fields. Returns nothing.
//
// P: +0 state (0..7), +16 anchor quad, +32 follow flag, +33/+34/+35/+36 flag bytes, +44 deadline (-1.0 = none),
// +48 stamp (the clock when the state started), +52 side (+-1.0), +56 aligned distance, +60 distance ahead,
// +64 proximity flag, +65 window flag, +66 curve flag, +68 O, +72 T.
//
// Shape of the original, which is a handful of small helpers copied many times (each copy has its own block counts):
//   gate       the pair is only updated while O is "active"; otherwise the flags at +32/+36 are cleared;
//   timer      s0 = the deadline at +44 has passed (BlinkPatternStep runs first);
//   refresh    the five fields +56..+66 from the calls to the distance helpers and the curve window (copies R0..R3);
//   window     the curve-window flag alone (copies W1, W2);
//   set_state  state, stamp and deadline (stamp + the state's duration, or -1.0 when that duration is -1.0);
//   find       the next target from O's index into the car table (states 1, 2, 7);
//   continuous the anchor quad follows O (states 3 and 6) or T (the state-7 follow) with a VU0 side flip;
//   tail       s1 set -> flags +35/+36; otherwise +35 says whether any car in the table is within range of O.
// The calls to 0x0032f550 (no native) sit on four cold paths and are declined.
//
// Retired counts are the listing's block counts (delay slots included) per path; the callees' counts are added where
// they are called. Every store the original makes is made in the original's order.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0032ee00)
N3_CALLEE(00141ae0)
N3_CALLEE(001419f0)
N3_CALLEE(0032fd90)
N3_CALLEE(0032f0d0)
N3_CALLEE(0032ff80)
N3_CALLEE(0032f840)
N3_CALLEE(0032fb30)
N3_CALLEE(0015ac00)
N3_CALLEE(001419b0)
N3_CALLEE(00156170)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;
using Vec = guest::Gpr128;

constexpr std::uint32_t kFrameBytes = 976U;
constexpr std::uint32_t kCeiling = 20000U;

// Pair record P.
constexpr GuestAddress kState = 0U;
constexpr GuestAddress kAnchor = 16U;
constexpr GuestAddress kFollow = 32U;
constexpr GuestAddress kFlagA = 33U;
constexpr GuestAddress kFlagB = 34U;
constexpr GuestAddress kRangeFlag = 35U;
constexpr GuestAddress kDoneFlag = 36U;
constexpr GuestAddress kDeadline = 44U;
constexpr GuestAddress kStamp = 48U;
constexpr GuestAddress kSide = 52U;
constexpr GuestAddress kAligned = 56U;
constexpr GuestAddress kAhead = 60U;
constexpr GuestAddress kProximity = 64U;
constexpr GuestAddress kWindowFlag = 65U;
constexpr GuestAddress kCurveFlag = 66U;
constexpr GuestAddress kOwner = 68U;
constexpr GuestAddress kTarget = 72U;

// Car records (O, T).
constexpr GuestAddress kHeading = 0U;
constexpr GuestAddress kTrack = 32U;
constexpr GuestAddress kPosition = 48U;
constexpr GuestAddress kCurveArea = 4288U;
constexpr GuestAddress kRangeLimit = 4300U;
constexpr GuestAddress kLinkKind = 4924U;
constexpr GuestAddress kRangeTable = 5584U;
constexpr GuestAddress kCarIndex = 5696U;
constexpr GuestAddress kCurveOwner = 6324U;
constexpr GuestAddress kCurveIndex = 6328U;
constexpr GuestAddress kBusyByte = 6378U;
constexpr GuestAddress kActive = 6420U;
constexpr GuestAddress kSlot = 6572U;
constexpr GuestAddress kHeldByte = 7137U;
constexpr GuestAddress kFlagByte = 7170U;
constexpr GuestAddress kRating = 9168U;
constexpr GuestAddress kSlotWord = 9264U;
constexpr GuestAddress kMargin = 9272U;
constexpr GuestAddress kMode = 9280U;
constexpr GuestAddress kRecordWord = 176U;     // the quad at +176 of a slot table record (word 3 = w)
constexpr GuestAddress kRecordFlag = 5437U;    // byte of a slot table record

// Globals.
constexpr GuestAddress kClock = 0x00522680U;
constexpr GuestAddress kCarTable = 0x0064d388U;
constexpr GuestAddress kCarCount = 0x0064d3a0U;
constexpr GuestAddress kSlotTable = 0x0055efecU;     // 48-byte entries, word 0 = record pointer
constexpr GuestAddress kScan = 0x0064ed5cU;          // signed byte per car record, stride 10192
constexpr std::uint32_t kScanStride = 10192U;
constexpr GuestAddress kDurationScan = 0x01e755ccU;  // D_a
constexpr GuestAddress kDurationB = 0x01e755d0U;
constexpr GuestAddress kDurationC = 0x01e755d4U;
constexpr GuestAddress kDurationHigh = 0x01e755d8U;
constexpr GuestAddress kDurationState1 = 0x01e755e0U;
constexpr GuestAddress kDirectionScale = 0x01e755ecU;
constexpr GuestAddress kDurationState3 = 0x01e755f0U;
constexpr GuestAddress kDurationState4 = 0x01e755f4U;
constexpr std::int32_t kGpWindowScale = -30980;
constexpr std::int32_t kGpWindowLow = -30696;
constexpr std::int32_t kGpTrackScale = -30764;

constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kTwoAndHalf = 0x40200000U;
constexpr std::uint32_t kFifteen = 0x41700000U;
constexpr std::uint32_t kTwenty = 0x41a00000U;
constexpr std::uint32_t kSignBit = 0x80000000U;

// Retired counts of the straight-line pieces.
constexpr std::uint32_t kGateFirst = 13U;        // 0x330370..0x3303a0
constexpr std::uint32_t kGateKindFlag = 3U;      // O[6420] != 0: the kind test
constexpr std::uint32_t kGateKindZero = 4U;      // O[6420] == 0: the kind == 3 test
constexpr std::uint32_t kGateKindRetest = 3U;    // ... and then the nonzero test
constexpr std::uint32_t kInactiveExit = 3U;      // the two flag stores and the jump
constexpr std::uint32_t kEpilogue = 9U;
constexpr std::uint32_t kBlinkBlock = 9U;        // jal BlinkPatternStep ... the first deadline test
constexpr std::uint32_t kDeadlineSet = 5U;       // deadline is a time: the clock test
constexpr std::uint32_t kDeadlineLate = 1U;      // the timer has run out
constexpr std::uint32_t kTargetTest = 3U;
constexpr std::uint32_t kBusyTest = 3U;
constexpr std::uint32_t kDispatch = 4U;
constexpr std::uint32_t kJump = 7U;              // table address, load, jr
constexpr std::uint32_t kCalls = 13U;            // the three distance calls of a refresh and the re-read of T
constexpr std::uint32_t kNullEntry = 6U;         // refresh with T == 0: the five fields cleared
constexpr std::uint32_t kNullAfter = 2U;         // T == 0 when the window is looked at
constexpr std::uint32_t kGateBlock = 32U;        // the slot table gate
constexpr std::uint32_t kPassBlock = 15U;        // the two curve values
constexpr std::uint32_t kHighTest = 6U;
constexpr std::uint32_t kWindowExit = 2U;        // a window result or reject: a move and a jump
constexpr std::uint32_t kRefreshTail = 4U;       // window flag, PairCurveWindowCheck, curve flag
constexpr std::uint32_t kForceProximity = 8U;    // P[64] = 1 and the aligned distance test
constexpr std::uint32_t kThresholdTest = 7U;     // P[56] against 2.5 or 1.0 after a stored P[64]
constexpr std::uint32_t kStateJoin = 1U + 2U;    // v1 = 1; bne v1 (found target paths of states 1, 2, 7)
constexpr std::uint32_t kStateTail = 2U;         // the jump on the equal-to-minus-one path
constexpr std::uint32_t kStateStore = 4U;        // the deadline store and the jump
constexpr std::uint32_t kStateFall = 3U;         // the deadline store when the code falls through
constexpr std::uint32_t kLeaveTwo = 2U;          // a jump and its delay slot
constexpr std::uint32_t kTailKeep = 2U + 3U + 2U + 9U;
constexpr std::uint32_t kTailScanSetup = 2U + 11U + 3U;
constexpr std::uint32_t kTailScanPass = 14U + 1U + 3U;

// One refresh/window copy: the block counts that differ between the six copies in the listing.
struct Copy {
    std::uint32_t entry;   // the test of T before the calls
    std::uint32_t fail;    // the gate failed
    std::uint32_t byte;    // the list lookup
};
constexpr Copy kR0{3U, 2U, 13U};
constexpr Copy kR1{4U, 1U, 13U};
constexpr Copy kR2{4U, 1U, 12U};
constexpr Copy kR3{4U, 1U, 13U};
constexpr Copy kW1{3U, 1U, 12U};
constexpr Copy kW2{3U, 1U, 12U};

// Block counts of the target search up to the store of the target.
struct Find {
    std::uint32_t mode;
    std::uint32_t index;
    std::uint32_t car;
    std::uint32_t state;
};
constexpr Find kFindForward{4U, 4U, 7U, 4U};   // states 1 and 2
constexpr Find kFindLooping{7U, 4U, 7U, 3U};   // state 7 (its first block also stores +34 and +33)

// lq ignores the low four address bits; lqc2/sqc2 fault unless the address is 16-byte aligned and inside memory.
template <class Memory>
bool quad_masked(const Memory& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}
template <class Memory>
bool quad_aligned(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

Vec lane_x(const std::uint32_t bits) { return Vec{{bits, 0U, 0U, 0U}}; }

// VMULx.xyz / VADD.xyz / VSUB.xyz on a quad, w kept.
Vec scale_xyz(const Vec& quad, const std::uint32_t bits) {
    return guest::execute_vu0_macro_broadcast_multiply(quad, quad, lane_x(bits), n3::kMaskXyz, 0U).value;
}
Vec add_xyz(const Vec& left, const Vec& right) {
    return guest::execute_vu0_macro_vector_add_subtract(left, left, right, n3::kMaskXyz, false).value;
}
Vec sub_xyz(const Vec& left, const Vec& right) {
    return guest::execute_vu0_macro_vector_add_subtract(left, left, right, n3::kMaskXyz, true).value;
}
// x of VMUL.xyz, VADDAy.x, VMADDz.x with VF1.x = 1.0: the xyz dot product the way the vector unit rounds it.
std::uint32_t dot_xyz(const Vec& left, const Vec& right) {
    const Vec product = guest::execute_vu0_macro_vector_multiply(left, left, right, n3::kMaskXyz).value;
    const Vec lanes =
        guest::execute_vu0_macro_broadcast_add_subtract(product, product, product, 8U, 1U, false).value;
    const Vec one = lane_x(kOne);
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(one, lanes, one, product, 8U, 2U, false)
        .value.words[0];
}
std::uint32_t copysign_one(const std::uint32_t source) { return (source & kSignBit) | kOne; }

template <class CallT>
class PairLink {
  public:
    using Memory = std::remove_reference_t<decltype(std::declval<CallT&>().memory())>;

    explicit PairLink(CallT& call) : call_(call), m_(call.memory()), p_(call.arg(0U)) {}

    n3::Run run() {
        const GuestAddress owner = u32(p_ + kOwner);
        const std::uint32_t active = u32(owner + kActive);
        const std::uint32_t kind = u32(owner + kLinkKind);

        // Gate: O is active when (O[6420] != 0 && O[4924] != 0) || (O[6420] == 0 && O[4924] == 3).
        bool live = true;
        retired_ = kGateFirst;
        if (active != 0U) {
            retired_ += kGateKindFlag;
            live = kind != 0U;
        } else {
            retired_ += kGateKindZero;
            if (kind != 3U) {
                live = false;
            } else {
                retired_ += kGateKindRetest;
            }
        }
        if (!live) {
            put8(p_ + kFollow, 0U);
            put8(p_ + kDoneFlag, 0U);
            return call_.finish(retired_ + kInactiveExit + kEpilogue);
        }

        // BlinkPatternStep, then the deadline: elapsed when it is a time that the clock has reached.
        call_.set_arg(0U, p_);
        retired_ += kBlinkBlock;
        if (!native([](auto& mm) { return n3::fn_0032ee00(mm); })) {
            return call_.decline();
        }
        const std::uint32_t deadline = u32(p_ + kDeadline);
        if (fpu::eq(kMinusOne, deadline)) {
            elapsed_ = false;
        } else {
            retired_ += kDeadlineSet;
            elapsed_ = !fpu::lt(u32(kClock), deadline);
            if (elapsed_) {
                retired_ += kDeadlineLate;
            }
        }

        // A busy target is left to the original (its call to 0x0032f550 has no native).
        retired_ += kTargetTest;
        if (const GuestAddress target = u32(p_ + kTarget); target != 0U) {
            retired_ += kBusyTest;
            if (u8(target + kBusyByte) != 0U) {
                return call_.decline();
            }
        }

        if (!refresh(kR0)) {
            return call_.decline();
        }

        retired_ += kDispatch;
        const std::uint32_t state = u32(p_ + kState);
        if (state < 8U) {
            retired_ += kJump;
            if (!step(state)) {
                return call_.decline();
            }
        }
        return leave();
    }

  private:
    // --- memory ---------------------------------------------------------------------------------------------
    std::uint32_t u32(const GuestAddress address) { return m_.read_u32_le(address); }
    std::uint32_t u8(const GuestAddress address) { return m_.read_u8(address); }
    void put32(const GuestAddress address, const std::uint32_t value) { m_.write_u32_le(address, value); }
    void put8(const GuestAddress address, const std::uint32_t value) {
        m_.write_u8(address, static_cast<std::uint8_t>(value));
    }
    std::uint32_t gp_float(const std::int32_t offset) {
        return u32(call_.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(offset));
    }
    Vec quad(const GuestAddress address) { return n3::load_quad_any(m_, address); }

    // jal to another native: its count is added when it returns; false when it must be left to the original.
    template <class Callee>
    bool native(Callee&& callee) {
        const n3::Run run = call_.invoke(std::forward<Callee>(callee), kFrameBytes);
        retired_ += run.retired;
        return !run.declined;
    }

    // The slot table record of a car: *(0x55efec + 48 * slot).
    GuestAddress slot_record(const GuestAddress car) {
        const auto slot = static_cast<std::int32_t>(static_cast<std::int8_t>(u8(car + kSlot)));
        return u32(kSlotTable + static_cast<std::uint32_t>(slot) * 48U);
    }

    // The byte of O's curve list: *(*(*(O+6324) + 4) + 8) + 2 + 10 * O[6328].
    std::uint32_t list_byte(const GuestAddress owner) {
        const GuestAddress curve = u32(owner + kCurveOwner);
        const std::uint32_t index = m_.read_u16_le(owner + kCurveIndex);
        const GuestAddress entries = u32(u32(curve + 4U) + 8U);
        return u8(entries + 2U + index * 10U);
    }

    // CurveValue(car + 4288) -> f0 bits (false: declined).
    bool curve_value(const GuestAddress car, std::uint32_t& value) {
        call_.set_arg(0U, car + kCurveArea);
        if (!native([](auto& mm) { return n3::fn_0015ac00(mm); })) {
            return false;
        }
        value = call_.float_result();
        return true;
    }

    // --- refresh and window ---------------------------------------------------------------------------------
    // The curve-window flag: the two cars' slot table w words must agree, and O must be inside the window of T's
    // curve value, with the list byte of O's curve segment equal to 255. flag = 1 only on that last test.
    bool window(const Copy& copy, std::uint32_t& flag) {
        flag = 0U;
        const GuestAddress target = u32(p_ + kTarget);
        if (target == 0U) {
            retired_ += kNullAfter;
            return true;
        }
        const GuestAddress owner = u32(p_ + kOwner);
        retired_ += kGateBlock;
        const GuestAddress target_record = slot_record(target);
        const GuestAddress owner_record = slot_record(owner);
        if (!quad_masked(m_, target_record + kRecordWord) || !quad_masked(m_, owner_record + kRecordWord)) {
            return false;
        }
        const std::uint32_t scale = gp_float(kGpWindowScale);
        const std::uint32_t target_w = quad(target_record + kRecordWord).words[3];
        const Vec owner_quad = quad(owner_record + kRecordWord);
        const std::uint32_t owner_w = owner_quad.words[3];
        gate_quad_ = owner_quad;
        has_gate_quad_ = true;
        const std::uint32_t spread = fpu::abs(fpu::msub(fpu::mul(scale, target_w), scale, owner_w));
        if (!fpu::le(spread, kTwenty)) {
            retired_ += copy.fail;
            return true;
        }

        std::uint32_t target_curve = 0U;
        std::uint32_t owner_curve = 0U;
        if (!curve_value(target, target_curve) || !curve_value(u32(p_ + kOwner), owner_curve)) {
            return false;
        }
        const std::uint32_t behind = fpu::neg(fpu::sub(target_curve, owner_curve));
        const std::uint32_t margin = u32(u32(p_ + kOwner) + kMargin);
        retired_ += kPassBlock;
        if (fpu::lt(behind, fpu::add(gp_float(kGpWindowLow), margin))) {
            retired_ += kWindowExit;
            return true;
        }
        retired_ += kHighTest;
        if (!fpu::le(behind, fpu::add(margin, u32(kDurationHigh)))) {
            retired_ += kWindowExit;
            return true;
        }
        flag = list_byte(u32(p_ + kOwner)) == 255U ? 1U : 0U;
        retired_ += copy.byte + kWindowExit;
        return true;
    }

    // The five fields +56..+66 from O and T.
    bool refresh(const Copy& copy) {
        retired_ += copy.entry;
        if (u32(p_ + kTarget) == 0U) {
            put32(p_ + kAligned, 0U);
            put32(p_ + kAhead, 0U);
            put8(p_ + kProximity, 0U);
            put8(p_ + kWindowFlag, 0U);
            put8(p_ + kCurveFlag, 0U);
            retired_ += kNullEntry;
            return true;
        }
        call_.set_arg(0U, u32(p_ + kOwner));
        call_.set_arg(1U, u32(p_ + kTarget));
        if (!native([](auto& mm) { return n3::fn_00141ae0(mm); })) {
            return false;
        }
        put32(p_ + kAligned, call_.float_result());
        call_.set_arg(0U, u32(p_ + kOwner));
        call_.set_arg(1U, u32(p_ + kTarget));
        if (!native([](auto& mm) { return n3::fn_001419f0(mm); })) {
            return false;
        }
        put32(p_ + kAhead, call_.float_result());
        call_.set_arg(0U, p_);
        if (!native([](auto& mm) { return n3::fn_0032fd90(mm); })) {
            return false;
        }
        put8(p_ + kProximity, call_.result());
        retired_ += kCalls;

        std::uint32_t flag = 0U;
        if (!window(copy, flag)) {
            return false;
        }
        put8(p_ + kWindowFlag, flag);
        call_.set_arg(0U, p_);
        if (!native([](auto& mm) { return n3::fn_0032f0d0(mm); })) {
            return false;
        }
        put8(p_ + kCurveFlag, call_.result());
        retired_ += kRefreshTail;
        return true;
    }

    // --- state helpers --------------------------------------------------------------------------------------
    // State, stamp and deadline. `head` is the block that holds the stores, `falls` whether the code falls through
    // to its continuation instead of jumping to it.
    void set_state(const std::uint32_t state, const std::uint32_t duration, const std::uint32_t head,
                   const bool falls) {
        put32(p_ + kState, state);
        put32(p_ + kStamp, u32(kClock));
        retired_ += head;
        if (fpu::eq(kMinusOne, duration)) {
            put32(p_ + kDeadline, kMinusOne);
            retired_ += kStateTail;
        } else {
            put32(p_ + kDeadline, fpu::add(u32(p_ + kStamp), duration));
            retired_ += falls ? kStateFall : kStateStore;
        }
    }

    // States 3 and 4 by the aligned distance against `limit` (1.0 or 2.5): at or below it the pair holds (state 3).
    void pick_hold(const std::uint32_t limit, const bool falls_hold) {
        if (fpu::le(u32(p_ + kAligned), limit)) {
            set_state(3U, u32(kDurationState3), 9U, falls_hold);
        } else {
            set_state(4U, u32(kDurationState4), 9U, false);
        }
    }

    // The next target from O's car index. Blocks are counted up to and including the one that rejects.
    GuestAddress find(const Find& counts) {
        const GuestAddress owner = u32(p_ + kOwner);
        retired_ += counts.mode;
        if (u32(owner + kMode) != 0U) {
            return 0U;
        }
        const std::uint32_t index = u32(owner + kCarIndex);
        retired_ += counts.index;
        if (index == 0xffffffffU) {
            return 0U;
        }
        const GuestAddress candidate = u32(kCarTable + (index << 2U));
        retired_ += counts.car;
        if (candidate == 0U) {
            return 0U;
        }
        retired_ += counts.state;
        if (u32(candidate + kActive) != 1U) {
            return 0U;
        }
        put32(p_ + kTarget, candidate);
        return candidate;
    }

    // jal 0x0032f840 (a0 = P).
    bool linked_curve_step() {
        call_.set_arg(0U, p_);
        if (has_gate_quad_) {
            Vec v1 = call_.cpu().read_gpr(3U);
            v1.words[2] = gate_quad_.words[2];
            v1.words[3] = gate_quad_.words[3];
            call_.cpu().write_gpr(3U, v1);
        }
        return native([](auto& mm) { return n3::fn_0032f840(mm); });
    }

    // From GetLinkedRecordByte on: the follow flag from the car's curve list. `join` is the block that stores it.
    bool store_follow(const std::uint32_t join) {
        call_.set_arg(0U, u32(p_ + kOwner));
        if (!native([](auto& mm) { return n3::fn_001419b0(mm); })) {
            return false;
        }
        std::uint32_t flag = 0U;
        if (call_.result64() == 0U) {
            retired_ += 1U + join;
        } else {
            retired_ += 7U;
            const GuestAddress owner = u32(p_ + kOwner);
            call_.set_float_arg(0U, kTwo);
            call_.set_arg(0U, owner + kCurveArea);
            if (!native([](auto& mm) { return n3::fn_00156170(mm); })) {
                return false;
            }
            if (call_.result64() != 0U) {
                retired_ += 2U + join;
            } else {
                const GuestAddress car = u32(p_ + kOwner);
                retired_ += 13U;
                if (list_byte(car) == 255U) {
                    flag = 1U;
                } else {
                    retired_ += 3U;
                    flag = u8(car + kFlagByte) == 0U ? 1U : 0U;
                }
                retired_ += 2U + join;
            }
        }
        put8(p_ + kFollow, flag);
        return true;
    }

    // States 3 and 6 while the deadline has not passed: the anchor quad is O's position plus its track vector
    // scaled by the slot word, plus its heading scaled by the side. State 3 recomputes the side from where O is
    // relative to T; state 6 mirrors the stored one.
    bool continuous(const bool mirror) {
        const GuestAddress owner = u32(p_ + kOwner);
        const GuestAddress target = mirror ? 0U : u32(p_ + kTarget);
        const GuestAddress slot_quad = u32(owner + kSlotWord) + kRecordWord;
        if (!quad_aligned(m_, owner + kHeading) || !quad_aligned(m_, owner + kTrack) ||
            !quad_aligned(m_, owner + kPosition) || !quad_aligned(m_, p_ + kAnchor) ||
            !quad_masked(m_, slot_quad) || (!mirror && !quad_aligned(m_, target + kPosition))) {
            return false;
        }
        retired_ += mirror ? 43U : 56U;
        const Vec heading = quad(owner + kHeading);
        const Vec position = quad(owner + kPosition);
        std::uint32_t side = 0U;
        if (mirror) {
            side = copysign_one(fpu::neg(u32(p_ + kSide)));
        } else {
            const Vec away = sub_xyz(position, quad(target + kPosition));
            side = copysign_one(dot_xyz(away, heading));
            put32(p_ + kSide, side);
        }
        const Vec lateral = scale_xyz(scale_xyz(heading, u32(kDirectionScale)), side);
        const std::uint32_t reach = fpu::mul(gp_float(kGpTrackScale), quad(slot_quad).words[3]);
        const Vec anchor = add_xyz(add_xyz(position, scale_xyz(quad(owner + kTrack), reach)), lateral);
        n3::store_quad_any(m_, p_ + kAnchor, anchor);
        return store_follow(kLeaveTwo);
    }

    // State 7 once the window holds: the anchor quad ahead of T along the unit vector from T to O.
    bool follow() {
        const GuestAddress owner = u32(p_ + kOwner);
        const GuestAddress target = u32(p_ + kTarget);
        if (!quad_aligned(m_, target + kPosition) || !quad_aligned(m_, owner + kPosition) ||
            !quad_aligned(m_, owner + kHeading) || !quad_aligned(m_, target + kTrack) ||
            !quad_aligned(m_, target + kHeading) || !quad_aligned(m_, p_ + kAnchor)) {
            return false;
        }
        retired_ += 79U;
        const Vec away = sub_xyz(quad(owner + kPosition), quad(target + kPosition));
        const std::uint32_t inverse =
            ee_cop2::reciprocal_square_root_bits(kOne, dot_xyz(away, away));
        const Vec unit = scale_xyz(away, inverse);

        std::uint32_t target_curve = 0U;
        std::uint32_t owner_curve = 0U;
        if (!curve_value(target, target_curve) || !curve_value(owner, owner_curve)) {
            return false;
        }
        const std::uint32_t behind = fpu::neg(fpu::sub(target_curve, owner_curve));
        const std::uint32_t margin = u32(u32(p_ + kOwner) + kMargin);
        const std::uint32_t ratio = fpu::div(behind, fpu::add(margin, u32(kDurationHigh)));
        const std::uint32_t ahead = fpu::madd(fpu::add(0U, kFifteen), ratio, kFifteen);

        const std::uint32_t side = copysign_one(dot_xyz(unit, quad(u32(p_ + kOwner) + kHeading)));
        const std::uint32_t flip = fpu::mul(fpu::neg(side), kOne);
        Vec anchor = add_xyz(quad(u32(p_ + kTarget) + kPosition), scale_xyz(quad(u32(p_ + kTarget) + kTrack), ahead));
        n3::store_quad_any(m_, p_ + kAnchor, anchor);
        anchor = add_xyz(anchor, scale_xyz(quad(u32(p_ + kTarget) + kHeading), flip));
        n3::store_quad_any(m_, p_ + kAnchor, anchor);
        return store_follow(3U);
    }

    // --- the states -----------------------------------------------------------------------------------------
    bool step(const std::uint32_t state) {
        switch (state) {
        case 0U: return state_new();
        case 1U: return state_search(false);
        case 2U: return state_search(true);
        case 3U: return state_hold();
        case 4U: return state_timed();
        case 5U: return state_reset();
        case 6U: return state_mirror();
        default: return state_loop();
        }
    }

    // State 0: ask PickPairTargetAndFlags for a target; on success the pair starts waiting (state 1).
    bool state_new() {
        retired_ += 6U;
        put8(p_ + kFollow, 0U);
        keep_ = false;
        call_.set_arg(0U, p_);
        if (!native([](auto& mm) { return n3::fn_0032ff80(mm); })) {
            return false;
        }
        if (call_.result64() == 0U) {
            return true;
        }
        put8(p_ + kFlagA, 1U);
        set_state(1U, u32(kDurationState1), 10U, false);
        return true;
    }

    // States 1 and 2: look for a target; with one, refresh and start holding (3) or timing (4); without one,
    // state 1 may step the pair curve and start state 7, state 2 may only step it.
    bool state_search(const bool second) {
        const GuestAddress found = find(kFindForward);
        if (found != 0U) {
            if (!refresh(second ? kR2 : kR1)) {
                return false;
            }
            put8(p_ + kProximity, 1U);
            retired_ += kForceProximity;
            pick_hold(kOne, true);
            retired_ += kStateJoin;
            return true;
        }
        retired_ += kLeaveTwo + 2U;
        if (!second) {
            return search_lost_first();
        }
        return search_lost_second();
    }

    bool search_lost_first() {
        retired_ += 5U;
        put8(p_ + kFlagB, u8(p_ + kCurveFlag));
        keep_ = false;
        if (u8(p_ + kProximity) != 0U) {
            retired_ += kThresholdTest;
            pick_hold(kTwoAndHalf, false);
            return true;
        }
        retired_ += 4U;
        if (!linked_curve_step()) {
            return false;
        }
        if (call_.result64() == 0U) {
            return true;
        }
        retired_ += kW1.entry;
        std::uint32_t flag = 0U;
        if (!window(kW1, flag)) {
            return false;
        }
        retired_ += kLeaveTwo;
        if (flag != 0U) {
            const std::uint32_t owner = u32(p_ + kOwner);
            const std::uint32_t first = u32(kDurationB);
            const std::uint32_t duration =
                fpu::madd(fpu::add(0U, first), u32(owner + kRating), fpu::sub(u32(kDurationC), first));
            set_state(7U, duration, 17U, false);
        }
        return true;
    }

    bool search_lost_second() {
        retired_ += 4U;
        put8(p_ + kFlagB, u8(p_ + kCurveFlag));
        keep_ = false;
        if (!elapsed_) {
            retired_ += 3U;
            if (u8(p_ + kProximity) == 0U) {
                retired_ += 4U;
                return linked_curve_step();
            }
        }
        retired_ += 3U;
        if (u8(p_ + kProximity) == 0U) {
            return false;   // call 0x0032f550(P, 0, 0)
        }
        retired_ += kThresholdTest;
        pick_hold(kTwoAndHalf, false);
        return true;
    }

    // State 3: while the deadline has not passed the anchor follows O; afterwards the target's slot record is
    // released and the pair times out (state 4).
    bool state_hold() {
        retired_ += 4U;
        put8(p_ + kFlagB, u8(p_ + kCurveFlag));
        keep_ = false;
        if (!elapsed_) {
            return continuous(false);
        }
        put8(slot_record(u32(p_ + kTarget)) + kRecordFlag, 0U);
        set_state(4U, u32(kDurationState4), 19U, false);
        return true;
    }

    // State 4: the owner is marked held; the timed object state runs until the deadline.
    bool state_timed() {
        retired_ += 6U;
        put8(p_ + kFlagB, 1U);
        keep_ = false;
        put8(u32(p_ + kOwner) + kHeldByte, 1U);
        if (elapsed_) {
            return false;   // call 0x0032f550(P, 0, 0)
        }
        retired_ += 4U;
        call_.set_arg(0U, p_);
        return native([](auto& mm) { return n3::fn_0032fb30(mm); });
    }

    // State 5: once the deadline passed everything is cleared and the pair returns to state 0.
    bool state_reset() {
        retired_ += 2U;
        keep_ = false;
        if (!elapsed_) {
            return true;
        }
        retired_ += 14U + 6U;
        put32(p_ + kState, 0U);
        put32(p_ + kStamp, u32(kClock));
        put32(p_ + kDeadline, kMinusOne);
        put8(p_ + kFollow, 0U);
        put8(p_ + kFlagA, 0U);
        put8(p_ + kFlagB, 0U);
        put8(u32(p_ + kOwner) + kHeldByte, 0U);
        put32(p_ + kTarget, 0U);
        put32(p_ + kAligned, 0U);
        put32(p_ + kAhead, 0U);
        put8(p_ + kProximity, 0U);
        put8(p_ + kWindowFlag, 0U);
        put8(p_ + kCurveFlag, 0U);
        return true;
    }

    // State 6: the mirror of state 3 (the anchor on the other side) until the deadline.
    bool state_mirror() {
        retired_ += 3U;
        put8(p_ + kFlagB, 0U);
        keep_ = false;
        if (elapsed_) {
            return false;   // call 0x0032f550(P, 0, 0)
        }
        return continuous(true);
    }

    // State 7: look for a target again; without one, the pair either restarts (state 1) or, while the window
    // holds, follows T until it can hold or time out.
    bool state_loop() {
        put8(p_ + kFlagB, 0U);
        put8(p_ + kFlagA, 1U);
        const GuestAddress found = find(kFindLooping);
        if (found != 0U) {
            if (!refresh(kR3)) {
                return false;
            }
            put8(p_ + kProximity, 1U);
            retired_ += kForceProximity;
            pick_hold(kOne, true);
            retired_ += kStateJoin;
            return true;
        }
        retired_ += kLeaveTwo + kLeaveTwo + kLeaveTwo;
        if (elapsed_) {
            set_state(1U, u32(kDurationState1), 9U, false);
            return true;
        }
        retired_ += kW2.entry;
        std::uint32_t flag = 0U;
        if (!window(kW2, flag)) {
            return false;
        }
        retired_ += kLeaveTwo;
        if (flag == 0U) {
            retired_ += 1U + kLeaveTwo;
            set_state(1U, u32(kDurationState1), 9U, false);
            return true;
        }
        if (!follow()) {
            return false;
        }
        retired_ += kLeaveTwo + 3U;
        if (u8(p_ + kProximity) == 0U) {
            return true;
        }
        retired_ += kThresholdTest;
        pick_hold(kOne, false);
        return true;
    }

    // --- leaving --------------------------------------------------------------------------------------------
    // With the keep flag the range flag is set; otherwise it says whether any car record in the table has a range
    // limit of O below O's own limit table entry plus the scan duration.
    n3::Run leave() {
        if (keep_) {
            put8(p_ + kRangeFlag, 1U);
            put8(p_ + kDoneFlag, 0U);
            return call_.finish(retired_ + kTailKeep);
        }
        const GuestAddress owner = u32(p_ + kOwner);
        const auto count = static_cast<std::int32_t>(u32(kCarCount));
        const std::uint32_t duration = u32(kDurationScan);
        bool hit = false;
        retired_ += kTailScanSetup;
        for (std::int32_t i = 0; i < count; ++i) {
            const auto slot = static_cast<std::int32_t>(
                static_cast<std::int8_t>(u8(kScan + static_cast<std::uint32_t>(i) * kScanStride)));
            const std::uint32_t limit =
                fpu::add(u32(owner + kRangeTable + static_cast<std::uint32_t>(slot) * 4U), duration);
            const bool near = fpu::lt(u32(owner + kRangeLimit), limit);
            hit = hit || near;
            retired_ += kTailScanPass + (near ? 1U : 0U);
            if (retired_ > kCeiling) {
                return call_.finish(retired_);
            }
        }
        put8(p_ + kRangeFlag, hit ? 1U : 0U);
        put8(p_ + kDoneFlag, 0U);
        return call_.finish(retired_ + 2U + (hit ? 3U : 1U) + 2U + kEpilogue);
    }

    CallT& call_;
    Memory& m_;
    GuestAddress p_;
    std::uint32_t retired_{};
    bool elapsed_{};
    bool keep_{true};
    // The original's gate loads O's slot quad into $v1 (lq) and nothing after it rewrites the upper doubleword;
    // LinkedPairCurveStep builds a vector from $v1 and stores that upper w word in P[28], so $v1 is handed over.
    Vec gate_quad_{};
    bool has_gate_quad_{};
};

template <class Context>
n3::Run update_pair_link(Context& context) {
    auto call = n3::guest_call(context);
    PairLink<decltype(call)> link{call};
    return link.run();
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00330370, 6068U, 0xea35b5f90fd49033ULL, "UpdatePairLink", kCeiling, update_pair_link, EngineReturn::Nothing)
