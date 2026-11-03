// 0x00361f70 (320 bytes): the visibility update of a look tracker. The target record at +552 goes to 001cb780 with a
// 30 degree cone (its result is 1 while the target's direction is inside the cone), and a zero answer marks the
// target hidden this frame (byte 446). The frame step (the float at 0x0052267c) grows the time hidden in a row (448)
// and in total (456) while hidden, and grows the time visible in a row (452) while visible; the other run restarts.
//
// The engage latch (444) sets on a visible frame while it is clear: the target's negated y (VU0 vsub.y of its quad at
// +176, 0 - y) is compared with the peak at 468, which also takes the object's handle (+408) into 464 when it is
// raised, and with the running peak at 472. Byte 445 is set on the frame the latch sets and cleared every frame. A latched target
// that has been hidden for longer than the float at $gp-30832 releases the latch.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001cb780)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Object fields (the original's s0) and the target record they point at.
constexpr GuestAddress kTarget = 552U;
constexpr GuestAddress kTargetQuad = 176U;
constexpr GuestAddress kHandle = 408U;
constexpr GuestAddress kEngaged = 444U;
constexpr GuestAddress kEngagedThisFrame = 445U;
constexpr GuestAddress kHidden = 446U;
constexpr GuestAddress kHiddenTime = 448U;
constexpr GuestAddress kVisibleTime = 452U;
constexpr GuestAddress kHiddenTotal = 456U;
constexpr GuestAddress kPeakHandle = 464U;
constexpr GuestAddress kPeak = 468U;
constexpr GuestAddress kRunningPeak = 472U;

// Globals: the frame step (lui 0x52, +9852), the hidden-time limit ($gp - 30832) and the 30 degree cone.
constexpr GuestAddress kFrameStep = 0x0052267cU;
constexpr std::int32_t kHiddenLimitOffset = -30832;
constexpr std::uint32_t kConeDegreesBits = 0x41f00000U;
constexpr std::uint8_t kMaskY = 4U;

constexpr std::uint32_t kFrameBytes = 96U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions per block of the listing, delay slots included. Each path adds the callee's count (cone).
constexpr std::uint32_t kCallBlock = 15U;          // 0x00361f70-0x00361fa8: frame, target, the call, hidden flag
constexpr std::uint32_t kHiddenBlock = 12U;        // 0x00361fac-0x00361fd8: hidden timers
constexpr std::uint32_t kVisibleBlock = 6U;        // 0x00361fdc-0x00361ff0: visible timer
constexpr std::uint32_t kJoinBlock = 4U;           // 0x00361ff4-0x00362000: clear byte 445, branch on hidden
constexpr std::uint32_t kExitBlock = 2U;           // 0x00362004-0x00362008: a visible target that is latched exits here
constexpr std::uint32_t kHiddenLatchBlock = 3U;    // 0x0036200c-0x00362014: hidden, latch test
constexpr std::uint32_t kHiddenLimitBlock = 5U;    // 0x00362018-0x00362028: latched and hidden, hidden time against the limit
constexpr std::uint32_t kDisengageBlock = 1U;      // 0x0036202c: release the latch
constexpr std::uint32_t kReturnBlock = 2U;         // 0x00362030-0x00362034: jump to the restore, ld ra in its delay slot
constexpr std::uint32_t kVisibleLatchBlock = 3U;   // 0x00362038-0x00362040: visible, latch test
constexpr std::uint32_t kEngageBlock = 10U;        // 0x00362044-0x00362068: target quad, negated y, first peak test
constexpr std::uint32_t kRunningTestBlock = 4U;    // 0x0036206c-0x00362078: running peak test
constexpr std::uint32_t kRunningStoreBlock = 1U;   // 0x0036207c: running peak store
constexpr std::uint32_t kPeakStoreBlock = 4U;      // 0x00362090-0x0036209c: peak and its handle
constexpr std::uint32_t kLatchBlock = 4U;          // 0x00362080-0x0036208c: latch and byte 445
constexpr std::uint32_t kEpilogueBlock = 1U;       // 0x003620a0: ld ra
constexpr std::uint32_t kRestoreBlock = 3U;        // 0x003620a4-0x003620ac: lq s0, jr ra, addiu sp

