// 0x00365e80 (a0 = ring buffer object, a1 = Q, a2 = P, f12 = header float, f13 = solver argument):
// PushRingBufferEntry. The buffer is 144-byte groups, each a 16-byte header of four floats and four 32-byte
// entries. The counter at +40 selects the group (counter >> 2, rounded toward zero for a negative counter) and
// the slot (counter & 3): the entry gets two quadwords and the slot's header gets f12. The counter then advances
// and returns to 0 when it reaches the limit at +36. Which quadword pair is written depends on the discriminant
// disc = x*x - f5*f13, with x = P.y (negated when P.y < 0 and f13 <= 0): disc <= 0 writes Q and P (each with
// w = its own y lane), disc > 0 writes the pair built from sqrt(disc) by write_solved_pair. VF1..VF4 and the
// COP1 accumulator end as the original leaves them. The original's stack copies are locals; FCR31 (the
// condition and flag bits) is not updated, as in the other n3 natives.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// Fields of the ring buffer object (a0).
constexpr GuestAddress kFloat16 = 16U;  // f5 in the solver
constexpr GuestAddress kFloat20 = 20U;  // f2 in the complex path
constexpr GuestAddress kFloat24 = 24U;  // f0 in the complex path
constexpr GuestAddress kBase = 28U;     // first group
constexpr GuestAddress kLimit = 36U;    // the counter returns to 0 when it reaches this
constexpr GuestAddress kCounter = 40U;  // write counter

constexpr std::uint32_t kGroupBytes = 144U;
constexpr std::uint32_t kHeaderBytes = 16U;
constexpr std::uint32_t kEntryBytes = 32U;
constexpr std::uint32_t kHalf = 0x3f000000U;  // 0.5f, from lui 0x3f00
constexpr std::uint32_t kPositiveZero = 0U;
constexpr std::uint32_t kMaxCost = 95U;       // the longest path

// VU0 destination mask of the y lane (bit 3 = x ... bit 0 = w).
constexpr std::uint8_t kMaskY = 4U;

// Retired counts of the original's straight-line blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 8U;      // 0x365e80..0x365e9c: loads, stack copies, bgez
constexpr std::uint32_t kNegativeCost = 2U;      // 0x365ea0..0x365ea4: n < 0 rounds the group index
constexpr std::uint32_t kAddressCost = 16U;      // 0x365ea8..0x365ee4: group, entry, counter
constexpr std::uint32_t kWrapCost = 2U;          // 0x36602c..0x366030: the counter reached its limit
constexpr std::uint32_t kCopyCost = 8U;          // 0x365ee8..0x365f04: P copy, P.y sign test
constexpr std::uint32_t kNegativeYCost = 3U;     // 0x365f08..0x365f10: P.y < 0, f13 sign test
constexpr std::uint32_t kNegateCost = 1U;        // 0x365f14: neg.s, P.y < 0 and f13 <= 0
constexpr std::uint32_t kDiscCost = 7U;          // 0x365f18..0x365f30: constant, ACC, disc, disc sign test
constexpr std::uint32_t kSimpleCost = 16U;       // 0x365f34..0x365f70: disc <= 0
constexpr std::uint32_t kComplexCost = 46U;      // 0x365f74..0x366028: disc > 0
constexpr std::uint32_t kReturnCost = 2U;        // 0x366034..0x366038: jr ra and the stack pop

// A 32-bit result written to a GPR: the low doubleword becomes the sign-extended value and the high doubleword
// is kept (write_gpr_sign_extended_32, which LUI, MFC1 and the 32-bit ALU forms use).
Gpr128 with_sign_extended_low(Gpr128 value, const std::uint32_t bits) {
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

// disc <= 0: the entry holds Q, then P, each made from zero through VU0 adds (VF0 = (0, 0, 0, 1)). The first
// quadword's w lane is 0 + Q.y and the second's is 0 + P.y. VF1 ends as P; VF4 is not written.
void write_simple_pair(guest::R5900State& cpu, auto& memory, const Gpr128& p, const Gpr128& q,
                       const GuestAddress entry, const GuestAddress header, const std::uint32_t f12) {
    cpu.write_vu0_vector(2U, q);                                       // lqc2 vf2, Q (stack copy)
    cpu.write_vu0_vector(3U, n3::load_quad_any(memory, entry));            // lqc2 vf3, 0(E)
    cpu.write_vu0_vector(1U, p);                                       // lqc2 vf1, P (stack copy)
    ee_cop2::vadd_vector(cpu, 3U, 0U, 2U, n3::kMaskXyz, false);        // vadd.xyz vf3, vf0, vf2
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, n3::kMaskW, 3U, true);    // vsubw.w vf3, vf0, vf0w
    ee_cop2::vadd_broadcast(cpu, 3U, 3U, 2U, n3::kMaskW, 1U, false);   // vaddy.w vf3, vf3, vf2y
    n3::store_quad_any(memory, entry, cpu.read_vu0_vector(3U));            // sqc2 vf3, 0(E)
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, entry + 16U));      // lqc2 vf2, 16(E)
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, false);        // vadd.xyz vf2, vf0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, n3::kMaskW, 3U, true);    // vsubw.w vf2, vf0, vf0w
    ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, n3::kMaskW, 1U, false);   // vaddy.w vf2, vf2, vf1y
    n3::store_quad_any(memory, entry + 16U, cpu.read_vu0_vector(2U));      // sqc2 vf2, 16(E)
    memory.write_u32_le(header, f12);                                  // swc1 f12, 0(H)
}

