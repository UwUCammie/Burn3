// 0x00147d00 (a0 = strip object, a1 = quad index in its low halfword, a2 = destination): StripQuadStep.
// The object's strip pointer (u32 at +4) gives the index table (u32 at +0), the quad count (halfword at +12) and
// the closed flag (byte at +14). Quad k's corners are table halfwords 2k..2k+3, so neighbouring quads share an
// edge. The result is the xyz sum of quad k+2's corners less the xyz sum of quad k's corners, with w from quad
// k+2's third corner, stored as a quadword at a2. Corners past the end of the strip wrap to its start when it is
// closed, or repeat its last edge when it is open. Vertices are 16-byte entries at the u32 at 0x0064d354. The VU0
// sequence runs in the original's order on VF1..VF4. Returns nothing; its only memory write is the store to a2.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kVertices = 0x0064d354U;   // lui 0x65, lw -11436: 16-byte vertex entries
constexpr std::uint32_t kXyz = 14U;               // VU0 destination mask x, y, z (w is kept)

struct AheadCorners {
    std::uint32_t corner[4];  // table halfwords of quad k+2's corners
    std::uint32_t retired;    // the original's retired count on this path, delay slots and return included
};

// The corners of quad k+2 as the original's branches read them. Quad k is two before the last quad (listing
// 0x147e18: 66 retired when closed, 67 when open), the last quad (0x147d24: 65 closed, 66 open) or any other quad
// (0x147e60: 64). Every path then runs the common tail, which the counts above include.
template <class Memory>
AheadCorners corners_ahead(const Memory& m, const GuestAddress indices, const std::uint32_t quad,
                           const std::uint32_t quads, const bool closed) {
    const auto halfword = [&](const std::uint32_t j) {
        return static_cast<std::uint32_t>(m.read_u16_le(indices + j * 2U));
    };
    if (quad == quads - 2U) {
        if (closed) {
            return {{halfword(2U * quad + 4U), halfword(2U * quad + 5U), halfword(2U), halfword(3U)}, 66U};
        }
        return {{halfword(2U * quad + 4U), halfword(2U * quad + 5U), halfword(2U * quads),
                 halfword(2U * quads + 1U)}, 67U};
    }
    if (quad == quads - 1U) {
        if (closed) {
            return {{halfword(2U), halfword(3U), halfword(4U), halfword(5U)}, 65U};
        }
        return {{halfword(2U * quads), halfword(2U * quads + 1U), halfword(2U * quads),
                 halfword(2U * quads + 1U)}, 66U};
    }
    return {{halfword(2U * quad + 4U), halfword(2U * quad + 5U), halfword(2U * quad + 6U),
             halfword(2U * quad + 7U)}, 64U};
}

template <class Context>
n3::Run strip_quad_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress strip = m.read_u32_le(call.arg(0) + 4U);
    const GuestAddress indices = m.read_u32_le(strip);
    const std::uint32_t quads = m.read_u16_le(strip + 12U);
    const bool closed = m.read_u8(strip + 14U) != 0U;
    const std::uint32_t quad = call.arg(1) & 0xffffU;
    const AheadCorners ahead = corners_ahead(m, indices, quad, quads, closed);
    const auto halfword = [&](const std::uint32_t j) {
        return static_cast<std::uint32_t>(m.read_u16_le(indices + j * 2U));
    };
    const std::uint32_t c0 = halfword(2U * quad);
    const std::uint32_t c1 = halfword(2U * quad + 1U);
    const std::uint32_t c2 = halfword(2U * quad + 2U);
    const std::uint32_t c3 = halfword(2U * quad + 3U);
    const GuestAddress vertices = m.read_u32_le(kVertices);
    const auto vertex = [&](const std::uint32_t index) { return vertices + (index << 4U); };

    // The original's VU0 sequence (0x147da4..0x147e14), in its order on VF1..VF4.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex(ahead.corner[1])));  // lqc2 vf1 (ahead 1)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertex(c1)));               // lqc2 vf3 (corner 1)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, vertex(c0)));               // lqc2 vf4 (corner 0)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex(c2)));               // lqc2 vf2 (corner 2)
    ee_cop2::vadd_vector(cpu, 3U, 4U, 3U, kXyz, false);                       // vadd.xyz vf3,vf4,vf3
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, false);                       // vadd.xyz vf3,vf3,vf2
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, vertex(ahead.corner[0])));  // lqc2 vf2 (ahead 0)
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, vertex(c3)));               // lqc2 vf4 (corner 3)
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, false);                       // vadd.xyz vf2,vf2,vf1
    ee_cop2::vadd_vector(cpu, 4U, 3U, 4U, kXyz, false);                       // vadd.xyz vf4,vf3,vf4
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, vertex(ahead.corner[2])));  // lqc2 vf1 (ahead 2)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, vertex(ahead.corner[3])));  // lqc2 vf3 (ahead 3)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, kXyz, false);                       // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 3U, kXyz, false);                       // vadd.xyz vf1,vf1,vf3
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, kXyz, true);                        // vsub.xyz vf1,vf1,vf4
    n3::store_quad_any(m, call.arg(2), cpu.read_vu0_vector(1U));              // sqc2 vf1,0(a2)
    return call.finish(ahead.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00147d00, 392U, 0x52f2acd6d9287952ULL, "StripQuadStep", 67U, strip_quad_step, EngineReturn::Nothing)
