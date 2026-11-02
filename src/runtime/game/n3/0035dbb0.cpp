// 0x0035dbb0 InsertIntoSpatialHierarchy (a0 = hierarchy H, a1 = parent cluster or 0, a2 = head of the circular list
// of candidate clusters at this level, a3 = point record P, t0 = level L). Inserts P into a hierarchical grouping:
// at each level it finds the nearest candidate cluster (distance from P plus a per-level slack), makes a new
// cluster from H's free list when none is near enough, links P (leaf level) or the next level's search (inner
// levels) into the chosen cluster, and goes on with the next level, using the chosen cluster's member list as
// that level's candidates. The original does this by calling itself; the work after the call is only the return,
// so it is a loop here and each level adds its share of the original's count. Returns nothing.
//
//   H:  +16 active-cluster list head, +24 free-cluster list link, +32 + 4*L slack threshold of level L (float),
//       +52 last level, +60 number of free clusters.
//   P:  +0 quad (x, y, z, w), +16 owner cluster, +20 list link (next, prev), +48 float.
//   C:  +0 quad (centre xyz, w = radius), +16 parent, +20 list link, +28 active-list link, +36 member count,
//       +40 member-list head (next, prev). A list node is the address of a link; its record starts 20 bytes before.
//
// A candidate inside the threshold when the parent is not 0 is handed to 0x002bf530 by the original (no native
// for it): that call is left to the original (decline) before anything has been changed.
//
// The distance is the VU0 sequence of the original (vf1 gets x = 1.0 from vf0 and is saved and restored around
// the vector work, so its y/z/w lanes survive); vf1-vf3, ACC, Q, the flags and the stale upper half of $v1 are
// what the caller can still see, so the sequence runs on the VU0 state exactly as the listing does.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kSlackPerLevelGp = 0xffff8e90U;  // $gp - 29424: float added per level below the last
// Hierarchy H.
constexpr GuestAddress kActiveHead = 16U;
constexpr GuestAddress kFreeLink = 24U;
constexpr GuestAddress kThresholds = 32U;               // float per level; level 0's is also the base slack
constexpr GuestAddress kLastLevel = 52U;
constexpr GuestAddress kFreeCount = 60U;
// Point P and cluster C (a point's owner/link/member layout is the cluster's, shifted to the same offsets).
constexpr GuestAddress kWidth = 12U;                    // w lane of the quad
constexpr GuestAddress kOwner = 16U;                    // P.owner, C.parent
constexpr GuestAddress kLink = 20U;                     // list link (record + 20)
constexpr GuestAddress kActiveLink = 28U;               // C: link in H's active list
constexpr GuestAddress kActiveLinkSpare = 32U;          // cleared with it for a new cluster
constexpr GuestAddress kMembers = 36U;                  // C: member count
constexpr GuestAddress kMemberHead = 40U;               // C: member list head
constexpr GuestAddress kGapSquared = 48U;               // P/C: (parent radius - own radius)^2
constexpr std::uint32_t kActiveMembers = 2U;            // a cluster joins the active list from this many members

constexpr unsigned kVf1 = 1U;
constexpr unsigned kVf2 = 2U;
constexpr unsigned kVf3 = 3U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSetupCost = 40U + 2U;          // 0x35dbb0..0x35dc4c and the first list test
constexpr std::uint32_t kScanCost = 26U;                // one candidate: distance and threshold test
constexpr std::uint32_t kScanAcceptCost = 4U;           // parent == 0: remember the candidate
constexpr std::uint32_t kScanTestCost = 2U;             // list test after each candidate
constexpr std::uint32_t kPickCost = 2U;                 // 0x35dd0c: any candidate chosen?
constexpr std::uint32_t kKeepCost = 7U;                 // 0x35dd14: candidate radius already large enough
constexpr std::uint32_t kGrowCost = 7U + 6U;            // 0x35dd14 with the radius widened
constexpr std::uint32_t kNewCost = 21U;                 // 0x35ddf0: take a cluster from the free list
constexpr std::uint32_t kNewRootCost = 8U;              // 0x35ded8: new cluster heads the level's list
constexpr std::uint32_t kNewChildCost = 25U;            // 0x35de44: new cluster joins the parent's members
constexpr std::uint32_t kNewWatchCost = 3U;             // 0x35dea8: parent has >= 2 members
constexpr std::uint32_t kNewActivateCost = 9U;          // 0x35deb4: parent joins the active list
constexpr std::uint32_t kLevelCost = 3U;                // 0x35dd48: last level?
constexpr std::uint32_t kDescendCost = 8U + 2U + 14U;   // 0x35def8 call setup, 0x35dde8 and the return
constexpr std::uint32_t kLeafCost = 22U;                // 0x35dd54: link P into the cluster
constexpr std::uint32_t kLeafReturnCost = 1U + 14U;     // 0x35df18, 0x35df1c
constexpr std::uint32_t kLeafWatchCost = 4U;            // 0x35ddac: member count test
constexpr std::uint32_t kLeafActiveCost = 3U;           // 0x35ddbc: already active?
constexpr std::uint32_t kLeafActivateCost = 8U + 2U + 14U;  // 0x35ddc8, 0x35dde8, return
constexpr std::uint32_t kCeiling = 20000U;

