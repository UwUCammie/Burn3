// 0x00144180 CameraFollowDistance: the camera rig (a0) works out the distance value for the car it follows
// (f12 is the value returned when the rig has nothing to adjust). The rig keeps its subject record at +1952 and its
// target record at +1940; each record's slot in the 0x55efec table (48-byte entries, indexed by the signed byte at
// +6572) points at a block whose quadword at +176 has a float in lane 3 (the subject's and target's reference values).
// DistanceAhead (0x001419f0) says how far the target is ahead of the subject; the result starts from the target's
// lane scaled by that distance / 100, clamped to [-1, 1], and never goes below the floor at 0x01e754f0.
//
// Modes (word at +1936):
//   0      clears the "last target" word (+2588) and returns f12.
//   1/2/3  clear +2588; modes 2 and 3 (and mode 1 when the target is ahead) return the target's lane-3 float,
//          any other combination returns f12.
//   4/5    follow modes: mode 5 clamps the subject's lane around the blend and stores it at +2520 (the cold branch
//          that calls 0x0014dd80 is declined); mode 4 runs the curve/gate tests and the lane filter below.
// Retired counts per path are the sums of the listing's blocks (named constants below).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001419f0)
N3_CALLEE(001553a0)
N3_CALLEE(0015a9c0)
N3_CALLEE(0015ac00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 192U;

// Rig fields.
constexpr GuestAddress kMode = 1936U;
constexpr GuestAddress kTarget = 1940U;
constexpr GuestAddress kSubject = 1952U;
constexpr GuestAddress kSettledLane = 2520U;
constexpr GuestAddress kLaneBlend = 2528U;
constexpr GuestAddress kHoldFlag = 2583U;
constexpr GuestAddress kHoldFlagAlt = 2584U;
constexpr GuestAddress kHoldEnabled = 2585U;
constexpr GuestAddress kLastTarget = 2588U;
constexpr GuestAddress kSavedSubjectLane = 2592U;
constexpr GuestAddress kSavedTargetLane = 2596U;
constexpr GuestAddress kRampLow = 2600U;
constexpr GuestAddress kRampHigh = 2604U;
constexpr GuestAddress kAheadFlag = 2608U;
// Record fields.
constexpr GuestAddress kSlotIndex = 6572U;
constexpr GuestAddress kGateBlock = 4288U;
constexpr GuestAddress kGateTrigger = 6420U;
constexpr GuestAddress kSubjectMode = 9280U;
constexpr GuestAddress kSubjectCount = 4996U;
constexpr GuestAddress kTargetBusy = 4574U;
// Globals.
constexpr GuestAddress kSlotTable = 0x0055efecU;     // 48-byte entries, first word is the lane block pointer
constexpr std::uint32_t kSlotStride = 48U;
constexpr GuestAddress kLaneBlockQuad = 176U;
constexpr GuestAddress kFloorBits = 0x01e754f0U;
constexpr GuestAddress kHoldBits = 0x01e754ecU;
constexpr GuestAddress kMinStepBits = 0x01e75598U;
constexpr GuestAddress kScaleBits = 0x01e755bcU;
constexpr GuestAddress kGateBase = 0x01e755c0U;
constexpr GuestAddress kGateScale = 0x01e755c4U;
constexpr GuestAddress kRampStart = 0x00522680U;
constexpr GuestAddress kSuppress = 0x0064d344U;
constexpr std::int32_t kGlobalLimitOffset = -30640;
// Float constants.
constexpr std::uint32_t kZero = 0x00000000U;
constexpr std::uint32_t kNegZero = 0x80000000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr std::uint32_t kTwo = 0x40000000U;
constexpr std::uint32_t kFive = 0x40a00000U;
constexpr std::uint32_t kTen = 0x41200000U;
constexpr std::uint32_t kTwenty = 0x41a00000U;
constexpr std::uint32_t kHundred = 0x42c80000U;

// MAX.S / MIN.S on raw bits (EE sign/magnitude ordering, as guest::ee_cop1::max_s / min_s).
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = static_cast<std::int32_t>(left);
    const auto r = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(l < 0 && r < 0 ? std::min(l, r) : std::max(l, r));
}
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = static_cast<std::int32_t>(left);
    const auto r = static_cast<std::int32_t>(right);
    return static_cast<std::uint32_t>(l < 0 && r < 0 ? std::max(l, r) : std::min(l, r));
}

