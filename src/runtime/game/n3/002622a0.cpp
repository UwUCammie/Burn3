// 0x002622a0 (a0 = object S, a1 = slot): SelectThreeBufferSlots.
//
// The record at 0x00666270 holds a block pointer at +104. The block's vectors at +16 (VF1) and +32 (VF6) are loaded,
// and VF1 is scaled by the float at gp-30620. From the scaled lanes x, y, z, w the VU0 forms the products x*x, x*z,
// y*y, y*z, w*x and w*y, the FPU forms B = x*z + w*y, A = y*z - w*x and C = 1 - x*x - y*y, and VF6 + (-2) * (B, A, C)
// is stored at S + 0x1f810 (the w lane is what the GPR moves carry through VF1). Then SelectBufferSlot (0x00307500)
// runs on the three objects S + 12 + 52*i with a1 = the slot, for i = 0, 1, 2.
//
// The frame lives in guest memory, not in host variables: the saves at entry and the restores of ra, s2, s1 and s0
// are real accesses in the listing's order. SelectBufferSlot writes into its object, and an object may lie in this
// frame, so the epilogue must read back what the original reads back.
//
// Retired instructions: 63 up to the loop, 13 per pass (the jal and its delay slot included), 6 in the epilogue, and
// 23 per SelectBufferSlot call: 63 + 3 * (13 + 23) + 6 = 177. The loop is straight code in the count: s0 starts at 0
// and only its increment changes it, so the pass count is fixed at three.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00307500)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop1 = guest::ee_cop1;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 784U;                                 // addiu $sp, $sp, -784
constexpr GuestAddress kRecordGlobal = 0x00666270U;                         // lui $v0, 0x0066; lw 25200($v0)
constexpr std::uint32_t kScaleOffset = static_cast<std::uint32_t>(-30620); // lw $t0, -30620($gp)
constexpr std::uint32_t kBlockOffset = 104U;                                // lw $v0, 104($v0)
constexpr std::uint32_t kPositionOffset = 16U;                              // lqc2 $vf1, 16($v0)
constexpr std::uint32_t kOffsetOffset = 32U;                                // lqc2 $vf6, 32($v0)
constexpr std::uint32_t kOutputAdjust = 2032U;                              // sqc2 $vf1, -2032($v0), v0 = S + 0x20000
constexpr std::uint32_t kObjectOffset = 12U;                                // addiu $a0, $v0, 12
constexpr std::size_t kObjectBytes = 52U;                                   // the extent SelectBufferSlot works in
constexpr std::uint32_t kReturnAddress = 0x002623c0U;                       // link of the jal at 0x002623b8
constexpr std::uint32_t kPrologueCost = 63U;                                // 0x002622a0..0x00262398
constexpr std::uint32_t kLoopCost = 13U;                                    // 0x0026239c..0x002623cc, one pass
constexpr std::uint32_t kEpilogueCost = 6U;                                 // 0x002623d0..0x002623e4
constexpr std::uint32_t kCeiling = 20000U;

// The original faults on a misaligned word or quadword access and on an address outside guest memory, so a call that
// would do either is left to the original, which faults the same way.
template <class Memory>
bool word_access(const Memory& memory, const GuestAddress address) {
    return (address & 3U) == 0U && memory.contains(address, 4U);
}

