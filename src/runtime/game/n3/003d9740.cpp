// 0x003d9740 (a0 record, a1 output block, a2 four floats): BuildCornerBoundsBlock. The record's first word T
// points to a table of 16-byte quads. The function reduces twelve of them (offsets 0..480 from T) with
// MIN.S / MAX.S into the x / y / z lanes of four rows at a1+0..48 (rows keep their old w), clips rows 0/1 and
// 2/3 against the quads at T+128 and T+192 (two DIV.S), copies the rows to a1+64..112 and sets their y lanes
// from the four floats at a2 plus the quad y lanes, then raises each copy's y by MAX(row_y_difference, C)
// where C is the float at $gp-31136. It runs a fixed 378 instructions: the loop at 0x3d9be8 counts $a3 from
// 0 to 4. Returns nothing (the caller ignores v0/v1/f0).
//
// The VU0 macro instructions run on the real VU0 state through ee_cop2 and the general registers are tracked as
// 128-bit values, because qmtc2 copies a whole register (stale upper doublewords included) into a VF register.
// The stack traffic through $sp (store a lane, reload it) is replaced by locals; the one uninitialised
// lqc2 at sp-16 (0x3d97f0, frame slot 2656) is read from guest memory.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <array>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFixedCost = 378U;  // 298 + 5*3 + 4*15 + 5 (see the block listing)
constexpr std::uint32_t kGpC = 0xFFFF8660U;  // -31136 from $gp
constexpr std::uint32_t kFrame = 2672U;
constexpr std::uint32_t kUninitialisedSlot = 2656U;

// General register numbers.
constexpr unsigned kV1 = 3U, kA2 = 6U, kA3 = 7U, kT0 = 8U, kT1 = 9U, kT2 = 10U, kT3 = 11U, kT4 = 12U, kT5 = 13U,
                   kT6 = 14U, kT7 = 15U, kS0 = 16U, kS1 = 17U, kS2 = 18U, kT8 = 24U, kT9 = 25U;

