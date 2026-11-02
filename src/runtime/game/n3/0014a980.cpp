// 0x0014a980 (a0 = car C, a1 = start quad index, a2 = step (low halfword), a3 = mode (1 = forward), t0 = out point
// quad, t1 = out strip id byte, t2 = out quad index halfword): RouteAheadPoint. Walks along the road strips from the
// car's current strip and quad, following each quad record's link (selector 0 forward, 1 backward, from the mode)
// into the next strip while the route ahead stays clear, and outputs where it stopped: *t1 = strip id (initially
// 0xff), *t2 = quad index (initially 0xffff), and the world point `d` along the stopping quad's segment into *t0.
//
// A strip slot is 8 bytes at 0x0052268c + 8*id; its word at +4 is the strip record Z (Z+0: table of two vertex
// indices per quad; Z+8: quad records of 10 bytes: byte +4/+5 = link strip ids, halfword +6/+8 = link quad indices;
// Z+12: quad count). Callees: WrapOrClampIndex (0x145e70) gives the step budget (at most 20), SegmentRangeFlags
// (0x14a560) classifies a quad range (bit 4 = stop, bit 1 = the next strip is entered without a chase),
// RouteOpenAheadScan (0x148ca0) tells whether a link is open and FollowRouteQuads (0x14a290) advances the car.
// When the first range already has bit 4 the original first backtracks (two loops, one per link direction), and
// only then starts the forward walk. The distance d is the sum of up to three floats: the physics float at
// (u32 car+4)+9268 (when the walk ended in the car's own strip), that float again when car byte +499 is set, and the
// global at 0x01e75594 when car byte +531 is set. The point is the stopping segment's start vertex plus its unit
// direction times d, built with the same VU0 instructions as the original (the w lane is the old VF4's).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <optional>

