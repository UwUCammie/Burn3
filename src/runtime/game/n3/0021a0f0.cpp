// 0x0021a0f0 (no arguments, no result): InitViewportGsPackets. A leaf with no branches or calls: 102 instructions on
// every path, and every caller ignores the result. The viewport record r = [[0x00670bd0] + 96] gives x (halfword +28),
// y (halfword +30), w (word +12) and h (word +16). Written for the next 2D pass: the GS A+D quadwords for CLAMP_1,
// TEX1_1, ALPHA_1 and FRAME_1 (low doublewords from gp-28184, -28176, -28168, -28216), SCISSOR_1 from x, y, x + w - 1 and
// y + h - 1 (the original's sign-extending shifts and ORs, kept as they are), and the floats (fx, fy, fx, fy) at
// 0x0067cd90, where fx = (16 x + ox) / 16 with ox the low halfword of the doubleword at gp-28200 (fy uses y and the
// halfword in bits 32..47). Memory is read and written in the original's order, so an overlap between the record or
// the gp globals and a store comes out the same.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;

constexpr GuestAddress kViewportOwner = 0x00670bd0U;   // lw 3024(v1), v1 = 0x00670000
constexpr GuestAddress kFloatGlobal = 0x00670be8U;      // lwc1 3048(v1): truncated to an int into three gp words
constexpr GuestAddress kQuadClamp = 0x0067cfa0U;        // sq -12384(v1), v1 = 0x00680000
constexpr GuestAddress kQuadFrame = 0x0067cfc0U;        // sq -12352(a1), a1 = 0x00680000
constexpr GuestAddress kQuadTex1 = 0x0067cf90U;         // sq -12400(a3), a3 = 0x00680000
constexpr GuestAddress kQuadAlpha = 0x0067cf80U;        // sq -12416(t0), t0 = 0x00680000
constexpr GuestAddress kQuadScissor = 0x0067cfb0U;      // sq -12368(a2), a2 = 0x00680000
constexpr GuestAddress kQuadFloats = 0x0067cd90U;       // sq -12912(a1), a1 = 0x00680000
constexpr std::uint32_t kSixteenth = 0x3d800000U;       // 0.0625f (lui 0x3d80, into f2)

constexpr GuestAddress kGp28216 = static_cast<GuestAddress>(-28216);
constexpr GuestAddress kGp28200 = static_cast<GuestAddress>(-28200);
constexpr GuestAddress kGp28184 = static_cast<GuestAddress>(-28184);
constexpr GuestAddress kGp28176 = static_cast<GuestAddress>(-28176);
constexpr GuestAddress kGp28168 = static_cast<GuestAddress>(-28168);
constexpr GuestAddress kGp27936 = static_cast<GuestAddress>(-27936);
constexpr GuestAddress kGp24080 = static_cast<GuestAddress>(-24080);
constexpr GuestAddress kGp23952 = static_cast<GuestAddress>(-23952);
constexpr GuestAddress kGp23936 = static_cast<GuestAddress>(-23936);
constexpr GuestAddress kGp23932 = static_cast<GuestAddress>(-23932);
constexpr GuestAddress kGp23960 = static_cast<GuestAddress>(-23960);
constexpr GuestAddress kGp23956 = static_cast<GuestAddress>(-23956);
constexpr GuestAddress kGp23916 = static_cast<GuestAddress>(-23916);
constexpr GuestAddress kGp23928 = static_cast<GuestAddress>(-23928);
constexpr GuestAddress kGp23924 = static_cast<GuestAddress>(-23924);
constexpr GuestAddress kGp23920 = static_cast<GuestAddress>(-23920);
constexpr GuestAddress kGp23912 = static_cast<GuestAddress>(-23912);
constexpr GuestAddress kGp23908 = static_cast<GuestAddress>(-23908);

