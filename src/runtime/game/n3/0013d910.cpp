// 0x0013d910 (a0 = vehicle object): SlipOutputBytes. The physics state P (u32 at a0+1108) holds two vectors,
// A (quad at P+6880) and B (quad at P+6896), and the axis quad R3 (at (u32 at (u32 at P+9264)+516)+32). dotA and
// dotB are the VU0 sums (x+y)+z of the xyz products with R3; only their x lanes are used. With kG the float at
// $gp-31684: m = kG*|dotB|, s = m*kS2 + (0.5*|dotA|)*kS1 (kS1 = P+9268, kS2 = P+9272) and n = to_int(5*s).
// Three bytes go to the object: +1089 = a3, +1090 = a2, +1088 = (2n+2) & 0xff, where a3 = n+129 (not clamped,
// so a negative value wraps in its byte) and a2 = to_int(-5*s)+127. When n+129 >= 256, a3 = 255 and
// a2 = 255-2n. When that a2 is negative, a2 = 0 and a3 = 2n. Returns nothing. VF1..VF3, ACC and the COP1
// accumulator end as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Fields of the vehicle object (a0) and of its physics state (P).
constexpr GuestAddress kPhysicsPointer = 1108U;     // a0: P
constexpr GuestAddress kAxisHolder = 9264U;         // P: holder pointer
constexpr GuestAddress kAxisTable = 516U;           // holder: table pointer
constexpr GuestAddress kAxisQuad = 32U;             // table: R3
constexpr GuestAddress kQuadA = 6880U;              // P: quad A
constexpr GuestAddress kQuadB = 6896U;              // P: quad B
constexpr GuestAddress kScale1 = 9268U;             // P: kS1
constexpr GuestAddress kScale2 = 9272U;             // P: kS2
constexpr GuestAddress kGlobalScale = 0xffff843cU;  // $gp - 31684: kG
constexpr GuestAddress kLowByte = 1088U;            // a0: (2n+2) & 0xff
constexpr GuestAddress kA3Byte = 1089U;             // a0: a3
constexpr GuestAddress kA2Byte = 1090U;             // a0: a2

constexpr std::uint8_t kMaskX = 8U;                 // VU0 destination mask of the x lane
constexpr std::uint32_t kHalf = 0x3f000000U;        // 0.5f, from lui 0x3f00
constexpr std::uint32_t kFive = 0x40a00000U;        // 5.0f, from lui 0x40a0

// Retired counts of the three exits (0x13d910..0x13da2c, delay slots included).
constexpr std::uint32_t kClampedCost = 61U;         // n+129 >= 256
constexpr std::uint32_t kShortCost = 66U;           // normal path, a2 >= 0
constexpr std::uint32_t kLongestCost = 68U;         // normal path, a2 < 0

