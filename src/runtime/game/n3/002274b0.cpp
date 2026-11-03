// 0x002274b0 (a1 = colour quad, f12 = intensity): FillScreenTint. Draws a full-screen tint as 20 vertical sprites, each
// 32 pixels wide and 512 pixels tall (12.4 fixed point: 512 and 8192), all one colour. The colour is the quad at a1
// scaled by 127.5 * f12 on VU0, clamped to 255, rounded with +0.5 and truncated to integers; the packed word is
// R | G << 8 | B << 16, each part sign-extended to 64 bits. The packet is 28 quadwords at the current write position
// (CUR, gp-24240), which the reserve call (0x001f3080) makes room for first. CUR then moves past the packet, and the
// XYOFFSET_1 doubleword saved at gp-28200 is written back as the packet's last A+D pair. Returns nothing.
//
// The 384-byte frame is not written. The original's frame traffic is its own spills, reloads of lanes it has just
// stored (taken here from the VU0 registers), and the stale w lane of VF1: the one lane its 255.0 stores did not reach,
// which is read from memory. VF1..VF3 finish as the original leaves them, and the MAC/STATUS flags come from the same
// macro helpers in the same order around the reserve call.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 384U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired original instructions on every path, not counting the reserve call: the entry block up to the loop (100,
// the reserve jal and its delay slot included), 20 strip passes of 7 instructions, 21 loop tests of 3 (the last one
// exits) and the exit (21). The loop runs a constant 20 times (s0 = 20), so only the reserve call's count varies.
constexpr std::uint32_t kOwnRetired = 100U + 20U * 7U + 21U * 3U + 21U;

constexpr std::uint32_t kByteScale = 0x42ff0000U;        // 127.5f
constexpr std::uint32_t kByteMax = 0x437f0000U;          // 255.0f
constexpr std::uint32_t kRoundHalf = 0x3f000000U;        // 0.5f
constexpr std::uint8_t kMaskX = 8U;                      // VU0 destination masks, x being the high bit
constexpr std::uint8_t kMaskY = 4U;
constexpr std::uint8_t kMaskZ = 2U;

constexpr std::uint32_t kReserveFlags = 0x80000000U;     // lui $a0, 0x8000
constexpr std::uint32_t kReserveQwords = 28U;            // addiu $a1, $zero, 28
constexpr GuestAddress kCurOffset = static_cast<GuestAddress>(-24240);        // gp-relative packet write position
constexpr GuestAddress kXyOffsetShadow = static_cast<GuestAddress>(-28200);   // gp-relative XYOFFSET_1 doubleword

// GIF tags and A+D addresses. The high doubleword of a tag is its register descriptor: 14 = A+D pairs, 0x55 = two
// XYZ2 registers. The low doubleword holds NLOOP, PRE, PRIM and NREG: 0x10a34000 in its upper word is PRE, PRIM =
// SPRITE|ABE|FST and NREG 1; 0x24000000 is FLG = REGLIST and NREG 2.
constexpr std::uint64_t kSpriteTag = 0x10a34000ULL << 32U;
constexpr std::uint64_t kRegListTag = 0x24000000ULL << 32U;
constexpr std::uint64_t kAdDescriptor = 14U;
constexpr std::uint64_t kXyz2Pair = 0x55U;
constexpr std::uint32_t kXyOffsetAddress = 24U;          // XYOFFSET_1
constexpr std::uint32_t kTest1 = 71U;                    // TEST_1
constexpr std::uint32_t kAlpha1 = 66U;                   // ALPHA_1
constexpr std::uint32_t kRgbaq = 1U;

constexpr std::uint32_t kStrips = 20U;
constexpr std::uint64_t kStripWidth = 512U;              // 32 pixels in 12.4 fixed point
constexpr std::uint64_t kStripBottom = 0x20000000U;      // Y = 8192 (512 pixels) in the Y field of the second vertex

