// 0x0021d080 (a0 = P, one object with 17 region slots): UpdateRegionVisibility. P+88 holds the u16 word vis[s] of each
// slot s (0..16) and P+124 the pointer to its region R (0 = no region). For each viewer v (0..3) and slot s the region
// is classified against the viewer and its 2-bit code is written to bits 2v..2v+1 of vis[s]; a null region clears
// those bits. The classification looks the region up with LaunchVu0Program14A (0x00274af0) on the viewer record and
// the region's base quads: result 0 gives code 0, result 2 gives code 1. Any other result runs the detail test: the
// eight corners (the top four lifted by base quad 0's height) become a volume (BuildRegionVolume, 0x003d7f70), and a
// sphere of radius 100 at the viewer's position is tested against its planes (ClassifySphereInView, 0x002bf2e0):
// code 2 outside, 3 otherwise. Returns nothing.
//
// The caller's frame is sp - 688. BuildRegionVolume reads the eight corner quads at frame + 416, and
// ClassifySphereInView reads the sphere at frame + 672 and the volume at frame + 192 that BuildRegionVolume fills. The
// corner and sphere words are real stores here; the rest of the listing's frame traffic is stack-only and skipped.
//
// The retired count depends on the lookup's result, which only a live VU0 run produces. The lookup declines in a
// nested dry run (see 00274af0.cpp), so call_cost declines every call with a region in some slot and the original runs
// it. Calls whose slots are all null are admitted; their count is 218 + 17 per slot visit (1374 for 68 visits).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

N3_CALLEE(00274af0)
N3_CALLEE(003d7f70)
N3_CALLEE(002bf2e0)

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kRegionCount = 17U;
constexpr std::uint32_t kViewerCount = 4U;
constexpr GuestAddress kVisibilityOffset = 88U;      // u16 vis[17]
constexpr GuestAddress kRegionOffset = 124U;         // u32 region[17]
constexpr GuestAddress kViewerRecord = 0x00666330U;  // viewer 0's record (0x00665ec0 + 1136)
constexpr GuestAddress kViewerStride = 560U;
constexpr GuestAddress kViewerPosition = 208U;       // the position quad inside a viewer record
constexpr std::uint32_t kRadiusBits = 0x42c80000U;   // 100.0f: the test sphere's radius (f20)
constexpr std::uint8_t kMaskY = 4U;

constexpr std::uint32_t kFrameBytes = 688U;
constexpr GuestAddress kVolumeAt = 192U;             // frame + 192: BuildRegionVolume's 224-byte output
constexpr GuestAddress kCornersAt = 416U;            // frame + 416: the eight corner quads
constexpr GuestAddress kSphereAt = 672U;             // frame + 672: the test sphere quad

constexpr std::uint32_t kCeiling = 20000U;

// Retired counts of the listing's blocks, delay slots included. A jal and its delay slot are the caller's own
// instructions (the callee's count comes from its Run). Block addresses are in brackets.
constexpr std::uint32_t kPrologueCost = 12U;         // [0x0021d080..0x0021d0ac]
constexpr std::uint32_t kClearCostPerSlot = 7U;      // [0x0021d0b0..0x0021d0c8], once per slot
constexpr std::uint32_t kRadiusSetupCost = 3U;       // [0x0021d0cc..0x0021d0d4]
constexpr std::uint32_t kViewerHeadCost = 14U;       // [0x0021d0d8..0x0021d10c]
constexpr std::uint32_t kViewerTailCost = 4U;        // [0x0021d18c..0x0021d198]
constexpr std::uint32_t kEpilogueCost = 12U;         // [0x0021d19c..0x0021d1a0] and [0x0021d2ac..0x0021d2d0]
constexpr std::uint32_t kSlotHeadCost = 6U;          // [0x0021d110..0x0021d124]
constexpr std::uint32_t kNullSlotCost = 7U;          // [0x0021d290..0x0021d2a8]
constexpr std::uint32_t kLookupCost = 5U;            // [0x0021d128..0x0021d138]: addiu, jal, delay slot, beq, nop
constexpr std::uint32_t kZeroCost = 2U;              // [0x0021d1a4..0x0021d1a8]
constexpr std::uint32_t kTwoTestCost = 3U;           // [0x0021d13c..0x0021d144]
constexpr std::uint32_t kOneCost = 1U;               // [0x0021d148]
constexpr std::uint32_t kDetailCost = 53U;           // [0x0021d1ac..0x0021d27c]
constexpr std::uint32_t kDetailSelectCost = 2U;      // [0x0021d280..0x0021d28c]
constexpr std::uint32_t kStoreCost = 12U;            // [0x0021d14c..0x0021d178]
constexpr std::uint32_t kSlotTailCost = 4U;          // [0x0021d17c..0x0021d188]

