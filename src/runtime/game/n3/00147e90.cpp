// 0x00147e90 (a0 = strip object, a1 = quad index, a2 = destination quadword): StripQuadStepCross.
// Calls StripQuadStep (0x00147d00) with a2 = sp + 32, so the quad's step vector Q is written into the frame. Q is
// then crossed with the constant quad C at 0x004c9cf0: vopmula.xyz acc,vf2,vf1 and vopmsub.xyz vf1,vf1,vf2 leave
// VF1.xyz = cross(C, Q), and VF1.w keeps Q's w. That quadword is stored at the entry a2 (the original's s0), after
// the callee's own store, and is also left in v1 by qmfc2. The frame's saves of ra and s0 are not reproduced:
// neither register changes here, so the original's reloads leave them as they were on entry.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00147d00)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kCrossOperand = 0x004c9cf0U;  // lui 0x4d, lqc2 -25360: the constant quad C
constexpr std::uint32_t kFrameBytes = 48U;           // addiu $sp,$sp,-48
constexpr std::uint32_t kStepOffset = 32U;           // addiu $a2,$sp,32: the callee's destination in the frame
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kBeforeCall = 6U;  // 0x00147e90..0x00147ea4: frame, saves, the jal and its delay slot
constexpr std::uint32_t kAfterCall = 11U;  // 0x00147ea8..0x00147ed0: VU0 pair, store, reloads, return

template <class Context>
n3::Run strip_quad_step_cross(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress destination = call.arg(2);  // the original's s0: a2 on entry
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress step = frame + kStepOffset;
    call.set_arg(2, step);                         // addiu $a2,$sp,32 in the jal's delay slot
    const n3::Run callee = call.invoke([](auto& memory) { return n3::fn_00147d00(memory); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, step));           // lqc2 vf1,32(sp)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, kCrossOperand));  // lqc2 vf2,-25360(v1)
    ee_cop2::vopmula(cpu, 2U, 1U);                                  // vopmula.xyz acc,vf2,vf1
    ee_cop2::vopmsub(cpu, 1U, 1U, 2U);                              // vopmsub.xyz vf1,vf1,vf2
    const Gpr128 cross = cpu.read_vu0_vector(1U);                   // qmfc2 v1,vf1
    cpu.write_gpr(3U, cross);
    n3::store_quad_any(m, destination, cross);                      // sq v1,0(s0)
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00147e90, 68U, 0x6c559f01d72313e0ULL, "StripQuadStepCross", kCeiling, strip_quad_step_cross,
         EngineReturn::Nothing)