N3_CALLEE(00145e70)
N3_CALLEE(0014a560)
N3_CALLEE(00148ca0)
N3_CALLEE(0014a290)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kSlotBase = 0x0052268cU;      // slot of strip id: + 8*id; the strip record is the word at +4
constexpr GuestAddress kStripWords = 0x00522690U;    // the same strip record words, indexed by 8*id
constexpr GuestAddress kVertices = 0x0064d354U;      // lui 0x65, lw -11436: 16-byte vertex entries
constexpr GuestAddress kExtraDistance = 0x01e75594U; // lui 0x01e7, lwc1 21908
constexpr GuestAddress kCarSlot = 468U;              // car: current slot
constexpr GuestAddress kCarPhysics = 4U;             // car: physics state pointer
constexpr GuestAddress kPhysicsDistance = 9268U;     // physics state: distance float
constexpr GuestAddress kCarFlagA = 499U;             // car byte: add the distance float again
constexpr GuestAddress kCarFlagB = 531U;             // car byte: add the global float
constexpr std::uint32_t kFrameBytes = 336U;
constexpr std::uint32_t kMaxBudget = 20U;
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the blocks (delay slots included), from the listing.
constexpr std::uint32_t kEntryCost = 29U;          // 0x14a980..0x14a9f8: frame, entry stores, mode test + its slot
constexpr std::uint32_t kPrepareCost = 23U;        // 0x14a9fc..0x14aa54: both calls, budget, first flags test
constexpr std::uint32_t kBacktrackStartCost = 3U;  // 0x14abec..0x14abf4
constexpr std::uint32_t kFlagsLoopCost = 9U;       // 0x14ac70..0x14ac90: SegmentRangeFlags and its test
constexpr std::uint32_t kLinkProbeCost = 19U;      // 0x14abf8..0x14ac40: link read + RouteOpenAheadScan + test
constexpr std::uint32_t kLinkTakeCost = 11U;       // 0x14ac44..0x14ac6c: the link is open, enter its strip
constexpr std::uint32_t kLoopOneEndCost = 7U;      // 0x14ac94..0x14acac: flags after the first loop
constexpr std::uint32_t kLoopTwoStartCost = 3U;    // 0x14acb0..0x14acb8
constexpr std::uint32_t kLoopTwoProbeCost = 18U;   // 0x14acbc..0x14ad00
constexpr std::uint32_t kLoopTwoTakeCost = 19U;    // 0x14ad04..0x14ad4c: enter the strip, flags, test
constexpr std::uint32_t kLoopTwoDoneCost = 2U;     // 0x14ad50..0x14ad54
constexpr std::uint32_t kHeadSkipCost = 1U;        // 0x14aa58
constexpr std::uint32_t kHeadScanCost = 17U;       // 0x14aa5c..0x14aa9c: the first link look-ahead
constexpr std::uint32_t kLoopScanCost = 18U;       // 0x14ab50..0x14ab94: a later look-ahead
constexpr std::uint32_t kWrapCost = 9U;            // 0x14ad58..0x14ad78 / 0x14af94..0x14afb4: wrapped count
constexpr std::uint32_t kProbeCost = 6U;           // 0x14aaa0..0x14aab4 / 0x14ab98..0x14abac
constexpr std::uint32_t kOpenFlagsCost = 12U;      // 0x14aab8..0x14aae4 / 0x14abb0..0x14abdc
constexpr std::uint32_t kClosedCost = 1U;          // 0x14aae8 / 0x14abe0
constexpr std::uint32_t kHeadJoinCost = 1U;        // 0x14aaec
constexpr std::uint32_t kLoopJoinCost = 2U;        // 0x14abe4..0x14abe8
constexpr std::uint32_t kLoopTestCost = 4U;        // 0x14aaf0..0x14aafc
constexpr std::uint32_t kStepCost = 11U;           // 0x14ab00..0x14ab28
constexpr std::uint32_t kEnterStripCost = 9U;      // 0x14ab2c..0x14ab4c: flag 1 set, link taken directly
constexpr std::uint32_t kFollowCost = 15U;         // 0x14ad7c..0x14adb4: FollowRouteQuads and its test
constexpr std::uint32_t kFollowedCost = 17U;       // 0x14adb8..0x14adf8
constexpr std::uint32_t kExitCost = 2U;            // 0x14adfc..0x14ae00
constexpr std::uint32_t kOwnStripCost = 3U;        // 0x14afb8..0x14afc0 (distance float from the physics state)
constexpr std::uint32_t kZeroDistanceCost = 1U;    // 0x14ae04
constexpr std::uint32_t kFlagACheckCost = 3U;      // 0x14ae08..0x14ae10
constexpr std::uint32_t kFlagAAddCost = 4U;        // 0x14ae14..0x14ae20
constexpr std::uint32_t kFlagBCheckCost = 3U;      // 0x14afc4..0x14afcc
constexpr std::uint32_t kFlagBAddCost = 4U;        // 0x14afd0, 0x14ae24..0x14ae28
constexpr std::uint32_t kSegmentCost = 11U;        // 0x14ae2c..0x14ae54
constexpr std::uint32_t kDegenerateZeroCost = 5U;  // 0x14ae58 + 0x14afd8..0x14afe0
constexpr std::uint32_t kDegenerateOtherCost = 4U; // 0x14ae58..0x14ae64
constexpr std::uint32_t kModeTestCost = 4U;        // 0x14ae68..0x14ae74
constexpr std::uint32_t kForwardPointCost = 35U;   // 0x14ae78..0x14af00
constexpr std::uint32_t kBackwardPointCost = 34U;  // 0x14af04..0x14af88
constexpr std::uint32_t kPointJoinCost = 2U;       // 0x14af8c..0x14af90
constexpr std::uint32_t kReturnCost = 11U;         // 0x14afe4..0x14b00c

constexpr GuestAddress slot_of(const std::uint32_t id) { return kSlotBase + (id & 0xffU) * 8U; }

