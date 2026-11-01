// 0x002bf0f0 (a0 = record Q, a1 = VU0 table, a2 = output block): SlabPenetrationDepth. Q.w is the limit S.
// Table entries 0 and 1 are planes (xyz normal, w offset): each value dot(Q.xyz, E.xyz) - E.w must not exceed S
// (else the result is 0), and a value above -S sets the result to 1. T = a1[2]*Q.x + a1[3]*Q.y + a1[4]*Q.z - a1[5]
// (VU0, four lanes) must not exceed S in any lane (else 0). When a lane of T is above -S, S - max(T) is stored at
// a2+32 and the result is 1; otherwise the result is what the planes left it at (2 when no plane was above -S).
// The original's other writes go to its own stack frame, which is not reproduced (the check ignores it).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

// MAX.S on raw bits, ordered as guest::ee_cop1::max_s orders them.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// The original's loop runs exactly twice (t0 counts 0, 1 and stops at 2), so it is written as straight code. Its
// early exits are data dependent; the retired counts below follow the listing for every path (delay slots included).
template <class Context>
n3::Run slab_penetration_depth(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const auto exit_with = [&call](const std::uint64_t result, const std::uint32_t count) {
        call.return_value64(result);
        return call.finish(count);
    };

    const GuestAddress table = call.arg(1);
    // lq a3,0(a0): the record. Its word 3 is the limit S (lwc1 316(sp) reads it back from the frame copy).
    const guest::Gpr128 record = n3::load_quad_any(m, call.arg(0));
    const std::uint32_t limit = record.words[3];
    const std::uint32_t negative_limit = fpu::neg(limit);       // neg.s f2,f4

    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf2,vf0,vf0w
    cpu.write_vu0_vector(4U, record);                           // lqc2 vf4,720(sp): the record's frame copy

    // 0x2bf0f0..0x2bf124 (14 instructions) and the loop test at 0x2bf178 (3).
    std::uint32_t retired = 17U;
    std::uint64_t v0 = 2U;                                      // addiu v0,zero,2
    for (std::uint32_t plane = 0U; plane < 2U; ++plane) {
        retired += 15U;                                         // 0x2bf128..0x2bf160
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, table + plane * 16U));    // lqc2 vf3,0(v1)
        ee_cop2::vmul(cpu, 1U, 4U, 3U, 14U);                    // vmul.xyz vf1,vf4,vf3
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);          // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 2U, false, false, false);    // vmaddz.x vf1,vf2,vf1z
        ee_cop2::vadd_broadcast(cpu, 1U, 1U, 3U, 8U, 3U, true);                    // vsubw.x vf1,vf1,vf3w
        const std::uint32_t plane_value = cpu.read_vu0_vector(1U).words[0];        // qmfc2 v1,vf1; mtc1 v1,f0
        if (!fpu::le(plane_value, limit)) {                     // c.ole.s; bc1f to 0x2bf270
            return exit_with(0U, retired + 5U);                 // beq, its delay slot, the epilogue
        }
        if (fpu::le(plane_value, negative_limit)) {             // c.ole.s; bc1t taken to 0x2bf174
            retired += 4U;                                      // the bc1t's delay slot, addiu t0
        } else {
            v0 = 1U;                                            // daddu v0,a0,zero (a0 is 1 by now)
            retired += 5U;                                      // the daddu, addiu t0
        }
        retired += 3U;                                          // slti, bne, delay slot nop
    }

    // 0x2bf184..0x2bf1c8: the table columns transform Q.xyz; the x lane is tested against S here.
    cpu.write_vu0_vector(3U, record);                           // lqc2 vf3,688(sp): the record's frame copy
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, table + 32U));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, table + 48U));
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, 15U, 0U);          // vmulx.xyzw vf2,vf2,vf3x
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 15U, 1U);          // vmuly.xyzw vf1,vf1,vf3y
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, 15U, false);          // vadd.xyzw vf2,vf2,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, table + 64U));
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, table + 80U));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 15U, 2U);          // vmulz.xyzw vf1,vf1,vf3z
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 15U, false);          // vadd.xyzw vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 15U, true);           // vsub.xyzw vf1,vf1,vf4
    const guest::Gpr128 point = cpu.read_vu0_vector(1U);        // qmfc2 v1,vf1; the frame copies read back its words
    retired += 18U;

    // Lanes x, y, z, w against S: y, z and w have their own 6-instruction blocks.
    for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
        if (lane != 0U) {
            retired += 6U;
        }
        if (!fpu::le(point.words[lane], limit)) {
            return exit_with(0U, retired + 5U);                 // 0x2bf278: beq, its delay slot, the epilogue
        }
    }

    // Lanes x, y, z, w against -S: the first lane above -S stores the depth and returns 1.
    for (std::uint32_t lane = 0U; lane < 4U; ++lane) {
        retired += (lane == 0U ? 3U : 6U);
        if (!fpu::le(point.words[lane], negative_limit)) {
            const std::uint32_t largest = max_bits(max_bits(point.words[0], point.words[1]),
                                                   max_bits(point.words[2], point.words[3]));
            m.write_u32_le(call.arg(2) + 32U, fpu::sub(limit, largest));    // sub.s f0,f4,f0; swc1 f0,32(a2)
            return exit_with(1U, retired + 22U);                // 0x2bf280..0x2bf2c4, then the return
        }
    }
    retired += 4U;                                              // 0x2bf268, its delay slot, the return
    return exit_with(v0, retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(002bf0f0, 484U, 0x9b2464755d2994d7ULL, "SlabPenetrationDepth", 142U, slab_penetration_depth,
               EngineReturn::Integer)
