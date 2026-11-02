// 0x002f0440 TestBoxPairFrames(a0, frame_a a1, a2, frame_f a3, frame_b t0, t1, point_out t2, normal_out t3):
// TestBoxPair (0x002f0640) with a composed frame. The product C = A * B of the 4x4 frames at a1 and t0 (rows are
// 16-byte quads, VU0 multiply-accumulate: C.xyz = B0*A.x + B1*A.y + B2*A.z, plus B3.xyz for the translation row,
// w taken from A's row for rows 0-2 and from B3 for row 3) is built in the frame's stack area and handed to
// TestBoxPair as its third argument; a0, a2 (as a1), a3 (as a1+...), t1 and the two output pointers pass through.
// After the call the position output (t2) is moved by frame F (a3: rotation rows 0-2 plus translation row 3) and
// the normal output (t3) by F's rotation only; either pointer may be 0. Returns the callee's low byte.
//
// The original copies the four product rows through three byte-pair loops and a quad loop (8 + 8 + 8 + 4 passes,
// fixed) into the contiguous matrix at sp-304+224: that matrix is the only part of the frame the callee reads, so
// only it is stored. The loops run a fixed number of times and are counted as straight code.
//
// Count: 46 (product) + 3 * (8 * 8 + 3) + 4 * 7 + 8 (call setup) + 2 + 6 (epilogue) = 291, + 14 when the position
// output is moved, + 11 for the normal output, + the callee's. A misaligned or unmapped quad access is declined so
// the original faults itself.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(002f0640)

namespace takedown::runtime {
namespace {

using guest::Gpr128;
using guest::R5900State;
using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint8_t kXyz = 14U;
constexpr std::uint32_t kFrameBytes = 304U;
constexpr GuestAddress kMatrixOffset = 224U;  // matrix handed to the callee: sp - 304 + 224
constexpr std::uint32_t kBaseCost = 291U;
constexpr std::uint32_t kPointCost = 14U;
constexpr std::uint32_t kNormalCost = 11U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Memory>
bool quad_ok(Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

// vmulax.xyz acc,fs,vf1x ; vmadday.xyz acc,vf3,vf1y (the listing's register choice for the matrix B rows).
inline void begin_row(R5900State& cpu, const unsigned first, const unsigned second) {
    ee_cop2::vmula_broadcast(cpu, first, 1U, kXyz, false);                      // vmulax.xyz acc,first,vf1x
    ee_cop2::vmadd_broadcast(cpu, 0U, second, 1U, kXyz, 1U, false, true, false);  // vmadday.xyz acc,second,vf1y
}

// One row of A*B: vf1 holds the A row (w lane kept), vf4/vf3/vf2 hold B rows 0..2.
inline Gpr128 product_row(R5900State& cpu, const Gpr128& a_row) {
    cpu.write_vu0_vector(1U, a_row);
    begin_row(cpu, 4U, 3U);
    ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, kXyz, 2U, false, false, false);   // vmaddz.xyz vf1,vf2,vf1z
    return cpu.read_vu0_vector(1U);
}

// Moves the vector in vf2 by F's rows (vf1 reused as the listing does); rows 0..2 rotate, row 3 translates.
template <class Memory>
Gpr128 move_by_frame(Memory& m, R5900State& cpu, const GuestAddress frame, const Gpr128& vector,
                     const bool translate) {
    cpu.write_vu0_vector(2U, vector);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame));                      // lqc2 vf1,0(F)
    ee_cop2::vmula_broadcast(cpu, 1U, 2U, kXyz, false);                         // vmulax.xyz acc,vf1,vf2x
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 16U));                // qmtc2 row 1
    ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 1U, false, true, false);    // vmadday.xyz acc,vf1,vf2y
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 32U));                // qmtc2 row 2
    if (translate) {
        ee_cop2::vmadd_broadcast(cpu, 0U, 1U, 2U, kXyz, 2U, false, true, false);  // vmaddaz.xyz acc,vf1,vf2z
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 48U));            // qmtc2 row 3
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyz, 3U, false, false, false);  // vmaddw.xyz vf1,vf1,vf0w
    } else {
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kXyz, 2U, false, false, false);  // vmaddz.xyz vf1,vf1,vf2z
    }
    return cpu.read_vu0_vector(1U);
}

template <class Context>
n3::Run test_box_pair_frames(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    R5900State& cpu = call.cpu();
    const GuestAddress frame_a = call.arg(1);
    const GuestAddress callee_a2 = call.arg(2);
    const GuestAddress frame_f = call.arg(3);
    const GuestAddress frame_b = call.arg(4);
    const GuestAddress callee_a3 = call.arg(5);
    const GuestAddress point_out = call.arg(6);
    const GuestAddress normal_out = call.arg(7);
    const GuestAddress matrix = call.stack_pointer() - kFrameBytes + kMatrixOffset;

    if (!quad_ok(m, frame_a) || !quad_ok(m, frame_a + 48U) || !quad_ok(m, frame_b) || !quad_ok(m, frame_b + 48U) ||
        !m.contains(matrix, 64U) || (point_out != 0U && !quad_ok(m, point_out)) ||
        (normal_out != 0U && !quad_ok(m, normal_out)) ||
        ((point_out != 0U || normal_out != 0U) && (!quad_ok(m, frame_f) || !quad_ok(m, frame_f + 48U)))) {
        return call.decline();
    }

    // C = A * B
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, frame_b));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame_b + 16U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame_b + 32U));
    const Gpr128 row0 = product_row(cpu, n3::load_quad_any(m, frame_a));
    const Gpr128 row1 = product_row(cpu, n3::load_quad_any(m, frame_a + 16U));
    const Gpr128 row2 = product_row(cpu, n3::load_quad_any(m, frame_a + 32U));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame_a + 48U));
    begin_row(cpu, 4U, 3U);
    ee_cop2::vmadd_broadcast(cpu, 0U, 2U, 1U, kXyz, 2U, false, true, false);    // vmaddaz.xyz acc,vf2,vf1z
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame_b + 48U));
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 0U, kXyz, 3U, false, false, false);   // vmaddw.xyz vf1,vf1,vf0w
    const Gpr128 row3 = cpu.read_vu0_vector(1U);
    n3::store_quad_any(m, matrix, row0);
    n3::store_quad_any(m, matrix + 16U, row1);
    n3::store_quad_any(m, matrix + 32U, row2);
    n3::store_quad_any(m, matrix + 48U, row3);

    call.set_arg(1, callee_a2);
    call.set_arg(2, matrix);
    call.set_arg(3, callee_a3);
    call.set_arg(4, point_out);
    call.set_arg(5, normal_out);
    const n3::Run callee = call.invoke([](auto& memory) { return n3::fn_002f0640(memory); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    call.return_value(call.result() & 0xffU);
    std::uint32_t retired = kBaseCost + callee.retired;

    if (point_out != 0U) {
        n3::store_quad_any(m, point_out, move_by_frame(m, cpu, frame_f, n3::load_quad_any(m, point_out), true));
        retired += kPointCost;
    }
    if (normal_out != 0U) {
        n3::store_quad_any(m, normal_out, move_by_frame(m, cpu, frame_f, n3::load_quad_any(m, normal_out), false));
        retired += kNormalCost;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002f0440, 508U, 0x7c85d7fc6a224a2fULL, "TestBoxPairFrames", kCeiling, test_box_pair_frames,
         EngineReturn::Integer)
