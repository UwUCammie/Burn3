// 0x003bf720 SegmentTreeQuery (a0 = tree T, a1 = segment S, a2 = result record R): the closest hit of the segment
// S (start quad at S+0, end quad at S+16) against the meshes of the kd-tree T, returned as the OR of the leaf
// tests' results (0 when the tree's box is missed or nothing is hit).
//
//   * R+80 is set to 1.0f first (the nearest-t start value ClosestMeshHit works against).
//   * The segment is clipped to T's box (T+16 = low corner, T+0 = high corner) with an x, y, z slab test. A
//     direction component with a tiny exponent (<= 111) only asks that the start lies inside the slab; otherwise
//     the slab gives the entry/exit parameters, which narrow [tmin, tmax] (starting at [0, 1]).
//   * If the box is hit, the kd-tree is walked front to back with an explicit stack of {node, tnear, tfar}
//     entries. A node is a pair of 8-byte descriptors at N + (index << 4), N = u32[T+32]: descriptor 0 holds the
//     plane c0 at +0, the child index (u16) at +4, the split axis at +6 and the kind at +7 (255 = inner);
//     descriptor 1 (at +8) holds the plane c1. The root is a pseudo node whose kind comes from T+40/T+42.
//     For an inner node the segment's positions at the near and far parameters along the split axis are compared
//     with the planes to pick the child pair (descending into one, or both with the far one deferred on the
//     stack and its interval trimmed by the plane). A leaf calls ClosestMeshHit (0x00273bf0) with the mesh
//     u32[T+36] + (index << 4) when the flag u32[T+44] is set; the other leaf test (0x002755e0) is not native.
//   * The direction D = p1 - p0 and its reciprocals (the huge constant u32[gp-31324] for a tiny component) are
//     computed with the listing's VU0 sequence, so VF1..VF3 and the VU0 flags end as in the original.
//
// Declines (the original runs): a leaf with u32[T+44] == 0, a split axis above 2, a stack deeper than the
// original's 64-entry frame area, a declined ClosestMeshHit, or more than kCeiling instructions.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00273bf0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Tree record T.
constexpr GuestAddress kBoxHigh = 0U;        // quad: high corner
constexpr GuestAddress kBoxLow = 16U;        // quad: low corner
constexpr GuestAddress kNodes = 32U;         // u32: node array N
constexpr GuestAddress kMeshes = 36U;        // u32: leaf mesh data D
constexpr GuestAddress kRootKind = 40U;      // u16: low byte is the root's kind
constexpr GuestAddress kRootInner = 42U;     // u16: nonzero makes the root an inner node
constexpr GuestAddress kLeafMode = 44U;      // u32: nonzero = ClosestMeshHit is the leaf test
// Result record R.
constexpr GuestAddress kNearest = 80U;       // float: starts at 1.0
// Node descriptors.
constexpr GuestAddress kPlaneOffset = 0U;
constexpr GuestAddress kChildOffset = 4U;
constexpr GuestAddress kAxisOffset = 6U;
constexpr GuestAddress kKindOffset = 7U;
constexpr GuestAddress kSecondDescriptor = 8U;
constexpr std::uint32_t kInnerKind = 255U;
// Globals relative to $gp.
constexpr std::uint32_t kSegmentProgramSlot = 22808U;          // ClosestMeshHit: lw $v0, -22808($gp)
constexpr std::uint32_t kSegmentTestProgram = 0x004787d0U;   // the program it requires
constexpr std::uint32_t kEpsilonOffset = static_cast<std::uint32_t>(-30884);
constexpr std::uint32_t kHugeOffset = static_cast<std::uint32_t>(-31324);

constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kExponentMask = 0x7f800000U;
constexpr std::uint32_t kTinyBelow = 0x37800001U;   // exponent field 111 or less
constexpr std::uint32_t kFrameBytes = 1536U;
constexpr std::uint32_t kSpillQuad = 336U;          // frame quad the reciprocal lanes are merged into
constexpr std::uint32_t kStackEntries = 64U;        // the original's stack area (704..1471) holds 64 entries
constexpr GuestAddress kRootNode = 0xfffffff0U;     // stands for the pseudo root node in the original's frame
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 33U;           // 0x3bf720..0x3bf7a0: setup, R+80, quads, x direction, tiny test
constexpr std::uint32_t kGateCost = 2U;             // the beq / nop after each slab test
constexpr std::uint32_t kYSetupCost = 25U;          // 0x3bf888..0x3bf8e8
constexpr std::uint32_t kZSetupCost = 26U;          // 0x3bf9ac..0x3bfa0c
constexpr std::uint32_t kHitMark = 1U;              // 0x3bfad4: v0 = 1
constexpr std::uint32_t kDispatchCost = 2U;         // 0x3bfad8: bne v0 / nop
constexpr std::uint32_t kMissTail = 2U + 4U + 2U + 8U;   // 0x3bfae0 beq, 0x3bfc98 test, 0x3bfca8, epilogue
constexpr std::uint32_t kRootFlagCost = 3U;         // 0x3bfcb0: lhu, bne, sh
constexpr std::uint32_t kRootInnerCost = 3U;        // 0x3bfe60: inner root kind
constexpr std::uint32_t kRootLeafCost = 2U;         // 0x3bfcbc: lhu / sb
constexpr std::uint32_t kDirectionHead = 16U;       // 0x3bfcc4..0x3bfd00 (with the x tiny test)
constexpr std::uint32_t kReciprocalTiny = 7U;       // tiny: load the huge constant, merge the lane
constexpr std::uint32_t kReciprocalXDiv = 9U;       // x: divide path
constexpr std::uint32_t kYHead = 13U;               // 0x3bfd44..0x3bfd74
constexpr std::uint32_t kYDiv = 10U;
constexpr std::uint32_t kZHead = 13U;               // 0x3bfdbc..0x3bfdec
constexpr std::uint32_t kZDiv = 10U;
constexpr std::uint32_t kSeedCost = 11U;            // 0x3bfe34..0x3bfe5c: epsilon, root entry
constexpr std::uint32_t kLoopTest = 4U;             // 0x3bfc98
constexpr std::uint32_t kLoopEnd = 2U + 8U;         // 0x3bfca8 and the epilogue
constexpr std::uint32_t kPopCost = 6U;              // 0x3bfae8
constexpr std::uint32_t kNodeLoad = 4U;             // 0x3bfc38
constexpr std::uint32_t kLeafHead = 3U;             // 0x3bfc48
constexpr std::uint32_t kLeafCall = 9U;             // 0x3bfc54..0x3bfc74 around the jal (callee excluded)
constexpr std::uint32_t kInnerHead = 16U;           // 0x3bfb00: axis lookup and the two positions

struct Slab {
    bool pass{};
    std::uint32_t tmin{};
    std::uint32_t tmax{};
    std::uint32_t retired{};
};

constexpr bool tiny(const std::uint32_t bits) noexcept { return (bits & kExponentMask) < kTinyBelow; }

// A tiny direction component: the start must lie inside the slab (c.olt then c.ole, both ordered).
Slab slab_inside(const std::uint32_t p0, const std::uint32_t low, const std::uint32_t high, const std::uint32_t tmin,
                 const std::uint32_t tmax) noexcept {
    if (fpu::lt(p0, low)) {
        return {false, tmin, tmax, 8U};
    }
    if (fpu::le(p0, high)) {
        return {true, tmin, tmax, 10U};
    }
    return {false, tmin, tmax, 11U};
}

