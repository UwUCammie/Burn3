// 0x002ba9f0 (a0 = box-hull record, a1 = quad pointer: one box corner "min", a2 = quad pointer: the other
// corner "max"): BuildBoxHullTables. The record holds four pointers: +0 a 36-byte index table, +4 twelve plane
// quads (16 bytes each), +8 eight corner quads and +12 a second 36-byte index table; it also gets word 0 at +20 and
// the bytes +24 = 8, +25 = 12, +26 = 18.
//   A: each corner quad is loaded, then its x, y and z lanes are replaced by the matching lane of min (VF9) or
//      max (VF1) (one VU0 lane op each; w keeps what the quad held), and stored back.
//   B: each plane quad gets xyz from one of six axis-unit-vector constants at 0x004c1bd0..0x004c1c20 (vadd.xyz,
//      w keeps the old value).
//   C: each plane quad's w lane becomes 1.0 * one lane of VF1..VF7 (a negated min component from the vsub ops of
//      the preamble, or a max component).
//   D: the two index tables are byte stores of small constants.
// The record pointers are re-read from the record before every group of stores, exactly as the original does
// (the stores could alias the record). All VU0 work runs on the real VU0 registers (VF1..VF9, MAC/STATUS flags).
// The original saves and restores s0-s7/fp in its own frame; the native never touches them. Straight line,
// 313 instructions retired.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kRetired = 313U;

// VU0 destination masks (x is the high bit) and lanes.
constexpr std::uint8_t kX = 8U, kY = 4U, kZ = 2U;
constexpr std::uint8_t kXyz = n3::kMaskXyz;
constexpr std::uint8_t kW = n3::kMaskW;

struct Corner {
    std::uint32_t offset;   // quad offset in the corner table
    unsigned fd;            // the VU0 register the original uses for it
    unsigned x_src;         // 9 = min (vf9), 1 = max (vf1)
    unsigned y_src;
    unsigned z_src;
};

struct Plane {
    std::uint32_t offset;     // quad offset in the plane table
    std::uint32_t constant;   // address of the unit-vector quad
    unsigned w_fd;            // register loaded and written back by the w fixup
    unsigned w_src;           // register whose lane scales 1.0 into w
    std::size_t w_lane;       // 0 = x, 1 = y, 2 = z
};

constexpr Plane kPlanes[12] = {
    {0U, 0x004c1c20U, 8U, 7U, 1U},   {16U, 0x004c1c10U, 7U, 1U, 1U},  {32U, 0x004c1bd0U, 7U, 6U, 2U},
    {48U, 0x004c1be0U, 6U, 1U, 0U},  {64U, 0x004c1c00U, 6U, 1U, 2U},  {80U, 0x004c1bf0U, 6U, 5U, 0U},
    {96U, 0x004c1bd0U, 5U, 4U, 2U},  {112U, 0x004c1be0U, 4U, 1U, 0U}, {128U, 0x004c1c00U, 4U, 1U, 2U},
    {144U, 0x004c1bf0U, 4U, 3U, 0U}, {160U, 0x004c1c10U, 3U, 1U, 1U}, {176U, 0x004c1c20U, 1U, 2U, 1U},
};

// Index table 1 (record +0): 36 bytes, the pointer re-read before every third byte.
constexpr std::uint8_t kTable1[36] = {4, 5, 6, 0, 1, 2, 3, 2, 5, 2, 1, 6, 1, 0, 7, 0, 3, 4,
                                      3, 5, 4, 2, 6, 5, 1, 7, 6, 0, 4, 7, 0, 2, 3, 4, 6, 7};
// Index table 2 (record +12): 36 bytes, the pointer re-read before every second byte.
constexpr std::uint8_t kTable2[36] = {0, 1, 1, 2, 2, 3, 3, 0, 0, 2, 4, 5, 5, 6, 6, 7, 7, 4,
                                      4, 6, 0, 7, 1, 6, 2, 5, 3, 4, 0, 4, 3, 5, 2, 6, 1, 7};

