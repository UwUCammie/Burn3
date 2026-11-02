// 0x002b7a00 (a0 = OUT quads, a2 = SRC quads, a3 = SRC point count, t0 = the plane quads, t1 = plane count NP):
// clips the SRC points against the planes in order with ClipSegmentsToPlane (0x002b78a0). Plane 0 clips the input
// onto list 0 (0x01e84a00); each following plane (1 .. NP-1, while two or more points remain) clips the list before
// onto the other list (the two lists are 1920 bytes apart), so the lists ping-pong. Then the points of the last list
// are copied to OUT in order, except those within eps of a point already copied: a candidate is a duplicate when
// |candidate - kept| - eps is below zero on x, y and z (VU0 sub, abs and sub on VF1 with VF2 = the candidate and
// VF3.x = eps). Returns the number copied (v0). eps is the word at $gp - 31116. The plane counter is 8 bits, as the
// original's andi $v1, $s2, 0xff keeps it.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002b78a0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kScratch0 = 0x01e84a00U;     // lui 0x01e8, addiu 18944
constexpr GuestAddress kScratchStride = 1920U;      // list 1 follows list 0: 120 quads of 16 bytes each
constexpr GuestAddress kQuadBytes = 16U;
constexpr GuestAddress kEpsOffset = static_cast<GuestAddress>(-31116);  // lw $t3, -31116($gp)
constexpr unsigned kTemp3 = 11U;                    // $t3, the eps word on its way into VF3
constexpr std::uint32_t kFrameBytes = 176U;         // the caller's addiu $sp, $sp, -176
// Calls retiring more than this are left to the original (the dry run returns 0 above it).
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kHeadCost = 19U;            // 0x002b7a00-0x002b7a48: frame, the first call, plane = 1
constexpr std::uint32_t kPlaneTestCost = 4U;        // 0x002b7a4c-0x002b7a58: plane < NP
constexpr std::uint32_t kEnoughPointsCost = 3U;     // 0x002b7a5c-0x002b7a64: at least two points
constexpr std::uint32_t kPassCost = 20U;            // 0x002b7a68-0x002b7ab4: second call, plane step, back edge
constexpr std::uint32_t kDedupeSetupCost = 16U;     // 0x002b7ab8-0x002b7af4: eps into VF3, the kept count
constexpr std::uint32_t kPointTestCost = 3U;        // 0x002b7b8c-0x002b7b94: point < count
constexpr std::uint32_t kPointSetupCost = 5U;       // 0x002b7af8-0x002b7b08: the kept scan starts at 0
constexpr std::uint32_t kKeptTestCost = 3U;         // 0x002b7b74-0x002b7b7c: index < kept
constexpr std::uint32_t kPairCost = 13U;            // 0x002b7b0c-0x002b7b3c: the VU0 ops and the x lane test
constexpr std::uint32_t kLaneCost = 5U;             // 0x002b7b40-0x002b7b50 (y lane), 0x002b7b54-0x002b7b64 (z lane)
constexpr std::uint32_t kMatchCost = 2U;            // 0x002b7b68-0x002b7b6c: the duplicate flag
constexpr std::uint32_t kStepCost = 1U;             // 0x002b7b70: index++
constexpr std::uint32_t kFoundTailCost = 3U;        // 0x002b7b80-0x002b7b84 (not taken), 0x002b7b88: point++
constexpr std::uint32_t kCopyTailCost = 11U;        // 0x002b7b80-0x002b7b84 (taken), 0x002b7ba0-0x002b7bbc, 0x002b7b88
constexpr std::uint32_t kReturnCost = 11U;          // 0x002b7b98-0x002b7b9c, 0x002b7bc0-0x002b7be0

// The VU0 side of one pair, in the original's order on the real registers: VF1.xyz = |VF2.xyz - VF1.xyz| - VF3.x
// (vsub.xyz, vabs.xyz, vsubx.xyz), so the MAC flags and the STATUS sticky bits come out as the original's.
void eps_distance(guest::R5900State& cpu) {
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);          // vsub.xyz $vf1,$vf2,$vf1
    ee_cop2::vabs(cpu, 1U, 1U, n3::kMaskXyz);                           // vabs.xyz $vf1,$vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 3U, n3::kMaskXyz, 0U, true);   // vsubx.xyz $vf1,$vf1,$vf3x
}