// The x slab, which starts the interval at [0, 1] and tests it against 0 and 1 rather than the running ends.
Slab slab_x(const std::uint32_t p0, const std::uint32_t p1, const std::uint32_t low, const std::uint32_t high) noexcept {
    const std::uint32_t d = fpu::sub(p1, p0);
    if (tiny(d)) {
        return slab_inside(p0, low, high, 0U, kOne);
    }
    const std::uint32_t inv = fpu::div(kOne, d);
    const bool backwards = fpu::le(inv, 0U);
    const std::uint32_t tn = fpu::mul(inv, fpu::sub(backwards ? high : low, p0));
    const std::uint32_t tf = fpu::mul(inv, fpu::sub(backwards ? low : high, p0));
    const bool reaches = fpu::le(0U, tf);
    const bool starts = reaches && !fpu::lt(kOne, tn);
    std::uint32_t retired = 8U + (backwards ? 4U : 5U) + 5U + (reaches ? 0U : 1U) + 2U;
    if (reaches) {
        retired += 6U + (fpu::lt(kOne, tn) ? 1U : 0U);
    }
    retired += 3U + (starts ? 8U : 0U);
    if (!starts) {
        return {false, 0U, kOne, retired};
    }
    return {true, ee_cop2::maximum_bits(0U, tn), ee_cop2::minimum_bits(kOne, tf), retired};
}

// The y and z slabs, which narrow the running [tmin, tmax].
Slab slab_yz(const std::uint32_t p0, const std::uint32_t p1, const std::uint32_t low, const std::uint32_t high,
             const std::uint32_t tmin, const std::uint32_t tmax) noexcept {
    const std::uint32_t d = fpu::sub(p1, p0);
    if (tiny(d)) {
        return slab_inside(p0, low, high, tmin, tmax);
    }
    const std::uint32_t inv = fpu::div(kOne, d);
    const bool backwards = fpu::le(inv, 0U);
    const std::uint32_t tn = fpu::mul(inv, fpu::sub(backwards ? high : low, p0));
    const std::uint32_t tf = fpu::mul(inv, fpu::sub(backwards ? low : high, p0));
    const bool reaches = fpu::le(tmin, tf);
    const bool beyond = reaches && fpu::lt(tmax, tn);
    const bool pass = reaches && !beyond;
    std::uint32_t retired = 9U + (backwards ? 4U : 5U) + 3U + (reaches ? 0U : 1U) + 2U;
    if (reaches) {
        retired += 3U + (beyond ? 1U : 0U);
    }
    retired += 3U + (pass ? 3U : 0U);
    if (!pass) {
        return {false, tmin, tmax, retired};
    }
    return {true, ee_cop2::maximum_bits(tmin, tn), ee_cop2::minimum_bits(tmax, tf), retired};
}

// What the stack holds: a node (descriptor address, or kRootNode) and the parameter interval left to walk.
struct Entry {
    GuestAddress node{};
    std::uint32_t tnear{};
    std::uint32_t tfar{};
};

// The direction D = p1 - p0 and its reciprocals, with the listing's VU0 sequence on VF1..VF3 (the frame quads the
// lanes are merged through are locals; the quad at frame+336 is uninitialised stack in the original and is read
// from the same place). Returns the retired count of 0x3bfcc4..0x3bfe34.
struct Direction {
    std::array<std::uint32_t, 3> step{};     // D (VF3 lanes)
    std::array<std::uint32_t, 3> inverse{};  // 1 / D, or the huge constant for a tiny component
    std::uint32_t retired{};
};

