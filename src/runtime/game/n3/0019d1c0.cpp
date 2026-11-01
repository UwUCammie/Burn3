// 0x0019d1c0 (a0 list; a1..a3 and t0 the four vertex positions; t1..t3 and the first stack word their colours):
// AppendColouredQuad. Unless the list already holds more than 80 of its 85 vertices, writes the four positions
// (xyz) and colours into the next four 32-byte entries at +5376 and counts them at +8068. A colour is its quad
// times 128 (VU0, VF4.x), truncated to ints (vftoi0), clamped by PMINW to 255 (x, y, z) and 128 (w), then packed
// to one byte per lane at entry +24. Entries 0 and 1 also get w = 0 + the float at gp-30752. Returns nothing.
// Every VU0 instruction of the listing runs in the same order through the same helpers, so the VU0 state (and the
// GPR flows into it, a1 and v1 after mfc1) is the original's.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/ee_mmi.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace ee_mmi = guest::ee_mmi;

constexpr GuestAddress kCount = 8068U;                               // word: the vertex count
constexpr GuestAddress kEntries = 5376U;                             // 32-byte entries, from entry 0
constexpr GuestAddress kColour = 5400U;                              // entry + 24, relative to the list
constexpr GuestAddress kWFloat = static_cast<GuestAddress>(-30752);  // gp-relative float: the w of entries 0, 1
constexpr std::int32_t kCountLimit = 85;                             // count + 4 must stay below this
constexpr std::uint32_t kEarlyCost = 55U;                            // the return when the list is full
constexpr std::uint32_t kFullCost = 158U;                            // the longest path

// PMINW limits: 255 for x, y and z and 128 for w. The listing builds them as pextlh(0, 0x008000ff00ff00ff).
Gpr128 colour_limits() {
    return ee_mmi::pextlh(Gpr128{}, Gpr128{{0x00ff00ffU, 0x008000ffU, 0U, 0U}});
}

// The packed colour word of a quad of VU0 ints: PMINW against the limits, then PPACH and PPACB. Byte k of the
// word is lane k.
std::uint32_t packed_colour(const Gpr128& ints, const Gpr128& limits) {
    const Gpr128 zero{};
    const Gpr128 clamped = ee_mmi::pmaxw(ints, limits, false);
    return ee_mmi::ppacb(zero, ee_mmi::ppach(zero, clamped)).words[0];
}

// The four colour bytes of a packed word, stored at address .. address + 3 (the listing's four sb).
template <class Memory>
void store_colour(Memory& memory, const GuestAddress address, const std::uint32_t packed) {
    for (unsigned lane = 0U; lane < 4U; ++lane) {
        memory.write_u8(address + lane, static_cast<std::uint8_t>(packed >> (8U * lane)));
    }
}