template <class Memory>
bool quad_access(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

template <class Context>
n3::Run select_three_buffer_slots(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();

    // addiu $sp, $sp, -784: the frame, used as memory from here on.
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    if ((frame & 15U) != 0U || !m.contains(frame, kFrameBytes)) {
        return call.decline();
    }

    // lui $v0, 0x0066; sd $ra, 48($sp); sq $s2, 32($sp); sq $s1, 16($sp); daddu $s2, $a0, $zero; sq $s0, 0($sp);
    // daddu $s1, $a1, $zero
    cpu.write_gpr_sign_extended_32(2U, 0x00660000U);
    m.write_u64_le(frame + 48U, cpu.read_gpr_u64(31U));
    n3::store_quad_any(m, frame + 32U, cpu.read_gpr(18U));
    n3::store_quad_any(m, frame + 16U, cpu.read_gpr(17U));
    cpu.write_gpr_u64(18U, cpu.read_gpr_u64(4U));
    n3::store_quad_any(m, frame, cpu.read_gpr(16U));
    cpu.write_gpr_u64(17U, cpu.read_gpr_u64(5U));

    // lw $v0, 25200($v0): the record pointer. lw $t0, -30620($gp): the scale. lw $v0, 104($v0): the block pointer.
    if (!word_access(m, kRecordGlobal)) {
        return call.decline();
    }
    cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(kRecordGlobal));
    const GuestAddress scale_address = cpu.read_gpr_u32(n3::kGlobalPointer) + kScaleOffset;
    if (!word_access(m, scale_address)) {
        return call.decline();
    }
    cpu.write_gpr_sign_extended_32(8U, m.read_u32_le(scale_address));
    const GuestAddress record = cpu.read_gpr_u32(2U);
    if (!word_access(m, record + kBlockOffset)) {
        return call.decline();
    }
    cpu.write_gpr_sign_extended_32(2U, m.read_u32_le(record + kBlockOffset));

    // lqc2 $vf1, 16($v0); lqc2 $vf6, 32($v0)
    const GuestAddress block = cpu.read_gpr_u32(2U);
    if (!quad_access(m, block + kPositionOffset) || !quad_access(m, block + kOffsetOffset)) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, block + kPositionOffset));
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, block + kOffsetOffset));

    // lui $v1, 0xc000; addiu $a3, $sp, 256; addiu $a2, $sp, 208; addiu $a1, $sp, 160; addiu $a0, $sp, 112;
    // daddu $s0, $zero, $zero; qmtc2 $t0, $vf2
    cpu.write_gpr_sign_extended_32(3U, 0xC0000000U);
    cpu.write_gpr_sign_extended_32(7U, frame + 256U);
    cpu.write_gpr_sign_extended_32(6U, frame + 208U);
    cpu.write_gpr_sign_extended_32(5U, frame + 160U);
    cpu.write_gpr_sign_extended_32(4U, frame + 112U);
    cpu.write_gpr_u64(16U, 0U);
    cpu.write_vu0_vector(2U, cpu.read_gpr(8U));

    // lui $v0, 0x3f80; mtc1 $v0, $f0; vmulx.xyzw $vf1, $vf1, $vf2x
    cpu.write_gpr_sign_extended_32(2U, 0x3F800000U);
    cpu.write_fpr_bits(0U, cpu.read_gpr_u32(2U));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 2U, 15U, 0U);

    // lui $v0, 0x0002; addu $v0, $s2, $v0; qmtc2 $v1, $vf5; vmulx.x $vf2, $vf1, $vf1x; qmfc2 $v1, $vf2; mtc1 $v1, $f1
    cpu.write_gpr_sign_extended_32(2U, 0x00020000U);
    cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(18U) + cpu.read_gpr_u32(2U));
    cpu.write_vu0_vector(5U, cpu.read_gpr(3U));
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 1U, 8U, 0U);
    cpu.write_gpr(3U, cpu.read_vu0_vector(2U));
    cpu.write_fpr_bits(1U, cpu.read_gpr_u32(3U));

    // lqc2 $vf3, 672($sp)
    if (!quad_access(m, frame + 672U)) {
        return call.decline();
    }
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 672U));

    // sub.s $f0, $f0, $f1; vmulz.x $vf2, $vf1, $vf1z; qmfc2 $v1, $vf2; vmuly.y $vf2, $vf1, $vf1y; mtc1 $v1, $f2
    ee_cop1::sub_s(cpu, 0U, 0U, 1U);
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 1U, 8U, 2U);
    cpu.write_gpr(3U, cpu.read_vu0_vector(2U));
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 1U, 4U, 1U);
    cpu.write_fpr_bits(2U, cpu.read_gpr_u32(3U));

    // sqc2 $vf2, 0($a3); vmulz.y $vf2, $vf1, $vf1z; lwc1 $f1, 260($sp); sqc2 $vf2, 0($a2); sub.s $f0, $f0, $f1
    n3::store_quad_any(m, frame + 256U, cpu.read_vu0_vector(2U));
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 1U, 4U, 2U);
    cpu.write_fpr_bits(1U, m.read_u32_le(frame + 260U));
    n3::store_quad_any(m, frame + 208U, cpu.read_vu0_vector(2U));
    ee_cop1::sub_s(cpu, 0U, 0U, 1U);

    // vmulx.w $vf2, $vf1, $vf1x; mfc1 $v1, $f0; vmuly.w $vf1, $vf1, $vf1y; lwc1 $f1, 212($sp); sqc2 $vf2, 0($a1);
    // lwc1 $f0, 172($sp); sqc2 $vf1, 0($a0)
    ee_cop2::vmul_broadcast(cpu, 2U, 1U, 1U, 1U, 0U);
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(0U));
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 1U, 1U, 1U);
    cpu.write_fpr_bits(1U, m.read_u32_le(frame + 212U));
    n3::store_quad_any(m, frame + 160U, cpu.read_vu0_vector(2U));
    cpu.write_fpr_bits(0U, m.read_u32_le(frame + 172U));
    n3::store_quad_any(m, frame + 112U, cpu.read_vu0_vector(1U));

    // qmtc2 $v1, $vf1; sub.s $f0, $f1, $f0; mfc1 $v1, $f0; lwc1 $f0, 124($sp); qmtc2 $v1, $vf2; add.s $f0, $f2, $f0
    cpu.write_vu0_vector(1U, cpu.read_gpr(3U));
    ee_cop1::sub_s(cpu, 0U, 1U, 0U);
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(0U));
    cpu.write_fpr_bits(0U, m.read_u32_le(frame + 124U));
    cpu.write_vu0_vector(2U, cpu.read_gpr(3U));
    ee_cop1::add_s(cpu, 0U, 2U, 0U);

    // mfc1 $v1, $f0; qmtc2 $v1, $vf4; vaddx.x $vf3, $vf0, $vf4x; vaddx.y $vf3, $vf0, $vf2x;
    // vaddx.z $vf3, $vf0, $vf1x; vmulx.xyz $vf1, $vf3, $vf5x; vadd.xyz $vf1, $vf6, $vf1
    cpu.write_gpr_sign_extended_32(3U, cpu.read_fpr_bits(0U));
    cpu.write_vu0_vector(4U, cpu.read_gpr(3U));
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 4U, 8U, 0U, false);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 2U, 4U, 0U, false);
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 1U, 2U, 0U, false);
    ee_cop2::vmul_broadcast(cpu, 1U, 3U, 5U, 14U, 0U);
    ee_cop2::vadd_vector(cpu, 1U, 6U, 1U, 14U, false);

    // sqc2 $vf1, -2032($v0): the output at S + 0x1f810.
    const GuestAddress output = cpu.read_gpr_u32(2U) - kOutputAdjust;
    if (!quad_access(m, output)) {
        return call.decline();
    }
    n3::store_quad_any(m, output, cpu.read_vu0_vector(1U));

    // The loop, 0x0026239c..0x002623cc, three passes (s0 = 0, 1, 2).
    std::uint32_t retired = kPrologueCost;
    do {
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(16U) << 1U);                     // sll $v0, $s0, 1
        cpu.write_gpr_u64(5U, cpu.read_gpr_u64(17U));                                        // daddu $a1, $s1, $zero
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(2U) + cpu.read_gpr_u32(16U));    // addu $v0, $v0, $s0
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(2U) << 2U);                      // sll $v0, $v0, 2
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(2U) + cpu.read_gpr_u32(16U));    // addu $v0, $v0, $s0
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(2U) << 2U);                      // sll $v0, $v0, 2
        cpu.write_gpr_sign_extended_32(2U, cpu.read_gpr_u32(18U) + cpu.read_gpr_u32(2U));    // addu $v0, $s2, $v0
        const GuestAddress object = cpu.read_gpr_u32(2U) + kObjectOffset;
        if (!m.contains(object, kObjectBytes)) {
            return call.decline();
        }
        cpu.write_gpr_sign_extended_32(31U, kReturnAddress);                                 // jal: ra = link
        cpu.write_gpr_sign_extended_32(4U, object);                                          // delay slot: addiu $a0, $v0, 12
        const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_00307500(machine); }, kFrameBytes);
        if (callee.declined) {
            return call.decline();
        }
        cpu.write_gpr_sign_extended_32(16U, cpu.read_gpr_u32(16U) + 1U);                    // addiu $s0, $s0, 1
        cpu.write_gpr_u64(3U, static_cast<std::int32_t>(cpu.read_gpr_u32(16U)) < 3 ? 1U : 0U); // slti $v1, $s0, 3
        retired += kLoopCost + callee.retired;
    } while (cpu.read_gpr_u64(3U) != 0U);                                                    // bne $v1, $zero

    // ld $ra, 48($sp); lq $s2, 32($sp); lq $s1, 16($sp); lq $s0, 0($sp); jr $ra; addiu $sp, $sp, 784
    cpu.write_gpr_u64(31U, m.read_u64_le(frame + 48U));
    cpu.write_gpr(18U, n3::load_quad_any(m, frame + 32U));
    cpu.write_gpr(17U, n3::load_quad_any(m, frame + 16U));
    cpu.write_gpr(16U, n3::load_quad_any(m, frame));
    return call.finish(retired + kEpilogueCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002622a0, 328U, 0x548d173565b08fffULL, "SelectThreeBufferSlots", kCeiling, select_three_buffer_slots,
         EngineReturn::Nothing)
