// 0x00141ae0 (a0 = object A, a1 = object B): HeadingAlignedDistance. The absolute distance of A's position from
// B's position, measured along A's heading in the XZ plane. Each position is the quadword at +48 (lqc2, x y z used)
// and A's heading is the quadword at +0 (lq). The VU0 sequence forces y to +0 on both delta and heading, so only
// x and z take part. A heading whose squared length is below 2^-32 is used as stored (not normalised), otherwise it
// is normalised with VRSQRT. Returns f0 = |delta . direction| (EngineReturn::Float). The original's only stores are
// to its own frame below the entry sp; the two stores here write the same words, so the frame is left identical.
// The VU0 steps follow the original's instruction order, so VF1..VF4, ACC and Q finish as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kPositionOffset = 48U;         // lqc2 at +48 of each object
constexpr GuestAddress kFrameBytes = 64U;             // addiu sp,sp,-64
constexpr std::uint32_t kShortLengthBits = 0x2f800000U;  // 2^-32: the squared-length threshold

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 27U;              // 0x141ae0..0x141b48: through the length test's delay slot
constexpr std::uint32_t kShortCost = 1U;              // 0x141b84: the heading store on a short length
constexpr std::uint32_t kNormaliseCost = 14U;         // 0x141b4c..0x141b80: normalisation through its delay slot
constexpr std::uint32_t kTailCost = 14U;              // 0x141b88..0x141bbc: dot product, abs, return

template <class Context>
n3::Run heading_aligned_distance(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object_a = call.arg(0);
    const GuestAddress object_b = call.arg(1);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    cpu.write_vu0_vector(3U, n3::load_quad_any(m, object_b + kPositionOffset));  // lqc2 vf3,48(a1)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, object_a + kPositionOffset));  // lqc2 vf4,48(a0)
    // daddu v0,zero,zero clears only the low doubleword; qmtc2 then copies the whole of v0 into VF2.
    guest::Gpr128 cleared = cpu.read_gpr(2U);
    cleared.words[0] = 0U;
    cleared.words[1] = 0U;
    cpu.write_gpr(2U, cleared);
    cpu.write_vu0_vector(2U, cleared);                                        // qmtc2 v0,vf2 (vf2.x is zero)
    ee_cop2::vadd_vector(cpu, 3U, 4U, 3U, 14U, true);                         // vsub.xyz vf3,vf4,vf3
    const guest::Gpr128 heading = n3::load_quad_any(m, object_a);                 // lq v1,0(a0)
    cpu.write_gpr(3U, heading);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, 4U, 0U, false);                  // vaddx.y vf3,vf0,vf2x: delta.y = +0
    n3::store_quad_any(m, frame + 48U, cpu.read_vu0_vector(3U));                  // sqc2 vf3,48(sp): delta

    // vf1.x = 1.0 is the factor of the z term in the multiply-adds below; qmfc2/qmtc2 carry vf1 through v0 around
    // the multiply, so its other lanes keep their entry values.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    cpu.write_vu0_vector(3U, heading);                                        // qmtc2 v1,vf3
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, 4U, 0U, false);                  // vaddx.y vf3,vf0,vf2x: direction.y = +0
    ee_cop2::vmul(cpu, 1U, 3U, 3U, 14U);                                      // vmul.xyz vf1,vf3,vf3
    cpu.write_gpr(4U, cpu.read_vu0_vector(1U));                               // qmfc2 a0,vf1
    cpu.write_vu0_vector(2U, cpu.read_gpr(4U));                               // qmtc2 a0,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                               // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    const std::uint32_t length_squared = cpu.read_gpr_u32(2U);
    cpu.write_fpr_bits(1U, length_squared);                                   // mtc1 v0,f1
    cpu.write_gpr_sign_extended_32(2U, kShortLengthBits);                     // lui v0,0x2f80
    cpu.write_fpr_bits(0U, kShortLengthBits);                                 // mtc1 v0,f0

    std::uint32_t retired = kHeadCost;
    guest::Gpr128 direction{};
    if (fpu::lt(length_squared, kShortLengthBits)) {                          // c.olt.s f1,f0; bc1t
        direction = heading;                                                  // sq v1,0(sp)
        retired += kShortCost;
    } else {
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);              // vaddw.x vf1,vf0,vf0w
        ee_cop2::vmul(cpu, 2U, 3U, 3U, 14U);                                  // vmul.xyz vf2,vf3,vf3
        cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                           // qmfc2 v0,vf1
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                        // vadday.x acc,vf2,vf2y
        cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                           // qmtc2 v0,vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
        cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                           // qmfc2 v0,vf1
        cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                           // qmtc2 v0,vf1
        ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                             // vrsqrt q,vf0w,vf1x (vwaitq: no-op)
        ee_cop2::vmul_control(cpu, 3U, 3U, 14U, ee_cop2::kControlQ);          // vmulq.xyz vf3,vf3,q
        cpu.write_gpr(2U, cpu.read_vu0_vector(3U));                           // qmfc2 v0,vf3
        direction = cpu.read_gpr(2U);                                         // sq v0,0(sp) (delay slot)
        retired += kNormaliseCost;
    }
    n3::store_quad_any(m, frame, direction);

    // The tail reads delta and the direction back from the frame, as the original does.
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 48U));                  // lqc2 vf3,48(sp)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame));                        // lqc2 vf2,0(sp)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    ee_cop2::vmul(cpu, 2U, 3U, 2U, 14U);                                      // vmul.xyz vf2,vf3,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                               // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    cpu.write_gpr(2U, cpu.read_vu0_vector(1U));                               // qmfc2 v0,vf1
    cpu.write_fpr_bits(0U, fpu::abs(cpu.read_gpr_u32(2U)));                   // mtc1 v0,f0; abs.s f0,f0
    retired += kTailCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00141ae0{0x00141ae0U, 224U, 0xb7e1e3004efce2b9ULL, "HeadingAlignedDistance",
                                                  55U, n3::live<&heading_aligned_distance<NativeFunctionContext>>, EngineReturn::Float};

} // namespace takedown::runtime

N3_BODY_PLAIN(00141ae0, heading_aligned_distance)
