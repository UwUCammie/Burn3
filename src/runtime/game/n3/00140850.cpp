// 0x00140850 (a0 = profile block A, a1 = flag): BuildAvoidanceProfiles. Three 256-entry tables in A are filled
// first: the bytes at A+64 .. A+319 become 0, the bytes at A+320 .. A+575 become 255 and the halfwords at
// A+576 .. A+1087 become 32767. Then RoadSegmentFrame(A, flag) runs. CheckRemainingRangeFits(P + 4288, 2.0) and the
// test P[4300] < P[5700] + G (P = A->[1108], G = the float at 0x01e75600) decide whether the lane projections are
// skipped. When they are not, MarkRouteLanesAroundCursor(A) and ProjectNearbyVehiclesIntoProfile(A) run, and with
// flag != 0 also ProjectLaneMembersIntoProfile4(A) and ProjectNeighboursIntoProfile3(A). SlipOutputBytes(A) always
// runs last. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0013d530)
N3_CALLEE(00156170)
N3_CALLEE(0013df60)
N3_CALLEE(0013eed0)
N3_CALLEE(0013ed50)
N3_CALLEE(0013e920)
N3_CALLEE(0013d910)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The profile block A (a0) and its physics record P (A->[1108]).
constexpr GuestAddress kPhysicsPointer = 1108U;        // A: P
constexpr GuestAddress kRangeRecord = 4288U;           // P: argument of CheckRemainingRangeFits
constexpr GuestAddress kNearLimit = 4300U;             // P: float
constexpr GuestAddress kFarLimit = 5700U;              // P: float
constexpr GuestAddress kGainAddress = 0x01e75600U;     // G: float (lui 0x1e7, +22016)

// The three tables (A-relative). Lane i in 0..255 owns one byte of the first two and one halfword of the third.
// The ranges do not overlap, so the order of the stores does not matter.
constexpr std::uint32_t kLanes = 256U;
constexpr GuestAddress kZeroTable = 64U;               // u8 [64 + i] = 0
constexpr GuestAddress kFullTable = 320U;              // u8 [320 + i] = 255
constexpr GuestAddress kLimitTable = 576U;             // s16 [576 + 2i] = 32767
constexpr std::uint8_t kZeroByte = 0U;
constexpr std::uint8_t kFullByte = 255U;
// The original computes this as 32767.0 * 500.0 / 500.0 (cvt.s.w, max.s/min.s on the constants 500 and -500,
// mul.s, div.s, cvt.w.s). Every step is exact (16383500 < 2^24), so each entry is 32767.
constexpr std::uint16_t kLimitEntry = 32767U;

constexpr std::uint32_t kFrameBytes = 64U;             // the caller's frame: every jal here lowers $sp by it
constexpr std::uint32_t kTwo = 0x40000000U;            // 2.0f, f12 for CheckRemainingRangeFits
constexpr std::uint32_t kCeiling = 200000U;

// Retired counts of the straight-line blocks, delay slots included (addresses from the listing).
constexpr std::uint32_t kInitCost = 18U + 256U * 16U;  // 0x00140850..0x00140894, then 256 passes of 16
constexpr std::uint32_t kRoadCallCost = 3U;            // 0x001408d8..0x001408e0: a0, jal 0x0013d530, a1
constexpr std::uint32_t kRangeCallCost = 5U;           // 0x001408e4..0x001408f4: P, f12, jal 0x00156170, a0
constexpr std::uint32_t kRangeTestCost = 2U;           // 0x001408f8..0x001408fc: beq v0 and its delay slot
constexpr std::uint32_t kRangeSkipCost = 1U;           // 0x00140900: skip = true, when v0 != 0
constexpr std::uint32_t kLimitTestCost = 9U;           // 0x00140904..0x00140924: P reloaded, G, add, compare, bc1f
constexpr std::uint32_t kLimitSkipCost = 1U;           // 0x00140928: skip = true, when the compare holds
constexpr std::uint32_t kJoinCost = 2U;                // 0x0014092c..0x00140930: bne skip and its delay slot
constexpr std::uint32_t kLaneCallsCost = 6U;           // 0x00140934..0x00140948: jal, jal, beq flag, delay slot
constexpr std::uint32_t kLaneFlagCost = 4U;            // 0x0014094c..0x00140958: jal, jal (flag != 0)
constexpr std::uint32_t kFinishCost = 8U;              // 0x0014095c..0x00140978: jal 0x0013d910, epilogue, jr