// The original's `jal 0x002b78a0`: ClipSegmentsToPlane(out, in, points, plane) with the arguments in a0-a3.
template <class Caller>
n3::Run call_clip_segments(Caller& call, const GuestAddress out, const GuestAddress in, const std::uint32_t points,
                           const GuestAddress plane) {
    call.set_arg(0U, out);
    call.set_arg(1U, in);
    call.set_arg(2U, points);
    call.set_arg(3U, plane);
    return call.invoke([](auto& m) { return n3::fn_002b78a0(m); }, kFrameBytes);
}

template <class Context>
n3::Run clip_and_dedupe(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress out = call.arg(0U);
    const GuestAddress src = call.arg(2U);
    const std::uint32_t count = call.arg(3U);
    const GuestAddress planes = call.arg(4U);
    const std::uint32_t plane_count = call.arg(5U);

    // The clip chain: plane 0 clips the input onto list 0, then planes 1.. clip the list before onto the other list
    // while two or more points remain. Each callee count is added as it comes.
    std::uint32_t retired = kHeadCost;
    n3::Run run = call_clip_segments(call, kScratch0, src, count, planes);
    if (run.declined) {
        return call.decline();
    }
    retired += run.retired;
    std::uint32_t points = call.result();
    std::uint32_t current = 0U;
    std::uint32_t plane = 1U;
    for (;;) {
        retired += kPlaneTestCost;
        if (plane >= plane_count) {
            break;
        }
        retired += kEnoughPointsCost;
        if (points < 2U) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kPassCost;
        const std::uint32_t next = 1U - current;
        run = call_clip_segments(call, kScratch0 + next * kScratchStride, kScratch0 + current * kScratchStride,
                                 points, planes + kQuadBytes * plane);
        if (run.declined) {
            return call.decline();
        }
        retired += run.retired;
        points = call.result();
        current = next;
        plane = (plane + 1U) & 0xffU;
    }

    // The dedupe over the last list. eps goes through t3 (lw, sign-extended) into VF3.
    retired += kDedupeSetupCost;
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress candidates = kScratch0 + current * kScratchStride;
    const std::uint32_t eps = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kEpsOffset);
    cpu.write_gpr_sign_extended_32(kTemp3, eps);
    cpu.write_vu0_vector(3U, cpu.read_gpr(kTemp3));

    std::uint32_t kept = 0U;
    for (std::uint32_t point = 0U;; ++point) {
        retired += kPointTestCost;
        if (point >= points) {
            break;
        }
        if (retired > kCeiling) {
            return call.finish(retired);
        }
        retired += kPointSetupCost;
        // Read once: nothing is stored while the kept points are scanned.
        const Gpr128 candidate = n3::load_quad_any(m, candidates + kQuadBytes * point);
        bool duplicate = false;
        for (std::uint32_t index = 0U;; ++index) {
            retired += kKeptTestCost;
            if (index >= kept) {
                break;
            }
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            cpu.write_vu0_vector(2U, candidate);                                       // lqc2 $vf2
            cpu.write_vu0_vector(1U, n3::load_quad_any(m, out + kQuadBytes * index));  // lqc2 $vf1
            eps_distance(cpu);
            const Gpr128 lanes = cpu.read_vu0_vector(1U);
            if (!fpu::lt(lanes.words[0], 0U)) {
                retired += kPairCost + kStepCost;
            } else if (!fpu::lt(lanes.words[1], 0U)) {
                retired += kPairCost + kLaneCost + kStepCost;
            } else if (!fpu::lt(lanes.words[2], 0U)) {
                retired += kPairCost + 2U * kLaneCost + kStepCost;
            } else {
                retired += kPairCost + 2U * kLaneCost + kMatchCost;
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            retired += kFoundTailCost;
        } else {
            retired += kCopyTailCost;
            n3::store_quad_any(m, out + kQuadBytes * kept, candidate);
            ++kept;
        }
    }
    retired += kReturnCost;
    call.return_value(kept);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002b7a00, 484U, 0xca24ab9b6281c909ULL, "ClipSegmentsAndDedupe", kCeiling, clip_and_dedupe,
         EngineReturn::Integer)