// The lane-3 float of the quadword at +176 of the block that a record's table slot points at.
template <class Memory>
std::uint32_t slot_lane(Memory& m, const GuestAddress record) {
    const auto slot = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(record + kSlotIndex))));
    const GuestAddress block = m.read_u32_le(kSlotTable + slot * kSlotStride);
    return m.read_u32_le(((block + kLaneBlockQuad) & ~15U) + 12U);
}

// Retired instructions of the listing's blocks.
constexpr std::uint32_t kHead = 15U;          // 0x144180..0x1441b8
constexpr std::uint32_t kIdleTail = 3U;
constexpr std::uint32_t kReturn = 12U;        // 0x144674..0x1446a0
constexpr std::uint32_t kSetup = 35U;         // 0x1441c8..0x144250 around the DistanceAhead call
constexpr std::uint32_t kBlend = 22U;         // 0x144258..0x1442ac

template <class Context>
n3::Run camera_follow_distance(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress self = call.arg(0);
    const std::uint32_t fallback = cpu.read_fpr_bits(12U);  // f24: the value returned when nothing adjusts it

    const std::uint32_t mode = m.read_u32_le(self + kMode);
    if (mode == 0U) {
        m.write_u32_le(self + kLastTarget, 0U);
        cpu.write_fpr_bits(0U, fallback);
        return call.finish(kHead + kIdleTail + kReturn);
    }
    const GuestAddress subject = m.read_u32_le(self + kSubject);
    const GuestAddress target = m.read_u32_le(self + kTarget);
    const std::uint32_t subject_lane = slot_lane(m, subject);  // f23
    const std::uint32_t target_lane = slot_lane(m, target);    // f20

    call.set_arg(0, subject);
    call.set_arg(1, target);
    const n3::Run ahead_run = call.invoke([](auto& mm) { return n3::fn_001419f0(mm); }, kFrameBytes);
    if (ahead_run.declined) {
        return call.decline();
    }
    const std::uint32_t distance = call.float_result();  // f22
    const bool ahead = !fpu::le(distance, kZero);        // s0
    std::uint32_t retired = kHead + kSetup + ahead_run.retired + (ahead ? 0U : 1U);

    // f25: the target's lane pushed by the clamped distance, floored.
    const std::uint32_t scaled = min_bits(max_bits(fpu::div(distance, kHundred), kMinusOne), kOne);
    const std::uint32_t pushed = fpu::add(target_lane, fpu::mul(target_lane, scaled));
    const std::uint32_t pushed_lane = max_bits(m.read_u32_le(kFloorBits), pushed);
    retired += kBlend;

    if (mode == 5U) {
        const std::uint32_t low = fpu::sub(subject_lane, kOne);
        const std::uint32_t high = fpu::add(kOne, subject_lane);
        const std::uint32_t settled = min_bits(max_bits(pushed_lane, low), high);
        m.write_u32_le(self + kSettledLane, settled);
        m.write_u32_le(self + kLastTarget, 0U);
        retired += 10U;  // 0x1442f4..0x144318
        if (m.read_u8(target + kTargetBusy) != 0U) {
            return call.decline();  // jal 0x0014dd80 is not native
        }
        m.write_u8(self + kHoldFlag, 0U);
        m.write_u8(self + kHoldFlagAlt, 0U);
        cpu.write_fpr_bits(0U, m.read_u32_le(self + kSettledLane));
        return call.finish(retired + 4U + kReturn);
    }

    std::uint32_t result = fallback;  // f21
    bool gate_open = true;            // s1
    if (mode != 4U) {
        m.write_u32_le(self + kLastTarget, 0U);
        retired += 3U + 5U;  // 0x1442b0 (mode 4 test), 0x1442bc..0x1442cc
        bool takes_target_lane = false;
        if (mode == 3U) {
            takes_target_lane = true;
            retired += 1U;  // 0x144638
        } else {
            retired += 2U;  // 0x1442d0
            if (!ahead) {
                retired += 3U;  // 0x14462c
                if (mode == 2U) {
                    takes_target_lane = true;
                    retired += 1U;  // 0x144638
                } else {
                    retired += 2U;  // 0x1442d8
                }
            } else {
                retired += 2U;  // 0x1442d8
                retired += 3U;  // 0x1442e0
                if (mode == 1U) {
                    takes_target_lane = true;
                    retired += 2U;  // 0x1442ec
                }
            }
        }
        if (takes_target_lane) {
            cpu.write_fpr_bits(0U, target_lane);
            return call.finish(retired + 13U + kReturn);  // 0x14463c..0x14466c
        }
        cpu.write_fpr_bits(0U, fallback);
        return call.finish(retired + 1U + kReturn);  // 0x144670
    }

    // Mode 4.
    retired += 3U + 6U;  // 0x1442b0 (taken), 0x144354..0x144368
    call.set_arg(0, subject + kGateBlock);
    call.set_float_arg(0, kOne);
    const n3::Run gate_run = call.invoke([](auto& mm) { return n3::fn_001553a0(mm); }, kFrameBytes);
    if (gate_run.declined) {
        return call.decline();
    }
    retired += gate_run.retired;
    if (const std::uint32_t gate = call.result(); gate != 0U) {
        retired += 3U;  // 0x14436c
        if (m.read_u32_le(gate + kGateTrigger) == 0U) {
            retired += 1U;  // 0x144378
            gate_open = false;
        }
    }
    call.set_arg(0, target + kGateBlock);
    const n3::Run curve_run = call.invoke([](auto& mm) { return n3::fn_0015a9c0(mm); }, kFrameBytes);
    if (curve_run.declined) {
        return call.decline();
    }
    retired += 8U + curve_run.retired;  // 0x14437c..0x144398
    if (fpu::eq(kZero, call.float_result())) {
        retired += 4U;  // 0x144488
        if (static_cast<std::int32_t>(m.read_u32_le(subject + kSubjectCount)) > 0) {
            retired += 4U;  // 0x144498
            if (m.read_u32_le(kSuppress) != 0U) {
                retired += 2U;  // 0x1444a8
                gate_open = false;
            }
        }
    }

    retired += 5U;  // 0x14439c..0x1443ac
    if (m.read_u32_le(subject + kSubjectMode) != 1U) {
        // 0x1444b0: the curve values of subject and target and their difference.
        call.set_arg(0, subject + kGateBlock);
        const n3::Run first = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
        if (first.declined) {
            return call.decline();
        }
        const std::uint32_t subject_curve = call.float_result();  // f22
        call.set_arg(0, target + kGateBlock);
        const n3::Run second = call.invoke([](auto& mm) { return n3::fn_0015ac00(mm); }, kFrameBytes);
        if (second.declined) {
            return call.decline();
        }
        const std::uint32_t target_curve = call.float_result();
        const std::uint32_t gap = fpu::sub(target_curve, subject_curve);  // f3
        retired += 10U + first.retired + second.retired;
        if (m.read_u32_le(self + kLastTarget) != target) {
            m.write_u8(self + kAheadFlag, static_cast<std::uint8_t>(ahead));
            retired += 1U;  // 0x1444d8
        } else {
            retired += 9U;  // 0x1444f0..0x144510
            std::uint32_t room = fpu::add(fpu::add(target_lane, kFive), target_curve);  // f2
            const std::uint32_t reach = fpu::add(fpu::add(subject_lane, kZero), subject_curve);
            room = fpu::sub(room, reach);
            bool done = false;
            if (gate_open) {
                retired += 4U;  // 0x144514..0x144520
                if (!fpu::le(gap, kNegZero)) {
                    retired += 5U;  // 0x144524..0x144534
                    result = m.read_u32_le(kHoldBits);
                    m.write_u8(self + kHoldFlag, 1U);
                    done = true;
                }
            }
            if (!done) {
                retired += 6U;  // 0x144538..0x14454c
                if (fpu::lt(gap, kTen)) {
                    retired += 3U;  // 0x144550
                    if (!fpu::le(room, kTen)) {
                        retired += 5U;  // 0x14455c
                        const std::uint32_t limit = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) +
                                                                  static_cast<std::uint32_t>(kGlobalLimitOffset));
                        if (!fpu::le(fpu::sub(subject_lane, target_lane), limit)) {
                            retired += 2U;  // 0x144570
                            m.write_u8(self + kHoldFlag, 1U);
                        }
                    }
                }
                retired += 8U;  // 0x144578..0x144594
                result = max_bits(target_lane, fpu::add(subject_lane, room));
                if (!fpu::le(fpu::abs(gap), m.read_u32_le(kMinStepBits))) {
                    retired += 5U;  // 0x144598
                    if (fpu::le(gap, kZero)) {
                        retired += 9U;  // 0x1445bc
                        result = max_bits(fpu::mul(result, m.read_u32_le(kScaleBits)), fpu::sub(fallback, kTwenty));
                    } else {
                        retired += 4U;  // 0x1445ac
                        result = fpu::mul(result, kTwo);
                    }
                }
            }
        }
        // 0x1444dc: remember the target and the lanes it was judged with.
        m.write_u32_le(self + kLastTarget, target);
        m.write_u32_le(self + kSavedSubjectLane, subject_lane);
        m.write_u32_le(self + kSavedTargetLane, target_lane);
        retired += 5U;
    } else {
        retired += 4U;  // 0x1443b0..0x1443bc
        result = pushed_lane;
        if (m.read_u32_le(self + kLastTarget) != target) {
            retired += 3U;  // 0x1443c0
            std::uint8_t flag = 0U;
            if (ahead) {
                retired += 6U;  // 0x1443cc
                flag = fpu::le(distance, kTen) ? 0U : 1U;
                retired += flag != 0U ? 1U : 2U;  // 0x1443e8 (+ 0x1443e4)
            }
            m.write_u8(self + kAheadFlag, flag);
            m.write_u32_le(self + kLastTarget, target);
            retired += 3U;  // 0x1443ec..0x1443f4
        } else {
            retired += 3U;  // 0x1445e0
            if (m.read_u8(self + kAheadFlag) != 0U) {
                retired += 9U;  // 0x1445ec..0x14460c
                const std::uint32_t gate_limit = fpu::madd(fpu::add(kZero, m.read_u32_le(kGateBase)),
                                                           m.read_u32_le(self + kLaneBlend),
                                                           m.read_u32_le(kGateScale));
                if (gate_open) {
                    retired += 3U;  // 0x144610
                    if (!fpu::le(distance, gate_limit)) {
                        retired += 4U;  // 0x14461c
                        result = m.read_u32_le(kHoldBits);
                        m.write_u8(self + kHoldFlag, 1U);
                    }
                }
            }
        }
    }

    // 0x1443f8: the ramp that widens once the hold flag is set.
    retired += 3U;
    if (m.read_u8(self + kHoldFlag) != 0U) {
        retired += 7U;  // 0x144404
        if (fpu::eq(kMinusOne, m.read_u32_le(self + kRampHigh))) {
            retired += 6U;  // 0x144420
            const std::uint32_t start = m.read_u32_le(kRampStart);
            if (!fpu::le(start, m.read_u32_le(self + kRampLow))) {
                retired += 7U;  // 0x144438
                const std::uint32_t high = fpu::add(kTwo, start);
                const std::uint32_t low = fpu::add(kTwo, high);
                m.write_u32_le(self + kRampHigh, high);
                m.write_u32_le(self + kRampLow, low);
            }
        }
    }
    retired += 3U;  // 0x144454
    if (m.read_u8(self + kHoldEnabled) != 0U) {
        retired += 3U;  // 0x144460
        if (m.read_u8(self + kHoldFlag) == 0U) {
            return call.decline();  // jal 0x001d17b0 is not native
        }
    }
    cpu.write_fpr_bits(0U, result);
    return call.finish(retired + 2U + kReturn);  // 0x144480..0x144484, return
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00144180, 1316U, 0x8398d16b55139006ULL, "CameraFollowDistance", kCeiling, camera_follow_distance,
         EngineReturn::Float)
