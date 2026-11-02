// 0x00220170 (a1 = object X, a2 = record Y): classifies X's bounding sphere against the culling frustum
// (ClassifySphereInCullingFrustum, 0x002202d0), then scans two byte lists of Y for the byte 3: the list at Y+1202 and
// the list at Y+1196, each as long as the signed byte count held in M. M = X+64 is a record that also holds the
// radius at M+20; the sphere is the position quad X+48 (xyz) with that radius in w. Y+4130 becomes 1 when a counted
// entry is 3 or Y+4131 is nonzero, and X+91 receives the classification, which is also returned in v0.
//
// The callee reads the sphere quad from the caller's frame at $sp+80, so the quad is stored there. The frame saves and
// restores are not reproduced (the native never writes $s0-$s2 or $ra), and neither is the original's stale load and
// intermediate store of that quad, which the final store overwrites. The two VU0 operations that publish status flags
// (vadd.xyz and vmulx.w) are repeated in the original's order, because the status flags are sticky and survive the
// callee.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002202d0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 96U;          // addiu $sp, $sp, -96
constexpr GuestAddress kSphereSlot = 80U;           // the sphere quad at the frame's $sp + 80
constexpr GuestAddress kPositionOffset = 48U;       // X+48: position quad
constexpr GuestAddress kRecordOffset = 64U;         // X+64: record M
constexpr GuestAddress kRadiusOffset = 20U;         // M+20: radius bits
constexpr GuestAddress kFirstCountOffset = 12U;     // M+12: signed count of the first list
constexpr GuestAddress kSecondCountOffset = 13U;    // M+13: signed count of the second list
constexpr GuestAddress kFirstListOffset = 1202U;    // Y+1202: first list
constexpr GuestAddress kSecondListOffset = 1196U;   // Y+1196: second list
constexpr GuestAddress kSourceOffset = 4131U;       // Y+4131: byte that also sets the flag
constexpr GuestAddress kFlagOffset = 4130U;         // Y+4130: flag result
constexpr GuestAddress kClassOffset = 91U;          // X+91: classification result
constexpr std::uint8_t kTarget = 3U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included. The callee's count is added where it is called.
constexpr std::uint32_t kHeadCost = 25U;            // 0x00220170..0x002201d0: frame, sphere, jal, the first list's entry
constexpr std::uint32_t kScanTestCost = 3U;         // slt, bne, nop: the loop test before each pass
constexpr std::uint32_t kScanMissCost = 5U;         // addu, lb, beq, nop, addiu: a pass without a 3
constexpr std::uint32_t kScanHitCost = 6U;          // addu, lb, beq, nop, then beq, daddiu: the pass that finds a 3
constexpr std::uint32_t kSecondEntryCost = 4U;      // lb, daddu, beq, addiu: the second list's entry
constexpr std::uint32_t kSourceTestCost = 3U;       // lbu, beq, nop: the flag source test
constexpr std::uint32_t kSourceSetCost = 1U;        // daddiu: the flag set by a nonzero source byte
constexpr std::uint32_t kTailCost = 9U;             // sb, beq, sb; ld, lq, lq, lq, jr, addiu

struct ListScan {
    std::uint32_t retired{};
    bool found{};
};

// Scans the first `count` bytes of a list for kTarget; a count of zero or less runs no pass. The pass that finds the
// target ends the scan early, and a pass without it costs its loop test and its miss.
template <class Memory>
ListScan scan_list(const Memory& m, const GuestAddress list, const std::int32_t count) {
    std::uint32_t retired = 0U;
    for (std::int32_t index = 0; index < count; ++index) {
        retired += kScanTestCost;
        if (retired > kCeiling) {
            return {retired, false};
        }
        if (m.read_u8(list + static_cast<GuestAddress>(index)) == kTarget) {
            return {retired + kScanHitCost, true};
        }
        retired += kScanMissCost;
    }
    return {retired + kScanTestCost, false};
}

template <class Context>
n3::Run classify_object_sphere(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(1);
    const GuestAddress record = call.arg(2);
    const GuestAddress holder = m.read_u32_le(object + kRecordOffset);
    const GuestAddress quad = call.stack_pointer() - kFrameBytes + kSphereSlot;

    // lqc2 $vf1, 48($s2); vadd.xyz $vf2,$vf0,$vf1. The original then stores VF2 to the frame and reloads it into VF1;
    // the copy below is the same value, and VF1.w is replaced by the multiply after it.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kPositionOffset));
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, false);
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(2U));

    // The radius (M+20) goes to VF2 as the original's lw and qmtc2 do; vmulx.w $vf1,$vf0,$vf2x sets the w lane, and
    // the quad goes to the sphere slot. Only VF2's x lane feeds the multiply, and the callee overwrites VF1 and VF2.
    cpu.write_vu0_vector(2U, Gpr128{{m.read_u32_le(holder + kRadiusOffset), 0U, 0U, 0U}});
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);
    n3::store_quad_any(m, quad, cpu.read_vu0_vector(1U));

    call.set_arg(0, quad);
    const n3::Run sphere = call.invoke([](auto& machine) { return n3::fn_002202d0(machine); }, kFrameBytes);
    if (sphere.declined) {
        return call.decline();
    }
    const std::uint32_t classification = call.result();

    const std::int32_t first_count = static_cast<std::int8_t>(m.read_u8(holder + kFirstCountOffset));
    const ListScan first = scan_list(m, record + kFirstListOffset, first_count);
    const std::int32_t second_count = static_cast<std::int8_t>(m.read_u8(holder + kSecondCountOffset));
    const ListScan second = scan_list(m, record + kSecondListOffset, second_count);
    const bool source = m.read_u8(record + kSourceOffset) != 0U;

    std::uint32_t retired = kHeadCost + sphere.retired + first.retired + kSecondEntryCost + second.retired +
                            kSourceTestCost + kTailCost;
    if (source) {
        retired += kSourceSetCost;
    }
    m.write_u8(object + kClassOffset, static_cast<std::uint8_t>(classification));
    const bool flagged = first.found || second.found || source;
    m.write_u8(record + kFlagOffset, flagged ? std::uint8_t{1U} : std::uint8_t{0U});
    call.return_value(classification);
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00220170, 248U, 0x705943b683e75918ULL, "ClassifyObjectSphereAndScan", kCeiling, classify_object_sphere,
         EngineReturn::Integer)