template <class Context>
n3::Run build_avoidance_profiles(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress profiles = call.arg(0);
    const std::uint64_t entry_a0 = call.arg64(0);
    const std::uint64_t flag = call.arg64(1);

    for (std::uint32_t lane = 0U; lane < kLanes; ++lane) {
        m.write_u8(profiles + kZeroTable + lane, kZeroByte);
        m.write_u8(profiles + kFullTable + lane, kFullByte);
        m.write_u16_le(profiles + kLimitTable + 2U * lane, kLimitEntry);
    }
    std::uint32_t retired = kInitCost;

    call.set_arg64(0, entry_a0);
    call.set_arg64(1, flag);
    const n3::Run road = call.invoke([](auto& machine) { return n3::fn_0013d530(machine); }, kFrameBytes);
    if (road.declined) {
        return call.decline();
    }
    retired += kRoadCallCost + road.retired;

    const GuestAddress physics = m.read_u32_le(profiles + kPhysicsPointer);
    call.set_arg(0, physics + kRangeRecord);
    call.set_float_arg(0, kTwo);
    const n3::Run range = call.invoke([](auto& machine) { return n3::fn_00156170(machine); }, kFrameBytes);
    if (range.declined) {
        return call.decline();
    }
    retired += kRangeCallCost + range.retired + kRangeTestCost;
    bool skip = call.result() != 0U;
    if (skip) {
        retired += kRangeSkipCost;
    }

    // The physics pointer is loaded again: the calls above may have changed A->[1108].
    const GuestAddress physics_again = m.read_u32_le(profiles + kPhysicsPointer);
    const std::uint32_t gain = m.read_u32_le(kGainAddress);
    const std::uint32_t far_limit = fpu::add(m.read_u32_le(physics_again + kFarLimit), gain);
    retired += kLimitTestCost;
    if (fpu::lt(m.read_u32_le(physics_again + kNearLimit), far_limit)) {
        skip = true;
        retired += kLimitSkipCost;
    }
    retired += kJoinCost;

    if (!skip) {
        call.set_arg64(0, entry_a0);
        const n3::Run lanes = call.invoke([](auto& machine) { return n3::fn_0013df60(machine); }, kFrameBytes);
        if (lanes.declined) {
            return call.decline();
        }
        call.set_arg64(0, entry_a0);
        const n3::Run nearby = call.invoke([](auto& machine) { return n3::fn_0013eed0(machine); }, kFrameBytes);
        if (nearby.declined) {
            return call.decline();
        }
        retired += kLaneCallsCost + lanes.retired + nearby.retired;

        if (flag != 0U) {
            call.set_arg64(0, entry_a0);
            const n3::Run members = call.invoke([](auto& machine) { return n3::fn_0013ed50(machine); }, kFrameBytes);
            if (members.declined) {
                return call.decline();
            }
            call.set_arg64(0, entry_a0);
            const n3::Run neighbours = call.invoke([](auto& machine) { return n3::fn_0013e920(machine); }, kFrameBytes);
            if (neighbours.declined) {
                return call.decline();
            }
            retired += kLaneFlagCost + members.retired + neighbours.retired;
        }
    }

    call.set_arg64(0, entry_a0);
    const n3::Run slip = call.invoke([](auto& machine) { return n3::fn_0013d910(machine); }, kFrameBytes);
    if (slip.declined) {
        return call.decline();
    }
    retired += kFinishCost + slip.retired;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00140850, 300U, 0x85730fc3799eb15fULL, "BuildAvoidanceProfiles", kCeiling, build_avoidance_profiles,
         EngineReturn::Nothing)