template <class Context>
n3::Run build_corner_bounds_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress out = call.arg(1);
    const GuestAddress floats = call.arg(2);
    const GuestAddress sp = call.stack_pointer();

    std::array<Gpr128, 32> r{};
    for (unsigned i = 0U; i < 32U; ++i) {
        r[i] = cpu.read_gpr(i);
    }
    // A 32-bit result (lw, mfc1, lui) writes the low doubleword sign-extended and keeps the upper one.
    const auto set32 = [&](const unsigned reg, const std::uint32_t value) {
        r[reg].words[0] = value;
        r[reg].words[1] = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    };
    const auto lq = [&](const unsigned reg, const GuestAddress address) { r[reg] = n3::load_quad_any(m, address); };
    const auto qmtc2 = [&](const unsigned vf, const unsigned reg) { cpu.write_vu0_vector(vf, r[reg]); };
    const auto vf = [&](const unsigned index) { return cpu.read_vu0_vector(index); };
    // vaddx.<mask> / vsubz.<mask> ...: FD = FS +/- FT.lane. Masks: x 8, y 4, z 2.
    const auto add_lane = [&](const unsigned fd, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                              const std::size_t lane) { ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, false); };
    const auto sub_lane = [&](const unsigned fd, const unsigned fs, const unsigned ft, const std::uint8_t mask,
                              const std::size_t lane) { ee_cop2::vadd_broadcast(cpu, fd, fs, ft, mask, lane, true); };
    constexpr std::uint8_t kX = 8U, kY = 4U, kZ = 2U;

    const GuestAddress table = m.read_u32_le(record);            // lw t2,0(a0)
    set32(kV1, m.read_u32_le(floats));                           // lw v1,0(a2)
    lq(kT1, table);                                              // lq t1,0(t2)
    qmtc2(1U, kV1);                                              // qmtc2 v1,vf1
    lq(kT0, table + 256U);                                       // lq t0,256(t2)
    qmtc2(8U, kT1);                                              // qmtc2 t1,vf8
    add_lane(1U, 8U, 1U, kY, 0U);                                // vaddx.y vf1,vf8,vf1x
    set32(kA3, m.read_u32_le(floats + 4U));                      // lw a3,4(a2)
    set32(kT5, vf(1U).words[1]);                                 // sqc2 vf1,1424(sp); lw t5,1428(sp)
    qmtc2(1U, kA3);                                              // qmtc2 a3,vf1
    qmtc2(7U, kT0);                                              // qmtc2 t0,vf7
    add_lane(1U, 7U, 1U, kY, 0U);                                // vaddx.y vf1,vf7,vf1x
    lq(kA3, table + 64U);                                        // lq a3,64(t2)
    set32(kT3, m.read_u32_le(floats + 8U));                      // lw t3,8(a2)
    set32(kT4, vf(1U).words[1]);                                 // sqc2 vf1,1392(sp); lw t4,1396(sp)
    qmtc2(1U, kT3);                                              // qmtc2 t3,vf1
    qmtc2(6U, kA3);                                              // qmtc2 a3,vf6
    add_lane(1U, 6U, 1U, kY, 0U);                                // vaddx.y vf1,vf6,vf1x
    set32(kA2, m.read_u32_le(floats + 12U));                     // lw a2,12(a2)
    lq(kV1, table + 320U);                                       // lq v1,320(t2)
    set32(kT3, vf(1U).words[1]);                                 // sqc2 vf1,1360(sp); lw t3,1364(sp)
    qmtc2(1U, kA2);                                              // qmtc2 a2,vf1
    set32(kT7, 0x3f800000U);                                     // lui t7,0x3f80
    r[kS0].words[0] = 0U;                                        // daddu s0,zero,zero
    r[kS0].words[1] = 0U;
    qmtc2(4U, kV1);                                              // qmtc2 v1,vf4
    add_lane(5U, 4U, 1U, kY, 0U);                                // vaddx.y vf5,vf4,vf1x
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, sp - kFrame + kUninitialisedSlot));  // lqc2 vf1,2656(sp)
    qmtc2(3U, kT7);                                              // qmtc2 t7,vf3
    qmtc2(2U, kS0);                                              // qmtc2 s0,vf2
    add_lane(1U, 0U, 3U, kX, 0U);                                // vaddx.x vf1,vf0,vf3x
    add_lane(1U, 0U, 2U, kY, 0U);                                // vaddx.y vf1,vf0,vf2x
    add_lane(1U, 0U, 2U, kZ, 0U);                                // vaddx.z vf1,vf0,vf2x
    set32(kA2, vf(5U).words[1]);                                 // sqc2 vf5,1328(sp); lw a2,1332(sp)

    // The y-lane minimum of the six quads at T+0, +64, +128, +192, +256 and +320.
    std::uint32_t f1 = vf(8U).words[1];                          // lwc1 f1,1316(sp)
    std::uint32_t f0 = vf(6U).words[1];                          // lwc1 f0,1300(sp)
    f1 = ee_cop2::minimum_bits(f1, f0);
    lq(kT7, table + 128U);
    f0 = r[kT7].words[1];
    f1 = ee_cop2::minimum_bits(f1, f0);
    lq(kT7, table + 192U);
    f0 = r[kT7].words[1];
    f1 = ee_cop2::minimum_bits(f1, f0);
    f0 = vf(7U).words[1];
    f1 = ee_cop2::minimum_bits(f1, f0);
    f0 = vf(4U).words[1];
    f0 = ee_cop2::minimum_bits(f1, f0);
    const std::uint32_t y_min = f0;

    lq(kT6, table + 32U);
    std::uint32_t f2 = r[kT1].words[0];                          // mtc1 t1,f2
    f1 = r[kT6].words[0];                                        // mtc1 t6,f1
    const std::uint32_t f10 = ee_cop2::maximum_bits(f2, f1);
    f2 = vf(8U).words[2];
    f1 = r[kT6].words[2];
    const std::uint32_t f9 = ee_cop2::maximum_bits(f2, f1);
    lq(kT1, table + 96U);
    f2 = r[kA3].words[0];
    f1 = r[kT1].words[0];
    const std::uint32_t f8 = ee_cop2::minimum_bits(f2, f1);
    f2 = vf(6U).words[2];
    f1 = r[kT1].words[2];
    const std::uint32_t f7 = ee_cop2::maximum_bits(f2, f1);
    lq(kT1, table + 288U);
    f2 = r[kT0].words[0];
    f1 = r[kT1].words[0];
    f2 = ee_cop2::maximum_bits(f2, f1);
    lq(kT0, table + 448U);
    f1 = r[kT0].words[0];
    const std::uint32_t f6 = ee_cop2::maximum_bits(f2, f1);
    f2 = vf(7U).words[2];
    f1 = r[kT1].words[2];
    f2 = ee_cop2::minimum_bits(f2, f1);
    f1 = r[kT0].words[2];
    const std::uint32_t f5 = ee_cop2::minimum_bits(f2, f1);
    lq(kT0, table + 352U);
    f2 = r[kV1].words[0];
    f1 = r[kT0].words[0];
    f2 = ee_cop2::minimum_bits(f2, f1);
    lq(kA3, table + 480U);
    f1 = r[kA3].words[0];
    const std::uint32_t f4 = ee_cop2::minimum_bits(f2, f1);
    f2 = vf(4U).words[2];
    f1 = r[kT0].words[2];
    f2 = ee_cop2::minimum_bits(f2, f1);
    f1 = r[kA3].words[2];
    f1 = ee_cop2::minimum_bits(f2, f1);

    // Rows 0..3 at a1+0..48: x, y, z lanes replaced by the reductions, w kept (0x3d9970..0x3d9a28).
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out));             // lqc2 vf2,0(a1)
    set32(kT0, f10);
    set32(kS1, f6);
    set32(kS0, y_min);
    qmtc2(3U, kT0);
    set32(kT9, f5);
    set32(kT1, y_min);
    set32(kS2, f9);
    set32(kT8, f8);
    add_lane(2U, 0U, 3U, kX, 0U);
    set32(kT7, y_min);
    set32(kT6, f7);
    set32(kT2, f4);
    qmtc2(3U, kT1);
    set32(kT0, f1);
    add_lane(2U, 0U, 3U, kY, 0U);
    set32(kT1, y_min);
    qmtc2(3U, kS2);
    add_lane(2U, 0U, 3U, kZ, 0U);
    n3::store_quad_any(m, out, vf(2U));                              // sqc2 vf2,0(a1)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 16U));
    qmtc2(11U, kS1);
    qmtc2(10U, kS0);
    add_lane(2U, 0U, 11U, kX, 0U);
    qmtc2(9U, kT9);
    add_lane(2U, 0U, 10U, kY, 0U);
    add_lane(2U, 0U, 9U, kZ, 0U);
    n3::store_quad_any(m, out + 16U, vf(2U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 32U));
    qmtc2(8U, kT8);
    qmtc2(7U, kT7);
    add_lane(2U, 0U, 8U, kX, 0U);
    qmtc2(6U, kT6);
    add_lane(2U, 0U, 7U, kY, 0U);
    add_lane(2U, 0U, 6U, kZ, 0U);
    n3::store_quad_any(m, out + 32U, vf(2U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 48U));
    qmtc2(5U, kT2);
    qmtc2(4U, kT1);
    add_lane(2U, 0U, 5U, kX, 0U);
    qmtc2(3U, kT0);
    add_lane(2U, 0U, 4U, kY, 0U);
    add_lane(2U, 0U, 3U, kZ, 0U);
    n3::store_quad_any(m, out + 48U, vf(2U));

    // Clip rows 0/1 against the quad at T+128 (MIN.S with f3 = 0.0), then rows 2/3 against T+192 (MAX.S).
    constexpr std::uint32_t kF3 = 0U;                            // mtc1 zero,f3
    constexpr std::uint8_t kXyz = 14U;
    set32(kT0, m.read_u32_le(record));                           // lw t0,0(a0)
    const GuestAddress clip_table = r[kT0].words[0];
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, out));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 16U));
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, clip_table + 128U));
    sub_lane(3U, 2U, 4U, kZ, 2U);                                // vsubz.z vf3,vf2,vf4z
    std::uint32_t denominator = vf(3U).words[2];                 // sqc2 vf3,336(sp); lwc1 f0,344(sp)
    sub_lane(3U, 2U, 4U, kX, 0U);                                // vsubx.x vf3,vf2,vf4x
    sub_lane(2U, 5U, 4U, kZ, 2U);                                // vsubz.z vf2,vf5,vf4z
    set32(kV1, vf(2U).words[2]);                                 // sqc2 vf2,384(sp); lw v1,392(sp)
    sub_lane(4U, 4U, 5U, kX, 0U);                                // vsubx.x vf4,vf4,vf5x
    qmtc2(2U, kV1);
    ee_cop2::vmul_broadcast(cpu, 2U, 3U, 2U, kX, 0U);            // vmulx.x vf2,vf3,vf2x
    r[kV1] = vf(2U);                                             // qmfc2 v1,vf2
    f1 = r[kV1].words[0];
    f0 = fpu::div(f1, denominator);                              // div.s f0,f1,f0
    set32(kV1, f0);
    qmtc2(2U, kV1);
    add_lane(2U, 4U, 2U, kX, 0U);                                // vaddx.x vf2,vf4,vf2x
    r[kV1] = vf(2U);
    f0 = r[kV1].words[0];
    f0 = ee_cop2::minimum_bits(kF3, f0);                         // min.s f0,f3,f0
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out));
    set32(kT1, f0);
    set32(kT0, f0);
    qmtc2(2U, kT1);
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, kXyz, 0U);          // vmulx.xyz vf2,vf1,vf2x
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, true);           // vsub.xyz vf3,vf3,vf2
    n3::store_quad_any(m, out, vf(3U));
    qmtc2(2U, kT0);
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out + 16U));
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, kXyz, 0U);
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, true);
    n3::store_quad_any(m, out + 16U, vf(3U));

    set32(4U, m.read_u32_le(record));                            // lw a0,0(a0)
    const GuestAddress clip_table2 = r[4U].words[0];
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, out + 32U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 48U));
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, clip_table2 + 192U));
    sub_lane(3U, 2U, 4U, kZ, 2U);
    denominator = vf(3U).words[2];
    sub_lane(3U, 2U, 4U, kX, 0U);
    sub_lane(2U, 5U, 4U, kZ, 2U);
    set32(kV1, vf(2U).words[2]);
    sub_lane(4U, 4U, 5U, kX, 0U);
    qmtc2(2U, kV1);
    ee_cop2::vmul_broadcast(cpu, 2U, 3U, 2U, kX, 0U);
    r[kV1] = vf(2U);
    f1 = r[kV1].words[0];
    f0 = fpu::div(f1, denominator);
    set32(kV1, f0);
    qmtc2(2U, kV1);
    add_lane(2U, 4U, 2U, kX, 0U);
    r[kV1] = vf(2U);
    f0 = r[kV1].words[0];
    f0 = ee_cop2::maximum_bits(kF3, f0);                         // max.s f0,f3,f0
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, out + 32U));
    set32(4U, f0);                                               // mfc1 a0,f0
    set32(kV1, f0);                                              // mfc1 v1,f0
    qmtc2(2U, 4U);
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 2U, kXyz, 0U);
    ee_cop2::vadd_vector(cpu, 3U, 3U, 2U, kXyz, true);
    qmtc2(2U, kV1);
    n3::store_quad_any(m, out + 32U, vf(3U));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, kXyz, 0U);          // vmulx.xyz vf1,vf1,vf2x
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, out + 48U));
    ee_cop2::vadd_vector(cpu, 2U, 2U, 1U, kXyz, true);
    n3::store_quad_any(m, out + 48U, vf(2U));

    // Copy the rows to a1+64..112, then set the copies' y lanes from the sums kept in t5, t4, t3 and a2.
    for (std::uint32_t row = 0U; row < 4U; ++row) {
        r[kV1] = n3::load_quad_any(m, out + 16U * row);
        n3::store_quad_any(m, out + 64U + 16U * row, r[kV1]);
    }
    const unsigned y_sources[4] = {kT5, kT4, kT3, kA2};
    for (std::uint32_t row = 0U; row < 4U; ++row) {
        const GuestAddress copy = out + 64U + 16U * row;
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, copy));
        qmtc2(2U, y_sources[row]);
        add_lane(1U, 0U, 2U, kY, 0U);                            // vaddx.y vf1,vf0,vf2x
        n3::store_quad_any(m, copy, vf(1U));
    }

    // Four passes: y(copy) = y(row) + MAX(y(copy) - y(row), C), C the float at $gp-31136.
    const std::uint32_t floor_value = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer) + kGpC);
    f1 = floor_value;                                            // lwc1 f1,-31136(gp)
    for (std::uint32_t row = 0U; row < 4U; ++row) {
        const GuestAddress base = out + 16U * row;
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, base + 64U));
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, base));
        sub_lane(1U, 1U, 3U, kY, 1U);                            // vsuby.y vf1,vf1,vf3y
        f0 = vf(1U).words[1];                                    // sqc2 vf1,448(sp); lwc1 f0,452(sp)
        f0 = ee_cop2::maximum_bits(f0, f1);
        set32(kA2, f0);
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, base + 64U));
        qmtc2(1U, kA2);
        add_lane(1U, 3U, 1U, kY, 0U);                            // vaddx.y vf1,vf3,vf1x
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kY, 1U, false);  // vaddy.y vf2,vf0,vf1y
        n3::store_quad_any(m, base + 64U, vf(2U));
    }

    cpu.write_fpr_bits(0U, f0);
    cpu.write_fpr_bits(1U, f1);
    cpu.write_fpr_bits(2U, f2);
    cpu.write_fpr_bits(3U, kF3);
    cpu.write_fpr_bits(4U, f4);
    cpu.write_fpr_bits(5U, f5);
    cpu.write_fpr_bits(6U, f6);
    cpu.write_fpr_bits(7U, f7);
    cpu.write_fpr_bits(8U, f8);
    cpu.write_fpr_bits(9U, f9);
    cpu.write_fpr_bits(10U, f10);

    return call.finish(kFixedCost);
}

} // namespace

extern constinit const EngineFunction n3_003d9740{0x003d9740U, 1284U, 0x1725fd8c12e0fcd7ULL, "BuildCornerBoundsBlock",
                                                  kFixedCost, n3::live<&build_corner_bounds_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003d9740, build_corner_bounds_block)