template <class Memory>
Direction make_direction(guest::R5900State& cpu, const Memory& m, const GuestAddress segment, const GuestAddress frame,
                         const std::uint32_t huge) {
    // v0 is a 64-bit register whose upper half is whatever the caller left: qmtc2 v0 copies all 128 bits.
    const Gpr128 v0_before = cpu.read_gpr(2U);
    const auto wide = [&](const std::uint32_t bits) {
        return Gpr128{{bits, (bits >> 31U) != 0U ? 0xffffffffU : 0U, v0_before.words[2], v0_before.words[3]}};
    };

    cpu.write_vu0_vector(1U, n3::load_quad_any(m, segment));          // lqc2 vf1,0(s1)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, segment + 16U));    // lqc2 vf2,16(s1)
    ee_cop2::vadd_vector(cpu, 3U, 2U, 1U, n3::kMaskXyz, true);        // vsub.xyz vf3,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);        // vsub.xyz vf1,vf2,vf1
    const Gpr128 step_quad = cpu.read_vu0_vector(1U);                 // sqc2 vf1,320(sp), lq 320(sp)
    Gpr128 merged = n3::load_quad_any(m, frame + kSpillQuad);         // the quad at 336(sp)

    Direction out;
    out.step = {cpu.read_vu0_vector(3U).words[0], cpu.read_vu0_vector(3U).words[1], cpu.read_vu0_vector(3U).words[2]};
    out.retired = kDirectionHead;

    // x: vf1 = merged, vf2 = wide(1/Dx), vaddx.x vf1,vf0,vf2x; merged = vf1.
    const bool tiny_x = tiny(step_quad.words[0]);
    out.inverse[0] = tiny_x ? huge : fpu::div(kOne, step_quad.words[0]);
    out.retired += tiny_x ? kReciprocalTiny : kReciprocalXDiv;
    cpu.write_vu0_vector(1U, merged);
    cpu.write_vu0_vector(2U, wide(out.inverse[0]));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, kMaskX, 0U, false);
    merged = cpu.read_vu0_vector(1U);
    cpu.write_vu0_vector(1U, step_quad);                              // lq 320(sp); qmtc2 vf1

    // y: the tiny path merges into vf1 (loaded from the quad), the divide path into vf2.
    out.retired += kYHead;
    const bool tiny_y = tiny(step_quad.words[1]);
    out.inverse[1] = tiny_y ? huge : fpu::div(kOne, step_quad.words[1]);
    if (tiny_y) {
        cpu.write_vu0_vector(1U, merged);
        cpu.write_vu0_vector(2U, wide(out.inverse[1]));
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, kMaskY, 0U, false);
        merged = cpu.read_vu0_vector(1U);
        out.retired += kReciprocalTiny;
    } else {
        cpu.write_vu0_vector(2U, merged);
        cpu.write_vu0_vector(1U, wide(out.inverse[1]));
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);
        merged = cpu.read_vu0_vector(2U);
        out.retired += kYDiv;
    }
    cpu.write_vu0_vector(1U, step_quad);

    out.retired += kZHead;
    const bool tiny_z = tiny(step_quad.words[2]);
    out.inverse[2] = tiny_z ? huge : fpu::div(kOne, step_quad.words[2]);
    if (tiny_z) {
        cpu.write_vu0_vector(1U, merged);
        cpu.write_vu0_vector(2U, wide(out.inverse[2]));
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, kMaskZ, 0U, false);
        out.retired += kReciprocalTiny;
    } else {
        cpu.write_vu0_vector(2U, merged);
        cpu.write_vu0_vector(1U, wide(out.inverse[2]));
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskZ, 0U, false);
        out.retired += kZDiv;
    }
    out.retired += kSeedCost;
    return out;
}

// plane + (inverse * (edge - position)) folded into one stack interval end: the far end is pushed out by
// eps + inv * (plane - position), the near end pulled in by inv * (plane - position) - eps.
std::uint32_t widen_far(guest::R5900State& cpu, const std::uint32_t end, const std::uint32_t inverse,
                        const std::uint32_t plane, const std::uint32_t position, const std::uint32_t epsilon) {
    const std::uint32_t acc = fpu::add(0U, epsilon);                 // adda.s f3,f2
    cpu.set_fpu_accumulator_bits(acc);
    return fpu::add(end, fpu::madd(acc, inverse, fpu::sub(plane, position)));
}

std::uint32_t narrow_near(const std::uint32_t end, const std::uint32_t inverse, const std::uint32_t plane,
                          const std::uint32_t position, const std::uint32_t epsilon) {
    return fpu::add(end, fpu::sub(fpu::mul(inverse, fpu::sub(plane, position)), epsilon));
}