// disc > 0: s = sqrt(disc) and the scalar chain (f0..f4, all bit-exact through n3::fpu) give the x lanes that
// the VU0 subtracts use. The first quadword is stored with its w lane still the old word, then overwritten
// with its final w lane; the second quadword's w lane is the chain's last f3 value. VF1 ends as
// (f3, sign(f3), P.z, P.w): the low doubleword of the GPR a1 (loaded with P, then LUI and MFC1) over P's high one.
void write_solved_pair(guest::R5900State& cpu, auto& memory, const Gpr128& p, const Gpr128& q,
                       const GuestAddress entry, const GuestAddress header, const std::uint32_t f12,
                       const std::uint32_t f13, const std::uint32_t x, const std::uint32_t k16,
                       const std::uint32_t disc) {
    const GuestAddress object = cpu.read_gpr_u32(4U);
    const Gpr128 a0 = cpu.read_gpr(4U);
    const Gpr128 a3 = cpu.read_gpr(7U);

    cpu.write_vu0_vector(2U, q);                                        // lqc2 vf2, Q (stack copy)
    cpu.write_vu0_vector(4U, n3::load_quad_any(memory, entry));             // lqc2 vf4, 0(E)
    cpu.write_vu0_vector(3U, p);                                        // lqc2 vf3, P (stack copy)
    ee_cop2::vadd_vector(cpu, 4U, 0U, 2U, n3::kMaskXyz, false);         // vadd.xyz vf4, vf0, vf2
    const std::uint32_t root = fpu::sqrt(disc);                         // sqrt.s f0, f1
    const std::uint32_t sum = fpu::add(root, x);                        // add.s f3, f0, f6
    const std::uint32_t k20 = memory.read_u32_le(object + kFloat20);    // lwc1 f2, 20(a0)
    const std::uint32_t k24 = memory.read_u32_le(object + kFloat24);    // lwc1 f0, 24(a0)
    cpu.write_vu0_vector(1U, with_sign_extended_low(a3, f13));          // mfc1 a3, f13 ; qmtc2 a3, vf1
    const std::uint32_t f4 = fpu::mul(k20, sum);                        // mul.s f4, f2, f3
    n3::store_quad_any(memory, entry, cpu.read_vu0_vector(4U));             // sqc2 vf4, 0(E)
    const std::uint32_t t3 = fpu::mul(k16, f4);                         // mul.s f3, f5, f4
    ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, kMaskY, 0U, true);         // vsubx.y vf2, vf2, vf1x
    const std::uint32_t t2 = fpu::add(x, t3);                           // add.s f2, f6, f3
    ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, n3::kMaskW, 3U, true);     // vsubw.w vf4, vf0, vf0w
    const std::uint32_t t0 = fpu::mul(k24, t2);                         // mul.s f0, f0, f2
    const std::uint32_t t3b = fpu::sub(t0, t3);                         // sub.s f3, f0, f3
    const std::uint32_t k16b = memory.read_u32_le(object + kFloat16);   // lwc1 f0, 16(a0)
    const std::uint32_t w2 = fpu::mul(t3b, f4);                         // mul.s f2, f3, f4
    const std::uint32_t half = fpu::mul(kHalf, k16b);                   // mul.s f0, f1, f0
    const std::uint32_t scaled = fpu::mul(half, f4);                    // mul.s f0, f0, f4
    const Gpr128 a0_w2 = with_sign_extended_low(a0, w2);                // mfc1 a0, f2
    const std::uint32_t w0 = fpu::mul(f4, scaled);                      // mul.s f0, f4, f0
    cpu.write_vu0_vector(1U, a0_w2);                                    // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, kMaskY, 0U, true);        // vsubx.y vf2, vf2, vf1x
    const Gpr128 a0_w0 = with_sign_extended_low(a0_w2, w0);             // mfc1 a0, f0
    const Gpr128 a1 = with_sign_extended_low(with_sign_extended_low(p, kHalf), t3b);  // lui a1 ; mfc1 a1, f3
    cpu.write_vu0_vector(1U, a0_w0);                                    // qmtc2 a0, vf1
    ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskY, 0U, true);        // vsubx.y vf1, vf2, vf1x
    ee_cop2::vadd_broadcast(cpu, 4U, 4U, 1U, n3::kMaskW, 1U, false);   // vaddy.w vf4, vf4, vf1y
    n3::store_quad_any(memory, entry, cpu.read_vu0_vector(4U));             // sqc2 vf4, 0(E)
    cpu.write_vu0_vector(2U, n3::load_quad_any(memory, entry + 16U));       // lqc2 vf2, 16(E)
    cpu.write_vu0_vector(1U, a1);                                       // qmtc2 a1, vf1
    ee_cop2::vadd_vector(cpu, 2U, 0U, 3U, n3::kMaskXyz, false);         // vadd.xyz vf2, vf0, vf3
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, n3::kMaskW, 3U, true);     // vsubw.w vf2, vf0, vf0w
    ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, n3::kMaskW, 0U, false);    // vaddx.w vf2, vf2, vf1x
    n3::store_quad_any(memory, entry + 16U, cpu.read_vu0_vector(2U));       // sqc2 vf2, 16(E)
    memory.write_u32_le(header, f12);                                   // swc1 f12, 0(H)
}