// The detail test's outcome: its code (2 or 3) and the callees' retired counts, or a decline.
struct Detail {
    bool declined{};
    std::uint32_t code{};
    std::uint32_t retired{};
};

// The listing's lift of one base quad: vf1 = quad, then vf1.xyz += vf2.xyz, where vf2 = (0, base0.w, 0, _).
Gpr128 lift_quad(guest::R5900State& cpu, const Gpr128& quad) {
    cpu.write_vu0_vector(1U, quad);
    ee_cop2::vadd_vector(cpu, 1U, 1U, 2U, n3::kMaskXyz, false);
    return cpu.read_vu0_vector(1U);
}

// The detail test for a region whose lookup result is neither 0 nor 2. The VU0 operations run in the listing's order,
// so the VU0 state the callees see, and the state left behind, are the original's.
template <class CallType>
Detail classify_detail(CallType& call, const GuestAddress frame, const GuestAddress record,
                       const GuestAddress region) {
    auto& m = call.memory();
    auto& cpu = call.cpu();

    // The sphere: vf3 = the viewer position; w = 100.0 (mfc1 f20 into v0, qmtc2 into vf1, vmulx.w). The listing's
    // stale load of the sphere's old quad into vf4 is overwritten lane by lane, so it is not repeated.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, record + kViewerPosition));
    call.return_value(kRadiusBits);
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));
    ee_cop2::vadd_vector(cpu, 4U, 0U, 3U, n3::kMaskXyz, false);
    ee_cop2::vmul_broadcast(cpu, 4U, 0U, 1U, n3::kMaskW, 0U);
    n3::store_quad_any(m, frame + kSphereAt, cpu.read_vu0_vector(4U));

    // The corners: base quads 0..3 (region + 16 .. + 64) and their lifted copies, lifted by base quad 0's height.
    const Gpr128 base0 = n3::load_quad_any(m, region + 16U);
    const Gpr128 base1 = n3::load_quad_any(m, region + 32U);
    const Gpr128 base2 = n3::load_quad_any(m, region + 48U);
    const Gpr128 base3 = n3::load_quad_any(m, region + 64U);
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);
    cpu.write_vu0_vector(1U, base0);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 3U, false);
    const Gpr128 top0 = lift_quad(cpu, base0);
    const Gpr128 top1 = lift_quad(cpu, base1);
    const Gpr128 top2 = lift_quad(cpu, base2);
    const Gpr128 top3 = lift_quad(cpu, base3);
    n3::store_quad_any(m, frame + kCornersAt, base0);
    n3::store_quad_any(m, frame + kCornersAt + 16U, base3);
    n3::store_quad_any(m, frame + kCornersAt + 32U, base2);
    n3::store_quad_any(m, frame + kCornersAt + 48U, base1);
    n3::store_quad_any(m, frame + kCornersAt + 64U, top0);
    n3::store_quad_any(m, frame + kCornersAt + 80U, top3);
    n3::store_quad_any(m, frame + kCornersAt + 96U, top2);
    n3::store_quad_any(m, frame + kCornersAt + 112U, top1);

    // BuildRegionVolume(corners, volume), then ClassifySphereInView(sphere, volume).
    call.set_arg(0U, frame + kCornersAt);
    call.set_arg(1U, frame + kVolumeAt);
    const n3::Run volume = call.invoke([](auto& machine) { return n3::fn_003d7f70(machine); }, kFrameBytes);
    if (volume.declined) {
        return Detail{true, 0U, 0U};
    }
    call.set_arg(0U, frame + kSphereAt);
    call.set_arg(1U, frame + kVolumeAt);
    const n3::Run sphere = call.invoke([](auto& machine) { return n3::fn_002bf2e0(machine); }, kFrameBytes);
    if (sphere.declined) {
        return Detail{true, 0U, 0U};
    }
    return Detail{false, call.result() != 0U ? 3U : 2U, volume.retired + sphere.retired};
}