// lh and lw as the R5900 leaves them: sign-extended over the whole 64-bit register.
std::uint64_t sign_extend_halfword(const std::uint16_t half) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(half)));
}
std::uint64_t sign_extend_word(const std::uint32_t word) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
}
// A quadword from its two doublewords; the low one is at the lower address, as sq stores it.
Gpr128 quad_from_doublewords(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

template <class Context>
n3::Run init_viewport_gs_packets(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // Reads that come before the first quadword store.
    const GuestAddress owner = m.read_u32_le(kViewportOwner);
    const GuestAddress record = m.read_u32_le(owner + 96U);
    const std::uint64_t clamp = m.read_u64_le(gp + kGp28184);
    const std::uint64_t frame = m.read_u64_le(gp + kGp28216);
    const std::uint64_t tex1 = m.read_u64_le(gp + kGp28176);
    const std::uint64_t alpha = m.read_u64_le(gp + kGp28168);
    const std::uint64_t offsets = m.read_u64_le(gp + kGp28200);
    const std::uint32_t word_27936 = m.read_u32_le(gp + kGp27936);
    const std::uint32_t word_24080 = m.read_u32_le(gp + kGp24080);

    n3::store_quad_any(m, kQuadClamp, quad_from_doublewords(clamp, 8U));
    n3::store_quad_any(m, kQuadFrame, quad_from_doublewords(frame, 76U));
    const std::uint32_t float_bits = m.read_u32_le(kFloatGlobal);
    n3::store_quad_any(m, kQuadTex1, quad_from_doublewords(tex1, 20U));
    n3::store_quad_any(m, kQuadAlpha, quad_from_doublewords(alpha, 66U));

    // The record's fields, then the SCISSOR_1 doubleword: x, x + w - 1, y, y + h - 1 with each half sign-extended and
    // OR-ed in place, so a negative x or y also sets the bits above its field.
    const std::uint64_t y_half = sign_extend_halfword(m.read_u16_le(record + 30U));
    const std::uint32_t height = m.read_u32_le(record + 16U);
    const std::uint32_t width = m.read_u32_le(record + 12U);
    const std::uint64_t x_half = sign_extend_halfword(m.read_u16_le(record + 28U));
    const auto x0 = static_cast<std::uint32_t>(x_half);
    const auto y0 = static_cast<std::uint32_t>(y_half);
    const std::uint32_t x1 = x0 + width - 1U;
    const std::uint32_t y1 = y0 + height - 1U;
    const std::uint64_t scissor =
        (y_half << 32U) | x_half | (sign_extend_word(x1) << 16U) | (sign_extend_word(y1) << 48U);

    m.write_u32_le(gp + kGp23952, 11U);
    m.write_u32_le(gp + kGp23936, word_24080);
    m.write_u32_le(gp + kGp23932, 0xffffffffU);
    m.write_u32_le(gp + kGp23960, word_27936);
    m.write_u32_le(gp + kGp23956, 0U);
    m.write_u8(gp + kGp23916, 0U);
    n3::store_quad_any(m, kQuadScissor, quad_from_doublewords(scissor, 64U));

    // The float global truncated toward zero (cvt.w.s), stored as an int into three gp words.
    const auto truncated = static_cast<std::uint32_t>(fpu::to_int(float_bits));
    m.write_u32_le(gp + kGp23928, truncated);
    m.write_u32_le(gp + kGp23924, truncated);
    m.write_u32_le(gp + kGp23920, truncated);

    // Viewport origin in 1/16 units: 16 * x + ox and 16 * y + oy. The x offset is the low halfword of the offsets
    // doubleword and the y offset the halfword in bits 32..47. The record is read again, as the original does.
    const std::uint64_t x_again = sign_extend_halfword(m.read_u16_le(record + 28U));
    const auto sum_x = static_cast<std::uint32_t>((offsets & 0xffffU) +
                                                  sign_extend_word(static_cast<std::uint32_t>(x_again) << 4U));
    m.write_u32_le(gp + kGp23912, sum_x);
    const std::uint64_t y_again = sign_extend_halfword(m.read_u16_le(record + 30U));
    const auto sum_y = static_cast<std::uint32_t>(((offsets >> 32U) & 0xffffU) +
                                                  sign_extend_word(static_cast<std::uint32_t>(y_again) << 4U));
    m.write_u32_le(gp + kGp23908, sum_y);

    const std::uint32_t fx = fpu::mul(kSixteenth, fpu::from_int(static_cast<std::int32_t>(sum_x)));
    const std::uint32_t fy = fpu::mul(kSixteenth, fpu::from_int(static_cast<std::int32_t>(sum_y)));
    n3::store_quad_any(m, kQuadFloats, Gpr128{{fx, fy, fx, fy}});

    return call.finish(102U);
}

} // namespace

extern constinit const EngineFunction n3_0021a0f0{0x0021a0f0U, 408U, 0x099be6f4e8d854dbULL, "InitViewportGsPackets",
                                                  102U, n3::live<&init_viewport_gs_packets<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0021a0f0, init_viewport_gs_packets)
