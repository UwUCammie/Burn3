// 0x0015fa60 (a0 = block B, 48 bytes; the callers ignore the results): InitUpVectorBlock. A leaf with no branches
// and 24 retired instructions (the last one is the delay slot of jr ra). It stores 0xff at B+46 and B+47, 0xffff at
// B+44 and at B+32, B+34, B+36 and B+38, and a zero word at B+40. It clears the quadword at B+0 (VU0 vf2 is +0.0 in
// all lanes after vsub.xyzw vf2,vf0,vf0) and rebuilds the quadword at B+16 from its old contents: x and z become +0.0,
// y becomes 1.0 (0x3f800000) and w keeps its old bits. The VU0 steps are the interpreter's own primitives, in the
// original order, so MAC and STATUS end as the original leaves them. The intermediate v0 and v1 values are only read
// by the stores above, so only the values the registers end with are written.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// R5900 general registers (the names in the listing).
constexpr unsigned kV0 = 2U;
constexpr unsigned kV1 = 3U;
constexpr unsigned kA1 = 5U;
constexpr unsigned kA2 = 6U;

// VU0 destination fields for a single lane (x is the high bit, as the instructions encode them).
constexpr std::uint8_t kMaskX = 8U;
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

constexpr std::uint8_t kByteFill = 0xffU;
constexpr std::uint16_t kHalfFill = 0xffffU;
constexpr std::uint32_t kOneBits = 0x3f800000U;  // lui v1,0x3f80

template <class Context>
n3::Run init_up_vector_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress block = call.arg(0);

    // daddu a2,zero,zero: the low doubleword only; the high doubleword keeps its entry value.
    cpu.write_gpr_u64(kA2, 0U);
    m.write_u8(block + 46U, kByteFill);                                // sb v1,46(a0) with v1 = 255
    m.write_u16_le(block + 44U, kHalfFill);                            // sh v0,44(a0) with v0 = -1
    cpu.write_gpr_u64(kA1, kHalfFill);                                 // ori a1,zero,0xffff
    m.write_u8(block + 47U, kByteFill);                                // sb v1,47(a0) with v1 = 255
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyzw, true);        // vsub.xyzw vf2,vf0,vf0
    m.write_u16_le(block + 32U, kHalfFill);                            // sh a1,32(a0)
    m.write_u16_le(block + 34U, kHalfFill);                            // sh a1,34(a0)
    m.write_u16_le(block + 36U, kHalfFill);                            // sh a1,36(a0)
    m.write_u16_le(block + 38U, kHalfFill);                            // sh a1,38(a0)
    cpu.write_gpr_sign_extended_32(kV1, kOneBits);                     // lui v1,0x3f80
    m.write_u32_le(block + 40U, 0U);                                   // sw zero,40(a0)
    n3::store_quad_any(m, block, cpu.read_vu0_vector(2U));                 // sqc2 vf2,0(a0)
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, block + 16U));           // lqc2 vf2,16(a0): the old contents
    // qmtc2 copies the whole 128-bit register, so the high doubleword of a2 and v1 comes along.
    cpu.write_vu0_vector(3U, cpu.read_gpr(kA2));                       // qmtc2 a2,vf3
    cpu.write_vu0_vector(1U, cpu.read_gpr(kV1));                       // qmtc2 v1,vf1
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 3U, kMaskX, 0U, false);       // vaddx.x vf2,vf0,vf3x
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kMaskY, 0U, false);       // vaddx.y vf2,vf0,vf1x
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 3U, kMaskZ, 0U, false);       // vaddx.z vf2,vf0,vf3x
    n3::store_quad_any(m, block + 16U, cpu.read_vu0_vector(2U));           // sqc2 vf2,16(a0) (jr delay slot)

    // addiu v0,zero,1 is the last write to v0: sign-extended, the high doubleword keeps its entry value.
    call.return_value(1U);
    return call.finish(24U);
}

} // namespace

extern constinit const EngineFunction n3_0015fa60{0x0015fa60U, 96U, 0x095213ea9310f22aULL, "InitUpVectorBlock", 24U,
                                                  n3::live<&init_up_vector_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015fa60, init_up_vector_block)
