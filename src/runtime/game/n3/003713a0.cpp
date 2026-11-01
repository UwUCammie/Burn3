// 0x003713a0 (TexRectToUvFlip): a pixel rectangle becomes four texture coordinates.
// f12/f13 = x0/y0 (rectangle origin), f14/f15 = w/h (rectangle size), f16/f17 = texture
// width/height, all raw bits. Writes four words to a0 in the order
// (u0, v0, u1, v1), where u0 = x0 / texW, v0 = y0 / texH, u1 = (x0 + w) / texW and
// v1 = (y0 + h) / texH. Bit 0 of a1 swaps the two u values and bit 1 swaps the two v
// values. Only memory is written: f0 and v1 are scratch, so nothing is returned.

#include "takedown/runtime/game/n3_support.hpp"

#include <utility>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

template <class Context>
n3::Run tex_rect_to_uv_flip(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress dst = call.arg(0);
    const std::uint32_t flags = call.arg(1);
    const std::uint32_t rect_x = call.cpu().read_fpr_bits(12U);
    const std::uint32_t rect_y = call.cpu().read_fpr_bits(13U);
    const std::uint32_t rect_w = call.cpu().read_fpr_bits(14U);
    const std::uint32_t rect_h = call.cpu().read_fpr_bits(15U);
    const std::uint32_t tex_w = call.cpu().read_fpr_bits(16U);
    const std::uint32_t tex_h = call.cpu().read_fpr_bits(17U);

    std::uint32_t u0 = fpu::div(rect_x, tex_w);
    std::uint32_t v0 = fpu::div(rect_y, tex_h);
    std::uint32_t u1 = fpu::div(fpu::add(rect_x, rect_w), tex_w);
    std::uint32_t v1 = fpu::div(fpu::add(rect_y, rect_h), tex_h);

    // 26 instructions on every path; the u swap adds two MOV.S, the v swap three.
    std::uint32_t retired = 26U;
    if ((flags & 1U) != 0U) {
        std::swap(u0, u1);
        retired += 2U;
    }
    if ((flags & 2U) != 0U) {
        std::swap(v0, v1);
        retired += 3U;
    }

    m.write_u32_le(dst, u0);
    m.write_u32_le(dst + 4U, v0);
    m.write_u32_le(dst + 8U, u1);
    m.write_u32_le(dst + 12U, v1);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_003713a0{0x003713a0U, 124U, 0x740cd3bf4d2d1d94ULL, "TexRectToUvFlip", 31U,
                                           n3::live<&tex_rect_to_uv_flip<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(003713a0, tex_rect_to_uv_flip)