template <class Context>
n3::Run segment_tree_query(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress tree = call.arg(0);
    const GuestAddress segment = call.arg(1);
    const GuestAddress result = call.arg(2);
    const std::uint64_t segment64 = call.arg64(1);
    const std::uint64_t result64 = call.arg64(2);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    // ClosestMeshHit (the leaf test) declines unless its segment test program is the one loaded in VU0, since
    // uploading it is a DMA the native does not model. Leave such calls to the original before doing any work.
    if (m.read_u32_le(gp - kSegmentProgramSlot) != kSegmentTestProgram) {
        return call.decline();
    }

    m.write_u32_le(result + kNearest, kOne);

    // Phase 1: clip the segment to the tree's box.
    const Gpr128 start = n3::load_quad_any(m, segment);
    const Gpr128 end = n3::load_quad_any(m, segment + 16U);
    const Gpr128 high = n3::load_quad_any(m, tree + kBoxHigh);
    const Gpr128 low = n3::load_quad_any(m, tree + kBoxLow);

    std::uint32_t retired = kEntryCost;
    std::uint32_t tmin = 0U;
    std::uint32_t tmax = kOne;
    bool hit = false;
    const Slab x = slab_x(start.words[0], end.words[0], low.words[0], high.words[0]);
    retired += x.retired + kGateCost;
    if (x.pass) {
        retired += kYSetupCost;
        const Slab y = slab_yz(start.words[1], end.words[1], low.words[1], high.words[1], x.tmin, x.tmax);
        retired += y.retired + kGateCost;
        if (y.pass) {
            retired += kZSetupCost;
            const Slab z = slab_yz(start.words[2], end.words[2], low.words[2], high.words[2], y.tmin, y.tmax);
            retired += z.retired + kGateCost;
            if (z.pass) {
                retired += kHitMark;
                hit = true;
                tmin = z.tmin;
                tmax = z.tmax;
            }
        }
    }
    retired += kDispatchCost;
    if (!hit) {
        call.return_value64(0U);
        return call.finish(retired + kMissTail);
    }

    // Phase 2: walk the kd-tree.
    const bool root_inner = m.read_u16_le(tree + kRootInner) != 0U;
    const std::uint32_t root_kind = root_inner ? kInnerKind : (m.read_u16_le(tree + kRootKind) & 0xffU);
    retired += kRootFlagCost + (root_inner ? kRootInnerCost : kRootLeafCost);

    const std::uint32_t epsilon_address = gp + kEpsilonOffset;
    const Direction dir = make_direction(cpu, m, segment, call.stack_pointer() - kFrameBytes, m.read_u32_le(gp + kHugeOffset));
    retired += dir.retired;
    const std::uint32_t epsilon = m.read_u32_le(epsilon_address);

    std::array<Entry, kStackEntries> stack{};
    stack[0] = {kRootNode, fpu::sub(tmin, epsilon), fpu::add(epsilon, tmax)};
    std::uint32_t top = 1U;   // the original's s4 as an index: entry top - 1 is the last one

    std::uint64_t found = 0U;
    for (;;) {
        retired += kLoopTest;
        if (top == 0U) {
            retired += kLoopEnd;
            break;
        }
        --top;
        retired += kPopCost;
        // Descend from the entry at `top` until it reaches a leaf.
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            retired += kNodeLoad;
            const GuestAddress node = stack[top].node;
            const bool is_root = node == kRootNode;
            const std::uint32_t kind = is_root ? root_kind : m.read_u8(node + kKindOffset);
            const std::uint32_t child = is_root ? 0U : m.read_u16_le(node + kChildOffset);
            if (kind != kInnerKind) {
                retired += kLeafHead;
                if (m.read_u32_le(tree + kLeafMode) == 0U) {
                    return call.decline();   // 0x002755e0 is not native
                }
                retired += kLeafCall;
                call.set_arg64(0, segment64);
                call.set_arg(1, m.read_u32_le(tree + kMeshes) + (child << 4U));
                call.set_arg64(2, result64);
                const n3::Run leaf = call.invoke([](auto& machine) { return n3::fn_00273bf0(machine); }, kFrameBytes);
                if (leaf.declined) {
                    return call.decline();
                }
                retired += leaf.retired;
                found |= call.result64();
                break;
            }

            retired += kInnerHead;
            const GuestAddress pair = m.read_u32_le(tree + kNodes) + (child << 4U);
            const std::uint32_t axis = m.read_u8(pair + kAxisOffset);
            if (axis > 2U) {
                return call.decline();
            }
            const std::uint32_t position = start.words[axis];
            const std::uint32_t step = dir.step[axis];
            const std::uint32_t inverse = dir.inverse[axis];
            Entry& current = stack[top];
            const std::uint32_t acc = fpu::add(0U, position);                       // adda.s f3,f5
            cpu.set_fpu_accumulator_bits(acc);
            const std::uint32_t far_position = fpu::madd(acc, current.tfar, step);
            const std::uint32_t near_position = fpu::madd(acc, current.tnear, step);
            const GuestAddress second = pair + kSecondDescriptor;
            if (!fpu::le(step, 0U)) {
                // Moving up the axis: the first child comes first.
                const std::uint32_t plane1 = m.read_u32_le(pair + kSecondDescriptor + kPlaneOffset);
                retired += 4U;
                if (!fpu::le(plane1, far_position)) {
                    current.node = pair;
                    retired += 2U;
                    continue;
                }
                const std::uint32_t plane0 = m.read_u32_le(pair + kPlaneOffset);
                retired += 4U;
                if (fpu::lt(plane0, near_position)) {
                    current.node = second;
                    retired += 3U;
                    continue;
                }
                retired += 9U;
                if (top + 1U >= kStackEntries) {
                    return call.decline();
                }
                Entry& pushed = stack[top + 1U];
                pushed = {pair, current.tnear, current.tfar};
                if (fpu::lt(plane0, far_position)) {
                    retired += 7U;
                    pushed.tfar = widen_far(cpu, pushed.tfar, inverse, plane0, far_position, epsilon);
                }
                retired += 5U;
                current.node = second;
                if (!fpu::le(plane1, near_position)) {
                    retired += 7U;
                    current.tnear = narrow_near(current.tnear, inverse, plane1, near_position, epsilon);
                }
                ++top;
                continue;
            }

            // Moving down the axis: the second child comes first.
            const std::uint32_t plane0 = m.read_u32_le(pair + kPlaneOffset);
            retired += 4U;
            if (fpu::lt(plane0, far_position)) {
                current.node = second;
                retired += 3U;
                continue;
            }
            const std::uint32_t plane1 = m.read_u32_le(pair + kSecondDescriptor + kPlaneOffset);
            retired += 4U;
            if (!fpu::le(plane1, near_position)) {
                current.node = pair;
                retired += 2U;
                continue;
            }
            retired += 10U;
            if (top + 1U >= kStackEntries) {
                return call.decline();
            }
            Entry& pushed = stack[top + 1U];
            pushed = {second, current.tnear, current.tfar};
            if (!fpu::le(plane1, far_position)) {
                retired += 7U;
                pushed.tfar = widen_far(cpu, pushed.tfar, inverse, plane1, far_position, epsilon);
            }
            retired += 4U;
            current.node = pair;
            if (fpu::lt(plane0, near_position)) {
                retired += 6U;
                current.tnear = narrow_near(current.tnear, inverse, plane0, near_position, epsilon);
            }
            ++top;
        }
    }
    call.return_value64(found);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003bf720, 1976U, 0x1d14874943a1fca6ULL, "SegmentTreeQuery", kCeiling, segment_tree_query,
         EngineReturn::Integer)