template <class Context>
n3::Run push_ring_buffer_entry(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const Gpr128 p = n3::load_quad_any(m, call.arg(2));                     // lq v1, 0(a2)
    const Gpr128 q = n3::load_quad_any(m, call.arg(1));                     // lq a1, 0(a1)
    const std::uint32_t f12 = cpu.read_fpr_bits(12U);
    const std::uint32_t f13 = cpu.read_fpr_bits(13U);

    // The group and slot come from the counter as it was before this call advanced it.
    const std::uint32_t n = m.read_u32_le(object + kCounter);            // lw a2, 40(a0)
    const bool negative = (n & 0x80000000U) != 0U;
    // (n + 3) >> 2 for a negative n: the compiler's round-toward-zero division by four.
    const auto quotient = negative ? static_cast<std::int32_t>(n + 3U) >> 2 : static_cast<std::int32_t>(n) >> 2;
    const auto group_index = static_cast<std::uint32_t>(quotient);
    const std::uint32_t slot = n & 3U;                                  // andi a3, a2, 3
    const GuestAddress group = m.read_u32_le(object + kBase) + kGroupBytes * group_index;
    const GuestAddress entry = group + kHeaderBytes + kEntryBytes * slot;  // E
    const GuestAddress header = group + 4U * slot;                      // H

    m.write_u32_le(object + kCounter, n + 1U);                          // sw v1, 40(a0)
    std::uint32_t retired = kPrologueCost + kAddressCost + kCopyCost + kDiscCost + kReturnCost;
    if (negative) {
        retired += kNegativeCost;
    }
    if (m.read_u32_le(object + kCounter) == m.read_u32_le(object + kLimit)) {
        m.write_u32_le(object + kCounter, 0U);                          // sw zero, 40(a0)
        retired += kWrapCost;
    }

    std::uint32_t x = p.words[1];                                       // lwc1 f6, 84(sp): P.y
    if (fpu::lt(x, kPositiveZero)) {                                    // c.olt.s f6, f0
        retired += kNegativeYCost;
        if (fpu::le(f13, kPositiveZero)) {                              // c.ole.s f13, f0
            x = fpu::neg(x);                                            // neg.s f6, f6
            retired += kNegateCost;
        }
    }
    const std::uint32_t k16 = m.read_u32_le(object + kFloat16);         // lwc1 f5, 16(a0)
    const std::uint32_t acc = fpu::mul(x, x);                           // mula.s f6, f6
    cpu.set_fpu_accumulator_bits(acc);
    const std::uint32_t disc = fpu::msub(acc, k16, f13);                // msub.s f1, f5, f13
    if (fpu::le(disc, kPositiveZero)) {                                 // c.ole.s f1, f0
        retired += kSimpleCost;
        write_simple_pair(cpu, m, p, q, entry, header, f12);
    } else {
        retired += kComplexCost;
        write_solved_pair(cpu, m, p, q, entry, header, f12, f13, x, k16, disc);
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00365e80{0x00365e80U, 444U, 0x5eedadcd6426ac72ULL, "PushRingBufferEntry",
                                                  kMaxCost, n3::live<&push_ring_buffer_entry<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00365e80, push_ring_buffer_entry)
