// 0x001ab610 (a0 = car, a1 = the four rows of a transform, 16 bytes each): SetCarTransform. Each row is stored
// unchanged at the block whose address is the u32 at car+516, and at car+112, +128, +144 and +160. VU0 then rewrites
// car+112..175 as the inverse: the 3x3 part of rows 0..2 is transposed (each row keeps its own w lane), and the
// position row becomes -(row i . row 3) for i = 0..2 over xyz, with row 1's w lane. The VU0 sequence is the listing's,
// run with the helpers the interpreter uses, so VF1..VF4, the accumulator and the MAC flags end as the original leaves
// them. The temporaries end holding what the listing leaves in them. The original writes nothing else.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kBlockPointer = 516U;  // lw t1,516(a0): the address of the block the rows are stored at
constexpr std::uint32_t kRetired = 39U;       // 0x001ab610..0x001ab6a8: one path, no loops, delay slot included

template <class Context>
n3::Run place_car_transform(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress car = call.arg(0);
    const GuestAddress source = call.arg(1);

    // lw t1,516(a0), then lq t0,0(a1), a3,16(a1), a2,32(a1), v1,48(a1). Every read comes before the first store.
    const GuestAddress block = m.read_u32_le(car + kBlockPointer);
    const guest::Gpr128 row0 = n3::load_quad_any(m, source + 0U);
    const guest::Gpr128 row1 = n3::load_quad_any(m, source + 16U);
    const guest::Gpr128 row2 = n3::load_quad_any(m, source + 32U);
    const guest::Gpr128 row3 = n3::load_quad_any(m, source + 48U);

    // sq t0,0(t1), a3,16(t1), a2,32(t1), v1,48(t1): the rows as they are, at the block.
    n3::store_quad_any(m, block + 0U, row0);
    n3::store_quad_any(m, block + 16U, row1);
    n3::store_quad_any(m, block + 32U, row2);
    n3::store_quad_any(m, block + 48U, row3);
    // sq t0,112(a0), a3,128(a0), a2,144(a0), v1,160(a0): the same rows at the car, for the VU0 sequence to read.
    n3::store_quad_any(m, car + 112U, row0);
    n3::store_quad_any(m, car + 128U, row1);
    n3::store_quad_any(m, car + 144U, row2);
    n3::store_quad_any(m, car + 160U, row3);

    // The VU0 sequence of 0x001ab644..0x001ab6a8. Each lqc2/sqc2 reads or writes the car at the listing's offset,
    // after the stores above and the sqc2s before it, as the original does.
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, car + 128U));  // lqc2 vf4,128(a0)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, car + 112U));  // lqc2 vf2,112(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, car + 144U));  // lqc2 vf3,144(a0)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 4U, 4U, 0U, false);     // vaddx.y vf2,vf0,vf4x
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, car + 112U));  // lqc2 vf1,112(a0)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 3U, 2U, 0U, false);     // vaddx.z vf2,vf0,vf3x
    n3::store_quad_any(m, car + 112U, cpu.read_vu0_vector(2U));  // sqc2 vf2,112(a0)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, car + 128U));  // lqc2 vf2,128(a0)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 8U, 1U, false);     // vaddy.x vf2,vf0,vf1y
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 3U, 2U, 1U, false);     // vaddy.z vf2,vf0,vf3y
    n3::store_quad_any(m, car + 128U, cpu.read_vu0_vector(2U));  // sqc2 vf2,128(a0)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, car + 144U));  // lqc2 vf2,144(a0)
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, 8U, 2U, false);     // vaddz.x vf2,vf0,vf1z
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 4U, 4U, 2U, false);     // vaddz.y vf2,vf0,vf4z
    n3::store_quad_any(m, car + 144U, cpu.read_vu0_vector(2U));  // sqc2 vf2,144(a0)
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, car + 160U));  // lqc2 vf3,160(a0)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, car + 112U));  // lqc2 vf1,112(a0)
    ee_cop2::vmul_broadcast(cpu, 4U, 2U, 3U, 14U, 2U);           // vmulz.xyz vf4,vf2,vf3z
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 3U, 14U, 0U);           // vmulx.xyz vf2,vf1,vf3x
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, car + 128U));  // lqc2 vf1,128(a0)
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 3U, 14U, 1U);           // vmuly.xyz vf1,vf1,vf3y
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, 14U, false);           // vadd.xyz vf1,vf2,vf1
    ee_cop2::vadd_vector(cpu, 1U, 1U, 4U, 14U, false);           // vadd.xyz vf1,vf1,vf4
    ee_cop2::vadd_vector(cpu, 1U, 0U, 1U, 14U, true);            // vsub.xyz vf1,vf0,vf1
    n3::store_quad_any(m, car + 160U, cpu.read_vu0_vector(1U));  // sqc2 vf1,160(a0)

    // The registers the listing leaves behind: t0..v1 hold the rows as loaded, t1 the block's address.
    cpu.write_gpr(8U, row0);
    cpu.write_gpr(7U, row1);
    cpu.write_gpr(6U, row2);
    cpu.write_gpr(3U, row3);
    cpu.write_gpr_sign_extended_32(9U, block);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001ab610, 156U, 0xb696b3d1223b6512ULL, "SetCarTransform", 39U, place_car_transform, EngineReturn::Nothing)
