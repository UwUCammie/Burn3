// 0x0021e490 (604 bytes, leaf): BuildSpriteDrawDescriptor. Fills the 144-byte descriptor at a0 from the 32-byte
// record at 0x004b9ca0 + (a1 << 5): reciprocal and slope values from the record's floats, raw copies of some of its
// words, the word at +32 (always zero) and the coefficients at +36..+44 (zero when the byte at +30 is 1), and one of
// two GS packet templates chosen by the byte at +31. Returns nothing: v1 and f0 are scratch, as in the original.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::Gpr128;
using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRecordTable = 0x004b9ca0U;  // lui 0x4c, addiu -25440; records are 32 bytes apart
constexpr std::uint32_t kOneBits = 0x3f800000U;     // 1.0f

template <class Context>
n3::Run build_sprite_draw_descriptor(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress out = call.arg(0);
    const GuestAddress rec = kRecordTable + (call.arg(1) << 5U);

    // Head. Bytes are read with lbu (0..255), so the original's bltz fallbacks never branch and each byte converts
    // with a plain integer-to-float.
    const std::uint32_t b28 = m.read_u8(rec + 28U);
    const std::uint32_t b28_float = fpu::from_int(static_cast<std::int32_t>(b28));
    const std::uint32_t e8 = m.read_u32_le(rec + 8U);
    const std::uint32_t e12 = m.read_u32_le(rec + 12U);
    m.write_u32_le(out + 0U, fpu::div(kOneBits, b28_float));
    m.write_u32_le(out + 4U, e12);
    const std::uint32_t e4 = m.read_u32_le(rec + 4U);
    m.write_u32_le(out + 8U, b28);
    const std::uint32_t e0 = m.read_u32_le(rec + 0U);
    m.write_u32_le(out + 12U, e8);

    // Slope (e4 - e0) / float(b29). The sub is the delay slot of the bltz, so it always runs.
    const std::uint32_t b29 = m.read_u8(rec + 29U);
    const std::uint32_t slope = fpu::sub(e4, e0);
    const std::uint32_t b29_float = fpu::from_int(static_cast<std::int32_t>(b29));
    const std::uint32_t slope_per_b29 = fpu::div(slope, b29_float);
    const std::uint32_t b30 = m.read_u8(rec + 30U);
    m.write_u32_le(out + 16U, slope_per_b29);
    m.write_u32_le(out + 20U, e0);
    m.write_u32_le(out + 24U, b29);
    m.write_u32_le(out + 28U, b30);
    // The original tests the copy it has just stored, so this reads the descriptor back.
    const std::uint32_t mode = m.read_u32_le(out + 28U);
    // Retired so far: head (10), b29 block with its convert (15 + 3), b30 reads and test (14).
    std::uint32_t retired = 42U;
    if (mode == 1U) {
        m.write_u32_le(out + 32U, 0U);
        m.write_u32_le(out + 36U, 0U);
        m.write_u32_le(out + 40U, 0U);
        m.write_u32_le(out + 44U, 0U);
        retired += 5U;
    } else {
        m.write_u32_le(out + 32U, 0U);
        // ADDA.S: ACC = 0 + (e0 + e0), where the 0 is f3 (+0, so a -0 sum becomes +0). MSUB.S: f6 = ACC - e0 * e0.
        // The same pair for e4 gives f5. det = f5 - f6 and the reciprocal is 1.0 / det.
        const std::uint32_t acc_e0 = fpu::add(0U, fpu::add(e0, e0));
        const std::uint32_t f6 = fpu::msub(acc_e0, e0, e0);
        const std::uint32_t acc_e4 = fpu::add(0U, fpu::add(e4, e4));
        const std::uint32_t f5 = fpu::msub(acc_e4, e4, e4);
        const std::uint32_t recip_det = fpu::div(kOneBits, fpu::sub(f5, f6));
        const std::uint32_t e16 = m.read_u32_le(rec + 16U);
        const std::uint32_t e20 = m.read_u32_le(rec + 20U);
        const std::uint32_t e24 = m.read_u32_le(rec + 24U);
        m.write_u32_le(out + 36U, fpu::div(e16, b28_float));
        // MULA.S: ACC = e20 * f5. MSUB.S: msub_term = ACC - e24 * f6, so +40 = recip_det * msub_term and
        // +44 = recip_det * (e24 - e20).
        const std::uint32_t acc_mula = fpu::mul(e20, f5);
        const std::uint32_t msub_term = fpu::msub(acc_mula, e24, f6);
        m.write_u32_le(out + 40U, fpu::mul(recip_det, msub_term));
        m.write_u32_le(out + 44U, fpu::mul(recip_det, fpu::sub(e24, e20)));
        retired += 19U + 3U + 12U;
    }
    // Common tail: the quadword {4, 8} at +96 and a zero dword at +136 are written on both paths.
    const std::uint32_t b31 = m.read_u8(rec + 31U);
    n3::store_quad_any(m, out + 96U, Gpr128{{4U, 0U, 8U, 0U}});
    m.write_u64_le(out + 136U, 0U);
    retired += 7U;
    if (b31 == 0U) {
        n3::store_quad_any(m, out + 64U, Gpr128{{0x00030000U, 0U, 71U, 0U}});
        n3::store_quad_any(m, out + 80U, Gpr128{{42U, 0U, 66U, 0U}});
        m.write_u32_le(out + 48U, 128U);
        m.write_u32_le(out + 52U, 128U);
        m.write_u32_le(out + 56U, 128U);
        // MOVN: v1 stays 32 when a1 is zero and becomes 0 when it is not.
        m.write_u32_le(out + 60U, call.arg64(1U) != 0U ? 0U : 32U);
        m.write_u64_le(out + 128U, 0U);
        retired += 16U + 2U + 2U;
    } else {
        n3::store_quad_any(m, out + 64U, Gpr128{{0x0003140bU, 0U, 71U, 0U}});
        n3::store_quad_any(m, out + 80U, Gpr128{{68U, 0U, 66U, 0U}});
        m.write_u32_le(out + 48U, 64U);
        // The original reloads the word it has just stored for the next two fields.
        m.write_u32_le(out + 52U, m.read_u32_le(out + 48U));
        m.write_u32_le(out + 56U, m.read_u32_le(out + 48U));
        m.write_u32_le(out + 60U, 128U);
        // LUI 0xff00 sign-extends to 0xffffffffff000000; DSLL32 by 0 moves it to the high word.
        m.write_u64_le(out + 128U, 0xFF00000000000000ULL);
        retired += 22U + 2U + 2U;
    }
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0021e490{0x0021e490U, 604U, 0x6f9575f4ab2868edULL, "BuildSpriteDrawDescriptor",
                                                  121U, n3::live<&build_sprite_draw_descriptor<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021e490, build_sprite_draw_descriptor)