template <class Context>
n3::Run build_box_hull_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    const auto lqc2 = [&](const unsigned vf, const GuestAddress address) {
        cpu.write_vu0_vector(vf, n3::load_quad_any(m, address));
    };
    const auto sqc2 = [&](const unsigned vf, const GuestAddress address) {
        n3::store_quad_any(m, address, cpu.read_vu0_vector(vf));
    };

    // ---- A: preamble and the eight corner quads (0x2ba9f0..0x2bab54) ----
    lqc2(9U, call.arg(1));                                                        // lqc2 vf9,0(a1)
    ee_cop2::vadd_vector(cpu, 7U, 0U, 9U, kY, true);                              // vsub.y vf7,vf0,vf9
    ee_cop2::vadd_vector(cpu, 6U, 0U, 9U, kZ, true);                              // vsub.z vf6,vf0,vf9
    m.write_u32_le(record + 20U, 0U);                                             // sw zero,20(a0)
    GuestAddress corners = m.read_u32_le(record + 8U);                            // lw s6,8(a0)
    ee_cop2::vadd_vector(cpu, 5U, 0U, 9U, kX, true);                              // vsub.x vf5,vf0,vf9
    lqc2(1U, call.arg(2));                                                        // lqc2 vf1,0(a2)
    lqc2(2U, corners);                                                            // lqc2 vf2,0(s6)
    ee_cop2::vadd_vector(cpu, 4U, 0U, 9U, kZ, true);                              // vsub.z vf4,vf0,vf9
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 9U, kX, 0U, false);                      // vaddx.x vf2,vf0,vf9x
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kY, 1U, false);                      // vaddy.y vf2,vf0,vf1y
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kZ, 2U, false);                      // vaddz.z vf2,vf0,vf1z
    sqc2(2U, corners);                                                            // sqc2 vf2,0(s6)
    corners = m.read_u32_le(record + 8U);                                         // lw s6,8(a0)
    ee_cop2::vadd_vector(cpu, 3U, 0U, 9U, kX, true);                              // vsub.x vf3,vf0,vf9
    ee_cop2::vadd_vector(cpu, 2U, 0U, 9U, kY, true);                              // vsub.y vf2,vf0,vf9
    // Corner quads 1..7, in the original's order (16, 32, 48, 112, 96, 80, 64).
    static constexpr Corner kCorners[7] = {
        {16U, 8U, 1U, 1U, 1U}, {32U, 8U, 1U, 1U, 9U},  {48U, 8U, 9U, 1U, 9U}, {112U, 8U, 9U, 9U, 1U},
        {96U, 8U, 1U, 9U, 1U}, {80U, 8U, 1U, 9U, 9U},  {64U, 8U, 9U, 9U, 9U},
    };
    for (std::size_t i = 0U; i < 7U; ++i) {
        const Corner& c = kCorners[i];
        lqc2(c.fd, corners + c.offset);                                           // lqc2 vf8,off(s6)
        ee_cop2::vadd_broadcast(cpu, c.fd, 0U, c.x_src, kX, 0U, false);           // vaddx.x
        ee_cop2::vadd_broadcast(cpu, c.fd, 0U, c.y_src, kY, 1U, false);           // vaddy.y
        ee_cop2::vadd_broadcast(cpu, c.fd, 0U, c.z_src, kZ, 2U, false);           // vaddz.z
        sqc2(c.fd, corners + c.offset);                                           // sqc2 vf8,off(s6)
        if (i + 1U < 7U) {
            corners = m.read_u32_le(record + 8U);                                 // lw s6,8(a0)
        }
    }
    m.write_u8(record + 24U, 8U);                                                 // sb s5,24(a0)

    // ---- B: the twelve plane xyz copies (0x2bab58..0x2bac44) ----
    for (const Plane& p : kPlanes) {
        lqc2(8U, p.constant);                                                     // lqc2 vf8,const
        const GuestAddress planes = m.read_u32_le(record + 4U);                   // lw rX,4(a0)
        lqc2(9U, planes + p.offset);                                              // lqc2 vf9,off(rX)
        ee_cop2::vadd_vector(cpu, 9U, 0U, 8U, kXyz, false);                       // vadd.xyz vf9,vf0,vf8
        sqc2(9U, planes + p.offset);                                              // sqc2 vf9,off(rX)
    }

    // ---- C: the twelve plane w fixups (0x2bac48..0x2bad04) ----
    for (const Plane& p : kPlanes) {
        const GuestAddress planes = m.read_u32_le(record + 4U);                   // lw v1,4(a0)
        lqc2(p.w_fd, planes + p.offset);                                          // lqc2 vfD,off(v1)
        ee_cop2::vmul_broadcast(cpu, p.w_fd, 0U, p.w_src, kW, p.w_lane);          // vmul{x,y,z}.w vfD,vf0,vfS
        sqc2(p.w_fd, planes + p.offset);                                          // sqc2 vfD,off(v1)
    }

    // ---- D: the two index tables and the last record bytes (0x2bad08..0x2baea4) ----
    m.write_u8(record + 25U, 12U);                                                // sb s7,25(a0)
    GuestAddress table = 0U;
    for (std::uint32_t i = 0U; i < 36U; ++i) {
        if (i % 3U == 0U) {
            table = m.read_u32_le(record);                                        // lw v1,0(a0)
        }
        m.write_u8(table + i, kTable1[i]);
    }
    for (std::uint32_t i = 0U; i < 36U; ++i) {
        if (i % 2U == 0U) {
            table = m.read_u32_le(record + 12U);                                  // lw v1,12(a0)
        }
        m.write_u8(table + i, kTable2[i]);
    }
    m.write_u8(record + 26U, 18U);                                                // sb fp,26(a0)
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002ba9f0{0x002ba9f0U, 1252U, 0x0caa7a84aed95bebULL, "BuildBoxHullTables",
                                                  kRetired, n3::live<&build_box_hull_tables<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002ba9f0, build_box_hull_tables)