template <class Context>
n3::Run append_coloured_quad(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // The arguments, read before any register is rewritten. The ninth is the stack word at the entry $sp.
    const GuestAddress list = call.arg(0U);
    const GuestAddress position0 = call.arg(1U);
    const GuestAddress position1 = call.arg(2U);
    const GuestAddress position2 = call.arg(3U);
    const GuestAddress position3 = call.arg(4U);
    const GuestAddress colour0 = call.arg(5U);
    const GuestAddress colour1 = call.arg(6U);
    const GuestAddress colour2 = call.arg(7U);
    const GuestAddress colour3 = m.read_u32_le(call.stack_pointer());

    // 0x19d1c4..0x19d26c: the four colour quads times 128 (VF4.x = 0x43000000), truncated to ints in VF5, VF3,
    // VF2 and VF1, with the positions loaded into the GPRs the listing uses (t4, a1, v1, t6).
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, colour0));                 // lqc2 vf1,0(t1)
    cpu.write_gpr_sign_extended_32(13U, 0x43000000U);                        // lui t5,0x4300
    cpu.write_vu0_vector(4U, cpu.read_gpr(13U));                             // qmtc2 t5,vf4
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 4U, 15U, 0U);                       // vmulx.xyzw vf1,vf1,vf4x
    cpu.write_gpr(12U, n3::load_quad_any(m, position0));                     // lq t4,0(a1)
    cpu.write_gpr(5U, n3::load_quad_any(m, position1));                      // lq a1,0(a2)
    cpu.write_gpr(3U, n3::load_quad_any(m, position2));                      // lq v1,0(a3)
    cpu.write_gpr(14U, n3::load_quad_any(m, position3));                     // lq t6,0(t0)
    ee_cop2::vftoi(cpu, 5U, 1U, 15U, 0U);                                    // vftoi0.xyzw vf5,vf1
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, colour3));                 // lqc2 vf1,0(t1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, colour1));                 // lqc2 vf3,0(t2)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, colour2));                 // lqc2 vf2,0(t3)
    ee_cop2::vmul_broadcast(cpu, 3U, 3U, 4U, 15U, 0U);                       // vmulx.xyzw vf3,vf3,vf4x
    ee_cop2::vftoi(cpu, 3U, 3U, 15U, 0U);                                    // vftoi0.xyzw vf3,vf3
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 4U, 15U, 0U);                       // vmulx.xyzw vf2,vf2,vf4x
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 4U, 15U, 0U);                       // vmulx.xyzw vf1,vf1,vf4x
    ee_cop2::vftoi(cpu, 2U, 2U, 15U, 0U);                                    // vftoi0.xyzw vf2,vf2
    ee_cop2::vftoi(cpu, 1U, 1U, 15U, 0U);                                    // vftoi0.xyzw vf1,vf1

    const Gpr128 limits = colour_limits();
    const std::uint32_t packed0 = packed_colour(cpu.read_vu0_vector(5U), limits);
    const std::uint32_t packed1 = packed_colour(cpu.read_vu0_vector(3U), limits);
    const std::uint32_t packed2 = packed_colour(cpu.read_vu0_vector(2U), limits);
    const std::uint32_t packed3 = packed_colour(cpu.read_vu0_vector(1U), limits);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(list + kCount));
    if (static_cast<std::int32_t>(static_cast<std::uint32_t>(count) + 4U) >= kCountLimit) {
        return call.finish(kEarlyCost);
    }

    // 0x19d294..0x19d42c: vertex k goes to entry count + k.
    const std::uint32_t first = static_cast<std::uint32_t>(count);
    const auto base = [list, first](const std::uint32_t k) { return list + ((first + k) << 5U); };

    // Entry 0: position 0 in xyz (VF0 + P0), colour 0, and w = 0 + the gp float.
    const GuestAddress entry0 = base(0U) + kEntries;
    const std::uint32_t w = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kWFloat);  // lwc1 f0,-30752(gp)
    cpu.write_fpr_bits(0U, w);
    cpu.write_vu0_vector(1U, cpu.read_gpr(12U));                             // qmtc2 t4,vf1: P0
    cpu.write_vu0_vector(7U, n3::load_quad_any(m, entry0));                  // lqc2 vf7,5376(a3)
    cpu.write_vu0_vector(6U, cpu.read_gpr(5U));                              // qmtc2 a1,vf6: P1
    ee_cop2::vadd_vector(cpu, 7U, 0U, 1U, 14U, false);                       // vadd.xyz vf7,vf0,vf1
    cpu.write_gpr_sign_extended_32(5U, w);                                   // mfc1 a1,f0
    cpu.write_vu0_vector(5U, cpu.read_gpr(3U));                              // qmtc2 v1,vf5: P2
    n3::store_quad_any(m, entry0, cpu.read_vu0_vector(7U));                  // sqc2 vf7,5376(a3)
    cpu.write_gpr_sign_extended_32(3U, w);                                   // mfc1 v1,f0
    cpu.write_vu0_vector(3U, cpu.read_gpr(5U));                              // qmtc2 a1,vf3
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));                              // qmtc2 v1,vf2
    cpu.write_vu0_vector(4U, cpu.read_gpr(14U));                             // qmtc2 t6,vf4: P3
    store_colour(m, base(0U) + kColour, packed0);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry0));                  // lqc2 vf1,5376(t8)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 1U, 3U, true);                 // vsubw.w vf1,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 3U, 1U, 0U, false);                // vaddx.w vf1,vf1,vf3x
    n3::store_quad_any(m, entry0, cpu.read_vu0_vector(1U));                  // sqc2 vf1,5376(t8)
    m.write_u32_le(list + kCount, first + 1U);

    // Entry 1: position 1 in xyz (VF0 + P1), colour 1, and w = 0 + the gp float (VF2.x).
    const GuestAddress entry1 = base(1U) + kEntries;
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry1));                  // lqc2 vf1,5376(t8)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 6U, 14U, false);                       // vadd.xyz vf1,vf0,vf6
    n3::store_quad_any(m, entry1, cpu.read_vu0_vector(1U));                  // sqc2 vf1,5376(t8)
    store_colour(m, base(1U) + kColour, packed1);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry1));                  // lqc2 vf1,5376(t4)
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 1U, 3U, true);                  // vsubw.w vf1,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 2U, 1U, 0U, false);                 // vaddx.w vf1,vf1,vf2x
    n3::store_quad_any(m, entry1, cpu.read_vu0_vector(1U));                  // sqc2 vf1,5376(t4)
    m.write_u32_le(list + kCount, first + 2U);

    // Entry 2: position 2 in xyz (VF0 + P2), colour 2; w kept.
    const GuestAddress entry2 = base(2U) + kEntries;
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry2));                  // lqc2 vf1,5376(t4)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 5U, 14U, false);                       // vadd.xyz vf1,vf0,vf5
    n3::store_quad_any(m, entry2, cpu.read_vu0_vector(1U));                  // sqc2 vf1,5376(t4)
    store_colour(m, base(2U) + kColour, packed2);
    m.write_u32_le(list + kCount, first + 3U);

    // Entry 3: position 3 in xyz (VF0 + P3), colour 3; w kept.
    const GuestAddress entry3 = base(3U) + kEntries;
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, entry3));                  // lqc2 vf1,5376(t0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 4U, 14U, false);                       // vadd.xyz vf1,vf0,vf4
    n3::store_quad_any(m, entry3, cpu.read_vu0_vector(1U));                  // sqc2 vf1,5376(t0)
    store_colour(m, base(3U) + kColour, packed3);
    m.write_u32_le(list + kCount, first + 4U);
    return call.finish(kFullCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0019d1c0, 632U, 0xfcecd14e5a0d5a0bULL, "AppendColouredQuad", 158U, append_coloured_quad,
               EngineReturn::Nothing)