// Insert `link` at the front of the circular list whose head is at `head`.
template <class Memory>
void link_front(Memory& m, const GuestAddress head, const GuestAddress link) {
    m.write_u32_le(link, m.read_u32_le(head));
    m.write_u32_le(link + 4U, head);
    m.write_u32_le(m.read_u32_le(head) + 4U, link);
    m.write_u32_le(head, link);
}

// Take `link` out of its circular list.
template <class Memory>
void unlink(Memory& m, const GuestAddress link) {
    m.write_u32_le(m.read_u32_le(link + 4U), m.read_u32_le(link));
    m.write_u32_le(m.read_u32_le(link) + 4U, m.read_u32_le(link + 4U));
}

// The 64-bit register write of mfc1 sign-extends; the upper doubleword of $v1 stays as it was.
inline Gpr128 vector_from_scalar(const std::uint32_t bits, const std::uint32_t upper2, const std::uint32_t upper3) {
    return Gpr128{{bits, (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U, upper2, upper3}};
}

// Distance between the points of two records on VU0 (sqrt of the sum of the squared differences, Y included).
// Leaves the vector state as the original does and the upper half of $v1 as qmfc2 left it.
template <class Memory>
std::uint32_t vector_distance(guest::R5900State& cpu, Memory& m, const GuestAddress point, const GuestAddress entry,
                              std::uint32_t (&upper_v1)[2]) {
    ee_cop2::vadd_broadcast(cpu, kVf1, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const Gpr128 saved = cpu.read_vu0_vector(kVf1);
    cpu.write_vu0_vector(kVf2, n3::load_quad_any(m, point));
    cpu.write_vu0_vector(kVf3, n3::load_quad_any(m, entry));
    ee_cop2::vadd_vector(cpu, kVf1, kVf3, kVf2, n3::kMaskXyz, true);            // vsub.xyz vf1,vf3,vf2
    ee_cop2::vmul(cpu, kVf2, kVf1, kVf1, n3::kMaskXyz);                         // vmul.xyz vf2,vf1,vf1
    ee_cop2::vadda_broadcast(cpu, kVf2, kVf2, 8U, 1U);                          // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(kVf1, saved);
    ee_cop2::vmadd_broadcast(cpu, kVf1, kVf1, kVf2, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    ee_cop2::vsqrt(cpu, kVf1, 0U);                                              // vsqrt q,vf1x
    ee_cop2::vadd_broadcast(cpu, kVf1, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul_control(cpu, kVf1, kVf1, 8U, ee_cop2::kControlQ);             // vmulq.x vf1,vf1,q
    const Gpr128 result = cpu.read_vu0_vector(kVf1);
    upper_v1[0] = result.words[2];
    upper_v1[1] = result.words[3];
    return result.words[0];
}

// (outer radius - inner radius)^2 on VU0 (vsubw.w), as written to a record's +48.
template <class Memory>
std::uint32_t radius_gap_squared(guest::R5900State& cpu, Memory& m, const GuestAddress inner, const GuestAddress outer) {
    cpu.write_vu0_vector(kVf1, n3::load_quad_any(m, inner));
    cpu.write_vu0_vector(kVf2, n3::load_quad_any(m, outer));
    ee_cop2::vadd_broadcast(cpu, kVf1, kVf2, kVf1, n3::kMaskW, 3U, true);       // vsubw.w vf1,vf2,vf1w
    const std::uint32_t gap = cpu.read_vu0_vector(kVf1).words[3];
    return fpu::mul(gap, gap);
}

template <class Context>
n3::Run insert_into_spatial_hierarchy(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress hierarchy = call.arg(0);
    GuestAddress parent = call.arg(1);
    GuestAddress candidates = call.arg(2);
    const GuestAddress point = call.arg(3);
    std::uint32_t level = call.arg(4);
    const std::uint32_t slack_per_level = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kSlackPerLevelGp);
    const Gpr128 v1 = cpu.read_gpr(3U);
    std::uint32_t upper_v1[2] = {v1.words[2], v1.words[3]};
    const std::uint32_t point_width = m.read_u32_le(point + kWidth);
    std::uint32_t retired = 0U;

    for (;;) {
        // Radius of a new cluster at this level: base slack + slack per remaining level + the point's own w.
        const std::uint32_t accumulator = fpu::add(0U, m.read_u32_le(hierarchy + kThresholds));
        cpu.set_fpu_accumulator_bits(accumulator);
        const std::uint32_t remaining =
            fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(hierarchy + kLastLevel) - level));
        const std::uint32_t radius = fpu::add(point_width, fpu::madd(accumulator, slack_per_level, remaining));
        std::uint32_t nearest_limit = m.read_u32_le(hierarchy + kThresholds + 4U * level);
        retired += kSetupCost;

        // Nearest candidate cluster (first on ties) among the level's list.
        GuestAddress best = 0U;
        for (GuestAddress link = m.read_u32_le(candidates); link != candidates;) {
            const GuestAddress entry = link - kLink;
            link = m.read_u32_le(link);
            const std::uint32_t distance = vector_distance(cpu, m, point, entry, upper_v1);
            const std::uint32_t reach = fpu::add(distance, radius);
            retired += kScanCost + kScanTestCost;
            if (fpu::lt(reach, nearest_limit)) {
                if (parent != 0U) {
                    return call.decline();  // 0x002bf530 has no native: the original runs the whole call
                }
                best = entry;
                nearest_limit = reach;
                retired += kScanAcceptCost;
            }
            if (retired > kCeiling) {
                return call.finish(retired);
            }
        }
        retired += kPickCost;

        bool widened = false;
        if (best == 0U) {
            // No candidate near enough: a new cluster from the free list, centred on P.
            const GuestAddress node = m.read_u32_le(hierarchy + kFreeLink);
            best = node - kLink;
            cpu.write_vu0_vector(kVf2, n3::load_quad_any(m, point));
            cpu.write_vu0_vector(kVf1, vector_from_scalar(radius, upper_v1[0], upper_v1[1]));
            unlink(m, node);
            cpu.write_vu0_vector(kVf3, n3::load_quad_any(m, best));
            ee_cop2::vadd_vector(cpu, kVf3, 0U, kVf2, n3::kMaskXyz, false);     // vadd.xyz vf3,vf0,vf2
            ee_cop2::vmul_broadcast(cpu, kVf3, 0U, kVf1, n3::kMaskW, 0U);       // vmulx.w vf3,vf0,vf1x
            n3::store_quad_any(m, best, cpu.read_vu0_vector(kVf3));
            m.write_u32_le(best + kActiveLinkSpare, 0U);
            m.write_u32_le(best + kActiveLink, 0U);
            m.write_u32_le(hierarchy + kFreeCount, m.read_u32_le(hierarchy + kFreeCount) - 1U);
            retired += kNewCost;
            if (parent == 0U) {
                link_front(m, candidates, best + kLink);
                retired += kNewRootCost;
            } else {
                const std::uint32_t members = m.read_u32_le(parent + kMembers) + 1U;
                m.write_u32_le(parent + kMembers, members);
                m.write_u32_le(best + kOwner, parent);
                link_front(m, parent + kMemberHead, best + kLink);
                m.write_u32_le(best + kGapSquared, radius_gap_squared(cpu, m, best, parent));
                retired += kNewChildCost;
                if (static_cast<std::int32_t>(m.read_u32_le(parent + kMembers)) >= static_cast<std::int32_t>(kActiveMembers)) {
                    retired += kNewWatchCost;
                    if (m.read_u32_le(parent + kActiveLink) == 0U) {
                        link_front(m, hierarchy + kActiveHead, parent + kActiveLink);
                        retired += kNewActivateCost;
                    }
                }
            }
        } else {
            // The chosen cluster's radius grows to the candidate's reach when that is larger.
            const std::uint32_t current = m.read_u32_le(best + kWidth);
            if (fpu::le(nearest_limit, current)) {
                retired += kKeepCost;
            } else {
                cpu.write_vu0_vector(kVf1, n3::load_quad_any(m, best));
                cpu.write_vu0_vector(kVf2, vector_from_scalar(nearest_limit, upper_v1[0], upper_v1[1]));
                ee_cop2::vmul_broadcast(cpu, kVf1, 0U, kVf2, n3::kMaskW, 0U);   // vmulx.w vf1,vf0,vf2x
                n3::store_quad_any(m, best, cpu.read_vu0_vector(kVf1));
                widened = true;
                retired += kGrowCost;
            }
        }

        retired += kLevelCost;
        if (level != m.read_u32_le(hierarchy + kLastLevel)) {
            // Next level: the chosen cluster's members are the candidates.
            retired += kDescendCost;
            parent = best;
            candidates = best + kMemberHead;
            ++level;
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            continue;
        }

        // Last level: P becomes a member of the chosen cluster.
        m.write_u32_le(best + kMembers, m.read_u32_le(best + kMembers) + 1U);
        m.write_u32_le(point + kOwner, best);
        link_front(m, best + kMemberHead, point + kLink);
        m.write_u32_le(point + kGapSquared, radius_gap_squared(cpu, m, point, best));
        retired += kLeafCost;
        if (!widened) {
            return call.finish(retired + kLeafReturnCost);
        }
        retired += kLeafWatchCost;
        if (static_cast<std::int32_t>(m.read_u32_le(best + kMembers)) < static_cast<std::int32_t>(kActiveMembers)) {
            return call.finish(retired + kLeafReturnCost);
        }
        retired += kLeafActiveCost;
        if (m.read_u32_le(best + kActiveLink) != 0U) {
            return call.finish(retired + kLeafReturnCost);
        }
        link_front(m, hierarchy + kActiveHead, best + kActiveLink);
        return call.finish(retired + kLeafActivateCost);
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0035dbb0, 932U, 0x29635672286738bbULL, "InsertIntoSpatialHierarchy", kCeiling, insert_into_spatial_hierarchy,
         EngineReturn::Nothing)
