// 0x001419f0 DistanceAhead: how far the object at a1 is ahead of the object at a0 along the flat heading (+6352
// of a0). The fallback vector (+32 of a0) takes the heading's place when the tested value (f1) is below 2^-32.
// The body is the listing's VU0 sequence in the listing's order, so VF1..VF4, ACC, Q, the MAC/status flags and
// the FPU results match the original. The original's stores to its own frame (sp+16, sp+64) are carried in
// locals and read back in the same order. Retired counts: 55 on the heading path, 43 on the fallback path.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kPosition = 48U;              // lqc2 at +48 of a1 and of a0
constexpr GuestAddress kFallback = 32U;              // lq at +32 of a0 (fallback path only)
constexpr GuestAddress kHeading = 6352U;             // lqc2 at +6352 of a0
constexpr std::uint32_t kShortBits = 0x2f800000U;    // 2^-32: lui 0x2f80, mtc1 into f0, c.olt.s f1,f0
constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kHead = 26U;                 // 0x1419f0..0x141a54: up to the branch and its delay slot
constexpr std::uint32_t kHeadingPath = 14U;          // 0x141a58..0x141a8c: the heading path and its beq
constexpr std::uint32_t kFallbackPath = 2U;          // 0x141a90..0x141a94: the fallback path
constexpr std::uint32_t kTail = 15U;                 // 0x141a98..0x141ad0: the reloads, the result and the return

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
bool quad_loadable(const auto& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

// lq ignores the low four address bits and faults only when the quadword is outside guest memory (Lq).
bool quad_maskable(const auto& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}

template <class Context>
n3::Run distance_ahead_native(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress self = call.arg(0);
    const GuestAddress other = call.arg(1);
    // Every quadword load either path can make is checked before anything changes: the original faults on any of
    // them, and a call declined here runs the original, which faults the same way.
    if (!quad_loadable(m, other + kPosition) || !quad_loadable(m, self + kPosition) ||
        !quad_loadable(m, self + kHeading) || !quad_maskable(m, self + kFallback)) {
        return call.decline();
    }

    // daddu v0,zero,zero clears the low doubleword only, so qmtc2 moves v0's high doubleword as it was on entry.
    guest::Gpr128 cleared_v0 = cpu.read_gpr(2U);
    cleared_v0.words[0] = 0U;
    cleared_v0.words[1] = 0U;

    cpu.write_vu0_vector(2U, n3::load_quad_any(m, other + kPosition));           // lqc2 vf2,48(a1)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, self + kPosition));           // lqc2 vf4,48(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, self + kHeading));            // lqc2 vf3,6352(a0)
    cpu.write_vu0_vector(1U, cleared_v0);                                       // qmtc2 v0,vf1
    ee_cop2::vadd_vector(cpu, 2U, 4U, 2U, 14U, true);                           // vsub.xyz vf2,vf4,vf2
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 4U, 0U, false);                    // vaddx.y vf2,vf0,vf1x
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, 4U, 0U, false);                    // vaddx.y vf3,vf0,vf1x
    ee_cop2::vmul(cpu, 1U, 3U, 3U, 14U);                                        // vmul.xyz vf1,vf3,vf3
    const guest::Gpr128 v1 = cpu.read_vu0_vector(1U);                           // qmfc2 v1,vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved_vf2 = cpu.read_vu0_vector(2U);                    // sqc2 vf2,64(sp), reloaded below
    guest::Gpr128 v0 = cpu.read_vu0_vector(1U);                                 // qmfc2 v0,vf1
    cpu.write_vu0_vector(2U, v1);                                               // qmtc2 v1,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, v0);                                               // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    v0 = cpu.read_vu0_vector(1U);                                               // qmfc2 v0,vf1
    const std::uint32_t tested = v0.words[0];                                   // mtc1 v0,f1
    cpu.write_fpr_bits(1U, tested);

    std::uint32_t retired = kHead;
    guest::Gpr128 frame_quad{};                                                 // sq v0,16(sp), reloaded below
    const bool short_length = fpu::lt(tested, kShortBits);
    // c.olt.s leaves its result in the FCR31 condition bit, which the original keeps after its return.
    cpu.set_fcr31_bits(short_length ? cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition
                                    : cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition);
    if (short_length) {
        // bc1t taken: lq v0,32(a0) and its frame store
        frame_quad = n3::load_quad_any(m, self + kFallback);
        retired += kFallbackPath;
    } else {
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                // vaddw.x vf1,vf0,vf0w
        ee_cop2::vmul(cpu, 2U, 3U, 3U, 14U);                                    // vmul.xyz vf2,vf3,vf3
        v0 = cpu.read_vu0_vector(1U);                                           // qmfc2 v0,vf1
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                          // vadday.x acc,vf2,vf2y
        cpu.write_vu0_vector(1U, v0);                                           // qmtc2 v0,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false); // vmaddz.x vf1,vf1,vf2z
        v0 = cpu.read_vu0_vector(1U);                                           // qmfc2 v0,vf1
        cpu.write_vu0_vector(1U, v0);                                           // qmtc2 v0,vf1
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                               // vrsqrt q,vf0w,vf1x
        // vwaitq: nothing to wait for, the interpreter's Q is already visible
        ee_cop2::vmul_control(cpu, 3U, 3U, 14U, ee_cop2::kControlQ);            // vmulq.xyz vf3,vf3,q
        frame_quad = cpu.read_vu0_vector(3U);                                   // qmfc2 v0,vf3
        retired += kHeadingPath;
    }

    // The reloads: VF3 from sp+64 (the saved VF2), VF2 from sp+16 (the quad the branch path stored).
    cpu.write_vu0_vector(3U, saved_vf2);                                        // lqc2 vf3,64(sp)
    cpu.write_vu0_vector(2U, frame_quad);                                       // lqc2 vf2,16(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                    // vaddw.x vf1,vf0,vf0w
    v0 = cpu.read_vu0_vector(1U);                                               // qmfc2 v0,vf1
    ee_cop2::vmul(cpu, 2U, 3U, 2U, 14U);                                        // vmul.xyz vf2,vf3,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, v0);                                               // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1,vf1,vf2z
    v0 = cpu.read_vu0_vector(1U);                                               // qmfc2 v0,vf1
    cpu.write_vu0_vector(1U, v0);                                               // qmtc2 v0,vf1
    ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, 8U, true);                            // vsub.x vf1,vf0,vf1
    v0 = cpu.read_vu0_vector(1U);                                               // qmfc2 v0,vf1
    cpu.write_fpr_bits(0U, v0.words[0]);                                        // mtc1 v0,f0
    cpu.write_gpr(2U, v0);
    cpu.write_gpr(3U, v1);
    return call.finish(retired + kTail);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001419f0, 228U, 0xfcf3ec5d6a5b6e72ULL, "DistanceAhead", kCeiling, distance_ahead_native, EngineReturn::Float)
