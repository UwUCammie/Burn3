// 0x00220270 CacheObjectFrustumClass (a1 = object record X): classifies the object's bounding sphere against the culling
// frustum and caches the class byte in the record. The sphere centre is the position quadword at X+48 (x y z used) and
// the radius is the float at M+20, where M is the pointer at X+64. The sphere quadword is built as the original builds
// it, in the caller's frame at entry sp - 16, because ClassifySphereInCullingFrustum (0x002202d0) reads it through a0:
//   vf1 = X+48; vadd.xyz vf2 = vf0 + vf1 (0.0 + x, so -0 comes out as +0 the way the VU adds it);
//   v0 = radius (lw); vf2 = v0 (qmtc2 copies the whole register); vmulx.w vf1.w = vf0.w * vf2.x (the VU's radius).
// The quadword is stored at sp - 16 and the classifier runs; X+91 = its v0 as a byte, and v0 is left as the classifier
// returned it (0 outside, 1 straddles, 2 inside). The original's own path is 22 instructions, the jal and its delay slot
// included, plus the classifier's count. Its saves of s0 and ra are frame words the check ignores, so they are not made.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002202d0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 64U;            // addiu sp,sp,-64
constexpr GuestAddress kQuadOffset = 48U;             // the quadword at entry sp - 64 + 48
constexpr GuestAddress kPositionOffset = 48U;         // X+48: position quadword
constexpr GuestAddress kModelOffset = 64U;            // X+64: pointer M to the model record
constexpr GuestAddress kRadiusOffset = 20U;           // M+20: bounding radius (float bits)
constexpr GuestAddress kClassOffset = 91U;            // X+91: cached class byte
constexpr unsigned kV0 = 2U;

// Retired instructions on the original's own path: 0x00220270..0x002202b0 (through the jal and its delay slot) and
// 0x002202b4..0x002202c4 (the byte store, the restores and the return with its delay slot).
constexpr std::uint32_t kOwnInstructions = 22U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run cache_object_frustum_class(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(1);
    const GuestAddress quad = call.stack_pointer() - kFrameBytes + kQuadOffset;

    cpu.write_vu0_vector(1U, n3::load_quad_any(m, object + kPositionOffset));  // lqc2 vf1,48(s0)
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, false);                // vadd.xyz vf2,vf0,vf1
    // sqc2 vf2 to the frame and lqc2 back into vf1 is a round trip of the same quadword.
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(2U));

    const GuestAddress model = m.read_u32_le(object + kModelOffset);            // lw v0,64(a1)
    cpu.write_gpr_sign_extended_32(kV0, m.read_u32_le(model + kRadiusOffset));  // lw v0,20(v0)
    cpu.write_vu0_vector(2U, cpu.read_gpr(kV0));                                // qmtc2 v0,vf2
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);                  // vmulx.w vf1,vf0,vf2x
    n3::store_quad_any(m, quad, cpu.read_vu0_vector(1U));                      // sqc2 vf1,48(sp), the jal's delay slot

    call.set_arg(0, quad);                                                     // addiu a0,sp,48
    const n3::Run classifier = call.invoke([](auto& machine) { return n3::fn_002202d0(machine); }, kFrameBytes);
    if (classifier.declined) {
        return call.decline();
    }
    m.write_u8(object + kClassOffset, static_cast<std::uint8_t>(call.result()));  // sb v0,91(s0)
    return call.finish(kOwnInstructions + classifier.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00220270, 88U, 0x45cacdb2ccbaef37ULL, "CacheObjectFrustumClass", kCeiling, cache_object_frustum_class,
         EngineReturn::Integer)