// Whole paths (the callee's count is added to each).
constexpr std::uint32_t kHiddenPath = kCallBlock + kHiddenBlock + kJoinBlock + kExitBlock + kHiddenLatchBlock;  // 36
constexpr std::uint32_t kVisiblePath = kCallBlock + kVisibleBlock + kJoinBlock + kVisibleLatchBlock;            // 28
constexpr std::uint32_t kReturnPath = kEpilogueBlock + kRestoreBlock;                                         // 4

template <class Context>
n3::Run track_target_visibility(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    call.set_arg(0, m.read_u32_le(object + kTarget));
    call.set_arg(1, 1U);
    call.set_float_arg(0, kConeDegreesBits);
    const n3::Run cone = call.invoke([](auto& machine) { return n3::fn_001cb780(machine); }, kFrameBytes);
    if (cone.declined) {
        return call.decline();
    }
    const std::uint32_t callee = cone.retired;
    const bool hidden = call.result() == 0U;
    m.write_u8(object + kHidden, static_cast<std::uint8_t>(hidden ? 1U : 0U));

    std::uint32_t hidden_time = 0U;
    if (hidden) {
        const std::uint32_t step = m.read_u32_le(kFrameStep);
        m.write_u32_le(object + kHiddenTotal, fpu::add(m.read_u32_le(object + kHiddenTotal), step));
        hidden_time = fpu::add(m.read_u32_le(object + kHiddenTime), step);
        m.write_u32_le(object + kHiddenTime, hidden_time);
        m.write_u32_le(object + kVisibleTime, 0U);
    } else {
        m.write_u32_le(object + kHiddenTime, 0U);
        const std::uint32_t step = m.read_u32_le(kFrameStep);
        m.write_u32_le(object + kVisibleTime, fpu::add(m.read_u32_le(object + kVisibleTime), step));
    }
    m.write_u8(object + kEngagedThisFrame, 0U);

    if (hidden) {
        if (m.read_u8(object + kEngaged) == 0U) {
            return call.finish(kHiddenPath + kReturnPath + callee);
        }
        const GuestAddress limit_address =
            call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kHiddenLimitOffset);
        if (fpu::le(hidden_time, m.read_u32_le(limit_address))) {
            return call.finish(kHiddenPath + kHiddenLimitBlock + kReturnPath + callee);
        }
        m.write_u8(object + kEngaged, 0U);
        return call.finish(kHiddenPath + kHiddenLimitBlock + kDisengageBlock + kReturnBlock + kRestoreBlock + callee);
    }

    if (m.read_u8(object + kEngaged) != 0U) {
        return call.finish(kVisiblePath + kExitBlock + kReturnPath + callee);
    }

    // Engage. vsub.y vf1,vf0,vf1 on the target quad (VF0 = 0, 0, 0, 1): the y lane is 0 - y. The whole result is left
    // in VF1 and the MAC flags are published, as the instruction does.
    const GuestAddress target = m.read_u32_le(object + kTarget);
    const n3::Gpr128 quad = n3::load_quad_any(m, target + kTargetQuad);
    const guest::Vu0MacroFmacResult negated =
        guest::execute_vu0_macro_vector_add_subtract(quad, n3::kVf0, quad, kMaskY, true);
    call.cpu().write_vu0_vector(1U, negated.value);
    guest::publish_vu0_macro_fmac_result(call.cpu(), negated.mac_flags);
    const std::uint32_t negated_y = negated.value.words[1];

    const bool beyond_peak = !fpu::le(negated_y, m.read_u32_le(object + kPeak));
    if (beyond_peak) {
        m.write_u32_le(object + kPeak, negated_y);
        m.write_u32_le(object + kPeakHandle, m.read_u32_le(object + kHandle));
    }
    const bool beyond_running = !fpu::le(negated_y, m.read_u32_le(object + kRunningPeak));
    if (beyond_running) {
        m.write_u32_le(object + kRunningPeak, negated_y);
    }
    m.write_u8(object + kEngaged, 1U);
    m.write_u8(object + kEngagedThisFrame, 1U);

    std::uint32_t retired =
        kVisiblePath + kEngageBlock + kRunningTestBlock + kLatchBlock + kReturnBlock + kRestoreBlock;
    if (beyond_peak) {
        retired += kPeakStoreBlock;
    }
    if (beyond_running) {
        retired += kRunningStoreBlock;
    }
    return call.finish(retired + callee);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00361f70, 320U, 0x99a1a5459898c753ULL, "UpdateTargetVisibility", kCeiling, track_target_visibility,
         EngineReturn::Nothing)