template <class Context>
n3::Run route_ahead_point(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    const std::uint64_t car64 = call.arg64(0);
    const GuestAddress car = call.arg(0);
    const std::uint64_t start64 = call.arg64(1);
    const std::uint32_t step = call.arg(2) & 0xffffU;
    const std::uint32_t mode = call.arg(3);
    const GuestAddress out_point = call.arg(4);
    const GuestAddress out_strip = call.arg(5);
    const GuestAddress out_quad = call.arg(6);

    std::uint32_t retired = kEntryCost;
    m.write_u8(out_strip, 0xffU);
    m.write_u16_le(out_quad, 0xffffU);

    // sel: link direction (0 forward, 1 backward); alt: the other one.
    const std::uint32_t sel = mode == 1U ? 0U : 1U;
    const std::uint32_t alt = 1U - sel;

    // Callee wrappers: the call's own instructions are counted at the call sites, the callee's here. They return
    // the callee's whole v0, or nothing when the callee declined.
    const auto segment_flags = [&](const GuestAddress slot, const std::uint64_t from, const std::uint32_t to)
        -> std::optional<std::uint64_t> {
        call.set_arg64(0, car64);
        call.set_arg(1, slot);
        call.set_arg64(2, from);
        call.set_arg(3, to);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_0014a560(mm); }, kFrameBytes);
        if (r.declined) {
            return std::nullopt;
        }
        retired += r.retired;
        return call.result64();
    };
    const auto route_open = [&](const std::uint32_t id, const std::uint32_t quad, const std::uint32_t count)
        -> std::optional<std::uint64_t> {
        call.set_arg(0, id);
        call.set_arg(1, quad);
        call.set_arg(2, count);
        const n3::Run r = call.invoke([](auto& mm) { return n3::fn_00148ca0(mm); }, kFrameBytes);
        if (r.declined) {
            return std::nullopt;
        }
        retired += r.retired;
        return call.result64();
    };

    // Quad record `q` of the strip in `slot`; its link strip id and link quad index in direction `dir`.
    const auto quad_record = [&](const GuestAddress slot, const std::uint32_t q) {
        return m.read_u32_le(m.read_u32_le(slot + 4U) + 8U) + q * 10U;
    };
    const auto link_id = [&](const GuestAddress record, const std::uint32_t dir) -> std::uint32_t {
        return m.read_u8(record + 4U + dir);
    };
    const auto link_quad = [&](const GuestAddress record, const std::uint32_t dir) -> std::uint32_t {
        return m.read_u16_le(record + 6U + dir * 2U);
    };

    // 0x14a9fc: the step budget, then the first range test.
    const GuestAddress start_slot = m.read_u32_le(car + kCarSlot);   // s3 (kept from the entry)
    GuestAddress s3 = start_slot;
    std::uint32_t s1 = static_cast<std::uint32_t>(start64) & 0xffffU;
    call.set_arg(0, m.read_u32_le(car + kCarSlot));
    call.set_arg64(1, start64);
    call.set_arg(2, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(0U - step))));
    const n3::Run wrap = call.invoke([](auto& mm) { return n3::fn_00145e70(mm); }, kFrameBytes);
    if (wrap.declined) {
        return call.decline();
    }
    retired += kPrepareCost + wrap.retired;
    const std::uint32_t budget = std::min(call.result() & 0xffffU, kMaxBudget);
    auto first = segment_flags(s3, start64, (s1 + 8U) & 0xffffU);
    if (!first) {
        return call.decline();
    }
    std::uint32_t flags = static_cast<std::uint32_t>(*first);

    std::uint32_t s7 = 0U;   // link quad of the quad being looked at
    std::uint32_t fp = 0U;   // link strip id
    GuestAddress s2 = 0U;
    std::uint32_t s0 = 0U;   // quad index of the walk

    if ((flags & 4U) != 0U) {
        // Backtrack. First along the walk direction while the flag range is non-empty and the link is open.
        retired += kBacktrackStartCost;
        std::uint32_t s6 = 0U;
        for (;;) {
            auto f = segment_flags(s3, s1, (s1 + 8U) & 0xffffU);
            if (!f) {
                return call.decline();
            }
            retired += kFlagsLoopCost;
            if (*f == 0U) {
                break;
            }
            const GuestAddress record = quad_record(s3, s1);
            s6 = link_id(record, sel);
            s7 = link_quad(record, sel);
            const auto open = route_open(s6, s7, 1U);
            if (!open) {
                return call.decline();
            }
            retired += kLinkProbeCost;
            if (*open == 0U) {
                break;
            }
            s1 = s7 & 0xffffU;
            m.write_u8(out_strip, static_cast<std::uint8_t>(s6));
            m.write_u16_le(out_quad, static_cast<std::uint16_t>(s7));
            s3 = slot_of(s6);
            retired += kLinkTakeCost;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
        }
        const auto after = segment_flags(s3, s1, (s1 + 8U) & 0xffffU);
        if (!after) {
            return call.decline();
        }
        retired += kLoopOneEndCost;
        if (*after == 0U) {
            retired += kHeadSkipCost;
        } else {
            // Restart from the car's own strip and quad and go the other way.
            s3 = m.read_u32_le(car + kCarSlot);
            s1 = static_cast<std::uint32_t>(start64) & 0xffffU;
            retired += kLoopTwoStartCost;
            for (;;) {
                const GuestAddress record = quad_record(s3, s1);
                s6 = link_id(record, alt);
                const std::uint32_t quad = link_quad(record, alt);
                const auto open = route_open(s6, quad, 1U);
                if (!open) {
                    return call.decline();
                }
                retired += kLoopTwoProbeCost;
                if (*open == 0U) {
                    retired += kHeadSkipCost;
                    break;
                }
                m.write_u8(out_strip, static_cast<std::uint8_t>(s6));
                s1 = quad & 0xffffU;
                s3 = slot_of(s6);
                m.write_u16_le(out_quad, static_cast<std::uint16_t>(quad));
                const auto f = segment_flags(s3, quad, (s1 + 8U) & 0xffffU);
                if (!f) {
                    return call.decline();
                }
                retired += kLoopTwoTakeCost;
                if (*f == 0U) {
                    retired += kLoopTwoDoneCost;
                    break;
                }
                if (retired > kCeiling) {
                    return call.finish(retired);
                }
            }
        }
    } else {
        retired += kHeadSkipCost;
    }

    // Forward walk. look_ahead reads the link of quad `q` in the strip of `slot`, looks at the quads beyond it and
    // returns the flags of that range (4 when the link is closed).
    const auto look_ahead = [&](const GuestAddress slot, const std::uint32_t q, const std::uint32_t scan_cost,
                                const std::uint32_t closed_join, const std::uint32_t open_join)
        -> std::optional<std::uint32_t> {
        const GuestAddress record = quad_record(slot, q);
        s7 = link_quad(record, sel);
        fp = link_id(record, sel);
        const std::uint32_t end = (s7 + 8U) & 0xffffU;
        std::int32_t count = static_cast<std::int32_t>(end - s7);
        retired += scan_cost;
        if (count < 0) {
            count += static_cast<std::int32_t>(m.read_u16_le(m.read_u32_le(kStripWords + fp * 8U) + 12U));
            retired += kWrapCost;
        }
        const auto open = route_open(fp, s7, static_cast<std::uint32_t>(count) & 0xffffU);
        if (!open) {
            return std::nullopt;
        }
        retired += kProbeCost;
        if (*open == 0U) {
            retired += kClosedCost + closed_join;
            return 4U;
        }
        const auto f = segment_flags(slot_of(fp), s7, end);
        if (!f) {
            return std::nullopt;
        }
        retired += kOpenFlagsCost + open_join;
        return static_cast<std::uint32_t>(*f);
    };

    s0 = s1 & 0xffffU;
    s2 = s3;
    {
        const auto f = look_ahead(s3, s0, kHeadScanCost, kHeadJoinCost, kHeadJoinCost);
        if (!f) {
            return call.decline();
        }
        flags = *f;
    }

    while (true) {
        retired += kLoopTestCost;
        if ((flags & 4U) != 0U) {
            break;
        }
        retired += kStepCost;
        const std::uint32_t record_offset = s0 * 10U;
        const GuestAddress records = m.read_u32_le(m.read_u32_le(s2 + 4U) + 8U);
        fp = m.read_u8(records + sel + record_offset + 4U);
        if ((flags & 1U) != 0U) {
            s0 = m.read_u16_le(records + (sel << 1U) + record_offset + 6U);
            s2 = slot_of(fp);
            retired += kEnterStripCost;
        } else {
            const GuestAddress next = slot_of(fp);
            call.set_arg64(0, car64);
            call.set_arg(1, m.read_u32_le(car + kCarSlot));
            call.set_arg(2, next);
            call.set_arg(3, step);
            call.set_arg(4, budget);
            const n3::Run follow = call.invoke([](auto& mm) { return n3::fn_0014a290(mm); }, kFrameBytes);
            if (follow.declined) {
                return call.decline();
            }
            retired += kFollowCost + follow.retired;
            if (call.result64() == 0U) {
                break;
            }
            retired += kFollowedCost;
            const GuestAddress here = m.read_u32_le(m.read_u32_le(s2 + 4U) + 8U);
            s3 = next;
            s1 = m.read_u16_le(here + (sel << 1U) + record_offset + 6U);
            m.write_u8(out_strip, static_cast<std::uint8_t>(fp));
            m.write_u16_le(out_quad, static_cast<std::uint16_t>(s1));
            s0 = m.read_u16_le(m.read_u32_le(m.read_u32_le(s2 + 4U) + 8U) + (sel << 1U) + record_offset + 6U);
            s2 = next;
        }
        const auto f = look_ahead(s2, s0, kLoopScanCost, kLoopJoinCost, kLoopJoinCost);
        if (!f) {
            return call.decline();
        }
        flags = *f;
        if (retired > kCeiling) {
            return call.finish(retired);
        }
    }

    // 0x14adfc: the distance d.
    retired += kExitCost;
    std::uint32_t d = 0U;
    if (s2 == s3) {
        d = m.read_u32_le(m.read_u32_le(car + kCarPhysics) + kPhysicsDistance);
        retired += kOwnStripCost;
    } else {
        retired += kZeroDistanceCost;
    }
    retired += kFlagACheckCost;
    if (m.read_u8(car + kCarFlagA) != 0U) {
        d = fpu::add(d, m.read_u32_le(m.read_u32_le(car + kCarPhysics) + kPhysicsDistance));
        retired += kFlagAAddCost;
    }
    retired += kFlagBCheckCost;
    if (m.read_u8(car + kCarFlagB) != 0U) {
        d = fpu::add(d, m.read_u32_le(kExtraDistance));
        retired += kFlagBAddCost;
    }

    // The stopping segment: vertex indices h1, h2 of quad s1 (a degenerate one steps to a neighbour quad).
    const GuestAddress table = m.read_u32_le(m.read_u32_le(s3 + 4U));
    std::uint32_t q = s1 & 0xffffU;
    retired += kSegmentCost;
    if (m.read_u16_le(table + q * 4U + 2U) == m.read_u16_le(table + q * 4U)) {
        if (q == 0U) {
            q = 1U;
            retired += kDegenerateZeroCost;
        } else {
            q = (q - 1U) & 0xffffU;
            retired += kDegenerateOtherCost;
        }
    }
    const std::uint32_t h1 = m.read_u16_le(table + q * 4U);
    const std::uint32_t h2 = m.read_u16_le(table + q * 4U + 2U);
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const bool forward = mode == 1U;
    // forward: edge = p(h2) - p(h1), result = p(h1) + unit * d; otherwise edge = p(h1) - p(h2), result = p(h2) + ...
    const GuestAddress tip = vertices + ((forward ? h2 : h1) << 4U);
    const GuestAddress base = vertices + ((forward ? h1 : h2) << 4U);
    retired += kModeTestCost + (forward ? kForwardPointCost : kBackwardPointCost) + kPointJoinCost + kReturnCost;

    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);     // vaddw.x vf1,vf0,vf0w
    const Gpr128 one_x = cpu.read_vu0_vector(1U);                    // qmfc2 a3,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, tip));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, base));
    ee_cop2::vadd_vector(cpu, 4U, 1U, 3U, n3::kMaskXyz, true);       // vsub.xyz vf4,vf1,vf3
    ee_cop2::vmul(cpu, 2U, 4U, 4U, n3::kMaskXyz);                    // vmul.xyz vf2,vf4,vf4
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);               // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, one_x);                                 // qmtc2 a3,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    const Gpr128 length = cpu.read_vu0_vector(1U);                   // qmfc2 v1,vf1 ; qmtc2 v1,vf1
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                        // vrsqrt q,vf0w,vf1x
    ee_cop2::vmul_control(cpu, 4U, 4U, n3::kMaskXyz, ee_cop2::kControlQ);  // vmulq.xyz vf4,vf4,q
    Gpr128 scale = length;                                           // mfc1 v1,f1 keeps v1's upper doubleword
    scale.words[0] = d;
    scale.words[1] = (d & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(4U));               // qmfc2 a0,vf4 ; qmtc2 a0,vf1
    cpu.write_vu0_vector(2U, scale);                                 // qmtc2 v1,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, n3::kMaskXyz, 0U);      // vmulx.xyz vf1,vf1,vf2x
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, n3::kMaskXyz, false);      // vadd.xyz vf1,vf1,vf3
    n3::store_quad_any(m, out_point, cpu.read_vu0_vector(1U));       // sqc2 vf1,0(t0)
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0014a980, 1680U, 0x7872d10a9c55100eULL, "RouteAheadPoint", kCeiling, route_ahead_point,
         EngineReturn::Nothing)