// One viewer and one region slot: the lookup, the branch on its result, the detail test when needed, and the two
// stores to the visibility word (the original clears the viewer's bits, then ORs in the code and reads the word back).
// Returns the slot's retired count with the callees' counts included, or declines when a callee does.
template <class CallType>
n3::Run visit_slot(CallType& call, const GuestAddress object, const std::uint32_t slot, const GuestAddress frame,
                   const GuestAddress record, const unsigned shift) {
    auto& m = call.memory();
    const GuestAddress visibility = object + kVisibilityOffset + 2U * slot;
    const auto keep = static_cast<std::uint16_t>(~(3U << shift));
    const GuestAddress region = m.read_u32_le(object + kRegionOffset + 4U * slot);
    if (region == 0U) {
        m.write_u16_le(visibility, static_cast<std::uint16_t>(m.read_u16_le(visibility) & keep));
        return {kSlotHeadCost + kNullSlotCost + kSlotTailCost, false};
    }

    // The lookup: a0 = the viewer record, a1 = the region's first base quad.
    call.set_arg(0U, record);
    call.set_arg(1U, region + 16U);
    const n3::Run lookup = call.invoke([](auto& machine) { return n3::fn_00274af0(machine); }, kFrameBytes);
    if (lookup.declined) {
        return n3::kDeclined;
    }
    const std::uint32_t value = call.result();
    std::uint32_t retired = kSlotHeadCost + kLookupCost + lookup.retired;
    std::uint32_t code = 0U;
    if (value == 0U) {
        retired += kZeroCost;
    } else {
        retired += kTwoTestCost;
        if (value == 2U) {
            retired += kOneCost;
            code = 1U;
        } else {
            const Detail detailed = classify_detail(call, frame, record, region);
            if (detailed.declined) {
                return n3::kDeclined;
            }
            retired += kDetailCost + kDetailSelectCost + detailed.retired;
            code = detailed.code;
        }
    }

    m.write_u16_le(visibility, static_cast<std::uint16_t>(m.read_u16_le(visibility) & keep));
    m.write_u16_le(visibility,
                   static_cast<std::uint16_t>(m.read_u16_le(visibility) | ((code << shift) & 0xffffU)));
    return {retired + kStoreCost + kSlotTailCost, false};
}

template <class Context>
n3::Run update_region_visibility(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // Every visibility word starts cleared. The loops below are fixed-size (4 viewers x 17 slots), so they cannot run
    // away and need no ceiling test.
    constexpr std::uint32_t kClearCost = kClearCostPerSlot * kRegionCount;
    for (std::uint32_t slot = 0U; slot < kRegionCount; ++slot) {
        m.write_u16_le(object + kVisibilityOffset + 2U * slot, static_cast<std::uint16_t>(0U));
    }
    std::uint32_t retired = kPrologueCost + kClearCost + kRadiusSetupCost;
    for (std::uint32_t viewer = 0U; viewer < kViewerCount; ++viewer) {
        const unsigned shift = 2U * viewer;
        const GuestAddress record = kViewerRecord + kViewerStride * viewer;
        retired += kViewerHeadCost;
        for (std::uint32_t slot = 0U; slot < kRegionCount; ++slot) {
            const n3::Run visit = visit_slot(call, object, slot, frame, record, shift);
            if (visit.declined) {
                return call.decline();
            }
            retired += visit.retired;
        }
        retired += kViewerTailCost;
    }
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0021d080, 596U, 0x4a1a81b6f806daa2ULL, "UpdateRegionVisibility", kCeiling, update_region_visibility,
         EngineReturn::Nothing)