template <class Context>
n3::Run slip_output_bytes(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress physics = m.read_u32_le(object + kPhysicsPointer);       // lw a1, 1108(a0)

    // Dot product of A with R3. VF1.x = 1.0 is the multiplier of the z product in the last step.
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);               // vaddw.x vf1,vf0,vf0w
    const Gpr128 saved_a = cpu.read_vu0_vector(1U);                            // qmfc2 a2,vf1
    const GuestAddress holder = m.read_u32_le(physics + kAxisHolder);          // lw v1, 9264(a1)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, physics + kQuadA));              // lqc2 vf1, 6880(a1)
    const GuestAddress table = m.read_u32_le(holder + kAxisTable);             // lw v1, 516(v1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, table + kAxisQuad));             // lqc2 vf3, 32(v1)
    ee_cop2::vmul(cpu, 1U, 1U, 3U, n3::kMaskXyz);                              // vmul.xyz vf1,vf1,vf3
    cpu.write_vu0_vector(2U, cpu.read_vu0_vector(1U));                         // qmfc2 v1,vf1 ; qmtc2 v1,vf2
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                         // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved_a);                                         // qmtc2 a2,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t dot_a = cpu.read_vu0_vector(1U).words[0];              // qmfc2 v1,vf1 ; mtc1 v1,f0

    // Dot product of B with R3, in the same order.
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, physics + kQuadB));              // lqc2 vf2, 6896(a1)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);               // vaddw.x vf1,vf0,vf0w
    ee_cop2::vmul(cpu, 2U, 2U, 3U, n3::kMaskXyz);                              // vmul.xyz vf2,vf2,vf3
    const Gpr128 saved_b = cpu.read_vu0_vector(1U);                            // qmfc2 v1,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                         // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved_b);                                         // qmtc2 v1,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);  // vmaddz.x vf1,vf1,vf2z
    const std::uint32_t dot_b = cpu.read_vu0_vector(1U).words[0];              // qmfc2 v1,vf1 ; mtc1 v1,f1

    // Scalar chain: m = kG*|dotB|, ACC = m*kS2, s = ACC + (0.5*|dotA|)*kS1, n = to_int(5*s).
    const std::uint32_t kg = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kGlobalScale);  // lwc1 f4, -31684(gp)
    const std::uint32_t abs_a = fpu::abs(dot_a);                               // abs.s f2,f0
    const std::uint32_t abs_b = fpu::abs(dot_b);                               // abs.s f5,f1
    const std::uint32_t half_a = fpu::mul(kHalf, abs_a);                       // mul.s f2,f0,f2
    const std::uint32_t scaled_b = fpu::mul(kg, abs_b);                        // mul.s f4,f4,f5
    const std::uint32_t scale2 = m.read_u32_le(physics + kScale2);             // lwc1 f3, 9272(a1)
    const std::uint32_t acc = fpu::mul(scaled_b, scale2);                      // mula.s f4,f3
    cpu.set_fpu_accumulator_bits(acc);
    const std::uint32_t scale1 = m.read_u32_le(physics + kScale1);             // lwc1 f0, 9268(a1)
    const std::uint32_t s = fpu::madd(acc, half_a, scale1);                    // madd.s f2,f2,f0
    const std::int32_t n = fpu::to_int(fpu::mul(kFive, s));                    // mul.s f0,f1,f2 ; cvt.w.s

    // Output bytes. Only the low byte of each value is stored, and the order of the stores is the original's.
    const auto n_bits = static_cast<std::uint32_t>(n);                         // mfc1 a1,f0
    std::uint32_t a3 = n_bits + 129U;                                          // addiu v1,a1,128 ; addiu a3,v1,1
    std::uint32_t a2{};
    std::uint32_t retired{};
    if (static_cast<std::int32_t>(a3) < 256) {                                 // slti at,a3,256
        const auto negated = fpu::to_int(fpu::mul(kFive, fpu::neg(s)));        // neg.s f0,f2 ; mul.s ; cvt.w.s
        a2 = static_cast<std::uint32_t>(negated) + 127U;                       // addiu v1,v1,128 ; addiu a2,v1,-1
        retired = kShortCost;
        if (static_cast<std::int32_t>(a2) < 0) {                               // bgez a2 (not taken)
            a2 = 0U;                                                           // daddu a2,zero,zero
            a3 = n_bits << 1U;                                                 // sll a3,a1,1
            retired = kLongestCost;
        }
    } else {
        a3 = 255U;                                                             // addiu a3,zero,255
        a2 = 255U - (n_bits << 1U);                                            // sll v1,a1,1 ; subu a2,a3,v1
        retired = kClampedCost;
    }
    m.write_u8(object + kA3Byte, static_cast<std::uint8_t>(a3));               // sb a3, 1089(a0)
    m.write_u8(object + kA2Byte, static_cast<std::uint8_t>(a2));               // sb a2, 1090(a0)
    m.write_u8(object + kLowByte, static_cast<std::uint8_t>((n_bits << 1U) + 2U));  // sll v1,a1,1 ; sb v1, 1088(a0)
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0013d910{0x0013d910U, 288U, 0x3679a3c98a85c138ULL, "SlipOutputBytes",
                                                  kLongestCost, n3::live<&slip_output_bytes<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0013d910, slip_output_bytes)