// A quadword holding two doublewords (low at the lower address), as sq stores them.
Gpr128 quadword(const std::uint64_t low, const std::uint64_t high) noexcept {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Memory>
void store_pair(Memory& memory, const GuestAddress address, const std::uint64_t low, const std::uint64_t high) {
    n3::store_quad_any(memory, address, quadword(low, high));
}

// mfc1 and the 32-bit ops write the low word of a GPR and sign-extend it into the low doubleword; the upper
// doubleword is kept.
Gpr128 with_sign_extended_low(Gpr128 value, const std::uint32_t bits) {
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

std::uint64_t sign_extended(const std::int32_t value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

template <class Context>
n3::Run fill_screen_tint(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    // lqc2 vf2,0(a1); mul.s f0 = 127.5 * f12; qmtc2 v0,vf3 (vf3.x is the product, the rest of v0 is kept);
    // vmulx.xyz vf2 = vf2 * vf3.x.
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, call.arg(1)));
    const std::uint32_t scale = fpu::mul(kByteScale, cpu.read_fpr_bits(12U));
    cpu.write_vu0_vector(3U, with_sign_extended_low(cpu.read_gpr(2U), scale));
    ee_cop2::vmul_broadcast(cpu, 2U, 2U, 3U, n3::kMaskXyz, 0U);

    // lqc2 vf1,192(sp): x, y and z are the 255.0 words the frame held; w was never stored, so it is the stale stack
    // word. vmini.xyz vf1 = min(vf2, vf1).
    const std::uint32_t stale = n3::load_quad_any(m, frame + 192U).words[3];
    cpu.write_vu0_vector(1U, Gpr128{{kByteMax, kByteMax, kByteMax, stale}});
    ee_cop2::vmini(cpu, 1U, 2U, 1U, n3::kMaskXyz);

    // jal reserve(0x80000000, 28). Its delay slot saves vf1 in s3 (kept here as saved_vf1) and vf1 is restored from it
    // after the call. A decline by the callee leaves the whole call to the original.
    const Gpr128 saved_vf1 = cpu.read_vu0_vector(1U);
    call.set_arg(0, kReserveFlags);
    call.set_arg(1, kReserveQwords);
    const n3::Run callee = call.invoke([](auto& callee_memory) { return n3::fn_001f3080(callee_memory); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    cpu.write_vu0_vector(1U, saved_vf1);

    // qmtc2 a0,vf3: a0 holds 0.5f in its low word; its upper doubleword is the TEST_1 address left by the pcpyld that
    // built the A+D pair at TEST_1.
    cpu.write_vu0_vector(3U, Gpr128{{kRoundHalf, 0U, kTest1, 0U}});
    ee_cop2::vadd_broadcast(cpu, 2U, 1U, 3U, kMaskZ, 0U, false);   // vaddx.z vf2,vf1,vf3x
    ee_cop2::vadd_broadcast(cpu, 2U, 1U, 3U, kMaskX, 0U, false);   // vaddx.x vf2,vf1,vf3x
    ee_cop2::vadd_broadcast(cpu, 1U, 1U, 3U, kMaskY, 0U, false);   // vaddx.y vf1,vf1,vf3x

    // cvt.w.s of the three lanes: R = vf2.x, G = vf1.y, B = vf2.z. The original reads G and B back from the words it
    // stored them to, which are these registers.
    const std::int32_t red = fpu::to_int(cpu.read_vu0_vector(2U).words[0]);
    const std::int32_t green = fpu::to_int(cpu.read_vu0_vector(1U).words[1]);
    const std::int32_t blue = fpu::to_int(cpu.read_vu0_vector(2U).words[2]);
    const std::uint64_t colour = sign_extended(red) | (sign_extended(green) << 8U) | (sign_extended(blue) << 16U);

    // The packet at CUR (read after the reserve call). Stores go in the original's order.
    const GuestAddress packet = m.read_u32_le(gp + kCurOffset);
    store_pair(m, packet + 0U, kSpriteTag | 4U, kAdDescriptor);             // GIF tag: 4 A+D pairs
    store_pair(m, packet + 16U, 0U, kXyOffsetAddress);                      // XYOFFSET_1 = 0
    store_pair(m, packet + 32U, 0x00031001U, kTest1);                       // TEST_1
    store_pair(m, packet + 48U, 88U, kAlpha1);                              // ALPHA_1
    store_pair(m, packet + 64U, colour, kRgbaq);                            // RGBAQ
    store_pair(m, packet + 80U, kRegListTag | 20U, kXyz2Pair);              // GIF tag: 20 loops of XYZ2 pairs
    // Twenty strips, one quadword each: the pair (X, 0) and (X + 512, 512 px), with X = 512 * strip. The loop count is
    // a constant, so it is written as a plain loop.
    for (std::uint32_t strip = 0U; strip < kStrips; ++strip) {
        const std::uint64_t left = kStripWidth * strip;
        store_pair(m, packet + 96U + 16U * strip, left, (left + kStripWidth) | kStripBottom);
    }
    store_pair(m, packet + 416U, kSpriteTag | 0x8001U, kAdDescriptor);      // GIF tag: 1 loop, EOP
    const std::uint64_t xy_offset = m.read_u64_le(gp + kXyOffsetShadow);    // ld: the saved XYOFFSET_1
    store_pair(m, packet + 432U, xy_offset, kXyOffsetAddress);
    m.write_u32_le(gp + kCurOffset, packet + 448U);                         // CUR past the 28 quadwords

    return call.finish(kOwnRetired + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(002274b0, 524U, 0x98952ab9dee03d3cULL, "FillScreenTint", kCeiling, fill_screen_tint, EngineReturn::Nothing)
