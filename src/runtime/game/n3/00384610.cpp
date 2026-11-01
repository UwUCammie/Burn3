// 0x00384610 (a0 = object O, a1 = destination D of a 16-byte vector): StoreObjectPositionVector. Leaf.
// obj = O[8] (a pointer to a sub-object); the floats at obj+72, +76, +80 are x, y, z. The original builds the
// vector in its own frame (sp-32): it stores x, y, z at frame+0, +4, +8, reloads the quadword with lq, and
// sq's it to D. Word frame+12 is never written, so the stored vector's fourth word is the stale stack value
// there. Bits are copied, never converted. Returns nothing; f0 = z, f1 = y, f2 = x and v1 = the vector are
// left behind as the original leaves them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kSubObjectOffset = 8U;      // obj = O[8]
constexpr GuestAddress kPositionX = 72U;           // obj + 72: x
constexpr GuestAddress kPositionY = 76U;           // obj + 76: y
constexpr GuestAddress kPositionZ = 80U;           // obj + 80: z
constexpr GuestAddress kFrameBytes = 32U;          // addiu $sp, $sp, -32
constexpr std::uint32_t kRetired = 15U;            // straight line, delay slot included

template <class Context>
n3::Run store_object_position_vector(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress destination = call.arg(1);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress sub_object = m.read_u32_le(object + kSubObjectOffset);
    const std::uint32_t x = m.read_u32_le(sub_object + kPositionX);
    const std::uint32_t y = m.read_u32_le(sub_object + kPositionY);
    const std::uint32_t z = m.read_u32_le(sub_object + kPositionZ);
    m.write_u32_le(frame, x);
    m.write_u32_le(frame + 4U, y);
    m.write_u32_le(frame + 8U, z);
    // lq: word 3 is the stale guest stack word at frame+12, which nothing above or below writes.
    const n3::Gpr128 vector = n3::load_quad_any(m, frame);
    m.write_u32_le(frame + 16U, x);
    m.write_u32_le(frame + 20U, y);
    m.write_u32_le(frame + 24U, z);
    n3::store_quad_any(m, destination, vector);
    call.cpu().write_fpr_bits(2U, x);
    call.cpu().write_fpr_bits(1U, y);
    call.cpu().write_fpr_bits(0U, z);
    call.cpu().write_gpr(3U, vector);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00384610{0x00384610U, 60U, 0xcbf6812b1fc40a08ULL,
                                                  "StoreObjectPositionVector", kRetired,
                                                  n3::live<&store_object_position_vector<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00384610, store_object_position_vector)
